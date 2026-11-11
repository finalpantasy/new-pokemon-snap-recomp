# Plays the game from a private copy of the build, so workers can keep building and test-booting meanwhile.
#   powershell -File host\play.ps1              play
#   powershell -File host\play.ps1 -SkipIntro   also feed scripted input through loading, name entry and the lab intro (host\skip_intro.txt);
#                                               real input stays active, so you can take over at any time
#   powershell -File host\play.ps1 -ListPads    print every game controller the game sees and show live pad input for 15 s, then exit
# Steps: take build\heavy.lock only while build\snap\snap_boot.exe (and SDL3.dll, the PDB and nvn_cache) are copied to build\play\, release it,
# create build\play.lock (PID; workers must not start boot runs while it exists), run the copy at normal priority with no timeout and
# Vulkan validation off, and delete play.lock on exit, crash or Ctrl+C. The save is %APPDATA%\SnapRecomp\save (SNAP_SAVE_DIR overrides);
# test runs (host\run_boot.ps1) use build\testsave instead. Logs: analysis\boot\play_stderr.log, play_stdout.log (device list), play_trace.log.
[CmdletBinding(PositionalBinding=$false)]
param([switch]$SkipIntro, [switch]$ListPads, [int]$PadSeconds = 15, [string]$Name = 'player', [int]$LockWaitMinutes = 60)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $root
$src = 'build\snap'
if (-not (Test-Path "$src\snap_boot.exe")) { throw "$src\snap_boot.exe not found: run tools\venv-recomp\Scripts\python -I host\build_snap.py first" }

if ($ListPads) {
    $exe = if (Test-Path 'build\play\snap_boot.exe') { 'build\play\snap_boot.exe' } else { "$src\snap_boot.exe" }
    & $exe pads $PadSeconds
    return
}

New-Item -ItemType Directory -Force 'build', 'build\play', 'analysis\boot' | Out-Null
$heavy = 'build\heavy.lock'
$playLock = 'build\play.lock'

function Take-Lock([string]$path, [string]$text, [int]$waitMinutes, [int]$staleMinutes) {
    $deadline = (Get-Date).AddMinutes($waitMinutes)
    while ($true) {
        try {
            New-Item -ItemType File $path -ErrorAction Stop | Out-Null
            $text | Set-Content $path
            return
        } catch {
            if ((Test-Path $path) -and ((Get-Date) - (Get-Item $path).LastWriteTime).TotalMinutes -gt $staleMinutes) { Remove-Item $path -Force -ErrorAction SilentlyContinue; continue }
            if ((Get-Date) -gt $deadline) { throw "gave up waiting for $path" }
            Write-Host "waiting for $path ($((Get-Content $path -ErrorAction SilentlyContinue) -join ' '))..."
            Start-Sleep -Seconds 30
        }
    }
}

# one game at a time; a play.lock whose PID is gone is stale
if (Test-Path $playLock) {
    $old = ((Get-Content $playLock -ErrorAction SilentlyContinue) -join ' ')
    $oldPid = if ($old -match 'pid=(\d+)') { [int]$Matches[1] } else { 0 }
    if ($oldPid -and (Get-Process -Id $oldPid -ErrorAction SilentlyContinue)) { throw "the game is already running ($old)" }
    Remove-Item $playLock -Force -ErrorAction SilentlyContinue
}

# 1. copy under heavy.lock (no build or test boot can run, and snap_boot.exe cannot change under us)
Take-Lock $heavy "$Name pid=$PID $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') (play.ps1 copy)" $LockWaitMinutes 20
try {
    foreach ($f in 'snap_boot.exe', 'SDL3.dll', 'snap_boot.pdb') { if (Test-Path "$src\$f") { Copy-Item "$src\$f" "build\play\$f" -Force } }
    if (Test-Path "$src\nvn_cache") { robocopy "$src\nvn_cache" 'build\play\nvn_cache' /E /MT:8 /NFL /NDL /NJH /NJS /NP | Out-Null }   # exit codes below 8 are success
} finally {
    Remove-Item $heavy -Force -ErrorAction SilentlyContinue
}

# 2. run the copy under play.lock
Take-Lock $playLock "$Name pid=$PID $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') (play.ps1)" 1 1000000
$p = $null
try {
    Remove-Item Env:NVN_VK_VALIDATION -ErrorAction SilentlyContinue
    Remove-Item Env:SNAP_INPUT_BACKGROUND -ErrorAction SilentlyContinue
    if ($SkipIntro) { $env:SNAP_INPUT_SCRIPT = (Resolve-Path 'host\skip_intro.txt').Path } else { Remove-Item Env:SNAP_INPUT_SCRIPT -ErrorAction SilentlyContinue }
    $p = Start-Process -FilePath 'build\play\snap_boot.exe' -ArgumentList @('run', '--trace', 'analysis\boot\play_trace.log') -NoNewWindow -PassThru `
        -RedirectStandardError 'analysis\boot\play_stderr.log' -RedirectStandardOutput 'analysis\boot\play_stdout.log'
    try { $p.PriorityClass = 'Normal' } catch {}
    Write-Host "game started (pid $($p.Id)); close its window to quit. Click the window once so it has focus."
    if ($SkipIntro) { Write-Host "-SkipIntro: scripted input runs until the first tutorial step (about 3 to 4 minutes); real input works at any time." }
    $p.WaitForExit()
    Write-Host "game exited with code $($p.ExitCode)"
} finally {
    if ($p -and -not $p.HasExited) { taskkill /PID $p.Id /T /F | Out-Null }
    Remove-Item $playLock -Force -ErrorAction SilentlyContinue
}
