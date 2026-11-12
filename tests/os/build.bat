@echo off
rem Builds build\os\os_test.exe (host-only nn::os tests) with tools\llvm clang++ (C++20).
rem Run at BelowNormal priority: cmd /c "start /belownormal /wait /b tests\os\build.bat"
rem Optional first argument: extra flags, e.g. "-fsanitize=address".
setlocal
set ROOT=%~dp0..\..
if not exist "%ROOT%\build\os" mkdir "%ROOT%\build\os"
"%ROOT%\tools\llvm\bin\clang++.exe" -std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter %~1 ^
  "%ROOT%\runtime\os\os_kernel.cpp" "%ROOT%\runtime\os\os_thread.cpp" "%ROOT%\runtime\os\os_sync.cpp" ^
  "%ROOT%\runtime\os\os_memory.cpp" "%ROOT%\runtime\os\os_overrides.cpp" "%ROOT%\runtime\os\perf.cpp" "%ROOT%\tests\os\os_test.cpp" ^
  -o "%ROOT%\build\os\os_test.exe" || exit /b 1
echo built %ROOT%\build\os\os_test.exe
