@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Test.ps1" -Restore
exit /b %ERRORLEVEL%
