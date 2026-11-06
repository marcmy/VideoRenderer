@cd /d "%~dp0"
@set "REGISTER_FILE="
@call :wait_for_player
@rem MPC-HC loads the copy in its folder. MPC-BE only loads the registered one, so it is last and wins.
@call :install_player "%ProgramW6432%\MPC-HC" mpc-hc64.exe MPCVR
@if %errorlevel% NEQ 0 goto error
@call :install_player "%ProgramFiles(x86)%\K-Lite Codec Pack\MPC-HC64" mpc-hc64.exe MPCVR
@if %errorlevel% NEQ 0 goto error
@call :install_player "%ProgramW6432%\MPC-BE" mpc-be64.exe Filters
@if %errorlevel% NEQ 0 goto error
@if not defined REGISTER_FILE goto no_player
@regsvr32.exe "%REGISTER_FILE%" /s
@if %errorlevel% NEQ 0 goto error
:success
@echo.
@echo.
@echo    Installation succeeded.
@echo.
@goto done
:install_player
@if not exist "%~1\%~2" exit /b 0
@if not exist "%~1\%~3" mkdir "%~1\%~3"
@if %errorlevel% NEQ 0 exit /b 1
@copy /Y "%~dp0MpcVideoRenderer64.ax" "%~1\%~3\MpcVideoRenderer64.ax" >NUL
@if %errorlevel% NEQ 0 exit /b 1
@echo    Installed to "%~1\%~3\MpcVideoRenderer64.ax".
@set "REGISTER_FILE=%~1\%~3\MpcVideoRenderer64.ax"
@exit /b 0
:wait_for_player
@call :player_running
@if not defined PLAYER_RUNNING exit /b 0
@echo.
@echo    MPC-HC or MPC-BE is running. Installation will continue after it closes.
:wait_for_player_loop
@timeout /t 1 /nobreak >NUL
@call :player_running
@if defined PLAYER_RUNNING goto wait_for_player_loop
@echo    The player closed. Continuing installation.
@echo.
@exit /b 0
:player_running
@set "PLAYER_RUNNING="
@tasklist /FI "IMAGENAME eq mpc-hc.exe" 2>NUL | find /I "mpc-hc.exe" >NUL && set "PLAYER_RUNNING=1"
@tasklist /FI "IMAGENAME eq mpc-hc64.exe" 2>NUL | find /I "mpc-hc64.exe" >NUL && set "PLAYER_RUNNING=1"
@tasklist /FI "IMAGENAME eq mpc-be.exe" 2>NUL | find /I "mpc-be.exe" >NUL && set "PLAYER_RUNNING=1"
@tasklist /FI "IMAGENAME eq mpc-be64.exe" 2>NUL | find /I "mpc-be64.exe" >NUL && set "PLAYER_RUNNING=1"
@exit /b 0
:no_player
@echo.
@echo.
@echo    No 64-bit MPC-HC, MPC-BE or K-Lite Codec Pack was found in its default folder.
@echo    Copy MpcVideoRenderer64.ax by hand; see the setup guide.
@echo.
@goto done
:error
@echo.
@echo.
@echo    Installation failed.
@echo.
@echo    You need to right click "Install_MPCVR_64.cmd" and choose "run as admin".
@echo.
:done
@pause >NUL
