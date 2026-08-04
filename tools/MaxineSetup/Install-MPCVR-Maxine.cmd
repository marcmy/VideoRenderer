@echo off
setlocal

set "WINDOWS_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%WINDOWS_POWERSHELL%" (
    echo Windows PowerShell 5.1 was not found at:
    echo %WINDOWS_POWERSHELL%
    pause
    exit /b 1
)

"%WINDOWS_POWERSHELL%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-MPCVR-Maxine.ps1"
set "EXITCODE=%ERRORLEVEL%"
if not "%EXITCODE%"=="0" pause
exit /b %EXITCODE%
