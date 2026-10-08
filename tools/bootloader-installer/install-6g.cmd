@echo off
rem Writes the PodBox bootloader to an iPod Classic 6G/7G, waiting for one to
rem appear in DFU mode.
cd /d "%~dp0"
echo Connect the iPod and put it in DFU mode (see README.txt).
echo Waiting for it...
:wait
mks5lboot.exe --dfuscan >nul 2>&1
if errorlevel 1 (
    timeout /t 1 /nobreak >nul
    goto wait
)
mks5lboot.exe --bl-inst bootloader-ipod6g.ipod
echo.
pause
