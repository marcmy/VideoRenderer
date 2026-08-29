@echo off
setlocal
cd /d "%~dp0"

set "CAPTURE=%~1"
if not defined CAPTURE (
    echo Drag an MPCVR NVOF capture folder onto this .cmd file,
    echo or paste the capture folder path below.
    echo.
    set /p "CAPTURE=Capture folder: "
)

if not defined CAPTURE exit /b 2
if not exist "%CAPTURE%\frame-A.bmp" (
    echo.
    echo ERROR: frame-A.bmp was not found in:
    echo   %CAPTURE%
    pause
    exit /b 3
)
if not exist "%CAPTURE%\frame-B.bmp" (
    echo.
    echo ERROR: frame-B.bmp was not found in:
    echo   %CAPTURE%
    pause
    exit /b 3
)

"%~dp0NativeNvofCostReplay.exe" "%CAPTURE%"
set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" (
    echo Replay completed successfully.
) else (
    echo Replay failed with exit code %RC%.
)
pause
exit /b %RC%
