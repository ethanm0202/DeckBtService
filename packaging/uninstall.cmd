@echo off
rem Double-click launcher: runs uninstall.ps1 as administrator and keeps the window open.
setlocal
set "SCRIPT=uninstall.ps1"

rem fltmc needs administrator rights and no service, so it tells whether this window is elevated.
fltmc >nul 2>&1
if errorlevel 1 (
    if "%~1"=="--elevated" (
        echo Administrator rights were not granted.
        pause
        exit /b 1
    )
    set "DECKBT_SELF=%~f0"
    set "DECKBT_ARGS=%*"
    powershell.exe -NoProfile -Command "try { Start-Process -FilePath $env:DECKBT_SELF -ArgumentList ('--elevated ' + $env:DECKBT_ARGS) -Verb RunAs -ErrorAction Stop } catch { exit 1 }"
    if errorlevel 1 (
        echo %SCRIPT% needs administrator rights; it did not run.
        pause
        exit /b 1
    )
    exit /b
)
if "%~1"=="--elevated" shift /1

rem 64-bit Windows PowerShell even if this window is a 32-bit command prompt.
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"

"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0%SCRIPT%" %1 %2 %3 %4
set "RC=%ERRORLEVEL%"
echo.
pause
exit /b %RC%
