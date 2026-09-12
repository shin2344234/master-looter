<#
.SYNOPSIS
    Publish the current release to the Master Looter mod page.

.DESCRIPTION
    A wrapper around Publish-NexusModUpdate.ps1 that fills in the three things
    that never change and the two that follow from the version, so a release
    does not depend on remembering an id.

        Mod id  38521561681226   the v3 id, not the 3402 in the page URL
        File id 7932777          the active "MasterLooter ... DMM" entry

    Both were read back from the API on 2026-09-08 and are stable for the life
    of the mod page. The file id is the one worth being careful about: point a
    release at the wrong one and it attaches as a version of some other file.
    Check it with

        py -3 nexus-ids.py

    if the page is ever restructured.

    The version comes from mod/src/version.h, and the archive and changelog are
    derived from it, so this only ever publishes what package.py built.

    Report only unless you pass -Apply, same as the script it wraps.

    The previous version is left listed, never archived. Archiving hides it,
    and people who need an older build (kfen72 asked, 9 September 2026) then
    have nothing to download. What it should become is an Old files entry,
    and the v3 API cannot do that: updateModFile changes the name and nothing
    else, and the category enum for a new file has no old_version value. So
    after -Apply, open Manage Files on the mod page and move the previous
    version to Old files by hand. -ArchivePrevious is there for the one case
    where an old build must be pulled outright.

.EXAMPLE
    .\publish-nexus.ps1
    .\publish-nexus.ps1 -Apply
#>
[CmdletBinding()]
param(
    # Nothing is sent to Nexus without this.
    [switch] $Apply,

    # Override the version read from version.h.
    [string] $Version,

    # Archive the previous version. Off by default: archiving hides it, and an
    # older build should stay downloadable under Old files, which is a manual
    # move on the site because the API cannot set that category.
    [switch] $ArchivePrevious
)

$ErrorActionPreference = 'Stop'

$here = $PSScriptRoot
$mod  = Split-Path $here -Parent
$repo = Split-Path $mod -Parent

if ([string]::IsNullOrWhiteSpace($Version)) {
    $header = Join-Path $mod 'src\version.h'
    $match  = Select-String -LiteralPath $header -Pattern '#define\s+ML_VERSION\s+"([^"]+)"'
    if (-not $match) { throw "No ML_VERSION in $header" }
    $Version = $match.Matches[0].Groups[1].Value
}

$archive   = Join-Path $mod  ("dist\MasterLooter-{0}-DMM.zip" -f $Version)
$changelog = Join-Path $repo ("private\nexus\nexus-changelog-{0}.txt" -f $Version)

if (-not (Test-Path -LiteralPath $archive)) {
    throw "No archive at $archive. Run package.py first."
}
if (-not (Test-Path -LiteralPath $changelog)) {
    throw "No changelog at $changelog. Write it before publishing."
}

Write-Host ("Version $Version, from version.h") -ForegroundColor Cyan

# The changelog endpoint appends rather than replaces, so a second run for one
# version posts the text twice. Check what is already up there and refuse
# rather than leave a duplicated page to clean up by hand.
# The two ids from nexus-ids.py, in one place. They were written out twice,
# once in the duplicate check and once in the arguments, which is one edit away
# from a release attaching itself to the wrong file entry.
$ids = @{
    FileId = '7932777'          # the active "MasterLooter ... DMM" entry
    ModId  = '38521561681226'   # the v3 mod id, not the 3402 in the page URL
}

if ($Apply) {
    $key = $env:NEXUS_API_KEY
    if ([string]::IsNullOrWhiteSpace($key)) {
        $keyFile = Join-Path $here 'keys.local.env'
        if (Test-Path -LiteralPath $keyFile) {
            foreach ($line in Get-Content -LiteralPath $keyFile) {
                $t = $line.Trim()
                if ($t -match '^\s*(#|$)') { continue }
                $n, $v = $t -split '=', 2
                if ($n.Trim() -eq 'NEXUS_API_KEY') { $key = $v.Trim().Trim('"').Trim("'"); break }
            }
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($key)) {
        try {
            $existing = Invoke-RestMethod -Uri "https://api.nexusmods.com/v3/mod-files/$($ids.FileId)/versions" `
                                          -Headers @{ 'apikey' = $key } -Method Get
            $already = $existing.data.versions | Where-Object { $_.version -eq $Version }
            if ($already) {
                Write-Host ""
                Write-Host ("Version {0} is already on the mod page, uploaded {1}." -f $Version, $already[0].uploaded_at) -ForegroundColor Red
                Write-Host "Publishing it again would list a second copy and post the changelog twice." -ForegroundColor Red
                Write-Host "Nothing was sent. Bump version.h and rebuild, or pass -Version for a different one." -ForegroundColor Red
                exit 1
            }
        } catch {
            Write-Warning ("Could not check what is already published ({0}); continuing." -f $_.Exception.Message)
        }
    }
}

$args = @{
    FilePath                  = $archive
    FileId                    = $ids.FileId
    ModId                     = $ids.ModId
    Version                   = $Version
    DisplayName               = ("MasterLooter {0} DMM" -f $Version)
    ChangelogPath             = $changelog
    Category                  = 'main'
    UpdateModVersion          = $true
    PrimaryModManagerDownload = $true
}
if ($ArchivePrevious) { $args['ArchiveExistingFile'] = $true }
if ($Apply)             { $args['Apply']               = $true }

& (Join-Path $here 'Publish-NexusModUpdate.ps1') @args

if ($Apply) {
    Write-Host ""
    Write-Host "Still manual, because the v3 API has no endpoint for either:" -ForegroundColor Yellow
    Write-Host "  the page description  -> private\nexus\nexus-description.bbcode"
    Write-Host ("  the update post       -> private\nexus\nexus-post-{0}.txt" -f $Version)
    Write-Host "  the previous version  -> Manage Files, change its category to Old files (it is still listed as Main)"
}
