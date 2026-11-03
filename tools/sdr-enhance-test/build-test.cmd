@echo off
setlocal
call "%~dp0..\Setup-VS-x64.cmd"
if errorlevel 1 exit /b %ERRORLEVEL%
cd /d "%~dp0..\.."
if not exist "_test-validation\sdr-enhance" mkdir "_test-validation\sdr-enhance"
cl /nologo /std:c++20 /EHsc /utf-8 /W4 /WX /O2 tools\sdr-enhance-test\SdrEnhanceTest.cpp d3d11.lib dxgi.lib d3dcompiler.lib /Fo:_test-validation\sdr-enhance\SdrEnhanceTest.obj /Fe:_test-validation\sdr-enhance\SdrEnhanceTest.exe
if errorlevel 1 exit /b %ERRORLEVEL%
rem WARP by default; pass --bench to time the pass on the hardware adapter.
_test-validation\sdr-enhance\SdrEnhanceTest.exe %1 "%~dp0..\..\Shaders\d3d11\ps_sdr_enhance.hlsl"
exit /b %ERRORLEVEL%
