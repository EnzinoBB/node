@echo off
rem Guided set-up of the node in this folder: double-click it, or run "setup.bat --defaults" to take
rem every default. The questions and the changes to config.ini are in setup.ps1, next to this file.
setlocal
set "ARGS="
if /i "%~1"=="--defaults" set "ARGS=-Defaults"
if /i "%~1"=="-y" set "ARGS=-Defaults"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %ARGS%
set "RC=%ERRORLEVEL%"
rem a double-clicked window stays open to show the result
echo.
pause
exit /b %RC%
