@echo off
rem check-reference.cmd - assert that the reference output has not drifted.
rem
rem reference\VIRTUAL-HCI-REFERENCE.txt is generated from the production sources.
rem When the real Qualcomm backend misbehaves, the synthetic stub backend is
rem diffed against these bytes to isolate front-end regressions from hardware
rem regressions. This script verifies that the committed reference matches the sources.
rem
rem Exit 0 = byte-identical. Exit 1 = drift (or build failure). No elevation, writes nothing
rem outside tools\_build.
setlocal
call "%~dp0toolchain.cmd" || exit /b 1
set "HERE=%~dp0"
set "OUT=%HERE%_build"
set "REFERENCE=%HERE%..\reference\VIRTUAL-HCI-REFERENCE.txt"
rem Per-invocation scratch directory to support concurrent test runs.
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%REFERENCE%" (
    echo [-] FAIL: reference\VIRTUAL-HCI-REFERENCE.txt not found
    exit /b 1
)
set /a TRIES=0
:mkwork
set "WORK=%OUT%\check-%RANDOM%%RANDOM%"
mkdir "%WORK%" 2>nul
if not errorlevel 1 goto :havework
set /a TRIES+=1
if %TRIES% LSS 64 goto :mkwork
echo [-] FAIL: could not create a private scratch dir under "%OUT%" after 64 attempts
exit /b 1
:havework
set "CANDIDATE=%WORK%\VIRTUAL-HCI-REFERENCE.candidate.txt"

cl.exe /nologo /W4 /WX /Fe:"%WORK%\refdump-check.exe" /Fo:"%WORK%\\" ^
   "%HERE%refdump.c" "%HERE%..\src\common\usb_descriptors.c" "%HERE%..\src\common\hci_stub.c"
if errorlevel 1 (
    echo [-] FAIL: refdump did not build
    exit /b 1
)

"%WORK%\refdump-check.exe" > "%CANDIDATE%"
if errorlevel 1 (
    echo [-] FAIL: refdump exited nonzero
    exit /b 1
)

fc /b "%REFERENCE%" "%CANDIDATE%" >nul 2>&1
if errorlevel 1 goto :drift
echo [+] REFERENCE INTACT: reference\VIRTUAL-HCI-REFERENCE.txt is byte-identical to the sources
rd /s /q "%WORK%" 2>nul
exit /b 0

:drift
echo [-] REFERENCE DRIFT: the production sources no longer reproduce the committed reference.
echo     Committed: "%REFERENCE%"
echo     Generated: "%CANDIDATE%"
echo.
echo     First differences (text view):
fc /n "%REFERENCE%" "%CANDIDATE%" 2>&1 | more +0
echo.
echo     If the change was intentional, regenerate via tools\refdump.cmd.
echo     Otherwise, investigate the unexpected change in descriptor or HCI definitions.
exit /b 1
