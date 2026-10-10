@echo off
setlocal
cd /d "%~dp0"
set "NASM_EXE=nasm.exe"
if not "%~1"=="" set "NASM_EXE=%~1"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%A in (`"%VSWHERE%" -latest -products * -property installationPath -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64`) do set "VS_PATH=%%A"
if not defined VS_PATH exit /b 1
call "%VS_PATH%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %ERRORLEVEL%
if not exist out mkdir out
for %%F in (const-a cpu-a pixel-a sad-a) do (
  "%NASM_EXE%" -f win64 -w-macro-params-legacy -DARCH_X86_64=1 -DHIGH_BIT_DEPTH=0 -DBIT_DEPTH=8 -I../../external/SVPflow1/ -o "out\%%F.obj" "..\..\external\SVPflow1\x86\%%F.asm"
  if errorlevel 1 exit /b 1
)
cl /nologo /O2 /MT /EHsc /std:c++20 /DQ_OS_WIN /DQ_PROCESSOR_X86_64 /D_CRT_SECURE_NO_WARNINGS /Fo"out\\" /c ..\..\Source\SvpSceneMotion.cpp ..\..\external\SVPflow1\mvframe.cpp ..\..\external\SVPflow1\mvframe_interpolation.cpp ..\..\external\SVPflow1\planeofblocks.cpp ..\..\external\SVPflow1\search.cpp ..\..\external\SVPflow1\groupofplanes.cpp ..\..\external\SVPflow1\blockmath.cpp ..\..\external\SVPflow1\x264_pixel.cpp ..\..\external\SVPflow1\x264_cpu.cpp
if errorlevel 1 exit /b %ERRORLEVEL%
lib /nologo /out:out\SvpMotion.lib out\SvpSceneMotion.obj out\mvframe.obj out\mvframe_interpolation.obj out\planeofblocks.obj out\search.obj out\groupofplanes.obj out\blockmath.obj out\x264_pixel.obj out\x264_cpu.obj out\const-a.obj out\cpu-a.obj out\pixel-a.obj out\sad-a.obj
if errorlevel 1 exit /b %ERRORLEVEL%
cl /nologo /O2 /MT /EHsc /std:c++20 /Fo:out\CoreTest.obj /Fe:out\CoreTest.exe CoreTest.cpp out\SvpMotion.lib
if errorlevel 1 exit /b %ERRORLEVEL%
out\CoreTest.exe
if errorlevel 1 exit /b %ERRORLEVEL%
cl /nologo /O2 /MT /EHsc /std:c++20 /DNOMINMAX /Fo:out\WarpTest.obj /Fe:out\WarpTest.exe WarpTest.cpp out\SvpMotion.lib d3d11.lib d3dcompiler.lib dxguid.lib
if errorlevel 1 exit /b %ERRORLEVEL%
out\WarpTest.exe
exit /b %ERRORLEVEL%
