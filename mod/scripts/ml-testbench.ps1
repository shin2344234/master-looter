<#
.SYNOPSIS
    Runs on the test machine and does the file moving, so a person only has to
    play the game.

.DESCRIPTION
    Polls a folder on the file server. When a new MasterLooter.asi appears
    there, it copies it into the game's bin64 and records what it did. After
    every game session it copies the logs back out. Both directions are
    verified by SHA-256, because a build that only half arrived has already
    cost an afternoon here.

    It never touches a file while the game is running, never modifies a game
    file, and only ever writes MasterLooter.* inside bin64.

    Leave it running in a PowerShell window. Ctrl+C stops it.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File ml-testbench.ps1
#>
[CmdletBinding()]
param(
    # Where builds arrive and logs are dropped.
    [string] $Share = '\\192.168.50.41\stuff\cd mods',

    # The game. Found automatically if this default is wrong.
    [string] $Bin64 = 'C:\games\Steam\steamapps\common\Crimson Desert\bin64',

    [int] $PollSeconds = 5
)

$ErrorActionPreference = 'Stop'

$Deploy = Join-Path $Share 'deploy'
$Logs   = Join-Path $Share 'logs\live'
$Status = Join-Path $Share 'logs\testbench-status.txt'

function Say([string] $msg) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $msg
    Write-Host $line
    try { Add-Content -LiteralPath $Status -Value $line -ErrorAction Stop } catch { }
}

function Hash([string] $p) {
    try { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower() } catch { $null }
}

function GameRunning() {
    [bool](Get-Process -Name 'CrimsonDesert' -ErrorAction SilentlyContinue)
}

# Find bin64 if the default is not where this machine keeps it.
if (-not (Test-Path -LiteralPath $Bin64)) {
    Say "bin64 not at $Bin64, searching"
    $exe = Get-ChildItem -Path 'C:\','D:\','E:\' -Filter 'CrimsonDesert.exe' -Recurse -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if (-not $exe) { throw "CrimsonDesert.exe not found on this machine." }
    $Bin64 = $exe.DirectoryName
}

foreach ($d in @($Deploy, $Logs)) {
    if (-not (Test-Path -LiteralPath $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
}

$target = Join-Path $Bin64 'MasterLooter.asi'
$source = Join-Path $Deploy 'MasterLooter.asi'

Say "testbench up. bin64 = $Bin64"
Say "watching $source"
Say ("deployed right now: " + $(if (Test-Path $target) { (Hash $target).Substring(0,16) } else { 'nothing' }))

$lastSeenSource = $null
$wasRunning     = $false

while ($true) {
    try {
        $running = GameRunning

        # A session just ended: ship the logs out while they are fresh.
        if ($wasRunning -and -not $running) {
            Start-Sleep -Seconds 2   # let the last write land
            $stamp = Get-Date -Format 'HHmmss'
            foreach ($n in 'MasterLooter.log','MasterLooter.01.log') {
                $src = Join-Path $Bin64 $n
                if (Test-Path -LiteralPath $src) {
                    $dst = Join-Path $Logs ("{0}-{1}" -f $stamp, $n)
                    Copy-Item -LiteralPath $src -Destination $dst -Force
                    $first = (Get-Content -LiteralPath $dst -TotalCount 1)
                    Say "log out: $(Split-Path $dst -Leaf)"
                    if ($first -match 'v(\S+)') { Say "   that run was $($Matches[1])" }
                    if (Select-String -LiteralPath $dst -Pattern '\[crash\] unhandled|\[scan\] fault' -Quiet) {
                        Say "   *** FAULT LINE PRESENT ***"
                        Select-String -LiteralPath $dst -Pattern '\[crash\] unhandled|\[scan\] fault' |
                            ForEach-Object { Say ("   " + $_.Line.Trim()) }
                    }
                }
            }
        }
        $wasRunning = $running

        # Plugin switches, so a test without Crimson Route or another .asi can
        # be set up from the far end. A file named disable-<plugin>.asi in the
        # deploy folder renames that plugin to .asi.off in bin64; enable-<plugin>.asi
        # renames it back. Only .asi files, only in bin64, only while the game
        # is closed, and the marker is removed once acted on. Added 10 September
        # 2026 for the Steam overlay test on the wrapped path.
        if (-not $running) {
            foreach ($m in Get-ChildItem -LiteralPath $Deploy -Filter '*.asi' -ErrorAction SilentlyContinue) {
                if ($m.Name -match '^(disable|enable)-(.+\.asi)$') {
                    $verb = $Matches[1]; $plugin = $Matches[2]
                    if ($plugin -ieq 'MasterLooter.asi') { Say "ignoring $($m.Name): this script only ever installs that one"; Remove-Item -LiteralPath $m.FullName -Force; continue }
                    $on  = Join-Path $Bin64 $plugin
                    $off = "$on.off"
                    if ($verb -eq 'disable' -and (Test-Path -LiteralPath $on)) { Rename-Item -LiteralPath $on -NewName (Split-Path $off -Leaf); Say "disabled $plugin (renamed to .off)" }
                    elseif ($verb -eq 'enable' -and (Test-Path -LiteralPath $off)) { Rename-Item -LiteralPath $off -NewName $plugin; Say "enabled $plugin" }
                    else { Say "nothing to do for $($m.Name): $plugin is already $verb`d or absent" }
                    Remove-Item -LiteralPath $m.FullName -Force
                }
            }
        }

        # A new build arrived: install it, but never under a running game.
        if (Test-Path -LiteralPath $source) {
            $srcHash = Hash $source
            if ($srcHash -and $srcHash -ne $lastSeenSource) {
                if ($running) {
                    Say "new build waiting, game is running; will install when it exits"
                } else {
                    $before = Hash $target
                    if ($srcHash -eq $before) {
                        Say "build $($srcHash.Substring(0,16)) already installed"
                        $lastSeenSource = $srcHash
                    } else {
                        Copy-Item -LiteralPath $source -Destination $target -Force
                        $after = Hash $target
                        if ($after -eq $srcHash) {
                            Say "installed $($srcHash.Substring(0,16)) (was $(if($before){$before.Substring(0,16)}else{'nothing'}))"
                            $lastSeenSource = $srcHash
                        } else {
                            Say "COPY VERIFY FAILED, bin64 has $after, expected $srcHash"
                        }
                    }
                }
            }
        }
    }
    catch {
        Say "error: $($_.Exception.Message)"
    }
    Start-Sleep -Seconds $PollSeconds
}
