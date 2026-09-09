# Capture everything about a hung or crashing Crimson Desert in one go, for
# issue 34. Run it while the game is stuck, before ending the process.
#
#   powershell -File capture34.ps1          gather and leave the game running
#   powershell -File capture34.ps1 -Kill    gather, then end the process
#
# What it collects, into docs\repro-34-captures\<timestamp>\:
#   CrimsonDesert.dmp   a full minidump via comsvcs.dll, no download needed.
#                       Nothing here can read it, but it keeps the whole state
#                       of the hang for a machine that can.
#   threads.txt         every thread with its state, wait reason and the module
#                       its start address falls in. The closest thing to a stack
#                       that PowerShell alone can produce, and enough to see
#                       which module the game's main thread is parked inside.
#   modules.txt         every loaded module with base, size and full path, which
#                       is how a bin64\dxgi.dll proxy is told from the system one.
#   the three logs and the four inis, as they were at that moment.

param([switch]$Kill)

$game = "D:\SteamLibrary\steamapps\common\Crimson Desert\bin64"
$root = "C:\working\cd mods\master looter\docs\repro-34-captures"
$p = Get-Process CrimsonDesert -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Host "CrimsonDesert.exe is not running; nothing to capture."; exit 1 }

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$out = Join-Path $root $stamp
New-Item -ItemType Directory -Force -Path $out | Out-Null
Write-Host "capturing pid $($p.Id) into $out"

# Modules first: the thread report needs them to name start addresses.
$mods = @()
try {
    foreach ($m in $p.Modules) {
        $base = [uint64]$m.BaseAddress.ToInt64()
        $mods += [pscustomobject]@{ Name = $m.ModuleName; Base = $base; End = $base + [uint64]$m.ModuleMemorySize; Path = $m.FileName }
    }
} catch { Write-Host "module list partial: $($_.Exception.Message)" }
$mods | Sort-Object Base | ForEach-Object { "{0:X16}  {1,10}  {2,-32} {3}" -f $_.Base, ($_.End - $_.Base), $_.Name, $_.Path } |
    Set-Content (Join-Path $out "modules.txt")

function Owner([uint64]$a) {
    foreach ($m in $mods) { if ($a -ge $m.Base -and $a -lt $m.End) { return "{0}+0x{1:X}" -f $m.Name, ($a - $m.Base) } }
    return "?"
}

$lines = @("tid        state       wait reason        start address       (module+offset)")
foreach ($t in $p.Threads) {
    $sa = [uint64]$t.StartAddress.ToInt64()
    $wr = if ($t.ThreadState -eq "Wait") { $t.WaitReason } else { "" }
    $lines += "{0,-10} {1,-11} {2,-18} {3:X16}  {4}" -f $t.Id, $t.ThreadState, $wr, $sa, (Owner $sa)
}
$lines | Set-Content (Join-Path $out "threads.txt")

# Full dump. comsvcs.dll ships with Windows and needs no elevation for a process
# of the same user. The argument order is pid, path, then "full".
$dmp = Join-Path $out "CrimsonDesert.dmp"
& rundll32.exe C:\Windows\System32\comsvcs.dll, MiniDump $p.Id $dmp full 2>&1 | Out-Null
Start-Sleep -Seconds 2
$tries = 0
while (-not (Test-Path $dmp) -and $tries -lt 15) { Start-Sleep -Seconds 1; $tries++ }

foreach ($f in "MasterLooter.log", "ReShade.log", "MasterLooter.ini", "ReShade.ini", "CrimsonRoute.ini", "PrivateStorageAnywhere.log") {
    $src = Join-Path $game $f
    if (Test-Path $src) { Copy-Item $src $out -Force }
}
$cr = Get-ChildItem (Join-Path $game "CrimsonRoute-*.log") -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($cr) { Copy-Item $cr.FullName $out -Force }

# Summary to the console, so the interesting part is visible without opening files.
$waiting = $p.Threads | Where-Object { $_.ThreadState -eq "Wait" }
Write-Host ""
Write-Host ("threads: {0} total, {1} waiting" -f $p.Threads.Count, $waiting.Count)
$waiting | Group-Object WaitReason | Sort-Object Count -Descending | ForEach-Object { Write-Host ("  {0,-20} {1}" -f $_.Name, $_.Count) }
$dx = $mods | Where-Object { $_.Name -match '^(dxgi|d3d12|d3d12core)\.dll$' } | ForEach-Object { "  $($_.Name) <- $($_.Path)" }
Write-Host "directx modules actually loaded:"; $dx | ForEach-Object { Write-Host $_ }
if (Test-Path $dmp) { Write-Host ("dump: {0:N0} MB" -f ((Get-Item $dmp).Length / 1MB)) } else { Write-Host "dump: FAILED to write" }
Write-Host "folder: $out"

if ($Kill) { Stop-Process -Id $p.Id -Force; Write-Host "process ended" }
