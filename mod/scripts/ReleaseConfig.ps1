# What every release script needs to know about the mod it is running in.
#
# Dot-source it:   . (Join-Path $PSScriptRoot 'ReleaseConfig.ps1')
#                  $cfg = Get-ReleaseConfig
#
# The scripts in this folder are the same in every mod. They are synced from
# release-kit and must not be edited here, because the next sync overwrites
# them. Everything that differs between mods lives in release.json at the repo
# root, and this is the PowerShell side of reading it. release_config.py is
# the Python side and the two must agree.
#
# Written for Windows PowerShell 5.1 as well as 7.

function Find-ReleaseRoot {
    param([string] $Start = $PSScriptRoot)
    $d = $Start
    while ($true) {
        if (Test-Path -LiteralPath (Join-Path $d 'release.json')) { return $d }
        $up = Split-Path $d -Parent
        if ([string]::IsNullOrEmpty($up) -or $up -eq $d) {
            throw "No release.json above $Start. Every mod keeps one at its repo root."
        }
        $d = $up
    }
}

function Get-ReleaseConfig {
    param([string] $Version)
    $root = Find-ReleaseRoot
    $data = Get-Content -LiteralPath (Join-Path $root 'release.json') -Raw -Encoding UTF8 | ConvertFrom-Json

    if ([string]::IsNullOrWhiteSpace($Version)) {
        $header = Join-Path $root $data.version.header
        # A C header names the macro; any other file gives a pattern whose
        # first group is the version, such as a Blender manifest's version line.
        if ($data.version.pattern) {
            $m = Select-String -LiteralPath $header -Pattern $data.version.pattern
            if (-not $m) { throw "No line in $header matches $($data.version.pattern)" }
            $Version = $m.Matches[0].Groups[1].Value
        }
        else {
            $pattern = '#define\s+' + [regex]::Escape($data.version.macro) + '\s+"([^"]+)"'
            $m = Select-String -LiteralPath $header -Pattern $pattern
            if (-not $m) { throw "No $($data.version.macro) in $header" }
            $Version = $m.Matches[0].Groups[1].Value
        }
    }

    $dist = Join-Path $root $data.dist
    $page = ''
    if ($data.nexus -and $data.nexus.page) { $page = [string] $data.nexus.page }
    $game = 'crimsondesert'
    if ($data.nexus -and $data.nexus.game) { $game = $data.nexus.game }

    [pscustomobject] @{
        Root         = $root
        Data         = $data
        Name         = $data.name
        FileBase     = $data.fileBase
        Version      = $Version
        Dist         = $dist
        Asi          = Join-Path $dist ($data.fileBase + '.asi')
        ZipManual    = Join-Path $dist ('{0}-{1}.zip' -f $data.fileBase, $Version)
        ZipDmm       = Join-Path $dist ('{0}-{1}-DMM.zip' -f $data.fileBase, $Version)
        ScriptsDir   = Join-Path $root $data.scriptsDir
        GitHub       = $data.github
        GitHubUrl    = 'https://github.com/' + $data.github
        ReleaseUrl   = 'https://github.com/{0}/releases/tag/v{1}' -f $data.github, $Version
        NexusFiles   = $(if ($page) { 'https://www.nexusmods.com/{0}/mods/{1}?tab=files' -f $game, $page } else { '' })
        Notes        = Join-Path $root ('private\github\release-{0}.md' -f $Version)
        Changelog    = Join-Path $root ('private\nexus\nexus-changelog-{0}.txt' -f $Version)
        Post         = Join-Path $root ('private\nexus\nexus-post-{0}.bbcode' -f $Version)
        DiscordText  = Join-Path $root ('private\discord\release-{0}.txt' -f $Version)
        Checksums    = Join-Path $root ('private\checksums\{0}.json' -f $Version)
    }
}

# Environment first, then keys.local.env beside the scripts. Never printed.
function Read-ReleaseKey {
    param([Parameter(Mandatory = $true)] [string] $Name)
    $v = [Environment]::GetEnvironmentVariable($Name)
    if (-not [string]::IsNullOrWhiteSpace($v)) { return $v.Trim() }
    $keyFile = Join-Path $PSScriptRoot 'keys.local.env'
    if (-not (Test-Path -LiteralPath $keyFile)) { return $null }
    foreach ($line in Get-Content -LiteralPath $keyFile) {
        $t = $line.Trim()
        if ($t -match '^\s*(#|$)' -or $t -notmatch '=') { continue }
        $n, $val = $t -split '=', 2
        if ($n.Trim() -eq $Name) {
            $val = $val.Trim().Trim('"').Trim("'")
            if ($val) { return $val }
        }
    }
    return $null
}

# Run a native command and return its exit code and output as text. Windows
# PowerShell 5.1 turns anything a native command writes to stderr into an
# error record, and with $ErrorActionPreference = 'Stop' that ends the script,
# even for git push, which reports progress on stderr when it succeeds.
function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)] [string] $Exe,
        [string[]] $ArgList = @()
    )
    # The application, never a function or alias of the same name.
    $app = Get-Command $Exe -CommandType Application -ErrorAction Stop | Select-Object -First 1
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $app.Source @ArgList 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $old }
    [pscustomobject] @{ Code = $code; Out = @($out) }
}
