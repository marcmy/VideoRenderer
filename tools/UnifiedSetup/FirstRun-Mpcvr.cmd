@echo off
setlocal
where pwsh.exe >nul 2>nul
if %errorlevel%==0 (
  pwsh.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Invoke-MpcvrFirstRun.ps1" %*
) else (
  "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Invoke-MpcvrFirstRun.ps1" %*
)
exit /b %errorlevel%
