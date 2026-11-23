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
& clang++ -std=c++20 -O1 -g -D_CRT_SECURE_NO_WARNINGS -I"$root\runtime" (Join-Path $PSScriptRoot 'mods_tests.cpp') (Join-Path $root 'runtime\mods\mods_core.cpp') (Join-Path $root 'runtime\hle\zip_read.cpp') (Join-Path $root 'runtime\mods\mods_thumb.cpp') (Join-Path $root 'runtime\nvn\nvn_tex.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "build failed" }
$p = Start-Process -FilePath $exe -NoNewWindow -PassThru
$p.PriorityClass = 'BelowNormal'
if (-not $p.WaitForExit(120000)) { taskkill /PID $p.Id /T /F | Out-Null; throw "tests timed out" }
$code = $p.ExitCode
# Interface pacing decision and pad sampling (host/mods/uipace_core.h).
$exe2 = Join-Path $out 'uipace_test.exe'
& clang++ -std=c++20 -O1 -g (Join-Path $PSScriptRoot 'uipace_test.cpp') -o $exe2
if ($LASTEXITCODE -ne 0) { throw "uipace_test build failed" }
$p2 = Start-Process -FilePath $exe2 -NoNewWindow -PassThru
$p2.PriorityClass = 'BelowNormal'
if (-not $p2.WaitForExit(60000)) { taskkill /PID $p2.Id /T /F | Out-Null; throw "uipace_test timed out" }
if ($p2.ExitCode -ne 0) { $code = $p2.ExitCode }
# Smooth interface above 60 fps (runtime/nvn/nvn_uismooth.h).
$exe3 = Join-Path $out 'uismooth_test.exe'
& clang++ -std=c++20 -O1 -g (Join-Path $PSScriptRoot 'uismooth_test.cpp') -o $exe3
if ($LASTEXITCODE -ne 0) { throw "uismooth_test build failed" }
$p3 = Start-Process -FilePath $exe3 -NoNewWindow -PassThru
$p3.PriorityClass = 'BelowNormal'
if (-not $p3.WaitForExit(60000)) { taskkill /PID $p3.Id /T /F | Out-Null; throw "uismooth_test timed out" }
if ($p3.ExitCode -ne 0) { $code = $p3.ExitCode }
# Frame rate target parsing and pacing rate, interface steps at every rate (host/mods/highfps_core.h, uipace_core.h).
$exe5 = Join-Path $out 'highfps_test.exe'
& clang++ -std=c++20 -O1 -g (Join-Path $PSScriptRoot 'highfps_test.cpp') -o $exe5
if ($LASTEXITCODE -ne 0) { throw "highfps_test build failed" }
$p5 = Start-Process -FilePath $exe5 -NoNewWindow -PassThru
$p5.PriorityClass = 'BelowNormal'
if (-not $p5.WaitForExit(60000)) { taskkill /PID $p5.Id /T /F | Out-Null; throw "highfps_test timed out" }
if ($p5.ExitCode -ne 0) { $code = $p5.ExitCode }
# Pose-event / request tag probe (host/mods/evprobe_core.h).
$exe4 = Join-Path $out 'evprobe_test.exe'
& clang++ -std=c++20 -O1 -g (Join-Path $PSScriptRoot 'evprobe_test.cpp') -o $exe4
if ($LASTEXITCODE -ne 0) { throw "evprobe_test build failed" }
$p4 = Start-Process -FilePath $exe4 -NoNewWindow -PassThru
$p4.PriorityClass = 'BelowNormal'
if (-not $p4.WaitForExit(60000)) { taskkill /PID $p4.Id /T /F | Out-Null; throw "evprobe_test timed out" }
if ($p4.ExitCode -ne 0) { $code = $p4.ExitCode }
# Route aim tool math (host/mods/aim_core.h).
$exe5 = Join-Path $out 'aim_test.exe'
& clang++ -std=c++20 -O1 -g (Join-Path $PSScriptRoot 'aim_test.cpp') -o $exe5
if ($LASTEXITCODE -ne 0) { throw "aim_test build failed" }
$p5 = Start-Process -FilePath $exe5 -NoNewWindow -PassThru
$p5.PriorityClass = 'BelowNormal'
if (-not $p5.WaitForExit(60000)) { taskkill /PID $p5.Id /T /F | Out-Null; throw "aim_test timed out" }
if ($p5.ExitCode -ne 0) { $code = $p5.ExitCode }
exit $code
