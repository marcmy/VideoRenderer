@echo off
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-MPCVRRifeRuntime.ps1" %*
exit /b %ERRORLEVEL%
