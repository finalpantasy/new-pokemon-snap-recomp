<# Decompile module-relative function offsets to a text file (read-only, no analysis, BelowNormal).
   Usage: powershell -NoProfile -File scripts\ghidra_decomp.ps1 -Out <file> -Addrs 4418b0,442cd0
   Caller takes build\heavy.lock first. Kills only its own PID on timeout. #>
param([Parameter(Mandatory)][string]$Out, [Parameter(Mandatory)][string]$Addrs, [string]$Project='snap-v262144', [int]$TimeoutMin=15)
$root = Split-Path -Parent $PSScriptRoot
$jdk = Join-Path $root 'tools\jdk-21.0.9+10'
$env:JAVA_HOME = $jdk; $env:PATH = "$jdk\bin;$env:PATH"; $env:GHIDRA_HEADLESS_MAXMEM = '6G'
$headless = Join-Path $root 'tools\ghidra_12.1.2_PUBLIC\support\analyzeHeadless.bat'
$pd = Join-Path $root 'analysis\ghidra'
$log = "$Out.log"
$p = Start-Process -FilePath $headless -PassThru -NoNewWindow -ArgumentList @("`"$pd`"", $Project, '-process', 'main.uncompressed.nso', '-noanalysis', '-readOnly',
   '-scriptPath', "`"$(Join-Path $root 'scripts\ghidra')`"", '-postScript', 'DecompileFns.java', "`"$Out`"", "`"$Addrs`"") -RedirectStandardOutput $log -RedirectStandardError "$log.err"
$null = $p.Handle; try { $p.PriorityClass = 'BelowNormal' } catch {}
if (-not $p.WaitForExit($TimeoutMin*60000)) { taskkill /PID $p.Id /T /F | Out-Null; Write-Host 'timeout' ; exit 1 }
Write-Host "exit=$($p.ExitCode)"
