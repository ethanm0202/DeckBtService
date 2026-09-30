@echo off
rem Compile and run the host-side test suites with the EWDK or an active Visual Studio environment (tools\toolchain.cmd).
rem No elevation, no installation, no hardware: each suite compiles production sources into a
rem user-mode test program. The QCA suites read the firmware of the installed Qualcomm package.
setlocal EnableDelayedExpansion

call "%~dp0toolchain.cmd" || exit /b 1

set "HERE=%~dp0"
set "ROOT=%HERE%..\"
set "OUT=%HERE%_build"
if not exist "%OUT%" mkdir "%OUT%"

rem Firmware for the QCA suites: the newest installed vendor package, unless QCA_FW_DIR is set.
rem DECKBT_NO_FIRMWARE=1 (CI: Valve's firmware cannot be redistributed) skips those suites, reported as such.
set "FIRMWARE_SUITES=qca_selftest qca_fsm_selftest nvm_selftest tlv_segment_selftest qca_backend_selftest"
set "FW_REPO=%SystemRoot%\System32\DriverStore\FileRepository"
if "%DECKBT_NO_FIRMWARE%"=="1" (
  echo Firmware: not used ^(DECKBT_NO_FIRMWARE=1^); skipping %FIRMWARE_SUITES%
  goto firmware_done
)
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
:firmware_done

pushd "%ROOT%"
echo Toolset: %DECKBT_TOOLSET%

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
 "qca_backend_selftest|tools\qca_backend_selftest.c src\service\qca_backend.c src\common\h4_codec.c src\common\hci_bridge.c src\common\sco_route.c src\common\qca_identify.c src\common\qca_init_fsm.c src\common\qca_tlv.c"^
 "usbip_device_selftest|tools\usbip_device_selftest.c src\service\usbip_device.c src\common\sco_usb.c src\common\usb_descriptors.c"^
 "lifecycle_selftest|tools\lifecycle_selftest.c src\service\usbip_device.c src\service\qca_backend.c src\service\uart_win32.c src\service\handsfree.c src\common\usb_descriptors.c src\common\sco_usb.c src\common\hci_stub.c src\common\h4_codec.c src\common\hci_bridge.c src\common\sco_route.c src\common\qca_identify.c src\common\qca_init_fsm.c src\common\qca_tlv.c"

set /a FAILED=0
for %%E in (%SUITES%) do (
  for /f "tokens=1* delims=|" %%a in (%%E) do (
    set "SKIP="
    if "%DECKBT_NO_FIRMWARE%"=="1" for %%s in (%FIRMWARE_SUITES%) do if /i "%%s"=="%%a" set "SKIP=1"
    if defined SKIP (
      set "RESULT_%%a=skipped (no firmware)"
    ) else (
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
