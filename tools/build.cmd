@echo off
rem Build DeckBtService with the EWDK or an active Visual Studio environment (tools\toolchain.cmd). No elevation needed to build.
rem Output: tools\_build\deckbt-usbip.exe (USB/IP server) and tools\_build\deckbt-uartprobe.exe
rem (user-mode UART identify probe). Set DECKBT_BUILD_OUT to build elsewhere, e.g. while the
rem service runs (and locks) the default output.
setlocal EnableDelayedExpansion

call "%~dp0toolchain.cmd" || exit /b 1

set "HERE=%~dp0"
set "ROOT=%HERE%..\"
if defined DECKBT_BUILD_OUT (set "OUT=%DECKBT_BUILD_OUT%") else (set "OUT=%HERE%_build")
set "OBJ=%OUT%\deckbt-usbip"
if not exist "%OBJ%" mkdir "%OBJ%"

pushd "%ROOT%"
rem One generated header supplies the C compiler, resource compiler and package identity.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop'; $v=(Get-Content -LiteralPath VERSION -Raw).Trim(); if ($v -notmatch '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$') { throw 'VERSION must be major.minor.patch' }; $n=$v.Split('.'); foreach ($part in $n) { if ([int]$part -gt 65535) { throw 'VERSION component exceeds 65535' } }; $id='nogit'; if (Get-Command git.exe -ErrorAction SilentlyContinue) { try { $hash=git.exe rev-parse --short HEAD 2>$null } catch { $hash=$null }; if ($hash -and $LASTEXITCODE -eq 0) { $id=$hash.Trim(); $dirty=git.exe status --porcelain --untracked-files=normal; if ($LASTEXITCODE -ne 0) { throw 'git status failed' }; if ($dirty) { $id+='-dirty' } } }; $version=$v+'+'+$id; $quote=[char]34; $header='#define DECKBT_VERSION '+$quote+$version+$quote+[Environment]::NewLine+'#define DECKBT_VERSION_NUMERIC '+($n -join ',')+',0'+[Environment]::NewLine; [IO.File]::WriteAllText('%OBJ%\build_version.h',$header,[Text.Encoding]::ASCII); [IO.File]::WriteAllText('%OUT%\build-version.txt',$version+[Environment]::NewLine,[Text.Encoding]::ASCII)"
if errorlevel 1 goto failed
rc.exe /nologo /i "%OBJ%" /fo "%OBJ%\deckbt_version.res" src\service\deckbt_usbip.rc
if errorlevel 1 goto failed
cl.exe /nologo /W4 /WX /O2 /Zi /DNDEBUG /FI"%OBJ%\build_version.h" /Fe:"%OUT%\deckbt-usbip.exe" /Fd:"%OBJ%\\" /Fo:"%OBJ%\\" ^
  src\service\deckbt_usbip.c src\service\usbip_device.c src\service\qca_backend.c src\service\uart_win32.c ^
  src\service\handsfree.c ^
  src\common\usb_descriptors.c src\common\sco_usb.c src\common\hci_stub.c ^
  src\common\h4_codec.c src\common\hci_bridge.c src\common\sco_route.c ^
  src\common\qca_identify.c src\common\qca_init_fsm.c src\common\qca_tlv.c "%OBJ%\deckbt_version.res"
if errorlevel 1 goto failed
cl.exe /nologo /W4 /WX /O2 /Zi /DNDEBUG /FI"%OBJ%\build_version.h" /Fe:"%OUT%\deckbt-uartprobe.exe" /Fd:"%OBJ%\\" /Fo:"%OBJ%\\" ^
  src\service\uart_probe.c src\service\uart_win32.c ^
  src\common\h4_codec.c src\common\qca_identify.c src\common\qca_tlv.c "%OBJ%\deckbt_version.res"
if errorlevel 1 goto failed
popd
echo Built %OUT%\deckbt-usbip.exe and %OUT%\deckbt-uartprobe.exe
exit /b 0

:failed
popd
echo BUILD FAILED
exit /b 1
