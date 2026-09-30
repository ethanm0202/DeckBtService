@echo off
rem Compiler environment for the build and test scripts, which call this. An active Visual Studio
rem developer environment (VCToolsInstallDir set, for example by vcvars64.bat on a CI runner) is used
rem as it is; otherwise the extracted EWDK at %EWDK% (default C:\EWDK). Sets DECKBT_TOOLSET for display.
if not defined VCToolsInstallDir goto ewdk
set "DECKBT_TOOLSET=Visual Studio environment: %VCToolsInstallDir%"
exit /b 0

:ewdk
if not defined EWDK set "EWDK=C:\EWDK"
set "MSVC_ROOT=%EWDK%\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC"
set "SDK=%EWDK%\Program Files\Windows Kits\10"
set "SDKVER=10.0.26100.0"
set "MSVC="
for /d %%d in ("%MSVC_ROOT%\*") do set "MSVC=%%d"
if not defined MSVC (
  echo ERROR: no MSVC toolset under "%MSVC_ROOT%" and no Visual Studio developer environment
  exit /b 1
)
set "PATH=%MSVC%\bin\Hostx64\x64;%SDK%\bin\%SDKVER%\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\winrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"
set "DECKBT_TOOLSET=EWDK: %MSVC%"
exit /b 0
