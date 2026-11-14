# Option pages test. Run A boots the real game (hidden) with ui_options_script1.txt: it changes graphics, audio, photo and mods options as the F1 controls do, checks the
# "applies now / restart needed" marks and the pending count, then does what "Apply and restart" does (the relaunch itself is logged; a hidden run's job object ends the
# relaunched process with the run, so it is not used for the checks). Run B is a fresh game start on the same profile with ui_options_script2.txt: the saved choices must
# be back, must have reached the game as the documented env vars, and the game must log them (host policy, aa, gamemods lines are checked below).
# Frames with the overlay (NVN_FRAME_DUMP_POST) go to analysis\ui\opt_dump_a and opt_dump_b; tests\hle\opt_shots.py turns them into analysis\ui\f1_<tab>.png.
# Each run takes one run slot via run_boot.ps1; refuses while build\play.lock is live. Own files: build\testconfig_opts, build\testprofiles_opts, build\testsave_opts.
param([switch]$Visible, [string]$BuildDir)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
if (Test-Path 'build\play.lock') { $pl = (Get-Content 'build\play.lock') -join ' '; if ($pl -match 'pid=(\d+)' -and (Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue)) { Write-Host "play.lock held: $pl"; exit 3 } }
$lock = 'build\options_test.lock'
for ($i = 0; $i -lt 100; $i++) { try { New-Item -ItemType File $lock -ErrorAction Stop | Out-Null; break } catch { Start-Sleep 20 } if ($i -eq 99) { throw 'no options_test lock' } }
function Run-Stage($name, $script, $restartScript, $dump) {
  Remove-Item -Recurse -Force $dump -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force $dump | Out-Null
  $env:SNAP_UI_TEST = (Join-Path $root "tests\hle\$script")
  if ($restartScript) { $env:SNAP_UI_TEST_RESTART = (Join-Path $root "tests\hle\$restartScript") } else { Remove-Item Env:\SNAP_UI_TEST_RESTART -ErrorAction SilentlyContinue }
  $env:NVN_FRAME_DUMP = $dump
  $ra = @(); if ($Visible) { $ra += '-Visible' }; if ($BuildDir) { $ra += @('-BuildDir', $BuildDir) }
  $keep = powershell -NoProfile -File host\run_boot.ps1 -Timeout 200 @ra run | Tee-Object -Variable keep
  $rid = ($keep | Select-String -Pattern '^run (\S+) on slot' | Select-Object -First 1).Matches[0].Groups[1].Value
  if (-not $rid) { throw 'no run id' }
  if (-not (Test-Path (Join-Path $root "analysis\boot\runs\$rid\boot_stderr.log"))) { Write-Host "stage $name produced no log; run_boot said:"; $keep | Select-Object -Last 15 | Write-Host }
  $stderr = Join-Path $root "analysis\boot\runs\$rid\boot_stderr.log"
  $m = Select-String -Path $stderr -Pattern 'relaunched as pid (\d+)' | Select-Object -Last 1
  if ($m) { $cp = Get-Process -Id ([int]$m.Matches[0].Groups[1].Value) -ErrorAction SilentlyContinue; if ($cp) { taskkill /PID $cp.Id /T /F | Out-Null } }
  "=== stage $name (run $rid)" | Add-Content 'analysis\ui\options_test.log'
  Select-String -Path $stderr -Pattern 'ui-test|relaunched|host policy|aa: mode|gamemods: UI font|\[mods\] root|\[window\]|photoexport|audio: output' | ForEach-Object { $_.Line } | Add-Content 'analysis\ui\options_test.log'
}
try {
  "options-test $PID $(Get-Date)" | Set-Content $lock
  $cfg = Join-Path $root 'build\testconfig_opts'
  Remove-Item -Recurse -Force $cfg -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force $cfg, 'analysis\ui' | Out-Null
  '{"version":2,"mouse":{"lock_cursor":false}}' | Set-Content (Join-Path $cfg 'input.json')
  foreach ($m in 'alpha', 'beta') {
    $d = Join-Path $cfg "mods\$m"; New-Item -ItemType Directory -Force $d | Out-Null
    "{ `"name`": `"Test pack $m`", `"version`": `"1.0`", `"author`": `"test`", `"description`": `"Empty pack for the Mods tab test ($m).`" }" | Set-Content (Join-Path $d 'pack.json')
  }
  $pdir = Join-Path $root 'build\testprofiles_opts'
  Remove-Item -Recurse -Force $pdir -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force (Join-Path $pdir 'p1\save') | Out-Null
  $now = [int][double]::Parse((Get-Date -UFormat %s))
  @"
{"version": 1, "next": 2, "last_used": "p1", "profiles": [
 {"id": "p1", "name": "Options test", "created": $($now - 864000), "last_played": $($now - 86400)}], "pending": []}
"@ | Set-Content (Join-Path $pdir 'profiles.json') -Encoding ascii
  $env:SNAP_PROFILES_DIR = $pdir
  $env:SNAP_PROFILE = 'Options test'
  $env:SNAP_SAVE_DIR = (Join-Path $root 'build\testsave_opts')
  $env:SNAP_WINDOW_SAVE = '1'
  $env:SNAP_CONFIG_DIR = $cfg
  $env:SNAP_MODS_DIR = (Join-Path $cfg 'mods')
  $env:SNAP_INPUT_BACKGROUND = '1'
  $env:SNAP_WINDOW_W = '1920'; $env:SNAP_WINDOW_H = '1080'
  $env:SNAP_PROFILE_PICKER = '0'
  $env:SNAP_SHADER_PRECOMPILE = 'playing'
  $env:NVN_FRAME_DUMP_POST = '1'; $env:NVN_FRAME_DUMP_FULL = '1'
  $env:NVN_FRAME_DUMP_FROM = '22'; $env:NVN_FRAME_DUMP_EVERY = '2'; $env:NVN_FRAME_DUMP_MAX = '40'
  Remove-Item 'analysis\ui\options_test.log' -ErrorAction SilentlyContinue
  Run-Stage 'A (change options, Apply and restart)' 'ui_options_script1.txt' $null (Join-Path $root 'analysis\ui\opt_dump_a')
  Run-Stage 'B (fresh start on the same profile)' 'ui_options_script2.txt' $null (Join-Path $root 'analysis\ui\opt_dump_b')
  Get-Content 'analysis\ui\options_test.log' | Where-Object { $_ -notmatch '^\[ *[0-9.]+\] \[hle\] ui-test' } | Select-Object -Last 90
  # 2026-11-14: the test used to print the log tail and exit 0 whatever happened. Fail on any FAIL line, a nonzero "failed" count, or no SUMMARY line at all.
  $ulines = @(Get-Content 'analysis\ui\options_test.log' | Where-Object { $_ -match '^ui-test:' })
  $nfail = @($ulines | Where-Object { $_ -match ' FAIL\b' -or $_ -match 'SUMMARY \d+ checks, [1-9]\d* failed' }).Count
  $nsum = @($ulines | Where-Object { $_ -match 'SUMMARY \d+ checks, 0 failed' }).Count
  if ($nfail -gt 0) { Write-Host "OPTIONS TEST FAILED: $nfail failing ui-test line(s) (see analysis\ui\options_test.log)"; $script:testExit = 1 }
  elseif ($nsum -eq 0) { Write-Host 'OPTIONS TEST FAILED: no ui-test SUMMARY line (the game did not finish the scripted checks)'; $script:testExit = 1 }
  else { Write-Host "OPTIONS TEST PASSED ($nsum summary line(s), no failures)" }
} finally { Remove-Item $lock -Force -ErrorAction SilentlyContinue }
if ($script:testExit) { exit $script:testExit }
