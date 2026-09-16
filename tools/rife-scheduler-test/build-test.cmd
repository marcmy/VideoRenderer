@echo off
setlocal
cd /d %~dp0

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%A in (`"%VSWHERE%" -latest -products * -property installationPath -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64`) do set "VS_PATH=%%A"
if not defined VS_PATH (
  echo Visual Studio not found.
  exit /b 1
)

call "%VS_PATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 >nul
if errorlevel 1 exit /b %errorlevel%

if exist RifeSchedulerTest.exe del /q RifeSchedulerTest.exe

cl /nologo /std:c++20 /EHsc /W4 /WX /I"..\..\Source" ^
  RifeSchedulerTest.cpp "..\..\Source\FrameInterpolationScheduler.cpp" ^
  /Fe:RifeSchedulerTest.exe
if errorlevel 1 exit /b %errorlevel%

RifeSchedulerTest.exe
exit /b %errorlevel%
