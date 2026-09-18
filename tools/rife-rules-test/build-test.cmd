@echo off
setlocal
cd /d %~dp0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%A in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%A"
if not defined VS_PATH exit /b 1
call "%VS_PATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 >nul
if errorlevel 1 exit /b %errorlevel%
if exist RifeRateRulesTest.exe del /q RifeRateRulesTest.exe
cl /nologo /std:c++20 /EHsc /W4 /WX /I"..\..\Source" RifeRateRulesTest.cpp /Fe:RifeRateRulesTest.exe
if errorlevel 1 exit /b %errorlevel%
RifeRateRulesTest.exe
exit /b %errorlevel%
