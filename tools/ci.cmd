@echo off
rem CI entry point (.github/workflows/build.yml). Enters the runner's Visual Studio x64 developer
rem environment, then builds, runs the host suites (the firmware suites are skipped: Valve's
rem firmware is not redistributable), the socket regressions and a 60 s fuzz run per target. With
rem "package" it then builds the release zip, as tools\package.cmd does locally.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
  echo ERROR: no Visual Studio with the x64 C++ tools
  exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "DECKBT_NO_FIRMWARE=1"
call "%~dp0build.cmd" || exit /b 1
call "%~dp0selftest.cmd" || exit /b 1
python "%~dp0usbip_selftest.py" || exit /b 1
call "%~dp0fuzz.cmd" 60 || exit /b 1
if /i "%~1"=="package" (
  call "%~dp0package.cmd" || exit /b 1
)
exit /b 0
