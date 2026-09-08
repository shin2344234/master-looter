<#
.SYNOPSIS
    Publishes a new file version (and optional changelog entry) to an existing
    Nexus Mods mod page using the official Nexus Mods v3 REST API.

.DESCRIPTION
    Implements the documented four-step upload flow from the v3 OpenAPI spec
    (https://api.nexusmods.com/openapi.yaml):

        1. POST /uploads                  -> get an upload id + presigned URL
        2. PUT  <presigned_url>           -> push the archive bytes to storage
        3. POST /uploads/{id}/finalise    -> close the upload session
        4. GET  /uploads/{id}             -> poll until state == "available"
        5. POST /mod-files/{id}/versions  -> attach it as a new version of a file
        6. POST /mods/{id}/changelogs     -> append changelog text (optional)

    WHAT THIS SCRIPT CANNOT DO, because the v3 API has no endpoint for it as of
    2026-09-08 (verified against the OpenAPI spec and a v2 GraphQL introspection):

        * Edit the mod page description       - no endpoint exists
        * Post a news/article/update post     - no endpoint exists
        * Create a brand new mod page         - on Nexus' roadmap, not shipped

    Those three still need the web UI or browser automation.

.NOTES
    Written for Windows PowerShell 5.1. No PS7-only syntax is used, so this runs
    on a stock Windows box or a windows-latest GitHub runner without changes.

    Default behaviour is REPORT ONLY. Nothing is sent to Nexus until you pass
    -Apply. Run it once without -Apply and read the plan before you let it touch
    a live mod page.

.EXAMPLE
    # Dry run: prints exactly what would be sent, makes zero write calls.
    .\Publish-NexusModUpdate.ps1 -FilePath .\dist\my-mod.zip -FileId 123456 `
        -Version 1.4.2 -ModId 78910 -ChangelogPath .\CHANGELOG-latest.md

.EXAMPLE
    # Same call, for real this time.
    .\Publish-NexusModUpdate.ps1 -FilePath .\dist\my-mod.zip -FileId 123456 `
        -Version 1.4.2 -ModId 78910 -ChangelogPath .\CHANGELOG-latest.md -Apply
#>

[CmdletBinding()]
param(
    # Personal API key from https://www.nexusmods.com/settings/api-keys
    # Defaults to the NEXUS_API_KEY environment variable so the key never has to
    # live in a script file or in your shell history.
    [string] $ApiKey = $env:NEXUS_API_KEY,

    # The archive to upload. Must already be zipped; this script does not package.
    [Parameter(Mandatory = $true)]
    [string] $FilePath,

    # The mod FILE id (not the mod id, and not the number in your mod's URL).
    # Find it via "API Info" on the Files tab of your public mod page, or in the
    # edit menu on Manage Files.
    [Parameter(Mandatory = $true)]
    [string] $FileId,

    # Version string for this new file version. Nexus enforces ^[a-zA-Z0-9.-]+$
    # so "1.4.2" and "1.4.2-beta" pass, but "1.4.2 (hotfix)" does not.
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[a-zA-Z0-9.-]+$')]
    [ValidateLength(1, 50)]
    [string] $Version,

    # Display name shown on the Files tab. Nexus enforces ^[a-zA-Z0-9 _'().-]+$
    # and a 50 character cap. Defaults to the archive name without its extension.
    [string] $DisplayName,

    # Optional per-file description shown under the file on the mod page.
    [string] $FileDescription,

    # Nexus only accepts these three values on a new file version.
    [ValidateSet('main', 'optional', 'miscellaneous')]
    [string] $Category = 'main',

    # --- Changelog options (all optional) -----------------------------------
    # The v3 mod id, which is NOT the number in your mod's URL. Required only if
    # you are posting a changelog. Leave it blank and supply -GameDomain plus
    # -GameScopedModId instead and the script will resolve it for you.
    [string] $ModId,

    # e.g. "skyrimspecialedition" - used only to resolve $ModId.
    [string] $GameDomain,

    # The number from your mod's URL, e.g. 12604 - used only to resolve $ModId.
    [string] $GameScopedModId,

    # Changelog text, or a path to a file containing it. Path wins if both given.
    [string] $ChangelogText,
    [string] $ChangelogPath,

    # --- Behaviour switches -------------------------------------------------
    # Bump the mod page's headline version to match this file's version.
    [switch] $UpdateModVersion,

    # Archive the previous file version instead of leaving both listed.
    [switch] $ArchiveExistingFile,

    # Make this the default download for mod managers (Vortex, MO2).
    [switch] $PrimaryModManagerDownload,

    # Nothing is written to Nexus unless this is present.
    [switch] $Apply
)

# Stop on the first unhandled error. A half-finished upload is worse than a
# clean failure, because a dangling upload session is invisible on the mod page
# but a half-attached file version is not.
$ErrorActionPreference = 'Stop'

# PowerShell 5.1 defaults to older TLS on some builds and Nexus will simply reset
# the connection. Force TLS 1.2 rather than debugging a mystery socket error.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# Expect: 100-continue makes presigned storage PUTs fail or stall on some S3
# compatible backends. Turning it off costs nothing here.
[Net.ServicePointManager]::Expect100Continue = $false

# Invoke-WebRequest's progress bar can slow a large upload down by an order of
# magnitude in 5.1. This is not cosmetic, it is a real throughput fix.
$ProgressPreference = 'SilentlyContinue'

$ApiBase = 'https://api.nexusmods.com/v3'

# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------

function Get-ProblemDetail {
    <#
        Nexus returns RFC 9457 "problem details" JSON on 4xx responses, but
        PowerShell 5.1 throws before you ever see the body. This digs the body
        back out of the exception so errors say what actually went wrong instead
        of just "422 Unprocessable Entity".
    #>
    param([System.Management.Automation.ErrorRecord] $ErrorRecord)

    try {
        $response = $ErrorRecord.Exception.Response
        if ($null -eq $response) { return $ErrorRecord.Exception.Message }

        $stream = $response.GetResponseStream()
        $reader = New-Object System.IO.StreamReader($stream)
        $body   = $reader.ReadToEnd()
        $reader.Close()

        if ([string]::IsNullOrWhiteSpace($body)) { return $ErrorRecord.Exception.Message }

        # Try to surface the human-readable fields; fall back to the raw body.
        try {
            $problem = $body | ConvertFrom-Json
            return ("{0}: {1}" -f $problem.title, $problem.detail)
        } catch {
            return $body
        }
    } catch {
        return $ErrorRecord.Exception.Message
    }
}

function Invoke-NexusApi {
    <#
        Thin wrapper around Invoke-RestMethod that attaches the apikey header and
        turns failures into readable messages.

        The body is encoded to UTF-8 bytes by hand. PowerShell 5.1 will otherwise
        send a JSON string as ASCII-ish, which mangles any non-English text in a
        changelog or file description.
    #>
    param(
        [Parameter(Mandatory = $true)][string] $Method,
        [Parameter(Mandatory = $true)][string] $Path,
        [object] $Body
    )

    $uri     = "$ApiBase$Path"
    $headers = @{ 'apikey' = $ApiKey }

    try {
        if ($null -ne $Body) {
            $json  = $Body | ConvertTo-Json -Depth 6 -Compress
            $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
            return Invoke-RestMethod -Uri $uri -Method $Method -Headers $headers `
                                     -ContentType 'application/json; charset=utf-8' -Body $bytes
        }
        return Invoke-RestMethod -Uri $uri -Method $Method -Headers $headers
    } catch {
        throw ("Nexus API {0} {1} failed - {2}" -f $Method, $Path, (Get-ProblemDetail $_))
    }
}

# --------------------------------------------------------------------------
# Validate inputs before touching the network
# --------------------------------------------------------------------------

if ([string]::IsNullOrWhiteSpace($ApiKey)) {
    throw "No API key. Pass -ApiKey or set the NEXUS_API_KEY environment variable."
}

$file = Get-Item -LiteralPath $FilePath
if ($file.PSIsContainer) { throw "FilePath must be a file, not a directory: $FilePath" }

# Single-part uploads are capped at 100 MiB by the API. Above that you need
# POST /uploads/multipart, which is a different flow this script does not cover.
$maxSinglePart = 100MB
if ($file.Length -gt $maxSinglePart) {
    throw ("{0} is {1:N1} MiB. Files over 100 MiB require the multipart upload flow (POST /uploads/multipart), which this script does not implement." -f $file.Name, ($file.Length / 1MB))
}

# Default the display name to the archive name, then check it against the
# pattern Nexus enforces so we fail here rather than after a full upload.
if ([string]::IsNullOrWhiteSpace($DisplayName)) {
    $DisplayName = [System.IO.Path]::GetFileNameWithoutExtension($file.Name)
}
if ($DisplayName -notmatch "^[a-zA-Z0-9 _'().-]+$") {
    throw "DisplayName '$DisplayName' contains characters Nexus rejects. Allowed: letters, digits, space, _ ' ( ) . -"
}
if ($DisplayName.Length -gt 50) {
    throw "DisplayName is $($DisplayName.Length) characters. Nexus caps it at 50."
}

# Resolve the changelog text from a file if a path was given.
if (-not [string]::IsNullOrWhiteSpace($ChangelogPath)) {
    $ChangelogText = Get-Content -LiteralPath $ChangelogPath -Raw -Encoding UTF8
}
$wantsChangelog = -not [string]::IsNullOrWhiteSpace($ChangelogText)

# Changelog posting needs the v3 mod id. Resolve it from the URL number if the
# caller gave us a game domain and scoped id instead.
if ($wantsChangelog -and [string]::IsNullOrWhiteSpace($ModId)) {
    if ([string]::IsNullOrWhiteSpace($GameDomain) -or [string]::IsNullOrWhiteSpace($GameScopedModId)) {
        throw "A changelog was supplied but no -ModId. Provide -ModId, or -GameDomain and -GameScopedModId so it can be looked up."
    }
    Write-Host "Resolving mod id from $GameDomain/$GameScopedModId ..." -ForegroundColor Cyan
    $modLookup = Invoke-NexusApi -Method 'GET' -Path "/games/$GameDomain/mods/$GameScopedModId"
    $ModId = $modLookup.data.id
    if ([string]::IsNullOrWhiteSpace($ModId)) { $ModId = $modLookup.id }  # tolerate either envelope shape
    Write-Host "  Resolved mod id: $ModId" -ForegroundColor Cyan
}

# MD5 serves two purposes: Nexus binds the presigned URL to this exact file, and
# the storage backend verifies the bytes landed intact. Hex goes in the JSON,
# base64 of the same digest goes in the Content-MD5 header on the PUT.
Write-Host "Hashing $($file.Name) ..." -ForegroundColor Cyan
$md5Hex = (Get-FileHash -LiteralPath $file.FullName -Algorithm MD5).Hash.ToLower()
$md5Bytes = New-Object byte[] 16
for ($i = 0; $i -lt 16; $i++) {
    # Walk the 32 hex characters two at a time back into raw bytes for base64.
    $md5Bytes[$i] = [Convert]::ToByte($md5Hex.Substring($i * 2, 2), 16)
}
$md5Base64 = [Convert]::ToBase64String($md5Bytes)

# --------------------------------------------------------------------------
# Report the plan
# --------------------------------------------------------------------------

Write-Host ""
Write-Host "=== PLAN ===" -ForegroundColor Yellow
Write-Host ("  Archive          : {0} ({1:N2} MiB)" -f $file.Name, ($file.Length / 1MB))
Write-Host ("  MD5              : {0}" -f $md5Hex)
Write-Host ("  Target file id   : {0}" -f $FileId)
Write-Host ("  Version          : {0}" -f $Version)
Write-Host ("  Display name     : {0}" -f $DisplayName)
Write-Host ("  Category         : {0}" -f $Category)
Write-Host ("  Update mod ver.  : {0}" -f $UpdateModVersion.IsPresent)
Write-Host ("  Archive previous : {0}" -f $ArchiveExistingFile.IsPresent)
Write-Host ("  Primary MM dl    : {0}" -f $PrimaryModManagerDownload.IsPresent)
if ($wantsChangelog) {
    Write-Host ("  Changelog        : {0} chars appended to version {1} on mod {2}" -f $ChangelogText.Length, $Version, $ModId)
    # The changelog endpoint is APPEND ONLY. Re-running with the same version
    # stacks a second copy of the text rather than replacing it, so this warning
    # is the difference between a clean page and a duplicated mess.
    Write-Host "  NOTE: changelog posts are append-only. Re-running this for the same version duplicates the text." -ForegroundColor DarkYellow
} else {
    Write-Host "  Changelog        : (none)"
}
Write-Host ""

if (-not $Apply) {
    Write-Host "REPORT ONLY - nothing was sent. Re-run with -Apply to publish." -ForegroundColor Green
    return
}

# --------------------------------------------------------------------------
# Step 1: create the upload session
# --------------------------------------------------------------------------

Write-Host "[1/5] Creating upload session ..." -ForegroundColor Cyan
$createBody = @{
    size_bytes = $file.Length
    filename   = $file.Name
    md5        = $md5Hex   # optional today, mandatory from 2026-12-01
}
$created      = Invoke-NexusApi -Method 'POST' -Path '/uploads' -Body $createBody
$uploadId     = $created.data.id
$presignedUrl = $created.data.presigned_url
Write-Host "      upload id: $uploadId"

# --------------------------------------------------------------------------
# Step 2: push the bytes to the presigned URL
# --------------------------------------------------------------------------

Write-Host "[2/5] Uploading archive ..." -ForegroundColor Cyan

# Two headers are load-bearing here:
#   Content-Disposition must match the filename sent in step 1 exactly, because
#     that value is baked into the presigned URL signature.
#   Content-MD5 must be the base64 digest matching the hex md5 sent in step 1.
# Deliberately NOT sending the apikey header, since adding headers the signature
# does not cover will get the PUT rejected by storage.
$putHeaders = @{
    'Content-Disposition' = ('attachment; filename="{0}"' -f $file.Name)
    'Content-MD5'         = $md5Base64
}

try {
    Invoke-WebRequest -Uri $presignedUrl -Method Put -InFile $file.FullName `
                      -Headers $putHeaders -ContentType 'application/octet-stream' `
                      -UseBasicParsing | Out-Null
} catch {
    throw ("Presigned upload failed - {0}" -f (Get-ProblemDetail $_))
}
Write-Host "      bytes sent"

# --------------------------------------------------------------------------
# Step 3: finalise the session
# --------------------------------------------------------------------------

Write-Host "[3/5] Finalising upload ..." -ForegroundColor Cyan
Invoke-NexusApi -Method 'POST' -Path "/uploads/$uploadId/finalise" | Out-Null

# --------------------------------------------------------------------------
# Step 4: wait for Nexus to finish processing (virus scan, indexing)
# --------------------------------------------------------------------------

Write-Host "[4/5] Waiting for upload to become available ..." -ForegroundColor Cyan
$timeout  = (Get-Date).AddMinutes(15)   # generous; scanning a large archive is not instant
$interval = 5
$state    = 'created'

while ($state -ne 'available') {
    if ((Get-Date) -gt $timeout) {
        throw "Upload $uploadId never reached state 'available' within 15 minutes. It may still complete - check Manage Files before re-running, or you risk a duplicate."
    }
    Start-Sleep -Seconds $interval
    $status = Invoke-NexusApi -Method 'GET' -Path "/uploads/$uploadId"
    $state  = $status.data.state
    Write-Host "      state: $state"
}

# --------------------------------------------------------------------------
# Step 5: attach the upload as a new version of the existing mod file
# --------------------------------------------------------------------------

Write-Host "[5/5] Creating mod file version ..." -ForegroundColor Cyan
$versionBody = @{
    upload_id                    = $uploadId
    name                         = $DisplayName
    version                      = $Version
    file_category                = $Category
    update_mod_version           = [bool] $UpdateModVersion
    archive_existing_file        = [bool] $ArchiveExistingFile
    primary_mod_manager_download = [bool] $PrimaryModManagerDownload
}
# Only send description when there is one; the field is nullable but there is no
# reason to transmit an empty string and have it render as a blank line.
if (-not [string]::IsNullOrWhiteSpace($FileDescription)) {
    $versionBody['description'] = $FileDescription
}

$result = Invoke-NexusApi -Method 'POST' -Path "/mod-files/$FileId/versions" -Body $versionBody
Write-Host ("      created version id: {0}" -f $result.data.version.id) -ForegroundColor Green

# --------------------------------------------------------------------------
# Optional: append the changelog entry
# --------------------------------------------------------------------------

if ($wantsChangelog) {
    Write-Host "Posting changelog ..." -ForegroundColor Cyan

    # 65535 character cap per the API schema. Truncate loudly rather than eating
    # a 422 after the file has already gone live.
    if ($ChangelogText.Length -gt 65535) {
        Write-Warning "Changelog is $($ChangelogText.Length) characters, truncating to the 65535 limit."
        $ChangelogText = $ChangelogText.Substring(0, 65535)
    }

    $changelogBody = @{
        version   = $Version
        changelog = $ChangelogText
    }
    Invoke-NexusApi -Method 'POST' -Path "/mods/$ModId/changelogs" -Body $changelogBody | Out-Null
    Write-Host "      changelog appended" -ForegroundColor Green
}

Write-Host ""
Write-Host "Done. Still needs doing by hand: mod description edits and any update post on the mod's board." -ForegroundColor Yellow
