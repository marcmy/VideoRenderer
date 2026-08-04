@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-NvOFFRUCRuntime.ps1" %*
if errorlevel 1 (
  echo.
  echo Installation failed. Review the error above.
)
pause
