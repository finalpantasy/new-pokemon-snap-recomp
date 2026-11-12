# Profile switch test: boots the real game (hidden) on build\testprofiles, runs ui_switch_script.txt (imports the unlock_all fixture as a profile through the
# same code as "Import test save", then does what "Switch to this profile" does), and checks that the relaunched game runs that profile (ui_switch_script2.txt).
# Uses run slots via run_boot.ps1 (own private lock only) for the whole run (including the relaunched game); refuses while build\play.lock is live. Only build\testprofiles and
# build\testconfig_prof are used. The relaunched process is waited for and, if needed, killed by its own PID.
param([switch]$Visible)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
if (Test-Path 'build\play.lock') { $pl = (Get-Content 'build\play.lock') -join ' '; if ($pl -match 'pid=(\d+)' -and (Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue)) { Write-Host "play.lock held: $pl"; exit 3 } }
$lock = 'build\switch_test.lock'
for ($i = 0; $i -lt 100; $i++) { try { New-Item -ItemType File $lock -ErrorAction Stop | Out-Null; break } catch { Start-Sleep 20 } if ($i -eq 99) { throw 'no heavy.lock' } }
try {
  "switch-test $PID $(Get-Date)" | Set-Content $lock
  $cfg = Join-Path $root 'build\testconfig_prof'
  Remove-Item -Recurse -Force $cfg -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force $cfg, 'analysis\ui' | Out-Null
  '{"version":2,"mouse":{"lock_cursor":false}}' | Set-Content (Join-Path $cfg 'input.json')
  $pdir = Join-Path $root 'build\testprofiles'
  Remove-Item -Recurse -Force $pdir -ErrorAction SilentlyContinue
  foreach ($id in 'p1', 'p2') { New-Item -ItemType Directory -Force (Join-Path $pdir "$id\save") | Out-Null }
  $now = [int][double]::Parse((Get-Date -UFormat %s))
  @"
{"version": 1, "next": 3, "last_used": "p2", "profiles": [
 {"id": "p1", "name": "Profile 1", "created": $($now - 864000), "last_played": $($now - 86400)},
 {"id": "p2", "name": "Photo run", "created": $($now - 400000), "last_played": $($now - 7200)}], "pending": []}
"@ | Set-Content (Join-Path $pdir 'profiles.json') -Encoding ascii
  $env:SNAP_CONFIG_DIR = $cfg
  $env:SNAP_INPUT_BACKGROUND = '1'
  $env:SNAP_SAVE_DIR = (Join-Path $root 'build\testsave')
  $env:SNAP_SHADERCACHE_DIR = (Join-Path $cfg 'shadercache')
  $env:SNAP_PROFILES_DIR = $pdir
  $env:SNAP_PROFILE_PICKER = '0'
  $env:SNAP_UI_TEST = (Join-Path $root 'tests\hle\ui_switch_script.txt')
  $env:SNAP_UI_TEST_RESTART = (Join-Path $root 'tests\hle\ui_switch_script2.txt')
  $env:SNAP_SHADER_PRECOMPILE = 'playing'
  $ra = @(); if ($Visible) { $ra += '-Visible' }
  powershell -NoProfile -File host\run_boot.ps1 -Timeout 200 @ra run
  $child = 0
  $m = Select-String -Path 'analysis\boot\boot_stderr.log' -Pattern 'relaunched as pid (\d+)' | Select-Object -Last 1
  if ($m) { $child = [int]$m.Matches[0].Groups[1].Value }
  if ($child) {
    $cp = Get-Process -Id $child -ErrorAction SilentlyContinue
    if ($cp) {
      try { $cp.PriorityClass = 'BelowNormal' } catch {}
      if (-not $cp.WaitForExit(240000)) { Write-Host "relaunched game $child still running after 240 s; killing it"; taskkill /PID $child /T /F | Out-Null }
    }
  } else { Write-Host 'no relaunch seen' }
  Select-String -Path 'analysis\boot\boot_stderr.log' -Pattern 'ui-test|ui:|profiles:|relaunched' | ForEach-Object { $_.Line } | Set-Content 'analysis\ui\profiles_test_switch.log'
  Get-Content 'analysis\ui\profiles_test_switch.log' | Select-Object -Last 30
} finally { Remove-Item $lock -Force -ErrorAction SilentlyContinue }
