# Boots the real game with the scripted controls-menu test (tests\hle\ui_test_script.txt): F1 is posted to the game window, a keyboard
# slot is rebound, and the new binding is checked at the npad. Takes build\heavy.lock; refuses while the player runs (build\play.lock).
# Uses build\testsave and build\testconfig only. Output: analysis\ui\ui_test.log (the "ui-test:" lines) and analysis\ui\controls_menu.png
param([switch]$Visible, [string]$SaveFixture)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
if (Test-Path 'build\play.lock') { $pl = (Get-Content 'build\play.lock') -join ' '; if ($pl -match 'pid=(\d+)' -and (Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue)) { Write-Host "play.lock held: $pl"; exit 3 } }
$lock = 'build\ui_test.lock'
for ($i = 0; $i -lt 60; $i++) { try { New-Item -ItemType File $lock -ErrorAction Stop | Out-Null; break } catch { Start-Sleep 30 } if ($i -eq 59) { throw 'no heavy.lock' } }
try {
  "ui-test $PID $(Get-Date)" | Set-Content $lock
  $cfg = Join-Path $root 'build\testconfig'
  Remove-Item -Recurse -Force $cfg -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force $cfg | Out-Null
  '{"version":2,"mouse":{"lock_cursor":false}}' | Set-Content (Join-Path $cfg 'input.json')   # never confine the real cursor in a test
  $env:SNAP_CONFIG_DIR = $cfg
  $env:SNAP_UI_TEST = (Join-Path $root 'tests\hle\ui_test_script.txt')
  $env:SNAP_INPUT_BACKGROUND = '1'
  New-Item -ItemType Directory -Force 'analysis\ui' | Out-Null
  $ra = @(); if ($Visible) { $ra += '-Visible' }; if ($SaveFixture) { $ra += @('-SaveFixture', $SaveFixture) }
  powershell -NoProfile -File host\run_boot.ps1 -Timeout 240 @ra run
  Select-String -Path 'analysis\boot\boot_stderr.log' -Pattern 'ui-test' | ForEach-Object { $_.Line } | Set-Content 'analysis\ui\ui_test.log'
  Get-Content 'analysis\ui\ui_test.log' | Select-Object -Last 40
} finally { Remove-Item $lock -Force -ErrorAction SilentlyContinue }
