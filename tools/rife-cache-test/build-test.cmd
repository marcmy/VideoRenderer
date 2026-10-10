@echo off
setlocal
call "%~dp0..\Setup-VS-x64.cmd"
if errorlevel 1 exit /b %ERRORLEVEL%
cd /d "%~dp0..\.."
if not exist "_test-validation\release-startup\tests" mkdir "_test-validation\release-startup\tests"
cl /nologo /std:c++20 /EHsc /utf-8 /W4 /WX /DNOMINMAX /DWIN32_LEAN_AND_MEAN tools\rife-cache-test\CacheTest.cpp /Fo:_test-validation\release-startup\tests\CacheTest.obj /Fe:_test-validation\release-startup\tests\CacheTest.exe
if errorlevel 1 exit /b %ERRORLEVEL%
_test-validation\release-startup\tests\CacheTest.exe "_test-validation\release-startup\tests"
exit /b %ERRORLEVEL%
