# Builds and runs the host-only HLE tests. Needs Visual Studio Build Tools (vcvars64) and tools\llvm\bin\clang++.
# Runs at BelowNormal priority. Output: tests\hle\out\hle_tests.exe
param([switch]$NoRun)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$vc = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vc`" >nul && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) } }
$env:PATH = "$root\tools\llvm\bin;" + $env:PATH
$out = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$hle = Join-Path $root 'runtime\hle'
$srcs = @((Join-Path $PSScriptRoot 'hle_tests.cpp')) + (Get-ChildItem $hle -Filter *.cpp | ForEach-Object { $_.FullName }) + @((Join-Path $root 'runtime\os\perf.cpp'))
$exe = Join-Path $out 'hle_tests.exe'
& clang++ -std=c++20 -O1 -g -D_CRT_SECURE_NO_WARNINGS -I"$root\runtime" @srcs -o $exe -luser32 -lshell32 -lole32
if ($LASTEXITCODE -ne 0) { throw "build failed" }
if ($NoRun) { return }
$p = Start-Process -FilePath $exe -ArgumentList "`"$root`"" -NoNewWindow -PassThru
$p.PriorityClass = 'BelowNormal'
if (-not $p.WaitForExit(300000)) { Stop-Process -Id $p.Id -Force; throw "tests timed out" }
exit $p.ExitCode
