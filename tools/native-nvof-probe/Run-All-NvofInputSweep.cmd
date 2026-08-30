@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "ROOT=%~1"
if not defined ROOT (
  echo NVOF Input Format / Effective Grid Sweep - Batch
  echo =================================================
  echo.
  set /p "ROOT=Root folder containing capture folders: "
)
if not defined ROOT exit /b 2

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-All-NvofInputSweep.ps1" "%ROOT%"
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" (
  echo Batch sweep completed with one or more failures. Exit code %RC%.
) else (
  echo Batch sweep complete.
)
pause
exit /b %RC%
