@echo off
rem Writes the PodBox bootloader to an iPod Video 5G/5.5G connected as a drive.
rem ipodpatcher needs administrator rights, so this script takes them first:
rem left to the tool, Windows opens it in a window of its own that closes
rem before its result can be read.
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
cd /d "%~dp0"
ipodpatcher.exe -a bootloader-ipodvideo.ipod
echo.
pause
