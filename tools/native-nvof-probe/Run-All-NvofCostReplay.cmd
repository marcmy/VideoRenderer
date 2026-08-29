@echo off
setlocal
cd /d "%~dp0"

set "ROOT=%~1"
if not defined ROOT (
    echo Drag the root folder containing your MPCVR NVOF captures onto this file,
    echo or paste that root folder below.
    echo.
    set /p "ROOT=Capture root: "
)
if not defined ROOT exit /b 2

where pwsh.exe >nul 2>&1
if %errorlevel%==0 (
    pwsh.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-All-NvofCostReplay.ps1" "%ROOT%"
) else (
    powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-All-NvofCostReplay.ps1" "%ROOT%"
)
set "RC=%ERRORLEVEL%"
echo.
pause
exit /b %RC%
