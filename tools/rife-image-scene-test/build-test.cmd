@echo off
setlocal
cd /d "%~dp0"
call "%~dp0..\Setup-VS-x64.cmd"
if errorlevel 1 exit /b %ERRORLEVEL%
cl /nologo /std:c++20 /EHsc /W4 /WX /O2 RifeImageSceneTest.cpp d3d11.lib dxgi.lib d3dcompiler.lib /Fo:RifeImageSceneTest.obj /Fe:RifeImageSceneTest.exe
if errorlevel 1 exit /b %ERRORLEVEL%
RifeImageSceneTest.exe
exit /b %ERRORLEVEL%
