@echo off
setlocal
call "%~dp0..\Setup-VS-x64.cmd"
if errorlevel 1 exit /b %ERRORLEVEL%
cd /d "%~dp0"
cl /nologo /std:c++20 /EHsc /W4 /WX PausedImageDiagnosticsTest.cpp /Fe:PausedImageDiagnosticsTest.exe
if errorlevel 1 exit /b %ERRORLEVEL%
PausedImageDiagnosticsTest.exe
exit /b %ERRORLEVEL%
