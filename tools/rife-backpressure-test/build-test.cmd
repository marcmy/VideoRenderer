@echo off
setlocal
cd /d %~dp0

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%A in (`"%VSWHERE%" -latest -property installationPath -requires Microsoft.Component.MSBuild`) do set "VS_PATH=%%A"
if not defined VS_PATH (
  echo Visual Studio not found.
  exit /b 1
)

call "%VS_PATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 >nul
if errorlevel 1 exit /b %errorlevel%

if exist RifeBackpressureTest.exe del /q RifeBackpressureTest.exe

cl /nologo /std:c++20 /EHsc /W4 /WX /I"..\..\Source" ^
  RifeBackpressureTest.cpp /Fe:RifeBackpressureTest.exe
if errorlevel 1 exit /b %errorlevel%

RifeBackpressureTest.exe
exit /b %errorlevel%
