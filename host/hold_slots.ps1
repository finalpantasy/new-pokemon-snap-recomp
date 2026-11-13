# Owner 2026-11-12: "while i use the pc you can only have 1 instance of pokemon snap open".
# Holds run slots >= the cap in build\slots\max.txt so no test run can take them, even runs that loaded locks.ps1 before the cap was set.
# Start: write a number (1..3) to build\slots\max.txt, then run this hidden. Stop: delete max.txt (this script then frees the slots and exits).
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$env:SNAP_LOCK_OWNER = 'pc-in-use'
. (Join-Path $PSScriptRoot 'locks.ps1')
$dir = Join-Path $root 'build\slots'; $capFile = Join-Path $dir 'max.txt'
$held = @{}
while (Test-Path $capFile) {
    $cap = 0; [void][int]::TryParse(((Get-Content $capFile -TotalCount 1) -as [string]).Trim(), [ref]$cap); if ($cap -lt 1) { $cap = 1 }
    foreach ($n in 0..3) {
        $p = Join-Path $dir "run$n.lock"
        if ($n -lt $cap) { if ($held[$n]) { Release-Lock $p; $held[$n] = $false }; continue }
        if (-not $held[$n] -and ((Get-LockText $p) -match "pid=$PID ")) { $held[$n] = $true }   # adopt a lock written for us
        if ($held[$n]) { (Get-Item -LiteralPath $p -ErrorAction SilentlyContinue).LastWriteTime = Get-Date }   # stay fresh (stale after 20 min)
        elseif (Try-TakeLock $p 'owner is using the PC: slot closed') { $held[$n] = $true }
    }
    Start-Sleep -Seconds 2
}
foreach ($n in @($held.Keys)) { if ($held[$n]) { Release-Lock (Join-Path $dir "run$n.lock") } }
