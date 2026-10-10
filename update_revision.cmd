@ECHO OFF
SETLOCAL EnableDelayedExpansion
CD /D %~dp0

ECHO #pragma once > revision.h

SET gitexe="git.exe"
%gitexe% --version
IF /I %ERRORLEVEL%==0 GOTO :GitOK

SET gitexe="c:\Program Files\Git\cmd\git.exe"
IF NOT EXIST %gitexe% set gitexe="c:\Program Files\Git\bin\git.exe"
IF NOT EXIST %gitexe% GOTO :END

:GitOK

%gitexe% log -1 --date=format:%%Y.%%m.%%d --pretty=format:"#define REV_DATE %%ad%%n" >> revision.h
%gitexe% log -1 --pretty=format:"#define REV_HASH %%h%%n" >> revision.h

SET "revbranch=LOCAL"
FOR /F "delims=" %%I IN ('%gitexe% symbolic-ref --short -q HEAD 2^>NUL') DO SET "revbranch=%%I"
ECHO #define REV_BRANCH !revbranch!>> revision.h

SET "revnum=0"
IF DEFINED MPCVR_TEST_REVISION (
SET "revnum=%MPCVR_TEST_REVISION%"
) ELSE (
FOR /F %%I IN ('%gitexe% rev-list --count HEAD 2^>NUL') DO SET "revnum=%%I"
FOR /F "tokens=3" %%I IN ('FINDSTR /C:"define VER_REVISION_BASE " Include\Version.h') DO SET "revisionbase=%%I"
FOR /F "tokens=3" %%I IN ('FINDSTR /C:"define VER_REVISION_BASE_COMMIT " Include\Version.h') DO SET "revisionbasecommit=%%I"
IF DEFINED revisionbase IF DEFINED revisionbasecommit (
%gitexe% merge-base --is-ancestor !revisionbasecommit! HEAD >nul 2>nul
IF !ERRORLEVEL! EQU 0 (
FOR /F %%I IN ('%gitexe% rev-list --first-parent --count !revisionbasecommit!..HEAD 2^>NUL') DO SET /A "revnum=!revisionbase!+%%I"
)
)
)
ECHO #define REV_NUM !revnum!>> revision.h

:END
ENDLOCAL
EXIT /B
