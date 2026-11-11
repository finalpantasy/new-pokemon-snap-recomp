@echo off
rem Builds build\nvn\<test>.exe with the installed MSVC (BuildTools 2022) and the project's Vulkan SDK.
rem   tests\nvn\build.bat            -> clear_test   (milestone 1)
rem   tests\nvn\build.bat mesh_test  -> mesh_test    (milestone 2: textured, depth-tested mesh with a game shader)
rem Run at BelowNormal priority: cmd /c "start /belownormal /wait /b tests\nvn\build.bat mesh_test"
rem The shader harness DLL (dotnet build scripts\shader_harness\ShaderHarness.csproj -c Release) is found through
rem NVN_HARNESS_DLL (baked in below) or the NVN_SHADER_HARNESS environment variable.
setlocal
set TEST=%1
if "%TEST%"=="" set TEST=clear_test
set ROOT=%~dp0..\..
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build\nvn\%TEST%" mkdir "%ROOT%\build\nvn\%TEST%"
set VK=%ROOT%\tools\vulkan_sdk
for %%I in ("%VK%") do set VKF=%%~fI
set VKF=%VKF:\=/%
for %%I in ("%ROOT%") do set ROOTF=%%~fI
set ROOTF=%ROOTF:\=/%
cl /nologo /std:c++20 /EHsc /O2 /W3 /MD /D_CRT_SECURE_NO_WARNINGS /DNVN_TEST_LAYER_PATH="\"%VKF%/Bin\"" ^
  /DNVN_HARNESS_DLL="\"%ROOTF%/tools/shader_harness/bin/Release/net10.0/ShaderHarness.dll\"" ^
  /DNVN_TEST_ROOT="\"%ROOTF%\"" ^
  /I"%ROOT%\runtime\nvn" /I"%VK%\Include" ^
  "%ROOT%\runtime\nvn\nvn_vk.cpp" "%ROOT%\runtime\nvn\nvn_entry.cpp" "%ROOT%\runtime\nvn\nvn_gfx.cpp" ^
  "%ROOT%\runtime\nvn\nvn_shader.cpp" "%ROOT%\runtime\nvn\nvn_draw.cpp" "%ROOT%\runtime\nvn\nvn_tex.cpp" ^
  "%ROOT%\runtime\nvn\nvn_tex_vk.cpp" "%ROOT%\runtime\nvn\nvn_astc.cpp" "%ROOT%\runtime\nvn\nvn_bc7.cpp" ^
  "%ROOT%\runtime\nvn\nvn_alpha.cpp" "%ROOT%\runtime\nvn\nvn_xfer.cpp" "%ROOT%\tests\nvn\%TEST%.cpp" ^
  /Fo"%ROOT%\build\nvn\%TEST%\\" /Fe"%ROOT%\build\nvn\%TEST%.exe" ^
  /link /LIBPATH:"%VK%\Lib" vulkan-1.lib user32.lib gdi32.lib bcrypt.lib || exit /b 1
echo built %ROOT%\build\nvn\%TEST%.exe
