<#
 Re-creates the Ghidra analysis project and all analysis\maps exports from the NSO.
 Usage: powershell -NoProfile -File scripts\ghidra_analyze.ps1 [-Stage All|Analyze|Export] [-Project snap-v262144]
          [-Nso <abs path>] [-MapsDir <dir>] [-TimeoutMin 60]
 Analyze: headless import + FDE seeding preScript + full default auto-analysis (BelowNormal), then dumps analyzer options.
          Wall time and provenance go to <ProjectDir>\<Project>.provenance.txt (not part of the deterministic maps).
 Export : headless -process (no analysis, read-only) running ExportMaps.java -> MapsDir.
 Reconcile: python -I scripts\reconcile_functions.py -> functions_final.{tsv,md}, jump_tables.tsv, functions_size_buckets.tsv, functions_interior_entries.tsv (needs the Export maps; about 1 min).
 Never kills anything by name; on timeout it kills only the PID it started.
#>
param(
  [ValidateSet('All','Analyze','Export','Reconcile')][string]$Stage = 'All',
  [string]$Project = 'snap-v262144',
  [string]$Nso,
  [string]$MapsDir,
  [string]$ProjectDir,
  [int]$TimeoutMin = 60,
  [string]$Heap = '12G'
)
$root = Split-Path -Parent $PSScriptRoot
if (-not $Nso)        { $Nso = Join-Path $root 'extracted\v262144\exefs\main.uncompressed.nso' }
if (-not $MapsDir)    { $MapsDir = Join-Path $root 'analysis\maps' }
if (-not $ProjectDir) { $ProjectDir = Join-Path $root 'analysis\ghidra' }
New-Item -ItemType Directory -Force $ProjectDir, $MapsDir | Out-Null
$jdk = Join-Path $root 'tools\jdk-21.0.9+10'
$env:JAVA_HOME = $jdk; $env:PATH = "$jdk\bin;$env:PATH"
$env:GHIDRA_HEADLESS_MAXMEM = $Heap
$ghidra = Join-Path $root 'tools\ghidra_12.1.2_PUBLIC'
$headless = Join-Path $ghidra 'support\analyzeHeadless.bat'
$scripts = Join-Path $root 'scripts\ghidra'
$prog = Split-Path -Leaf $Nso
$fde = Join-Path $ProjectDir "$Project.fde.tsv"
$log = Join-Path $ProjectDir "$Project.$($Stage.ToLower()).log"

function Run-Headless([string[]]$ArgList, [string]$LogFile, [int]$LimitMin) {
  $p = Start-Process -FilePath $headless -PassThru -NoNewWindow -ArgumentList $ArgList `
        -RedirectStandardOutput $LogFile -RedirectStandardError "$LogFile.err"
  $null = $p.Handle
  try { $p.PriorityClass = 'BelowNormal' } catch {}
  [Console]::Error.WriteLine("headless PID $($p.Id)")
  if (-not $p.WaitForExit($LimitMin * 60000)) {
    Write-Host "timeout: killing PID $($p.Id)"; taskkill /PID $p.Id /T /F | Out-Null; return -1
  }
  $p.WaitForExit()
  return [int]$p.ExitCode
}

if ($Stage -in 'All','Analyze') {
  $sha = (Get-FileHash -Algorithm SHA256 $Nso).Hash.ToLower()
  $opts = Join-Path $ProjectDir "$Project.analyzer-options.tsv"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $rc = Run-Headless @("`"$ProjectDir`"", $Project, '-import', "`"$Nso`"", '-overwrite',
      '-analysisTimeoutPerFile', ($TimeoutMin * 60),
      '-scriptPath', "`"$scripts`"",
      '-preScript', 'SeedFdeStarts.java', "`"$fde`"",
      '-postScript', 'DumpAnalysisOptions.java', "`"$opts`"") "$log" $TimeoutMin
  $sw.Stop()
  @("input=$Nso", "input_sha256=$sha", "ghidra=12.1.2 PUBLIC 20260605", "switchloader=1.6.1-0f38957",
    "jdk=temurin 21.0.9+10", "heap=$Heap", "exit=$rc", "wall_seconds=$([int]$sw.Elapsed.TotalSeconds)") |
    Set-Content -Encoding utf8 (Join-Path $ProjectDir "$Project.provenance.txt")
  Write-Host "analysis exit=$rc wall=$([int]$sw.Elapsed.TotalSeconds)s"
  if ($rc -ne 0) { exit $rc }
}

if ($Stage -in 'All','Export') {
  $log = Join-Path $ProjectDir "$Project.export.log"
  $sha = (Get-FileHash -Algorithm SHA256 $Nso).Hash.ToLower()
  $fs = [IO.File]::OpenRead($Nso); $hdr = New-Object byte[] 0x60; [void]$fs.Read($hdr, 0, 0x60); $fs.Close()
  $bid = (($hdr[0x40..0x53] | ForEach-Object { $_.ToString('X2') }) -join '')
  $rc = Run-Headless @("`"$ProjectDir`"", $Project, '-process', $prog, '-noanalysis', '-readOnly',
      '-scriptPath', "`"$scripts`"",
      '-postScript', 'ExportMaps.java', "`"$MapsDir`"", "`"$fde`"", $sha, $bid) "$log" 20
  Write-Host "export exit=$rc"
  if ($Stage -eq 'Export' -or $rc -ne 0) { exit $rc }
}

if ($Stage -in 'All','Reconcile') {
  $py = Join-Path $root 'scripts\reconcile_functions.py'
  $p = Start-Process -FilePath python -PassThru -NoNewWindow -ArgumentList @('-I', "`"$py`"", '--root', "`"$root`"", '--project', $Project, '--out', "`"$MapsDir`"")
  $null = $p.Handle
  try { $p.PriorityClass = 'BelowNormal' } catch {}
  if (-not $p.WaitForExit(30 * 60000)) { taskkill /PID $p.Id /T /F | Out-Null; Write-Host 'reconcile timeout'; exit -1 }
  $p.WaitForExit()
  Write-Host "reconcile exit=$($p.ExitCode)"
  exit $p.ExitCode
}
