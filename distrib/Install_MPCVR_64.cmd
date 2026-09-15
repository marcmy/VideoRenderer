@cd /d "%~dp0"
@set "TARGET_DIR=%ProgramFiles(x86)%\K-Lite Codec Pack\MPC-HC64\MPCVR"
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
:error
@echo.
@echo.
@echo    Installation failed.
@echo.
@echo    You need to right click "Install_MPCVR_64.cmd" and choose "run as admin".
@echo.
:done
@pause >NUL
