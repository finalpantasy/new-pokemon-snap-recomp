@echo off
rem Builds build\audio\sink_test.exe with the installed MSVC (BuildTools 2022).
rem Run at BelowNormal priority: cmd /c "start /belownormal /wait /b tests\audio\build.bat"
setlocal
set ROOT=%~dp0..\..
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build\audio" mkdir "%ROOT%\build\audio"
if not exist "%ROOT%\analysis\audio" mkdir "%ROOT%\analysis\audio"
cl /nologo /std:c++20 /EHsc /O2 /W3 /MD /D_CRT_SECURE_NO_WARNINGS /D_USE_MATH_DEFINES ^
  /I"%ROOT%\runtime\audio" ^
  "%ROOT%\runtime\audio\audio_out.cpp" "%ROOT%\runtime\audio\audio_null.cpp" "%ROOT%\runtime\audio\audio_wasapi.cpp" ^
  "%ROOT%\tests\audio\sink_test.cpp" ^
  /Fo"%ROOT%\build\audio\\" /Fe"%ROOT%\build\audio\sink_test.exe" ^
  /link user32.lib ole32.lib winmm.lib avrt.lib || exit /b 1
echo built %ROOT%\build\audio\sink_test.exe
