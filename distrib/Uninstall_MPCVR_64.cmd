@cd /d "%~dp0"
@set "TARGET_DIR=%ProgramFiles(x86)%\K-Lite Codec Pack\MPC-HC64\MPCVR"
@regsvr32.exe "%TARGET_DIR%\MpcVideoRenderer64.ax" /u /s
@if %errorlevel% NEQ 0 goto error
@del /Q "%TARGET_DIR%\MpcVideoRenderer64.ax" >NUL 2>&1
:success
@echo.
@echo.
@echo    Uninstallation succeeded.
@echo.
@goto done
:error
@echo.
@echo.
@echo    Uninstallation failed.
@echo.
@echo    You need to right click "Uninstall_MPCVR_64.cmd" and choose "run as admin".
@echo.
:done
@pause >NUL
