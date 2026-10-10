@echo off
setlocal
cd /d %~dp0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%A in (`"%VSWHERE%" -latest -products * -property installationPath -requires Microsoft.Component.MSBuild`) do set "VS_PATH=%%A"
if not defined VS_PATH (
  echo Visual Studio not found.
  exit /b 1
)
call "%VS_PATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 >nul
if errorlevel 1 exit /b %ERRORLEVEL%
cl /nologo /std:c++20 /EHsc /W4 /DUNICODE /D_UNICODE /DNOMINMAX /I..\..\external\BaseClasses DialogTest.cpp ..\..\Source\SettingsDialogTheme.cpp /Fe:DialogTest.exe /link user32.lib gdi32.lib comctl32.lib uxtheme.lib dwmapi.lib
if errorlevel 1 exit /b %ERRORLEVEL%
DialogTest.exe ..\..\_bin\Filter_x64\MpcVideoRenderer64.ax
exit /b %ERRORLEVEL%
