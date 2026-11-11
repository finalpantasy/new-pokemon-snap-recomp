@echo off
rem Builds build\nvn\astc_test.exe (ASTC decoder + BC7 encoder + layout helpers only; no Vulkan).
setlocal
set ROOT=%~dp0..\..
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build\nvn\astc" mkdir "%ROOT%\build\nvn\astc"
cl /nologo /std:c++20 /EHsc /O2 /W3 /MD /I"%ROOT%\runtime\nvn" "%ROOT%\runtime\nvn\nvn_astc.cpp" "%ROOT%\runtime\nvn\nvn_bc7.cpp" "%ROOT%\runtime\nvn\nvn_tex.cpp" "%ROOT%\tests\nvn\astc_test.cpp" /Fo"%ROOT%\build\nvn\astc\\" /Fe"%ROOT%\build\nvn\astc_test.exe" || exit /b 1
echo built %ROOT%\build\nvn\astc_test.exe
