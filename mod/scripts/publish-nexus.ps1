<#
.SYNOPSIS
    Publish the current release to both file entries on the mod's Nexus page.

.DESCRIPTION
    Synced from release-kit. Do not edit it here; the next sync overwrites it.

    A wrapper around Publish-NexusModUpdate.ps1. The ids come from the nexus
    block in release.json, and the archives and changelog follow from the
    version, so a release does not depend on remembering an id. Check the ids
    against the live page with nexus-ids.py.

    One run updates both entries, manual first:
      1. the manual entry (manualFileId) with <fileBase>-<version>.zip, with
         its Mod manager download button turned off and no changelog
      2. the DMM entry (dmmFileId) with <fileBase>-<version>-DMM.zip, as the
         primary mod-manager download, carrying the changelog and setting the
         page version

    The order matters. The Files tab lists main files newest first, so the one
    uploaded last sits on top, and the manual archive must not offer a mod
    manager download at all. On 5 October 2026 Ahplla found Flight Freedom
    1.1.9's manual file above the DMM one with both offering it, so mod
    managers such as DMM fetched the manual archive. The v3 API cannot change
    either flag on a version already up, so a page in that state is fixed in
    the file's Edit dialog on Nexus.

    A page with no manual entry yet (manualFileId empty) gets one: the manual
    archive goes up as a new file entry through POST /mod-files, and the new id
    is written into release.json. Commit that change with the release's
    antivirus commit.

    The changelog endpoint appends rather than replaces, so publishing one
    version twice posts the text twice. Before sending, this asks the API what
    each entry already lists. A version counts as already there only when it is
    not archived or removed, and when its archive is the same size as the local
    one, read from the v1 files list, because the v3 versions list does not say
    which archive a version holds. A match is skipped, which makes a second run
    safe after the first failed half way. A version that is there with a
    different archive stops the run, since that is a wrong upload to repair,
    and the repair command is printed.

    The previous version is left listed, never archived. Nexus demotes it to
    old_version on its own once the new one goes up, and an old_version file
    keeps its download button. -ArchivePrevious pulls a build outright, which
    is the thing Seth does not want.

    Report only unless you pass -Apply.

.EXAMPLE
    .\publish-nexus.ps1
    .\publish-nexus.ps1 -Apply
#>
[CmdletBinding()]
param(
    # Nothing is sent to Nexus without this.
    [switch] $Apply,

    # Override the version read from the header.
    [string] $Version,

    # Archive the previous version. Off by default and meant to stay off.
    [switch] $ArchivePrevious
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ReleaseConfig.ps1')
$cfg = Get-ReleaseConfig -Version $Version
$Version = $cfg.Version
$nx = $cfg.Data.nexus
$game = if ($nx.game) { $nx.game } else { 'crimsondesert' }

foreach ($k in @('modId', 'dmmFileId', 'page')) {
    if (-not $nx -or [string]::IsNullOrWhiteSpace([string] $nx.$k)) {
        throw ("nexus.{0} is not set in release.json. Run nexus-ids.py and copy it in." -f $k)
    }
}
foreach ($f in @($cfg.ZipDmm, $cfg.ZipManual)) {
    if (-not (Test-Path -LiteralPath $f)) { throw "No archive at $f. Run package.ps1 first." }
}
if (-not (Test-Path -LiteralPath $cfg.Changelog)) { throw "No changelog at $($cfg.Changelog). Write it before publishing." }
if ((Get-Content -LiteralPath $cfg.Changelog -Raw) -match '\{\{[A-Z_]+\}\}') {
    throw "$($cfg.Changelog) still has a {{PLACEHOLDER}}. Run release-docs.py stamp."
}

Write-Host ("{0} {1}, page {2}" -f $cfg.Name, $Version, $nx.page) -ForegroundColor Cyan

$entries = @(
    [pscustomobject] @{
        Label = 'manual'; FileId = [string] $nx.manualFileId; Archive = $cfg.ZipManual
        DisplayName = ('{0} {1} manual' -f $cfg.FileBase, $Version); Changelog = $false
        State = 'send'; Note = ''
    },
    [pscustomobject] @{
        Label = 'DMM'; FileId = [string] $nx.dmmFileId; Archive = $cfg.ZipDmm
        DisplayName = ('{0} {1} DMM' -f $cfg.FileBase, $Version); Changelog = $true
        State = 'send'; Note = ''
    }
)
foreach ($e in $entries) { if ([string]::IsNullOrWhiteSpace($e.FileId)) { $e.State = 'create' } }

# What is already up there, per entry. Sizes come from the v1 files list,
# keyed by the version's game_scoped_id.
$key = Read-ReleaseKey -Name 'NEXUS_API_KEY'
if ($Apply -and [string]::IsNullOrWhiteSpace($key)) {
    throw "No NEXUS_API_KEY in the environment or keys.local.env beside this script."
}
$sizes = $null
$problems = @()
if (-not [string]::IsNullOrWhiteSpace($key)) {
    try {
        $v1 = Invoke-RestMethod -Uri ("https://api.nexusmods.com/v1/games/{0}/mods/{1}/files.json" -f $game, $nx.page) `
                                -Headers @{ 'apikey' = $key; 'accept' = 'application/json' } -Method Get
        $sizes = @{}
        foreach ($f in $v1.files) { $sizes[[string] $f.file_id] = [long] $f.size_in_bytes }
    } catch {
        $problems += ("could not read the page's file sizes from the v1 API ({0})" -f $_.Exception.Message)
    }
    foreach ($e in $entries) {
        if ($e.State -eq 'create') { continue }
        try {
            $existing = Invoke-RestMethod -Uri "https://api.nexusmods.com/v3/mod-files/$($e.FileId)/versions" `
                                          -Headers @{ 'apikey' = $key } -Method Get
        } catch {
            $problems += ("could not read what file {0} already lists ({1})" -f $e.FileId, $_.Exception.Message)
            continue
        }
        $hits = @($existing.data.versions | Where-Object {
            $_.version -eq $Version -and @('archived', 'removed') -notcontains $_.category })
        if (-not $hits) { continue }
        $local = (Get-Item -LiteralPath $e.Archive).Length
        $same = @($hits | Where-Object { $sizes -and $sizes[[string] $_.game_scoped_id] -eq $local })
        if ($same) {
            $e.State = 'done'
            $e.Note = 'uploaded {0}, {1:N0} bytes, same as the local archive' -f $same[0].uploaded_at, $local
        } elseif ($null -eq $sizes) {
            $problems += ("{0} entry {1} lists {2}, and its archive could not be checked against the local one" -f $e.Label, $e.FileId, $Version)
        } else {
            $theirs = ($hits | ForEach-Object { '{0:N0}' -f $sizes[[string] $_.game_scoped_id] }) -join ', '
            $e.State = 'wrong'
            $flags = if ($e.Label -eq 'manual') { '-NoModManagerDownload' } else { '-PrimaryModManagerDownload' }
            $problems += (("{0} entry {1} lists {2} with an archive of {3} bytes, but {4} is {5:N0} bytes. " +
                           "It holds a different archive. Repair it with a new version that archives the wrong one:`n" +
                           "    Publish-NexusModUpdate.ps1 -FilePath `"{6}`" -FileId {1} -Version {2} " +
                           "-DisplayName `"{7}`" {8} -ArchiveExistingFile -Apply`n" +
                           "then run this again.") -f $e.Label, $e.FileId, $Version, $theirs,
                          (Split-Path $e.Archive -Leaf), $local, $e.Archive, $e.DisplayName, $flags)
        }
    }
}

# The changelog rides on the DMM upload. If the DMM entry already has this
# version, its changelog is already posted, so it is not posted again.
foreach ($e in $entries) {
    Write-Host ""
    switch ($e.State) {
        'done'   { Write-Host ("{0} entry {1} already has {2} ({3}). Skipped." -f $e.Label, $e.FileId, $Version, $e.Note) -ForegroundColor Yellow }
        'wrong'  { Write-Host ("{0} entry {1} holds the wrong archive for {2}. See below." -f $e.Label, $e.FileId, $Version) -ForegroundColor Red }
        'create' { Write-Host ("{0} entry: none yet. {1} goes up as a new file entry." -f $e.Label, (Split-Path $e.Archive -Leaf)) -ForegroundColor Cyan }
        default  { Write-Host ("{0} entry {1}: {2}" -f $e.Label, $e.FileId, (Split-Path $e.Archive -Leaf)) -ForegroundColor Cyan }
    }
}
if ($problems) {
    Write-Host ""
    foreach ($p in $problems) { Write-Host "REFUSED  $p" -ForegroundColor Red }
    if ($Apply) { Write-Host "Nothing was sent." -ForegroundColor Red; exit 1 }
}
if (($entries | Where-Object { $_.Label -eq 'DMM' }).State -eq 'create') {
    throw "The DMM entry has to exist already; it is the page's primary download. Set nexus.dmmFileId."
}

foreach ($e in $entries) {
    if (@('done', 'wrong') -contains $e.State) { continue }
    Write-Host ""
    $call = @{
        FilePath    = $e.Archive
        ModId       = [string] $nx.modId
        Version     = $Version
        DisplayName = $e.DisplayName
        Category    = 'main'
    }
    if ($e.State -eq 'create') { $call['NewFile'] = $true } else { $call['FileId'] = $e.FileId }
    if ($e.Label -eq 'manual') { $call['NoModManagerDownload'] = $true }
    if ($e.Changelog) {
        $call['ChangelogPath']             = $cfg.Changelog
        $call['UpdateModVersion']          = $true
        $call['PrimaryModManagerDownload'] = $true
    }
    if ($ArchivePrevious -and $e.State -ne 'create') { $call['ArchiveExistingFile'] = $true }
    if ($Apply) { $call['Apply'] = $true }
    $out = & (Join-Path $PSScriptRoot 'Publish-NexusModUpdate.ps1') @call
    $made = @($out | Where-Object { $_ -and $_.PSObject.Properties['FileId'] }) | Select-Object -First 1
    if ($e.State -eq 'create' -and $made) {
        # Record the new entry in release.json by editing the one value, so the
        # file keeps its layout.
        $json = Join-Path $cfg.Root 'release.json'
        $text = [System.IO.File]::ReadAllText($json)
        $new = [regex]::Replace($text, '("manualFileId"\s*:\s*)""', ('${1}"' + $made.FileId + '"'), 1)
        if ($new -eq $text) {
            Write-Host ("Created manual entry {0}, but release.json did not have an empty manualFileId to fill. Set it by hand." -f $made.FileId) -ForegroundColor Red
        } else {
            [System.IO.File]::WriteAllText($json, $new, (New-Object System.Text.UTF8Encoding($false)))
            Write-Host ("Created manual entry {0} and wrote it into release.json as nexus.manualFileId. Commit it with the antivirus commit." -f $made.FileId) -ForegroundColor Green
        }
    }
}

Write-Host ""
if ($Apply) {
    Write-Host "Still by hand, because the v3 API has no endpoint for either:" -ForegroundColor Yellow
} else {
    Write-Host "REPORT ONLY. Nothing was sent. After -Apply, these stay by hand:" -ForegroundColor Yellow
}
Write-Host ("  paste the page description   private\nexus\nexus-description.bbcode")
Write-Host ("  post the update              private\nexus\nexus-post-{0}.bbcode" -f $Version)
if ($problems) { exit 1 }
