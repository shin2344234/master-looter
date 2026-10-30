<#
.SYNOPSIS
    Tag, push, and create the GitHub release for the current version.

.DESCRIPTION
    Synced from release-kit. Do not edit it here; the next sync overwrites it.

    Before anything is sent it checks that:
      - release-docs.py check passes (documents, checksums, verification, prose)
      - no tracked file has uncommitted changes
      - HEAD is the release commit, subject "Version <version>: <summary>"
      - the branch tracks a remote
      - tag v<version> does not exist yet, or already points at HEAD
      - GitHub has no release for v<version> yet

    With -Apply it then:
      1. creates the annotated tag v<version>, message "Version <version>"
      2. pushes the branch, then the tag
      3. confirms origin holds the tagged commit on the branch. Glint Spotter
         1.1.22 was cut with three commits unpushed, so the tag GitHub made
         pointed at the previous release's source, and every casual check
         passed. This reads origin back instead of trusting the push.
      4. runs gh release create with the notes file, every file under assets
         in release.json and both archives, titled "<name> <version>"

    Report only unless you pass -Apply.

.EXAMPLE
    .\publish-github.ps1
    .\publish-github.ps1 -Apply
#>
[CmdletBinding()]
param([switch] $Apply)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ReleaseConfig.ps1')
$cfg = Get-ReleaseConfig
$v = $cfg.Version
$tag = "v$v"
$title = '{0} {1}' -f $cfg.Name, $v

# git that must succeed: returns its output lines, throws on a non-zero exit.
function Invoke-Git {
    $r = Invoke-Native git (@('-C', $cfg.Root) + $args)
    if ($r.Code -ne 0) { throw ("git {0} failed: {1}" -f ($args -join ' '), ($r.Out -join "`n").Trim()) }
    return $r.Out
}
# git that may fail: returns the result for the caller to read.
function Test-Git { Invoke-Native git (@('-C', $cfg.Root) + $args) }

$problems = @()

Write-Host "release-docs.py check" -ForegroundColor Cyan
$chk = Invoke-Native py @('-3', (Join-Path $PSScriptRoot 'release-docs.py'), 'check')
$chk.Out | ForEach-Object { Write-Host $_ }
if ($chk.Code -ne 0) { $problems += 'release-docs.py check refused (see above)' }

$dirty = @(Invoke-Git status --porcelain --untracked-files=no | Where-Object { $_ })
if ($dirty) { $problems += "uncommitted changes to tracked files:`n    " + ($dirty -join "`n    ") }

$head = @(Invoke-Git rev-parse HEAD)[0].Trim()
$subject = @(Invoke-Git log -1 --format=%s)[0]
$want = '^Version ' + [regex]::Escape($v) + ': .+'
if ($subject -notmatch $want) {
    $problems += "HEAD is '$subject'. The release commit's subject must be 'Version ${v}: <summary>'."
}

$branch = @(Invoke-Git rev-parse --abbrev-ref HEAD)[0].Trim()
$up = Test-Git rev-parse --abbrev-ref '@{u}'
$remote = 'origin'
if ($up.Code -ne 0) { $problems += "branch $branch tracks no remote" } else { $remote = (@($up.Out)[0] -split '/')[0] }

$tagExists = $false
if ((Test-Git rev-parse -q --verify "refs/tags/$tag").Code -eq 0) {
    $tagExists = $true
    $tagCommit = @(Invoke-Git rev-parse "$tag^{commit}")[0].Trim()
    if ($tagCommit -ne $head) { $problems += "tag $tag already exists on $tagCommit, not on HEAD $head" }
    $kind = @(Invoke-Git cat-file -t "refs/tags/$tag")[0].Trim()
    if ($kind -ne 'tag') { $problems += "tag $tag exists but is lightweight; delete it and let this script make an annotated one" }
}

if ($up.Code -eq 0) {
    $rt = Test-Git ls-remote $remote "refs/tags/$tag^{}" "refs/tags/$tag"
    if ($rt.Code -ne 0) {
        $problems += "could not read $remote's tags: " + ($rt.Out -join ' ').Trim()
    } else {
        $lines = @($rt.Out | Where-Object { $_ -match '\S' })
        $peeled = $lines | Where-Object { $_ -match '\^\{\}$' } | Select-Object -First 1
        $plain = $lines | Where-Object { $_ -notmatch '\^\{\}$' } | Select-Object -First 1
        $onRemote = if ($peeled) { ($peeled -split '\s+')[0] } elseif ($plain) { ($plain -split '\s+')[0] } else { '' }
        if ($onRemote -and $onRemote -ne $head) { $problems += "$remote already has $tag on $onRemote, not on HEAD $head" }
    }
}

$view = Invoke-Native gh @('release', 'view', $tag, '--repo', $cfg.GitHub)
if ($view.Code -eq 0) {
    $problems += "GitHub already has a release for $tag"
} elseif (-not ($view.Out -match 'release not found')) {
    $problems += "could not ask GitHub about ${tag}: " + ($view.Out -join ' ').Trim()
}

$uploads = @($cfg.Assets | ForEach-Object { $_.Path }) + @($cfg.ZipManual, $cfg.ZipDmm)
foreach ($f in @($cfg.Notes) + $uploads) {
    if (-not (Test-Path -LiteralPath $f)) { $problems += "missing $f" }
}

Write-Host ""
Write-Host "$title from $branch at $($head.Substring(0, 7)) to $($cfg.GitHub)" -ForegroundColor Cyan
Write-Host ("  tag      {0} (annotated, 'Version {1}'){2}" -f $tag, $v, $(if ($tagExists) { ', exists' } else { '' }))
Write-Host ("  push     {0} {1}, then {0} {2}" -f $remote, $branch, $tag)
Write-Host ("  notes    {0}" -f $cfg.Notes)
$label = '  assets '
foreach ($f in $uploads) {
    Write-Host ("{0}  {1}" -f $label, (Split-Path $f -Leaf))
    $label = '         '
}

if ($problems) {
    Write-Host ""
    foreach ($p in $problems) { Write-Host "REFUSED  $p" -ForegroundColor Red }
    exit 1
}
if (-not $Apply) {
    Write-Host "`nREPORT ONLY. Nothing was tagged, pushed or released. Re-run with -Apply." -ForegroundColor Yellow
    exit 0
}

if (-not $tagExists) { Invoke-Git tag -a $tag -m "Version $v" | Out-Null }
Invoke-Git push $remote $branch | Out-Null
Invoke-Git push $remote $tag | Out-Null

# Read origin back rather than trusting the push's exit status.
Invoke-Git fetch $remote --tags | Out-Null
$remoteTag = @(Invoke-Git ls-remote $remote "refs/tags/$tag^{}") | Where-Object { $_ } | Select-Object -First 1
$remoteTagCommit = if ($remoteTag) { ($remoteTag -split '\s+')[0] } else { '' }
if ($remoteTagCommit -ne $head) { throw "$remote has $tag on '$remoteTagCommit', not on $head. Nothing was released." }
$contains = Invoke-Git branch -r --contains $tag
if (-not ($contains | Where-Object { $_.Trim() -eq "$remote/$branch" })) {
    throw "$remote/$branch does not contain $tag. Nothing was released."
}
Write-Host "$remote/$branch holds $tag at $($head.Substring(0, 7))" -ForegroundColor Green

$made = Invoke-Native gh (@('release', 'create', $tag, '--repo', $cfg.GitHub, '--verify-tag', '--title', $title,
                            '--notes-file', $cfg.Notes) + $uploads)
$made.Out | ForEach-Object { Write-Host $_ }
if ($made.Code -ne 0) { throw "gh release create failed ($($made.Code)). The tag is pushed; fix the cause and run this again." }
Write-Host "Released: $($cfg.ReleaseUrl)" -ForegroundColor Green
