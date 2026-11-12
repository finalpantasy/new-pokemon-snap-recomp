# Builds and runs the host-only mod loader tests (runtime/mods/mods_core). Needs Visual Studio Build Tools and tools\llvm\bin\clang++.
# BelowNormal priority. Output: tests\mods\out\mods_tests.exe
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$vc = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vc`" >nul && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) } }
$env:PATH = "$root\tools\llvm\bin;" + $env:PATH
$out = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'mods_tests.exe'
& clang++ -std=c++20 -O1 -g -D_CRT_SECURE_NO_WARNINGS -I"$root\runtime" (Join-Path $PSScriptRoot 'mods_tests.cpp') (Join-Path $root 'runtime\mods\mods_core.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "build failed" }
$p = Start-Process -FilePath $exe -NoNewWindow -PassThru
$p.PriorityClass = 'BelowNormal'
if (-not $p.WaitForExit(120000)) { taskkill /PID $p.Id /T /F | Out-Null; throw "tests timed out" }
exit $p.ExitCode
