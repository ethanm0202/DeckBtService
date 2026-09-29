@echo off
rem Regenerate reference\VIRTUAL-HCI-REFERENCE.txt from the production sources.
setlocal
if not defined EWDK set "EWDK=C:\EWDK"
set "MSVC_ROOT=%EWDK%\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC"
set "SDK=%EWDK%\Program Files\Windows Kits\10"
set "SDKVER=10.0.26100.0"
for /d %%d in ("%MSVC_ROOT%\*") do set "MSVC=%%d"
if not defined MSVC ( echo ERROR: no MSVC toolset under "%MSVC_ROOT%" & exit /b 1 )
set "PATH=%MSVC%\bin\Hostx64\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\um"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"
set "HERE=%~dp0"
set "OUT=%HERE%_build"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%HERE%..\reference" mkdir "%HERE%..\reference"
cl.exe /nologo /W4 /WX /Fe:"%OUT%\refdump.exe" /Fo:"%OUT%\\" ^
   "%HERE%refdump.c" "%HERE%..\src\common\usb_descriptors.c" "%HERE%..\src\common\hci_stub.c"
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
rem Written beside the build first, so a failed run leaves the existing reference untouched.
"%OUT%\refdump.exe" > "%OUT%\refdump.txt"
if errorlevel 1 ( echo ERROR: refdump.exe failed; reference\VIRTUAL-HCI-REFERENCE.txt was not updated & exit /b 1 )
move /y "%OUT%\refdump.txt" "%HERE%..\reference\VIRTUAL-HCI-REFERENCE.txt" >nul
if errorlevel 1 ( echo ERROR: cannot replace reference\VIRTUAL-HCI-REFERENCE.txt & exit /b 1 )
echo Wrote reference\VIRTUAL-HCI-REFERENCE.txt
exit /b 0
