@echo off
rem Writes the PodBox bootloader to an iPod Video 5G/5.5G connected as a drive.
rem ipodpatcher needs administrator rights, so this script takes them first:
rem left to the tool, Windows opens it in a window of its own that closes
rem before its result can be read.
fltmc >nul 2>&1
if not errorlevel 1 goto admin
if "%~1"=="elevated" (
    echo Administrator rights were not granted.
    pause
    exit /b 1
)
rem The path goes through the environment, not the command line, so a folder
rem name with a quote or a bracket in it survives.
set "PODBOX_INSTALLER=%~f0"
powershell -NoProfile -Command "try { Start-Process -FilePath cmd.exe -ArgumentList ('/c \"\"' + $env:PODBOX_INSTALLER + '\" elevated\"') -Verb RunAs -ErrorAction Stop } catch { exit 1 }"
if errorlevel 1 (
    echo Administrator rights were not granted, so nothing was written.
    pause
    exit /b 1
)
exit /b
:admin
cd /d "%~dp0"
ipodpatcher.exe -a bootloader-ipodvideo.ipod
echo.
pause
