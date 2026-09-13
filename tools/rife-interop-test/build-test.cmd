@echo off
setlocal
cd /d %~dp0
if not defined VS_PATH (
  for /f "usebackq delims=" %%A in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%A"
)
if not defined VS_PATH exit /b 2
call "%VS_PATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /EHsc /W4 /O2 RifeInteropConcurrencyTest.cpp d3d11.lib dxgi.lib d3dcompiler.lib /Fo:RifeInteropConcurrencyTest.obj /Fe:RifeInteropConcurrencyTest.exe
exit /b %errorlevel%
