@echo off
rem PodBox Theme Lens — double-click to open.
rem
rem With no argument it looks for a theme folder in this directory, then asks.
rem Drag a theme folder onto this file to open that one instead.
rem
rem pythonw runs without a console window, which is what makes this feel like an
rem application rather than a script — and it pairs with pywebview, which opens
rem a native window of its own. If pythonw is missing, fall back to python and
rem accept the console.

setlocal
set "HERE=%~dp0"

where pythonw >nul 2>&1 && (
  start "" pythonw "%HERE%serve.py" %*
  exit /b
)

py -0 >nul 2>&1 && (
  start "" pyw "%HERE%serve.py" %*
  exit /b
)

where python >nul 2>&1 && (
  python "%HERE%serve.py" %*
  exit /b
)

echo Python 3 was not found on PATH.
echo Install it from https://python.org and run this again.
pause
