@cd /d "%~dp0"
@set "REMOVED="
@call :remove_file "%ProgramFiles(x86)%\K-Lite Codec Pack\MPC-HC64\MPCVR\MpcVideoRenderer64.ax"
@if %errorlevel% NEQ 0 goto error
@call :remove_file "%ProgramW6432%\MPC-HC\MPCVR\MpcVideoRenderer64.ax"
@if %errorlevel% NEQ 0 goto error
@call :remove_file "%ProgramW6432%\MPC-BE\Filters\MpcVideoRenderer64.ax"
@if %errorlevel% NEQ 0 goto error
@if not defined REMOVED goto error
:success
@echo.
@echo.
@echo    Uninstallation succeeded.
@echo.
@goto done
:remove_file
@if not exist "%~1" exit /b 0
@regsvr32.exe "%~1" /u /s
@del /Q "%~1" >NUL 2>&1
@if exist "%~1" exit /b 1
@set "REMOVED=1"
@exit /b 0
:error
@echo.
@echo.
@echo    Uninstallation failed.
@echo.
@echo    You need to right click "Uninstall_MPCVR_64.cmd" and choose "run as admin".
@echo.
:done
@pause >NUL
