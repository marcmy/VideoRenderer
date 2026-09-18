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
if errorlevel 1 exit /b %errorlevel%

for %%D in (good-runtime bad-runtime unsupported-cc-runtime builder-missing-runtime future-failure-runtime missing-runtime) do (
  if exist "%%D" rmdir /s /q "%%D"
  mkdir "%%D"
)
if exist "RIFE" rmdir /s /q "RIFE"
mkdir "RIFE\runtime"

cl /nologo /std:c++20 /EHsc /W4 /WX /LD /DFAKE_ABI_VERSION=2 /I"..\..\Source" ^
  FakeRifeRuntime.cpp /link /OUT:good-runtime\MPCVRRifeRuntime64.dll
if errorlevel 1 exit /b %errorlevel%

cl /nologo /std:c++20 /EHsc /W4 /WX /LD /DFAKE_ABI_VERSION=999 /I"..\..\Source" ^
  FakeRifeRuntime.cpp /link /OUT:bad-runtime\MPCVRRifeRuntime64.dll
if errorlevel 1 exit /b %errorlevel%

cl /nologo /std:c++20 /EHsc /W4 /WX /LD /DFAKE_CREATE_RESULT=MPCVR_RIFE_UNSUPPORTED_COMPUTE_CAPABILITY /I"..\..\Source" ^
  FakeRifeRuntime.cpp /link /OUT:unsupported-cc-runtime\MPCVRRifeRuntime64.dll
if errorlevel 1 exit /b %errorlevel%

cl /nologo /std:c++20 /EHsc /W4 /WX /LD /DFAKE_CREATE_RESULT=MPCVR_RIFE_BUILDER_RESOURCE_MISSING /I"..\..\Source" ^
  FakeRifeRuntime.cpp /link /OUT:builder-missing-runtime\MPCVRRifeRuntime64.dll
if errorlevel 1 exit /b %errorlevel%

cl /nologo /std:c++20 /EHsc /W4 /WX /LD /DFAKE_CREATE_RESULT=-99 /I"..\..\Source" ^
  FakeRifeRuntime.cpp /link /OUT:future-failure-runtime\MPCVRRifeRuntime64.dll
if errorlevel 1 exit /b %errorlevel%

copy /y "unsupported-cc-runtime\MPCVRRifeRuntime64.dll" "RIFE\runtime\MPCVRRifeRuntime64.dll" >nul
if errorlevel 1 exit /b %errorlevel%

cl /nologo /std:c++20 /EHsc /W4 /WX /I"..\..\Source" ^
  RifeRuntimeAbiTest.cpp "..\..\Source\RifeFrameInterpolation.cpp" ^
  shell32.lib ole32.lib /Fe:RifeRuntimeAbiTest.exe
if errorlevel 1 exit /b %errorlevel%

RifeRuntimeAbiTest.exe
exit /b %errorlevel%
