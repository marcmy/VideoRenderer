@echo off
setlocal

where pwsh.exe >nul 2>&1
if errorlevel 1 (
    echo PowerShell 7 ^(pwsh.exe^) was not found.
    echo Install PowerShell 7 and run this installer again.
    pause
    exit /b 1
)

pwsh.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-KLiteMPCVRUpdater.ps1"
if errorlevel 1 pause
