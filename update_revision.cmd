@ECHO OFF
SETLOCAL
CD /D %~dp0
REM A build launched from PowerShell 7 must not make Windows PowerShell 5.1
REM load incompatible PowerShell 7 utility modules through inherited paths.
SET "PSModulePath=%SystemRoot%\System32\WindowsPowerShell\v1.0\Modules"

"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Dev\Version.ps1" -RepoRoot "%~dp0." -Generate >nul
EXIT /B %ERRORLEVEL%
