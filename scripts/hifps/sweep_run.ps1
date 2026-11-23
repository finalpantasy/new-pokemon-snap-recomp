# One frame-rate sweep run (docs/high-fps-native.md): route hifps_sweep at one frame rate target, hidden, with the pacing log and the
# interface trace. Prints "<tag> <run id> exit=<code> pace=<csv>" and appends it to build\w\hifps_runs.txt.
#   powershell -NoProfile -File scripts\hifps\sweep_run.ps1 -Tag r144 -Fps 144 -BuildDir build\w\hf1 [-Cap 0] [-Vsync on] [-Extra 'K=V;K=V'] [-Route hifps_sweep]
# Fps: any SNAP_FPS value (60, 128, 144, unlocked, monitor ...). Cap: SNAP_WINDOW_FRAME_CAP (always set, so the owner's saved limit is never used).
# Window 1920x1080, original visuals (CPU-bound pacing, not the GPU), SNAP_NEO_SPEED=3. Read the result with scripts\hifps\sweep_report.py.
param([Parameter(Mandatory = $true)][string]$Tag, [string]$Fps = '60', [string]$BuildDir = 'build\w\hf1', [int]$Cap = 0, [string]$Vsync = 'on',
      [string]$Extra = '', [string]$Route = 'hifps_sweep', [int]$Timeout = 590)
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
New-Item -ItemType Directory -Force build\w\hifps_pace | Out-Null
$pace = Join-Path $root "build\w\hifps_pace\$Tag.csv"
if (Test-Path $pace) { Remove-Item $pace -Force }
$env:SNAP_PACE_LOG = $pace
$env:SNAP_UI_PACE_TRACE = '1'
$env:SNAP_NEO_SPEED = '3'
$env:SNAP_WINDOW_W = '1920'; $env:SNAP_WINDOW_H = '1080'
$env:SNAP_VISUALS = 'original'
$env:SNAP_WINDOW_FRAME_CAP = "$Cap"
$env:SNAP_VSYNC = $Vsync
if ($Fps -eq '60' -or $Fps -eq 'default') { Remove-Item Env:SNAP_FPS -ErrorAction SilentlyContinue } else { $env:SNAP_FPS = $Fps }
Remove-Item Env:SNAP_FPS_UNCAPPED -ErrorAction SilentlyContinue
foreach ($kv in ($Extra -split ';')) { if ($kv -match '^([A-Z0-9_]+)=(.*)$') { if ($Matches[2] -eq '-') { Remove-Item "Env:$($Matches[1])" -ErrorAction SilentlyContinue } else { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] } } }
$o = & powershell -NoProfile -File host\run_boot.ps1 -Route $Route -BuildDir $BuildDir -Timeout $Timeout -ShotMax 0 2>&1 | Out-String
$id = ([regex]::Match($o, 'run (\d-\d+_\d+) on')).Groups[1].Value
$ec = ([regex]::Match($o, 'exit code (-?\d+)')).Groups[1].Value
$line = "$Tag $id exit=$ec pace=$pace fps=$Fps cap=$Cap vsync=$Vsync extra=$Extra"
$line | Add-Content build\w\hifps_runs.txt
$line
