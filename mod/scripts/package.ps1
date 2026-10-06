<#
.SYNOPSIS
    Build the two release archives from dist, record their checksums, and print them.

.DESCRIPTION
    Synced from release-kit. Do not edit it here; the next sync overwrites it.
    What goes in the archives, which sources count, and which checks must pass
    first all come from release.json at the repo root.

        <fileBase>-<version>.zip      manual install: the plugin, then every
                                      file listed under manualZip, flat
        <fileBase>-<version>-DMM.zip  the plugin, which is all Definitive Mod
                                      Manager deploys, then every file listed
                                      under dmmZip, such as a licence notice the
                                      plugin's code must ship with, named
                                      <fileBase>-<name>, because DMM unpacks
                                      an ASI mod flat into its library, where
                                      every mod's LICENSE would land on the
                                      same file

    Neither archive carries an ini. The plugin writes its own on first run, so
    an upgrade never overwrites settings someone has already changed.

    Refuses when:
      - the plugin is older than any file under the sources in release.json
      - a gate in release.json exits non-zero
      - the plugin is not signed (pass -Unsigned to package anyway)
      - manualZip or dmmZip lists an ini

    Writes private\checksums\<version>.json, which release-docs.py reads to
    fill the checksums into the release documents.

.EXAMPLE
    .\package.ps1
    .\package.ps1 -Unsigned
#>
[CmdletBinding()]
param([switch] $Unsigned)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ReleaseConfig.ps1')
$cfg = Get-ReleaseConfig
$data = $cfg.Data
Add-Type -AssemblyName System.IO.Compression.FileSystem

if (-not (Test-Path -LiteralPath $cfg.Asi)) { throw "Missing $($cfg.Asi). Run the build first." }

# Stale build: a source newer than the plugin means dist holds an old build.
$sourceDirs = @($data.sources | ForEach-Object { Join-Path $cfg.Root $_ })
$newest = Get-ChildItem -LiteralPath $sourceDirs -Recurse -File |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($newest -and $newest.LastWriteTime -gt (Get-Item -LiteralPath $cfg.Asi).LastWriteTime) {
    throw "$($newest.FullName) is newer than the built plugin. Rebuild first."
}

# The mod's own checks, from release.json. They run from the repo root.
foreach ($gate in @($data.gates)) {
    if (-not $gate) { continue }
    Write-Host ("Gate: {0}" -f $gate.name) -ForegroundColor Cyan
    Push-Location $cfg.Root
    try {
        & cmd /c $gate.run
        $code = $LASTEXITCODE
    } finally { Pop-Location }
    if ($code -ne 0) { throw "Gate '$($gate.name)' failed with exit code $code. Run '$($gate.run)' and read its output." }
}

# Sign before packaging, never after: the archives carry the plugin and the
# checksums on the Nexus page are of the signed file.
if (-not $Unsigned) {
    $sig = Get-AuthenticodeSignature -LiteralPath $cfg.Asi
    if ($sig.Status -ne 'Valid') {
        throw "$($cfg.Asi) is not signed (status $($sig.Status)). Run sign.ps1 first, or pass -Unsigned to package anyway."
    }
    Write-Host ("Signed by {0}" -f $sig.SignerCertificate.Subject)
}

# {version} in a path is this release's version, for a file whose name carries it.
$extra = @($data.manualZip | ForEach-Object { Join-Path $cfg.Root ($_ -replace '\{version\}', $cfg.Version) })
# dmmZip is optional, and a missing list piped on would still run once, on null.
$dmmExtra = @(if ($data.dmmZip) { $data.dmmZip | ForEach-Object { Join-Path $cfg.Root ($_ -replace '\{version\}', $cfg.Version) } })
function Get-DmmName($file) {
    $leaf = Split-Path $file -Leaf
    if ($leaf.StartsWith($cfg.FileBase, [StringComparison]::OrdinalIgnoreCase)) { return $leaf }
    return '{0}-{1}' -f $cfg.FileBase, $leaf
}
foreach ($list in @(@{ Key = 'manualZip'; Files = $extra; Archive = 'manual'; Name = { Split-Path $args[0] -Leaf } },
                    @{ Key = 'dmmZip'; Files = $dmmExtra; Archive = 'DMM'; Name = { Get-DmmName $args[0] } })) {
    foreach ($f in $list.Files) {
        if ($f -like '*.ini') { throw "$($list.Key) lists $f. No archive ships an ini; the plugin writes its own." }
        if (-not (Test-Path -LiteralPath $f)) { throw "$($list.Key) lists $f and it does not exist." }
    }
    $leaves = @((Split-Path $cfg.Asi -Leaf)) + @($list.Files | ForEach-Object { & $list.Name $_ })
    $dupes = $leaves | Group-Object | Where-Object Count -gt 1
    if ($dupes) { throw "Two files in the $($list.Archive) archive would share the name $($dupes[0].Name)." }
}

$staging = Join-Path $env:TEMP ("release-kit-{0}-{1}" -f $cfg.FileBase, $cfg.Version)
Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $staging | Out-Null
Copy-Item -LiteralPath $cfg.Asi -Destination $staging
foreach ($f in $extra) { Copy-Item -LiteralPath $f -Destination $staging }

Remove-Item -LiteralPath $cfg.ZipManual, $cfg.ZipDmm -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $cfg.ZipManual
Remove-Item -LiteralPath $staging -Recurse -Force
New-Item -ItemType Directory -Path $staging | Out-Null
Copy-Item -LiteralPath $cfg.Asi -Destination $staging
foreach ($f in $dmmExtra) { Copy-Item -LiteralPath $f -Destination (Join-Path $staging (Get-DmmName $f)) }
Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $cfg.ZipDmm
Remove-Item -LiteralPath $staging -Recurse -Force

Write-Host ("`n{0} {1}" -f $cfg.Name, $cfg.Version)
foreach ($z in @($cfg.ZipManual, $cfg.ZipDmm)) {
    Write-Host ("  {0,-40} {1,9:N0} bytes" -f (Split-Path $z -Leaf), (Get-Item -LiteralPath $z).Length)
    $zip = [System.IO.Compression.ZipFile]::OpenRead($z)
    try { $zip.Entries | ForEach-Object { Write-Host "      $($_.FullName)" } } finally { $zip.Dispose() }
}

# The order the README and the Nexus description list them.
$rows = @(
    @{ role = 'dmm';    path = $cfg.ZipDmm },
    @{ role = 'manual'; path = $cfg.ZipManual },
    @{ role = 'plugin'; path = $cfg.Asi }
)
$record = [ordered] @{ version = $cfg.Version; files = [ordered] @{} }
Write-Host "`nSHA-256:"
foreach ($r in $rows) {
    $hash = (Get-FileHash -LiteralPath $r.path -Algorithm SHA256).Hash.ToLower()
    $leaf = Split-Path $r.path -Leaf
    $record.files[$r.role] = [ordered] @{ name = $leaf; sha256 = $hash }
    Write-Host ("{0}  {1}" -f $hash, $leaf)
}

$outDir = Split-Path $cfg.Checksums -Parent
if (-not (Test-Path -LiteralPath $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
# No BOM: Python reads this file too.
[System.IO.File]::WriteAllText($cfg.Checksums, ($record | ConvertTo-Json -Depth 5), (New-Object System.Text.UTF8Encoding($false)))
Write-Host "`nRecorded in $($cfg.Checksums)"
Write-Host "Next: py -3 release-docs.py stamp"
