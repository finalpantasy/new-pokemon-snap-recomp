# Dot-source: . host\locks.ps1   (needs $root = project root, set by the caller or derived below)
# Lock files hold "<owner> pid=<pid> <MM/dd/yyyy HH:mm:ss> <what>"; created atomically (CreateNew). Stale = pid dead or older than 20 min.
#   build lock : build\heavy.lock              one build (host\build_snap.py, snap_build) at a time. Take-BuildLock / Release-Lock
#   run slots  : build\slots\run0..run3.lock   up to 4 hidden test runs at once (host\run_boot.ps1 does this itself)
#   priority   : while build\heavy.priority exists, run slot 0 is reserved for the run whose $env:SNAP_LOCK_OWNER equals its content
#   exclusive  : -Bench / perf runs: build\slots\exclusive.lock (one exclusive at a time), then all 4 slots; new normal runs yield meanwhile
if (-not $script:LockRoot) { $script:LockRoot = if ($root) { "$root" } else { (Resolve-Path (Join-Path $PSScriptRoot '..')).Path } }
$script:LockMax = 4   # owner 2026-11-12: 4 hidden runs at once (was 6: CPU at 100%, runs crawled; was 3 before that)

function Get-LockText([string]$Path) { try { (Get-Content -LiteralPath $Path -ErrorAction Stop) -join ' ' } catch { $null } }

function Test-LockStale([string]$Path) {
    $t = Get-LockText $Path
    if ($null -eq $t) { return $false }                       # vanished or unreadable: not ours to remove
    $age = ((Get-Date) - (Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue).LastWriteTime).TotalMinutes
    if ($age -gt 20) { return $true }
    if ($t -match 'pid=(\d+)') { return -not [bool](Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue) }
    return ($age -gt 0.2)                                      # no pid: a half-written file older than ~10 s
}

# Atomic create. Returns $true if we now own the lock. A machine-wide named mutex covers the stale check + removal + create, so two takers cannot both
# judge a lock stale and delete each other's fresh lock.
function Try-TakeLock([string]$Path, [string]$What) {
    New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null
    $mx = New-Object System.Threading.Mutex($false, 'Global\SnapRecompLocks')
    try { try { [void]$mx.WaitOne(10000) } catch [System.Threading.AbandonedMutexException] {}
        if ((Test-Path -LiteralPath $Path) -and (Test-LockStale $Path)) { Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue }
        try {
            $owner = if ($env:SNAP_LOCK_OWNER) { $env:SNAP_LOCK_OWNER } else { 'run_boot' }
            $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
            $b = [System.Text.Encoding]::UTF8.GetBytes("$owner pid=$PID $(Get-Date -Format 'MM/dd/yyyy HH:mm:ss') $What")
            $fs.Write($b, 0, $b.Length); $fs.Close()
            return $true
        } catch { return $false }
    } finally { try { $mx.ReleaseMutex() } catch {}; $mx.Dispose() }
}
function Release-Lock([string]$Path) { Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue }

# Build lock: poll until taken (default 30 min). Returns $true/$false. Release with Release-Lock (Join-Path $LockRoot 'build\heavy.lock').
function Take-BuildLock([string]$What = 'build', [int]$WaitSec = 1800) {
    $p = Join-Path $script:LockRoot 'build\heavy.lock'
    for ($i = 0; $i -lt $WaitSec; $i++) { if (Try-TakeLock $p $What) { return $true }; Start-Sleep -Seconds 1 }
    return $false
}

function Get-SlotOwners {
    (0..($script:LockMax - 1) | ForEach-Object { $f = Join-Path $script:LockRoot "build\slots\run$_.lock"; "run$_=" + $(if (Test-Path $f) { Get-LockText $f } else { 'free' }) }) -join ' | '
}

# Take one run slot (returns slot index) or, with -Exclusive, all slots (returns 0..2). $null on timeout. Release with Exit-RunSlots.
function Enter-RunSlots([string]$What, [switch]$Exclusive, [int]$WaitSec = 3600) {
    $dir = Join-Path $script:LockRoot 'build\slots'; New-Item -ItemType Directory -Force $dir | Out-Null
    $xl = Join-Path $dir 'exclusive.lock'; $prio = Join-Path $script:LockRoot 'build\heavy.priority'
    $got = @(); $haveX = $false; $lastMsg = Get-Date '2000-01-01'
    try {
        for ($i = 0; $i -lt ($WaitSec * 2); $i++) {
            if ($Exclusive) {
                if ($haveX -and -not (Test-Path $xl)) { $haveX = $false }   # lost to a stale clean-up: take it again
                if (-not $haveX) { $haveX = Try-TakeLock $xl "$What (exclusive)" }
                if ($haveX) { foreach ($n in 0..($script:LockMax - 1)) { if ($got -notcontains $n -and (Try-TakeLock (Join-Path $dir "run$n.lock") "$What (exclusive)")) { $got += $n } } }
                if ($got.Count -eq $script:LockMax) { $script:SlotsHeld = $got; return $got }
            } elseif (-not (Test-Path $xl) -or (Test-LockStale $xl)) {   # a waiting exclusive run goes first
                $reserved = $null
                if (Test-Path $prio) { $reserved = ((Get-Content $prio -ErrorAction SilentlyContinue) -join '').Trim() }
                $isOwner = $reserved -and $env:SNAP_LOCK_OWNER -and ($env:SNAP_LOCK_OWNER.Trim() -eq $reserved)
                foreach ($n in 0..($script:LockMax - 1)) {
                    if ($n -eq 0 -and $reserved -and -not $isOwner) { continue }
                    if (Try-TakeLock (Join-Path $dir "run$n.lock") $What) { $script:SlotsHeld = @($n); return @($n) }
                }
            }
            if (((Get-Date) - $lastMsg).TotalSeconds -ge 30) { $lastMsg = Get-Date; Write-Host "waiting for run slot$(if ($Exclusive) { 's (exclusive)' }): $(Get-SlotOwners)" }
            Start-Sleep -Milliseconds 500
        }
        return $null
    } finally {
        if ($Exclusive -and -not ($script:SlotsHeld -and $script:SlotsHeld.Count -eq $script:LockMax)) { foreach ($n in $got) { Release-Lock (Join-Path $dir "run$n.lock") }; if ($haveX) { Release-Lock $xl } }
    }
}
function Exit-RunSlots {
    $dir = Join-Path $script:LockRoot 'build\slots'
    foreach ($n in @($script:SlotsHeld)) { Release-Lock (Join-Path $dir "run$n.lock") }
    if (@($script:SlotsHeld).Count -gt 1) { $xl = Join-Path $dir 'exclusive.lock'; if ((Get-LockText $xl) -match "pid=$PID ") { Release-Lock $xl } }   # only our own exclusive.lock
    $script:SlotsHeld = @()
}
