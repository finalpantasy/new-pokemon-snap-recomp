@echo off
rem Builds and runs build\nvn\life_test.exe: CPU-only checks of nvn_life.cpp and the audit F3-F6 helpers (no Vulkan device).
setlocal
set ROOT=%~dp0..\..
if not exist "%ROOT%\build\nvn" mkdir "%ROOT%\build\nvn"
"%ROOT%\tools\llvm\bin\clang++.exe" -std=c++20 -O2 -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -fuse-ld=lld -I"%ROOT%\runtime\nvn" -I"%ROOT%\tools\vulkan_sdk\Include" "%ROOT%\runtime\nvn\nvn_life.cpp" "%ROOT%\tests\nvn\life_test.cpp" -o "%ROOT%\build\nvn\life_test.exe" || exit /b 1
"%ROOT%\build\nvn\life_test.exe"
