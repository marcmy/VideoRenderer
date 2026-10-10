@echo off
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_PATH="
for /f "usebackq delims=" %%A in (`"%VSWHERE%" -latest -products * -property installationPath -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64`) do set "VS_PATH=%%A"
if not defined VS_PATH (
  echo Visual Studio C++ build tools not found.
  exit /b 1
)
call "%VS_PATH%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
exit /b %ERRORLEVEL%
