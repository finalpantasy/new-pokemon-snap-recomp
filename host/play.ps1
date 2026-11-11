# Plays the game: takes build\heavy.lock, runs build\snap\snap_boot.exe at normal priority with no timeout and Vulkan validation off,
# and releases the lock when the game window closes (also on Ctrl+C or a crash). Stderr goes to analysis\boot\play_stderr.log and the
# trace to analysis\boot\play_trace.log.
#   powershell -File host\play.ps1
[CmdletBinding(PositionalBinding=$false)]
param([string]$Name = 'player', [int]$LockWaitMinutes = 60)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $root
$exe = 'build\snap\snap_boot.exe'
if (-not (Test-Path $exe)) { throw "$exe not found: run tools\venv-recomp\Scripts\python -I host\build_snap.py first" }
New-Item -ItemType Directory -Force 'build', 'analysis\boot' | Out-Null
$lock = 'build\heavy.lock'
$have = $false
$p = $null
try {
    # atomic create; a lock older than 20 minutes is stale
    $deadline = (Get-Date).AddMinutes($LockWaitMinutes)
    while (-not $have) {
        try {
            New-Item -ItemType File $lock -ErrorAction Stop | Out-Null
            $have = $true
        } catch {
            if ((Test-Path $lock) -and ((Get-Date) - (Get-Item $lock).LastWriteTime).TotalMinutes -gt 20) { Remove-Item $lock -Force -ErrorAction SilentlyContinue; continue }
            if ((Get-Date) -gt $deadline) { throw "gave up waiting for $lock" }
            Write-Host "waiting for $lock ($((Get-Content $lock -ErrorAction SilentlyContinue) -join ' '))..."
            Start-Sleep -Seconds 30
        }
    }
    "$Name pid=$PID $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') (play.ps1)" | Set-Content $lock
    Remove-Item Env:NVN_VK_VALIDATION -ErrorAction SilentlyContinue
    Remove-Item Env:SNAP_INPUT_BACKGROUND -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $exe -ArgumentList @('run', '--trace', 'analysis\boot\play_trace.log') -NoNewWindow -PassThru `
        -RedirectStandardError 'analysis\boot\play_stderr.log' -RedirectStandardOutput 'analysis\boot\play_stdout.log'
    try { $p.PriorityClass = 'Normal' } catch {}
    Write-Host "game started (pid $($p.Id)); close its window to quit. Log: analysis\boot\play_stderr.log"
    $p.WaitForExit()
    Write-Host "game exited with code $($p.ExitCode)"
} finally {
    if ($p -and -not $p.HasExited) { taskkill /PID $p.Id /T /F | Out-Null }
    if ($have) { Remove-Item $lock -Force -ErrorAction SilentlyContinue }
}
