# Boots the real game for the save-profile and shader-UI screenshots. Uses run slots via run_boot.ps1 (own private lock only); refuses while the player runs (build\play.lock).
#   -Mode saves     seeds build\testprofiles (3 profiles, two with the post_tutorial fixture), forces the startup picker (auto-picks #2 after a
#                   screenshot), then opens F1 > Saves and F1 > Graphics and takes pictures (analysis\ui\profiles_picker.png, saves_tab.png)
#   -Mode prompt    first-launch shader prompt (SNAP_SHADER_PROMPT=1)
#   -Mode progress  the progress screen (mode NOW, one compile thread, recorded keys copied from build\perfcache\a)
# Only build\testprofiles, build\testconfig_prof and build\testsave are used; the real %APPDATA% is never touched.
param([ValidateSet('saves','prompt','progress','audio1','audio2')][string]$Mode = 'saves', [switch]$Visible)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
if (Test-Path 'build\play.lock') { $pl = (Get-Content 'build\play.lock') -join ' '; if ($pl -match 'pid=(\d+)' -and (Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue)) { Write-Host "play.lock held: $pl"; exit 3 } }
$lock = 'build\profiles_test.lock'
for ($i = 0; $i -lt 120; $i++) { try { New-Item -ItemType File $lock -ErrorAction Stop | Out-Null; break } catch { Start-Sleep 30 } if ($i -eq 119) { throw 'no heavy.lock' } }
try {
  "profiles-test $PID $(Get-Date)" | Set-Content $lock
  $cfg = Join-Path $root 'build\testconfig_prof'
  if ($Mode -ne 'audio2') { Remove-Item -Recurse -Force $cfg -ErrorAction SilentlyContinue }   # audio2 reuses the config folder audio1 wrote
  foreach ($v in 'SNAP_VOL_MASTER','SNAP_VOL_BGM','SNAP_VOL_SE','SNAP_VOL_VOICE') { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
  New-Item -ItemType Directory -Force $cfg, 'analysis\ui' | Out-Null
  if ($Mode -ne 'audio2') { '{"version":2,"mouse":{"lock_cursor":false}}' | Set-Content (Join-Path $cfg 'input.json') }
  $env:SNAP_CONFIG_DIR = $cfg
  $env:SNAP_INPUT_BACKGROUND = '1'
  $env:SNAP_SAVE_DIR = (Join-Path $root 'build\testsave')
  $env:SNAP_SHADERCACHE_DIR = (Join-Path $cfg 'shadercache')
  if ($Mode -eq 'saves') {
    $pdir = Join-Path $root 'build\testprofiles'
    Remove-Item -Recurse -Force $pdir -ErrorAction SilentlyContinue
    $fx = Join-Path $root 'build\fixtures\saves\post_tutorial'
    foreach ($id in 'p1', 'p2', 'p3') { New-Item -ItemType Directory -Force (Join-Path $pdir "$id\save") | Out-Null }
    if (Test-Path $fx) { foreach ($id in 'p1', 'p2') { robocopy $fx (Join-Path $pdir "$id\save") /E /NFL /NDL /NJH /NJS /NP | Out-Null } }
    $now = [int][double]::Parse((Get-Date -UFormat %s))
    @"
{"version": 1, "next": 4, "last_used": "p1", "profiles": [
 {"id": "p1", "name": "Profile 1", "created": $($now - 864000), "last_played": $($now - 86400)},
 {"id": "p2", "name": "Photo run", "created": $($now - 400000), "last_played": $($now - 7200)},
 {"id": "p3", "name": "Unlock everything", "created": $($now - 3600), "last_played": 0}], "pending": []}
"@ | Set-Content (Join-Path $pdir 'profiles.json') -Encoding ascii
    $env:SNAP_PROFILES_DIR = $pdir
    $env:SNAP_PROFILE_PICKER = '1'
    $env:SNAP_PICKER_SHOT = (Join-Path $root 'analysis\ui\profiles_picker.bmp')
    $env:SNAP_PICKER_AUTO = '1'
    $env:SNAP_UI_TEST = (Join-Path $root 'tests\hle\ui_saves_script.txt')
    $env:SNAP_SHADER_PRECOMPILE = 'playing'
  } elseif ($Mode -eq 'audio1' -or $Mode -eq 'audio2') {
    $env:SNAP_SHADER_PRECOMPILE = 'playing'
    $env:SNAP_UI_TEST = (Join-Path $root ('tests/hle/ui_audio_script' + $Mode.Substring(5) + '.txt'))
  } elseif ($Mode -eq 'prompt') {
    $env:SNAP_SHADER_PROMPT = '1'
    $env:SNAP_UI_TEST = (Join-Path $root 'tests\hle\ui_shader_script.txt')
    if (Test-Path 'build\perfcache\a\pipeline_keys.bin') { New-Item -ItemType Directory -Force $env:SNAP_SHADERCACHE_DIR | Out-Null; Copy-Item 'build\perfcache\a\pipeline_keys.bin' $env:SNAP_SHADERCACHE_DIR }
  } else {
    $env:SNAP_SHADER_PRECOMPILE = 'playing'   # the real run finishes in well under a second, so the screen is shown with simulated numbers
    $env:SNAP_SHADER_UI_DEMO = '1'
    $env:SNAP_UI_TEST = (Join-Path $root 'tests\hle\ui_shader_script.txt')
    New-Item -ItemType Directory -Force $env:SNAP_SHADERCACHE_DIR | Out-Null
    Copy-Item 'build\perfcache\a\pipeline_keys.bin' $env:SNAP_SHADERCACHE_DIR
  }
  $ra = @(); if ($Visible) { $ra += '-Visible' }
  powershell -NoProfile -File host\run_boot.ps1 -Timeout 200 @ra run
  Select-String -Path 'analysis\boot\boot_stderr.log' -Pattern 'ui-test|ui:|profiles:' | ForEach-Object { $_.Line } | Set-Content "analysis\ui\profiles_test_$Mode.log"
  Get-Content "analysis\ui\profiles_test_$Mode.log" | Select-Object -Last 25
} finally { Remove-Item $lock -Force -ErrorAction SilentlyContinue }
