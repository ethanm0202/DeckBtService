@echo off
rem Compile and run the host-side test suites with the extracted EWDK toolchain.
rem No elevation, no installation, no hardware: each suite compiles production sources into a
rem user-mode test program. The QCA suites read the firmware of the installed Qualcomm package.
setlocal EnableDelayedExpansion

if not defined EWDK set "EWDK=C:\EWDK"
set "MSVC_ROOT=%EWDK%\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC"
set "SDK=%EWDK%\Program Files\Windows Kits\10"
set "SDKVER=10.0.26100.0"

for /d %%d in ("%MSVC_ROOT%\*") do set "MSVC=%%d"
if not defined MSVC (
  echo ERROR: no MSVC toolset under "%MSVC_ROOT%"
  exit /b 1
)

set "PATH=%MSVC%\bin\Hostx64\x64;%SDK%\bin\%SDKVER%\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\winrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"

set "HERE=%~dp0"
set "ROOT=%HERE%..\"
set "OUT=%HERE%_build"
if not exist "%OUT%" mkdir "%OUT%"

rem Firmware for the QCA suites: the newest installed vendor package, unless QCA_FW_DIR is set.
set "FW_REPO=%SystemRoot%\System32\DriverStore\FileRepository"
if not defined QCA_FW_DIR (
  for /f "delims=" %%d in ('dir /b /ad /o-d "%FW_REPO%\qcbtuart.inf_amd64_*" 2^>nul') do (
    if not defined QCA_FW_DIR if exist "%FW_REPO%\%%d\hpbtfw21.tlv" set "QCA_FW_DIR=%FW_REPO%\%%d"
  )
)
if defined QCA_FW_DIR (
  echo Firmware: %QCA_FW_DIR%
) else (
  echo Firmware: none found; the QCA suites will fail. Set QCA_FW_DIR to a directory holding
  echo hpbtfw21.tlv and the hpnv21* files.
)

pushd "%ROOT%"
echo Toolset: %MSVC%

set SUITES=^
 "descriptor_selftest|tools\descriptor_selftest.c src\common\usb_descriptors.c"^
 "hci_selftest|tools\hci_selftest.c src\common\hci_stub.c"^
 "qca_selftest|tools\qca_selftest.c src\common\qca_tlv.c"^
 "qca_fsm_selftest|tools\qca_fsm_selftest.c src\common\qca_init_fsm.c src\common\qca_tlv.c"^
 "nvm_selftest|tools\nvm_selftest.c src\common\qca_tlv.c src\common\qca_init_fsm.c"^
 "tlv_segment_selftest|tools\tlv_segment_selftest.c src\common\qca_tlv.c"^
 "identify_selftest|tools\identify_selftest.c src\common\qca_identify.c src\common\qca_tlv.c"^
 "h4_selftest|tools\h4_selftest.c src\common\h4_codec.c"^
 "bridge_selftest|tools\bridge_selftest.c src\common\hci_bridge.c src\common\h4_codec.c"^
 "sco_usb_selftest|tools\sco_usb_selftest.c src\common\sco_usb.c"^
 "sco_route_selftest|tools\sco_route_selftest.c src\common\sco_route.c"^
 "usbip_device_selftest|tools\usbip_device_selftest.c src\service\usbip_device.c src\common\sco_usb.c src\common\usb_descriptors.c"

set /a FAILED=0
for %%E in (%SUITES%) do (
  for /f "tokens=1* delims=|" %%a in (%%E) do (
    set "OBJDIR=%OUT%\%%a"
    if not exist "!OBJDIR!" mkdir "!OBJDIR!"
    cl.exe /nologo /W4 /WX /Fe:"%OUT%\%%a.exe" /Fo:"!OBJDIR!\\" %%b >"!OBJDIR!\build.txt" 2>&1
    if errorlevel 1 (
      type "!OBJDIR!\build.txt"
      echo BUILD FAILED: %%a
      set "RESULT_%%a=build FAILED"
      set /a FAILED+=1
    ) else (
      echo.
      "%OUT%\%%a.exe"
      if errorlevel 1 (
        set "RESULT_%%a=FAILED"
        set /a FAILED+=1
      ) else (
        set "RESULT_%%a=passed"
      )
    )
  )
)

echo.
echo Reference check:
call "%HERE%check-reference.cmd"
if errorlevel 1 (
  set "RESULT_reference=FAILED"
  set /a FAILED+=1
) else (
  set "RESULT_reference=passed"
)

echo.
for %%E in (%SUITES%) do (
  for /f "tokens=1 delims=|" %%a in (%%E) do (
    set "NAME=%%a                         "
    echo !NAME:~0,25! !RESULT_%%a!
  )
)
set "NAME=reference                         "
echo !NAME:~0,25! !RESULT_reference!
echo.
popd
if !FAILED! equ 0 (
  echo SELFTEST PASSED
  exit /b 0
)
echo SELFTEST FAILED: !FAILED! suite^(s^)
exit /b 1
