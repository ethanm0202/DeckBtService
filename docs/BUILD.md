# Building, testing and running

## Toolchain

The scripts use the Enterprise WDK (EWDK), a self-contained build environment (MSVC and the Windows SDK) that needs no installation. Only its compiler and SDK are used; nothing here is a driver.

1. Download the Windows 11 EWDK ISO (build 26100 or newer) from [Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk).
2. Extract it to `C:\EWDK`, or mount it and set `EWDK` to the mounted drive. The optional Python extractor runs without mounting or elevation:

   ```powershell
   python -m pip install pycdlib
   python tools\extract_ewdk.py <iso> C:\EWDK
   ```

   A non-zero exit means extraction failed; do not use an incomplete toolchain.

## Build

```cmd
tools\build.cmd
```

Builds `tools\_build\deckbt-usbip.exe`, the program and service, and `tools\_build\deckbt-uartprobe.exe`, a read-only check that the controller answers from user mode.

The build embeds `VERSION` plus the short Git hash and `-dirty` for a modified worktree (or `+nogit` without Git) in both executables' PE version resources. `deckbt-usbip.exe --version` prints the same identity.

To build and test without overwriting an executable used by a running service:

```cmd
set DECKBT_BUILD_OUT=%CD%\tools\_build\test
tools\build.cmd
tools\selftest.cmd
tools\_build\test\deckbt-usbip.exe --version
```

### Release zip

```cmd
tools\package.cmd
```

Builds in `tools\_build\package-release`, never the default output, and refuses to run unless the release inputs in `packaging/` are present. A clean build produces `dist\DeckBtService-<VERSION>.zip` with one top folder, `DeckBtService-<VERSION>\`; for v0.1.1 that is `DeckBtService-0.1.1.zip` and `DeckBtService-0.1.1\`. A dirty test build keeps the full build identity in both names. The folder contains:

| File | Purpose |
|---|---|
| `install.cmd`, `uninstall.cmd`, `collect-diagnostics.cmd` | double-click launchers; install and uninstall request elevation, run the matching `.ps1` and keep the window open to show the result |
| `install.ps1`, `uninstall.ps1`, `collect-diagnostics.ps1` | the release scripts |
| `README-install.md` | the user guide |
| `deckbt-usbip.exe`, `deckbt-uartprobe.exe` | the program and service, and the UART check |
| `SHA256SUMS` | lowercase SHA-256, two spaces, file name, for every other file in the folder |

A `-dirty` version (uncommitted changes) is refused unless `DECKBT_ALLOW_DIRTY=1` is set.

## Tests

```cmd
tools\selftest.cmd
```

No elevation and no hardware. Each suite compiles production sources into a user-mode program:

| Suite | Sources under test | Covers |
|---|---|---|
| `descriptor_selftest` | `usb_descriptors.c` | configuration descriptor bytes, high-speed and isochronous rules |
| `hci_selftest` | `hci_stub.c` | stub replies and their parameter sizes, LE state mask, FIFO limits |
| `qca_selftest` | `qca_tlv.c` | TLV parsing and segmentation against the real firmware files |
| `tlv_segment_selftest` | `qca_tlv.c` | segment boundaries for the firmware sizes, parameter lengths, acknowledgement rules |
| `qca_fsm_selftest` | `qca_init_fsm.c` | complete bring-up against a mock controller |
| `nvm_selftest` | `qca_init_fsm.c`, `qca_tlv.c` | NVM selection, the HCI rate-byte rewrite |
| `identify_selftest` | `qca_identify.c` | version-response parsing, including replies recorded from the controller |
| `h4_selftest` | `h4_codec.c` | H4 framing, split and malformed input, in-band sleep bytes |
| `bridge_selftest` | `hci_bridge.c` | readiness hold, vendor-event filtering, ACL credits (shared and LE pools, refill on `HCI_Reset`) |
| `sco_usb_selftest` | `sco_usb.c` | SCO pacing, OUT reassembly and resynchronisation, IN re-framing |
| `sco_route_selftest` | `sco_route.c` | enhanced synchronous-connection rewrite and opcode restore |
| `usbip_device_selftest` | `usbip_device.c` | event/ACL delivery in controller order across both endpoints, the 20 ms hold bound, a lost reply delivered again in order, isochronous descriptor checks (order, overlap, no packet above the setting's `wMaxPacketSize`) |
| `qca_backend_selftest` | `qca_backend.c` (UART mocked) | `QcaBackendStart`/`QcaBackendStop` against a simulated controller with the real firmware: identify ladder, baud switch, download, host IBS wake acknowledged on the first or third `WAKE_IND` or never (start fails with `ERROR_TIMEOUT`, no writer, controller handed back to ROM at 115200), restart after a failed start |

It then runs `tools\check-reference.cmd`, which regenerates the stub's descriptors and HCI exchanges with `tools\refdump.c` and compares them with `reference\VIRTUAL-HCI-REFERENCE.txt`. A difference means the USB device changed; if intended, run `tools\refdump.cmd` and commit the new reference.

The QCA suites read the firmware from the newest installed `qcbtuart.inf_amd64_*` package in the DriverStore; `QCA_FW_DIR` overrides it. `EWDK` overrides the toolchain location (default `C:\EWDK`).

### Socket regressions and fuzzing

After building, run `python tools\usbip_selftest.py` (Python 3, standard library only;
`DECKBT_USBIP_EXE` selects another build). It launches isolated console stub servers on temporary
loopback ports, without UART access or usbip-win2 attachment. Eight regressions cover rejected
isochronous replies followed by a valid request, a reply unlinked in flight being delivered again,
silent and fragmented clients, absolute handshake deadlines, non-kernel import denial, a refused
bus ID that cannot add lines to the log, and shutdown with all pending handshake slots occupied or
with a live session.

`tools\fuzz.cmd 60` builds two libFuzzer/AddressSanitizer targets and runs **each** for 60 seconds.
`fuzz_usbip_device` checks the request parser, device model and reply framing, including that no
accepted isochronous packet exceeds its setting's `wMaxPacketSize`;
`fuzz_controller` checks H4/controller parsing and the bridge. Corpus files and crash inputs stay
under `tools\_build\fuzz`; a sanitizer report or failed invariant makes the command fail.
These tests do not prove hardware voice quality or kernel-driver safety.


## Running it

### Install

Users install a release zip: see [packaging/README-install.md](../packaging/README-install.md). To test a local build the same way:

```cmd
set DECKBT_ALLOW_DIRTY=1
tools\package.cmd
```

Then extract `dist\DeckBtService-<version>.zip` and double-click its `install.cmd`, or run its `install.ps1` from an elevated 64-bit Windows PowerShell. Without `DECKBT_ALLOW_DIRTY`, `package.cmd` refuses a tree with uncommitted changes, so a release always names a real commit.

`install.ps1` makes these changes, in order, and records each with its prior value in `%ProgramData%\DeckBtService\install-state.json`. Re-running it resumes an interrupted install or upgrades the programs. Before any of them it refuses a `DeckBtService` service that runs another program; that service is never replaced.

1. **usbip-win2 0.9.8.1.** Downloaded from the official release and checked against a pinned SHA-256 if missing or older. 0.9.8.0 breaks headset reconnects ([issue #190](https://github.com/vadimgrn/usbip-win2/issues/190)); its installer can ask for a restart.
2. **Stock transport off.** A device-installation deny policy for `ACPI\QCOM2066`. Then Bluetooth is switched off (the Settings toggle, so headsets disconnect) and the radio's devnode is removed and rescanned, so it returns without a driver (problem 28 or 1). Removing it while a headset streams through the stock driver blocks inside PnP, so the removal is given 90 s and a restart is requested instead.
3. **UART published to user mode.** REG_SZ `SerCxFriendlyName` = `QCA2066` under `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\AMDI0020\4\Device Parameters`, then the controller `ACPI\AMDI0020\4` restarted.
4. **Programs.** Copied to `%ProgramFiles%\DeckBtService` (SYSTEM and Administrators full control, Users read and execute) and verified against `SHA256SUMS`.
5. **Service.** Registered with the installed `deckbt-usbip.exe install --backend uart`, started, and confirmed by `attach: usbip-win2 port` in the log.

`uninstall.ps1` reverses the record newest first and puts the stock driver back on the radio. On a setup without a record it detects and reverts the same changes. It removes the `DeckBtService` service only when it runs `%ProgramFiles%\DeckBtService\deckbt-usbip.exe`. `collect-diagnostics.ps1` bundles logs and device state (never pairing keys) into a zip on the Desktop.

To check that the controller answers from user mode once the UART is published and the service is stopped: `"%ProgramFiles%\DeckBtService\deckbt-uartprobe.exe"` (expected: `answered at 115200 baud (ROM); SoC 0x400C1211`).

### As a service

`"%ProgramFiles%\DeckBtService\deckbt-usbip.exe" install --backend uart` registers `DeckBtService` (LocalSystem, automatic start) to run that executable with the given options; `install.ps1` runs it. It refuses (exit code 3, before opening the service manager) unless its own path lies under the Program Files known folder. That path check keeps a LocalSystem service out of user folders; the folder's permissions are set by `install.ps1`. Recovery delays are 5 s, 15 s, then 60 s for every later failure; reported nonzero service exits also trigger recovery. At start it brings the controller up (about 3.5 s), then attaches its device through usbip-win2's `usbip.exe`; Windows shows the Bluetooth radio and paired devices reconnect. Before sleep it disconnects links, detaches and hands the controller back; after resume it brings it up and attaches again. An external detach, a lost connection or a controller fault resets and re-attaches the radio automatically. `sc stop DeckBtService` detaches and hands the controller back. `deckbt-usbip.exe uninstall` removes the service only after confirming it has stopped.

The log is `%ProgramData%\DeckBtService\deckbt-service.log`, rotated at 4 MiB with one `.1` kept; the service keeps that folder admin-owned and read-only for users (see [ARCHITECTURE.md](ARCHITECTURE.md)). Each process starts with its version, a UTC timestamp and its PID; later timestamps are elapsed seconds.

### In a console

```cmd
tools\_build\deckbt-usbip.exe --backend uart --log tools\_build\deckbt-usbip.log
```

The same run in the foreground; Ctrl+C (or the file named by `--stop-file`) stops it.

### Options

| Option | Default | Meaning |
|---|---|---|
| `--version` | | print `deckbt-usbip <version>` and exit |
| `--backend stub\|uart` | `stub` | synthetic controller, or the real one |
| `--controller ID` | `ACPI\AMDI0020\4` | UART controller whose published interface is opened |
| `--firmware-dir DIR` | newest `qcbtuart.inf_amd64_*` | directory with `hpbtfw21.tlv` and the `hpnv21*` files |
| `--port N` | 3241 | loopback TCP port. 3240, the USB/IP default, is left to other software such as usbipd-win |
| `--busid ID` | `1-1` | USB/IP bus ID offered for import |
| `--usbip PATH` | `%ProgramFiles%\USBip\usbip.exe` | usbip-win2 command-line client |
| `--no-attach` |  | serve only; attach by hand with `usbip.exe --tcp-port 3241 attach -r 127.0.0.1 -b 1-1 --receive-mode low-latency` (the service adds `--once`, so usbip-win2 does not retry an import by itself) |
| `--allow-user-import` | off | test clients may import only with console `--backend stub --no-attach`; rejected for UART or service installation/execution |
| `--log FILE` | service: `%ProgramData%\DeckBtService\deckbt-service.log` | console-only override; the service always uses its secured known-folder location |
| `--quiet` |  | no console output |
| `--stop-file FILE` |  | stop when FILE appears |

`--backend stub` needs neither the UART nor the controller: Windows enumerates a synthetic radio, which is useful for work on the USB side.

Production imports require the loopback connection to belong to PID 4 (System); device listing
remains available to user-mode clients. This distinguishes kernel from user-mode connections,
not a particular kernel driver or a hostile administrator.

## Diagnostics

| Tool | Use |
|---|---|
| `tools\miccheck.py` | records from the headset microphone while playing a tone into the headset, as a call does; PASS/FAIL/NODEV |
| `tools\bt-radio.ps1` | the Settings Bluetooth toggle, scripted |
| `tools\bt-scan.ps1` | Bluetooth LE and Classic discovery through the current radio |
| `tools\bt-hid-watch.ps1` | counts input events from Bluetooth HID devices |
