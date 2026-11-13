# Plays the game from a private copy of the build, so workers can keep building and test-booting meanwhile.
#   powershell -File host\play.ps1              play
#   powershell -File host\play.ps1 -SkipIntro   also feed scripted input through loading, name entry and the lab intro (host\skip_intro.txt);
#                                               real input stays active, so you can take over at any time
#   powershell -File host\play.ps1 -Profile "Photo run"   start with that save profile (created if new); -PickProfile always shows the profile picker
#   powershell -File host\play.ps1 -ListPads    print every game controller the game sees and show live pad input for 15 s, then exit
#   powershell -File host\play.ps1 -NoNativeOptions   do not (re)build the native-options pack (PC tabs in the game's Settings screen)
# Steps: take build\heavy.lock only while build\snap\snap_boot.exe (and SDL3.dll, the PDB and nvn_cache) are copied to build\play\, release it,
# create build\play.lock (PID; workers must not start boot runs while it exists), run the copy at normal priority with no timeout and
# Vulkan validation off, and delete play.lock on exit, crash or Ctrl+C. The save is %APPDATA%\SnapRecomp\save (SNAP_SAVE_DIR overrides);
# test runs (host\run_boot.ps1) use build\testsave instead. Logs: analysis\boot\play_stderr.log, play_stdout.log (device list), play_trace.log.
[CmdletBinding(PositionalBinding=$false)]
param([switch]$SkipIntro, [switch]$ListPads, [int]$PadSeconds = 15, [string]$Name = 'player', [int]$LockWaitMinutes = 60,
       [string]$Profile, [switch]$PickProfile, [switch]$NoNativeOptions)
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

# 1b. the "native-options" mod pack (PC tabs in the game's own Settings screen) is built from the user's own extracted
# option.lmb into the mods folder (%APPDATA%\SnapRecomp\mods, or SNAP_MODS_DIR) when missing or older than its generator;
# nothing game-derived is in the repo. A failure only means no PC tabs: the game and the F1 menu still work.
if (-not $NoNativeOptions) {
    $py = 'tools\venv-recomp\Scripts\python.exe'
    $lmb = 'extracted\drp-v262144\common\UI\master\Scenes\option.drp\option.lmb'
    if ((Test-Path $py) -and (Test-Path $lmb)) {
        $genArgs = @('-I', 'scripts\ui\make_native_options.py', '--if-stale')
        if ($env:SNAP_MODS_DIR) { $genArgs += @('--out', $env:SNAP_MODS_DIR) }
        & $py @genArgs
        if ($LASTEXITCODE -ne 0) { Write-Host "native-options pack not built (exit $LASTEXITCODE): PC tabs off, the F1 menu still works" }
    } else { Write-Host "native-options pack skipped: $lmb or $py missing" }
}

# 2. run the copy under play.lock
Take-Lock $playLock "$Name pid=$PID $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') (play.ps1)" 1 1000000
$p = $null
try {
    Remove-Item Env:NVN_VK_VALIDATION -ErrorAction SilentlyContinue
    Remove-Item Env:SNAP_PLAY_ROOT -ErrorAction SilentlyContinue
    Remove-Item Env:SNAP_PLAY_ROOT -ErrorAction SilentlyContinue
    Remove-Item Env:SNAP_INPUT_BACKGROUND -ErrorAction SilentlyContinue
    if ($Profile) { $env:SNAP_PROFILE = $Profile } else { Remove-Item Env:SNAP_PROFILE -ErrorAction SilentlyContinue }
    if ($PickProfile) { $env:SNAP_PROFILE_PICKER = '1' } else { Remove-Item Env:SNAP_PROFILE_PICKER -ErrorAction SilentlyContinue }
    if ($SkipIntro) { $env:SNAP_INPUT_SCRIPT = (Resolve-Path 'host\skip_intro.txt').Path } else { Remove-Item Env:SNAP_INPUT_SCRIPT -ErrorAction SilentlyContinue }
    $p = Start-Process -FilePath 'build\play\snap_boot.exe' -ArgumentList @('run', '--trace', 'analysis\boot\play_trace.log') -NoNewWindow -PassThru `
        -RedirectStandardError 'analysis\boot\play_stderr.log' -RedirectStandardOutput 'analysis\boot\play_stdout.log'
    try { $p.PriorityClass = 'Normal' } catch {}
    Write-Host "game started (pid $($p.Id)); close its window to quit. Click the window once so it has focus."
    if ($SkipIntro) { Write-Host "-SkipIntro: scripted input runs through the lab cutscene (about 3 to 4 minutes), then presses nothing: you take over at the tutorial panels and photo selection. Real input works at any time." }
    $env:SNAP_PLAY_ROOT = "$root"   # tells the game it was started here: "Restart now" / "Switch profile" in the F1 menu relaunch it (same exe, arguments, working directory, environment) and write the new pid to build\play.restart
    $restartFile = Join-Path $root 'build\play.restart'
    Remove-Item $restartFile -Force -ErrorAction SilentlyContinue
    while ($true) {
        $p.WaitForExit()
        Write-Host "game exited with code $($p.ExitCode)"
        $np = 0
        if (Test-Path $restartFile) {
            try { $np = [int](Get-Content $restartFile -ErrorAction Stop | Select-Object -First 1) } catch {}
            Remove-Item $restartFile -Force -ErrorAction SilentlyContinue
        }
        if ($np -le 0) { break }
        $p = Get-Process -Id $np -ErrorAction SilentlyContinue
        if (-not $p) { break }
        Write-Host "game restarted (pid $np)"
    }
} finally {
    if ($p -and -not $p.HasExited) { taskkill /PID $p.Id /T /F | Out-Null }
    Remove-Item $playLock -Force -ErrorAction SilentlyContinue
}
