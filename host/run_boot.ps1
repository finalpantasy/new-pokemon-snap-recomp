# Runs build\snap\snap_boot.exe at BelowNormal priority with a hard timeout; kills the run by PID if it hangs.
#   powershell -File host\run_boot.ps1 [-Timeout 300] [-Log analysis\boot\boot_trace.log] <snap_boot args...>   (default args: run)
[CmdletBinding(PositionalBinding=$false)]
param([int]$Timeout = 300, [switch]$KeepSave, [string]$Log = 'analysis\boot\boot_trace.log', [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $root
# a game is being played from build\play (host\play.ps1): do not start a test boot meanwhile
if (Test-Path 'build\play.lock') {
    $pl = (Get-Content 'build\play.lock' -ErrorAction SilentlyContinue) -join ' '
    if ($pl -match 'pid=(\d+)' -and (Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue)) { Write-Host "play.lock held ($pl): not starting a boot run"; exit 3 }
}
New-Item -ItemType Directory -Force (Split-Path $Log) | Out-Null
# Test runs never touch the player's save (%APPDATA%\SnapRecomp\save): they use build\testsave, wiped per run unless -KeepSave.
$testSave = Join-Path $root 'build\testsave'
if (-not $KeepSave -and (Test-Path $testSave)) { Remove-Item -Recurse -Force $testSave -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $testSave | Out-Null
if (-not $env:SNAP_SAVE_DIR) { $env:SNAP_SAVE_DIR = $testSave }
if (-not $Rest) { $Rest = @('run') }
$argl = @($Rest) + @('--trace', $Log, '--timeout', [string]($Timeout - 5))
$err = Join-Path (Split-Path $Log) 'boot_stderr.log'
$p = Start-Process -FilePath 'build\snap\snap_boot.exe' -ArgumentList $argl -NoNewWindow -PassThru -RedirectStandardError $err -RedirectStandardOutput (Join-Path (Split-Path $Log) 'boot_stdout.log')
try { $p.PriorityClass = 'BelowNormal' } catch {}
if (-not $p.WaitForExit($Timeout * 1000)) { taskkill /PID $p.Id /T /F | Out-Null; Write-Host "killed pid $($p.Id) after $Timeout s" }
Write-Host "exit code $($p.ExitCode); stderr in $err, trace in $Log"
