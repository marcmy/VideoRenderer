@echo off
setlocal

where pwsh.exe >nul 2>&1
if not errorlevel 1 (
    pwsh.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-KLiteMPCVRUpdater.ps1"
    set "EXITCODE=%ERRORLEVEL%"
    goto :done
)

where powershell.exe >nul 2>&1
if not errorlevel 1 (
    powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-KLiteMPCVRUpdater.ps1"
    set "EXITCODE=%ERRORLEVEL%"
    goto :done
)

echo Neither PowerShell 7 ^(pwsh.exe^) nor Windows PowerShell ^(powershell.exe^) was found.
set "EXITCODE=1"

:done
if not "%EXITCODE%"=="0" pause
exit /b %EXITCODE%
