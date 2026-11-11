<#
 Headless-import an NSO (or ELF) into a Ghidra project WITHOUT auto-analysis, then export an info JSON.
 Usage: powershell -File scripts\ghidra_import.ps1 -InputFile <file> -Project <name> [-ProjectDir <dir>] [-Out <json>]
 Requires tools\ghidra_12.1.2_PUBLIC with the Adubbz SwitchLoader extension in Ghidra\Extensions (see analysis\import\validation.md).
 Runs at BelowNormal priority. Overwrites an existing program of the same name in the project.
#>
param(
  [Parameter(Mandatory)][string]$InputFile,
  [Parameter(Mandatory)][string]$Project,
  [string]$ProjectDir,
  [string]$Out,
  [string]$GhidraDir
)
$root = Split-Path -Parent $PSScriptRoot
if (-not $GhidraDir)   { $GhidraDir = Join-Path $root 'tools\ghidra_12.1.2_PUBLIC' }
if (-not $ProjectDir)  { $ProjectDir = Join-Path $root 'analysis\import\ghidra' }
if (-not $Out)         { $Out = Join-Path $ProjectDir "$Project.info.json" }
New-Item -ItemType Directory -Force $ProjectDir | Out-Null
$jdk = Join-Path $root 'tools\jdk-21.0.9+10'
if (Test-Path $jdk) { $env:JAVA_HOME = $jdk; $env:PATH = "$jdkin;$env:PATH" }
$headless = Join-Path $GhidraDir 'support\analyzeHeadless.bat'
$p = Start-Process -FilePath $headless -PassThru -NoNewWindow -Wait:$false -ArgumentList @(
  "`"$ProjectDir`"", $Project, '-import', "`"$InputFile`"", '-overwrite', '-noanalysis',
  '-scriptPath', "`"$(Join-Path $root 'scripts\ghidra')`"",
  '-postScript', 'ExportImportInfo.java', "`"$Out`"")
try { $p.PriorityClass = 'BelowNormal' } catch {}
$p.WaitForExit()
exit $p.ExitCode
