@cd /d "%~dp0"
@set "TARGET_DIR=%ProgramFiles(x86)%\K-Lite Codec Pack\MPC-HC64\MPCVR"
@call :wait_for_player
@if not exist "%TARGET_DIR%" mkdir "%TARGET_DIR%"
@if %errorlevel% NEQ 0 goto error
@copy /Y "%~dp0MpcVideoRenderer64.ax" "%TARGET_DIR%\MpcVideoRenderer64.ax" >NUL
@if %errorlevel% NEQ 0 goto error
@regsvr32.exe "%TARGET_DIR%\MpcVideoRenderer64.ax" /s
@if %errorlevel% NEQ 0 goto error
:success
@echo.
@echo.
@echo    Installation succeeded.
@echo.
@echo    Installed to "%TARGET_DIR%\MpcVideoRenderer64.ax".
@echo.
@goto done
:wait_for_player
@set "PLAYER_RUNNING="
@tasklist /FI "IMAGENAME eq mpc-hc.exe" 2>NUL | find /I "mpc-hc.exe" >NUL && set "PLAYER_RUNNING=1"
@tasklist /FI "IMAGENAME eq mpc-hc64.exe" 2>NUL | find /I "mpc-hc64.exe" >NUL && set "PLAYER_RUNNING=1"
@if not defined PLAYER_RUNNING exit /b 0
@echo.
@echo    MPC-HC is running. Installation will continue after it closes.
:wait_for_player_loop
@timeout /t 1 /nobreak >NUL
@set "PLAYER_RUNNING="
@tasklist /FI "IMAGENAME eq mpc-hc.exe" 2>NUL | find /I "mpc-hc.exe" >NUL && set "PLAYER_RUNNING=1"
@tasklist /FI "IMAGENAME eq mpc-hc64.exe" 2>NUL | find /I "mpc-hc64.exe" >NUL && set "PLAYER_RUNNING=1"
@if defined PLAYER_RUNNING goto wait_for_player_loop
@echo    MPC-HC closed. Continuing installation.
@echo.
@exit /b 0
:error
@echo.
@echo.
@echo    Installation failed.
@echo.
@echo    You need to right click "Install_MPCVR_64.cmd" and choose "run as admin".
@echo.
:done
@pause >NUL
