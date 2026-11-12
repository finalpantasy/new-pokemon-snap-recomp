# Re-translates the whole corpus (build\xlate\corpus, from scripts\xlate_corpus.py) with one harness build, at BelowNormal.
#   powershell -File scripts\xlate_run.ps1 -Name base|r2 -Salt <salt> [-Out dir] [-First n -Count n]  (chunks: under 10 minutes each)
# Output dir default build\xlate\<Name>\out (<key>.spv/.json, key as the NVN layer computes it for that salt).
param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][string]$Salt, [string]$Out, [int]$First = 0, [int]$Count = 100000)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Out) { $Out = Join-Path $root "build\xlate\$Name\out" }
$dll = Join-Path $root "build\xlate\$Name\h\bin\Release\net10.0\ShaderHarness.dll"
$corp = Join-Path $root 'build\xlate\corpus'
$a = @($dll, '--nvn-batch', (Join-Path $corp 'list.tsv'), (Join-Path $corp 'bins'), $Out, $Salt, $First, $Count)
$p = Start-Process dotnet -ArgumentList ($a | ForEach-Object { '"' + $_ + '"' }) -PassThru -NoNewWindow
$p.PriorityClass = 'BelowNormal'
$p.WaitForExit()
exit $p.ExitCode
