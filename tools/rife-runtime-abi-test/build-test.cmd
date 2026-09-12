@echo off
setlocal
cd /d %~dp0

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%A in (`"%VSWHERE%" -latest -property installationPath -requires Microsoft.Component.MSBuild`) do set "VS_PATH=%%A"
if not defined VS_PATH (
  echo Visual Studio not found.
  exit /b 1
)

call "%VS_PATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 >nul
if errorlevel 1 exit /b %errorlevel%

for %%D in (good-runtime bad-runtime missing-runtime) do (
  if exist "%%D" rmdir /s /q "%%D"
  mkdir "%%D"
)

cl /nologo /std:c++20 /EHsc /W4 /WX /LD /DFAKE_ABI_VERSION=1 /I"..\..\Source" ^
  FakeRifeRuntime.cpp /link /OUT:good-runtime\MPCVRRifeRuntime64.dll
if errorlevel 1 exit /b %errorlevel%

cl /nologo /std:c++20 /EHsc /W4 /WX /LD /DFAKE_ABI_VERSION=999 /I"..\..\Source" ^
  FakeRifeRuntime.cpp /link /OUT:bad-runtime\MPCVRRifeRuntime64.dll
if errorlevel 1 exit /b %errorlevel%

cl /nologo /std:c++20 /EHsc /W4 /WX /I"..\..\Source" ^
  RifeRuntimeAbiTest.cpp "..\..\Source\RifeFrameInterpolation.cpp" ^
  /Fe:RifeRuntimeAbiTest.exe
if errorlevel 1 exit /b %errorlevel%

RifeRuntimeAbiTest.exe
exit /b %errorlevel%
