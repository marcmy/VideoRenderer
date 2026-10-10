@echo off
setlocal
cd /d "%~dp0"
call "%~dp0..\Setup-VS-x64.cmd"
if errorlevel 1 exit /b %ERRORLEVEL%
cl /nologo /std:c++20 /EHsc /W4 /WX /O2 NvofInputTest.cpp d3d11.lib dxgi.lib d3dcompiler.lib /Fo:NvofInputTest.obj /Fe:NvofInputTest.exe
if errorlevel 1 exit /b %ERRORLEVEL%
NvofInputTest.exe
exit /b %ERRORLEVEL%
