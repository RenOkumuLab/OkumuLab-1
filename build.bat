@echo off
rem OkumuLab 1 - build (Visual Studio 2022 / Build Tools, CMake + Ninja bundled with them)
rem   build.bat            configure + build everything (Release)
rem   build.bat dsp        only the DSP core and labium_check (no JUCE)
rem Keep this folder on a short path (the JUCE sources have long file names).
setlocal
set ROOT=%~dp0
set ROOT=%ROOT:~0,-1%
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
if not defined VSPATH (
  echo Visual Studio with the C++ tools was not found.
  exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set CM="%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set NJ=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe

if /i "%1"=="dsp" (
  %CM% -S "%ROOT%" -B "%ROOT%\build-dsp" -G Ninja -DCMAKE_MAKE_PROGRAM="%NJ%" -DCMAKE_BUILD_TYPE=Release -DOKL_WITH_PLUGIN=OFF || exit /b 1
  %CM% --build "%ROOT%\build-dsp" || exit /b 1
  exit /b 0
)

%CM% -S "%ROOT%" -B "%ROOT%\build" -G Ninja -DCMAKE_MAKE_PROGRAM="%NJ%" -DCMAKE_BUILD_TYPE=Release || exit /b 1
%CM% --build "%ROOT%\build" || exit /b 1
echo.
echo VST3:       %ROOT%\build\OkumuLab1_artefacts\Release\VST3\OkumuLab 1.vst3
echo Standalone: %ROOT%\build\OkumuLab1_artefacts\Release\Standalone\OkumuLab 1.exe
