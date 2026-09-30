@echo off
rem Build the libFuzzer targets with AddressSanitizer (MSVC: tools\toolchain.cmd) and run each for N seconds
rem (default 60). A crash or sanitizer report stops the target and leaves the input as
rem tools\_build\fuzz\<target>-crash-*. No elevation, no hardware.
setlocal EnableDelayedExpansion

set "SECONDS=%~1"
if "%SECONDS%"=="" set "SECONDS=60"
call "%~dp0toolchain.cmd" || exit /b 1

set "HERE=%~dp0"
set "ROOT=%HERE%..\"
set "OUT=%HERE%_build\fuzz"
if not exist "%OUT%" mkdir "%OUT%"
pushd "%ROOT%"

set TARGETS=^
 "fuzz_usbip_device|tools\fuzz_usbip_device.c src\service\usbip_device.c src\common\usb_descriptors.c src\common\sco_usb.c src\common\hci_stub.c"^
 "fuzz_controller|tools\fuzz_controller.c src\common\h4_codec.c src\common\hci_bridge.c src\common\sco_route.c"

set /a FAILED=0
for %%E in (%TARGETS%) do (
  for /f "tokens=1* delims=|" %%a in (%%E) do (
    if not exist "%OUT%\%%a" mkdir "%OUT%\%%a"
    cl.exe /nologo /W4 /WX /Zi /Od /fsanitize=address /fsanitize=fuzzer /Fe:"%OUT%\%%a.exe" /Fo:"%OUT%\%%a\\" /Fd:"%OUT%\%%a\\" %%b >"%OUT%\%%a\build.txt" 2>&1
    if errorlevel 1 (
      type "%OUT%\%%a\build.txt"
      echo BUILD FAILED: %%a
      set /a FAILED+=1
    ) else (
      if not exist "%OUT%\%%a\corpus" mkdir "%OUT%\%%a\corpus"
      echo.
      echo === %%a for %SECONDS% s
      "%OUT%\%%a.exe" "%OUT%\%%a\corpus" -max_total_time=%SECONDS% -max_len=8192 -artifact_prefix="%OUT%\%%a-" -print_final_stats=1
      if errorlevel 1 (
        echo FUZZ FAILURE: %%a
        set /a FAILED+=1
      )
    )
  )
)
popd
echo.
if !FAILED! equ 0 ( echo FUZZ PASSED & exit /b 0 )
echo FUZZ FAILED: !FAILED! target^(s^)
exit /b 1
