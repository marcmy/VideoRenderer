@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "CAPTURE=%~1"
if not defined CAPTURE (
  echo NVOF Input Format / Effective Grid Sweep
  echo ==========================================
  echo.
  set /p "CAPTURE=Capture folder containing frame-A.bmp and frame-B.bmp: "
)
if not defined CAPTURE exit /b 2

"%~dp0NativeNvofInputSweep.exe" "%CAPTURE%"
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" (
  echo Sweep failed with exit code %RC%.
) else (
  echo Sweep complete. A new nvof-input-sweep-* folder was created inside the capture folder.
)
pause
exit /b %RC%
