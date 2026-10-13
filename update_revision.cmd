@ECHO OFF
SETLOCAL
CD /D %~dp0

"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Dev\Version.ps1" -Generate >nul
EXIT /B %ERRORLEVEL%
