@echo off
setlocal
cd /d "%~dp0"
where pwsh.exe >nul 2>&1
if %errorlevel%==0 (
    pwsh.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Adaptive-Splat-V12.ps1" -Restore
) else (
    powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Adaptive-Splat-V12.ps1" -Restore
)
exit /b %errorlevel%
