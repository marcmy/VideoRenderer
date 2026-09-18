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

if exist MaxineSpatialPolicyTest.exe del /q MaxineSpatialPolicyTest.exe

cl /nologo /std:c++20 /EHsc /W4 /WX MaxineSpatialPolicyTest.cpp /Fe:MaxineSpatialPolicyTest.exe
if errorlevel 1 exit /b %errorlevel%

MaxineSpatialPolicyTest.exe
exit /b %errorlevel%
