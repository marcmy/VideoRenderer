@echo off
setlocal

echo This setup package is now MPCVR Maxine + RIFE.
if not exist "%~dp0Install-MPCVR-Maxine-RIFE.cmd" (
    echo The unified Maxine + RIFE setup entry point is missing.
    pause
    exit /b 1
)

call "%~dp0Install-MPCVR-Maxine-RIFE.cmd" %*
exit /b %ERRORLEVEL%
