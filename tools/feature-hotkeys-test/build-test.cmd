@echo off
setlocal
call "%~dp0..\Setup-VS-x64.cmd"
if errorlevel 1 exit /b %ERRORLEVEL%
cd /d "%~dp0..\.."
if not exist "_test-validation\feature-hotkeys" mkdir "_test-validation\feature-hotkeys"
cl /nologo /std:c++20 /EHsc /utf-8 /W4 /WX /O2 tools\feature-hotkeys-test\FeatureHotkeysTest.cpp user32.lib /Fo:_test-validation\feature-hotkeys\FeatureHotkeysTest.obj /Fe:_test-validation\feature-hotkeys\FeatureHotkeysTest.exe
if errorlevel 1 exit /b %ERRORLEVEL%
_test-validation\feature-hotkeys\FeatureHotkeysTest.exe
exit /b %ERRORLEVEL%
