# Verification

Results for DeckBtService on one Steam Deck OLED, with v0.1.4 verification and the v0.1.3 controller incident separated from earlier successful tests below. The install, upgrade and uninstall results come from the v0.1.0 release package; the other original device results come from the builds leading up to it. The full uninstall-and-reinstall run behind v0.1.2 is under [v0.1.2](#v012); the v0.1.1 fixes under [v0.1.1](#v011). Results measured with usbip-win2 0.9.8.0, before the move to 0.9.8.1, are marked as such.

## Test environment

| Item | Value |
|---|---|
| Hardware | Steam Deck OLED (one unit), Qualcomm QCA2066 Bluetooth controller on the AMD UART `ACPI\AMDI0020\4` |
| Windows | Windows 11 Home 25H2, build 26200; Secure Boot off, Memory Integrity off |
| Test signing | Off, with VirtBthUsb removed, for the install results, all usbip-win2 0.9.8.1 results, and the 0.9.8.0 robustness, forced-termination and three-cycle sleep results. The synthetic-controller, isochronous, first real-radio and first service measurements were made earlier with test signing on and VirtBthUsb installed but idle. |
| usbip-win2 | 0.9.8.1: `USBip-0.9.8.1-x64.exe`, SHA-256 `38cad6d4432b52d5bb9409d9ad03b72fdffc4ada4cd3a48fbeca1a2752a8518a` (matches the GitHub release digest), driver packages 12.1.30.403. Earlier results: 0.9.8.0, SHA-256 `81f426741f7ee2ed991febe24a22daca8400b6ae2f171054e3fb404897e15d39`. |
| Signatures | Both installers signed by Cloudyne Systems (OSSign). `usbip2_ude` and `usbip2_filter` load with Microsoft Windows Hardware Compatibility Publisher signatures; no Code Integrity block was recorded. |
| UART publication | `SerCxFriendlyName = "QCA2066"` on `ACPI\AMDI0020\4`. SerCx2 publishes `\\?\ACPI#AMDI0020#4#{86e0d1e0-8089-11d0-9ce4-08003e301f73}\SERCX` with port name `QCA2066`. Opening it without administrator rights fails with `ERROR_ACCESS_DENIED`. |
| Stock radio | `ACPI\QCOM2066` driverless (problem 28), held there by a device-installation deny policy so the stock transport cannot take the UART (bound to the idle VirtBthUsb driver during the earliest measurements) |
| USB/IP server | `127.0.0.1:3241` |
| Peripherals | Shokz OpenMeet (headset), AirPods Pro, Razer Orochi V2 (Bluetooth LE mouse) |

Test tools in `tools/`:

- `miccheck.py`: records from the headset's microphone while playing a quiet tone into the headset, as a call does, and reports samples captured and RMS level (PASS needs speech).
- `bt-radio.ps1`: switches Bluetooth off and on through the Windows radio API, as the Settings toggle does.
- `bt-scan.ps1`: active discovery of Classic and LE devices.
- `bt-hid-watch.ps1`: counts input reports from a Bluetooth HID device.

The usbip-win2 defect below was traced with Windows' ETW providers for BTHPORT, BTHUSB, Bluetooth-Policy, UCX and USBHUB3.

## Automated tests

### Host-side suites

`tools\selftest.cmd` (see [BUILD.md](BUILD.md)) builds and runs 14 suites against the production sources, then compares the synthetic controller's descriptors and HCI exchanges with the reference record. The suite results include the new backend and service-lifecycle cases below.

| Suite | Covers |
|---|---|
| `descriptor_selftest` | device and configuration descriptors, including the UDE constraints |
| `hci_selftest` | synthetic controller (stub backend) |
| `qca_selftest`, `tlv_segment_selftest`, `nvm_selftest` | TLV firmware parsing, rampatch segmentation, NVM selection and the baud-rate byte |
| `qca_fsm_selftest` | bring-up state machine |
| `identify_selftest` | controller identification, against the reply recorded from the stock driver |
| `h4_selftest` | H4 framing and removal of in-band sleep bytes |
| `bridge_selftest` | HCI bridge: readiness, ACL credits, link and address tracking, RFCOMM detection in both directions, clean-disconnect sequence |
| `sco_usb_selftest`, `sco_route_selftest` | voice framing and pacing; rewriting to the enhanced synchronous-connection commands |
| `qca_backend_selftest` | the controller backend against a simulated controller behind a mocked UART: full bring-up, host in-band-sleep wake acknowledged on the first or third try or never, transient low CTS before data or ACK, ACK-before-data when flow control clears, Stop during the CTS wait, partial-write termination without replay or later data, persistent CTS failure, and restart when the fixture becomes responsive |
| `usbip_device_selftest` | device model: cross-endpoint ordering in both directions, the 20 ms hold, arrival order with both reads parked, a lost ACL packet delivered ahead of a later event, replay order after repeated cancellation, a fresh hold for each new blockage, isochronous descriptors out of order, overlapping or longer than the setting's `wMaxPacketSize` |
| `lifecycle_selftest` | the real service lifecycle and TCP listener with only controller start replaced: three consecutive physical wake failures pause retries without exiting, imports refuse an unavailable radio, an intervening ordinary error resets the CTS sequence, ordinary failures retain their exit status, and Stop interrupts retries or the paused state |

The service and the UART probe build with MSVC `/W4 /WX`.

### Socket regression tests

`tools/usbip_selftest.py` runs 8 tests against real server processes (stub backend):

- silent and fragmented clients do not block device listing;
- an import sent slowly hits its absolute deadline;
- an import from a user-mode process is refused, even with no session active;
- a refused import's bus ID cannot add lines to the log (CR, LF, `\` and `'` are escaped);
- a rejected isochronous request does not consume the next reply;
- a reply unlinked in flight is delivered again, in order (this test fails without the replay mechanism);
- stop with all eight handshake slots occupied;
- stop with a live session.

### Fuzzing

`tools\fuzz.cmd` builds two libFuzzer targets with AddressSanitizer: the USB/IP device model (`fuzz_usbip_device.c`) and the controller side (`fuzz_controller.c`). Each run lasts 60 s per target. The local 0.1.4 run executed 918,167 USB/IP and 111,009 controller inputs with no findings. Before v0.1.1: 930,374 and 126,087; before v0.1.0: 1,040,357 and 154,179. These targets exercise parsers and protocol models, not physical UART flow control.

## v0.1.4

**Offline regressions:** low CTS before a queued H4 command, low CTS with only an IBS acknowledgement pending, and a partial write followed by a second queued command were added before the first writer fix. They produced six failed assertions on the original backend, then passed with that fix. A stricter fixture then held CTS low independently of RTS and released it as receiver flow control; the initial RTS-pulsing implementation produced eight failed assertions, including the ACK-priority case. All pass with passive CTS waiting, as does a new case that stops during the wait without sending queued data or reporting a fault. The mock enforces CTS for every byte, including IBS; it does not invent a bypass for a wake acknowledgement.

The lifecycle test runs the actual listener and recovery loop without opening the UART or attaching a device. Three consecutive CTS-unresponsive starts leave it alive but refusing imports. An intervening ordinary error resets that sequence; ordinary failures still return their error. Stop works during the retry delay and after retries have paused.

**Flow-control investigation and testing:** An initial development build that pulsed RTS on low CTS caused mouse input pauses during audio streaming, as RTS manipulation interfered with incoming controller-to-host traffic. Probing the AMD UART driver confirmed that `SERIAL_EV_CTS` notification registration is unsupported (Win32 error 50). The implementation was therefore changed to bounded passive CTS polling using writer events without RTS manipulation. This resolved the mouse stalls and audio artifacts.

**Published release artifact (v0.1.4):** built and attested by GitHub Actions run 36775528361 from commit `b1d06ee896f0e160f722af035f0f519226af7371`. Release zip SHA-256 is `dbafe04d71f35f05b16af7ecc6519f0ecc615e663dcffd36359ae32d1e5ccc5f`. Provenance verified with `gh attestation verify` for the zip, `deckbt-usbip.exe` (`815c0c63c403ff5f34ca9f6cc8109c56be1f8447cbf71745b10dbefb0062e1f8`), and `deckbt-uartprobe.exe` (`39bdee0866195e459537ff34bfa9c7875f04418769cf46ee6923fbf0bed0795b`). Installed via `install.ps1` in 12.6 s without restart; service confirmed running `0.1.4+b1d06ee`.

**Pre-release verification build, 2026-09-30:** built without diagnostic tracing. The zip's SHA-256 is `a42789b10142e822a4003bccfb8741285fd432d9a351adc9dba22cfdbe2653b2`. Both programs built with `/W4 /WX`; all 14 host suites, the reference check, and all 8 real-server socket checks passed.
| Check | Measured result |
|---|---|
| 15-second call with mouse movement | 239,680/240,000 microphone samples, -40.2 dBFS; 1,499 mouse reports; worst voice-pacing lateness 1.885 ms |
| 40-second call with mouse movement and three CPU-load bursts | 639,840/640,000 samples, -50.3 dBFS; 2,047 mouse reports; worst lateness 2.633 ms |
| Music and mouse during three CPU-load bursts | 1,046 mouse reports; user reported clean music and smooth movement |
| Bluetooth Off/On | microphone present 2.5 s after On completed; 10-second call delivered 159,680/160,000 samples; 1,004 mouse reports |
| Forced service termination | SCM recorded its five-second restart action; new process observed running in 6.6 s, microphone back in 21.5 s; subsequent 10-second call delivered 159,840/160,000 samples and 1,310 mouse reports |
| Sleep/wake, one cycle | clean handback to ROM; controller ready 3.437 s after resume started it; subsequent 10-second call delivered 159,840/160,000 samples and 1,175 mouse reports |

The user reported clean audio and smooth mouse movement for each completed call, including the end of playback. Each CPU-load burst launched 16 normal-priority workers, each busy for four seconds. The baseline and loaded-call sessions logged no UART errors, full TX queue, stalls, SCO rejections or ACL-credit refusals. Input counts support the record but do not substitute for the user's smoothness report.

The first post-crash movement window produced no input and did not start a call; whether the mouse was being moved was unknown. The subsequent call/input check completed without another service restart. Only that completed check is counted above.

## v0.1.3 controller failure

After about 1 h 42 min of a session on 2026-09-29, one UART write ended short with Win32 error 29. Subsequent attempts found CTS low and could not wake/reset the controller. Service restarts, sleep/wake, attempting writes with software CTS handshaking disabled, and a UART device restart did not recover it. Restarting Windows did.

Fortnite, Easy Anti-Cheat and a launcher power-plan change preceded the failure. Two short lobby retests after restarting Windows, one without and one with the launcher script, did not reproduce it. This establishes neither a causal link nor sustained gaming compatibility.

The final counters were `wakeInd 9865`, `sleepInd 8659`, `ack 8660`, `ackCtsLow 0`, `writeErr 1`, `readErr 0`. The wake/ack difference of 1,205 is cumulative and repeated indications can coalesce into one pending acknowledgement. It does not prove that 1,205 independent handshakes went unanswered, or that they all occurred during the failed write.

The initiating cause remains unknown. The v0.1.4 release fixes demonstrated writer defects and reports a controller that cannot be woken; it does not establish prevention of this incident or a non-reboot recovery. A pre-write CTS check cannot prevent CTS falling partway through a packet.

## v0.1.2

Measured on the test Deck on 2026-09-29 with the Shokz OpenMeet and the Razer Orochi V2, the user wearing the headset and moving the mouse.

**Full uninstall and reinstall from the published v0.1.1 zip** (SHA-256 checked against the release notes; all files against `SHA256SUMS`):

| Step | Result |
|---|---|
| `uninstall.cmd -RemoveUsbip -RemoveLogs` | 30 s; asked for a restart (usbip-win2 drivers). Service, program folder, data folder, usbip-win2, the three deny entries with both policy flags and keys, and `SerCxFriendlyName` all removed; stock `QcBluetooth` back on the radio, problem 0; all four pairings kept |
| Stock Bluetooth after the restart | Shokz connected as headphones only (no hands-free microphone, the stock limitation); music heard clearly |
| First `install.cmd` | 51 s, restart requested: usbip-win2 0.9.8.1 downloaded, hash-checked and installed; stock radio blocked and removed; UART published; service registered |
| After the restart | service started 8 s after boot, radio ready 3.45 s later, Shokz reconnected with its hands-free microphone |
| Second `install.cmd` | exit 0 in 13 s |

**Functional tests** (v0.1.1, then the v0.1.2 code):

| Test | Result |
|---|---|
| Mouse | 1,157 input reports in 15 s |
| Music | chime and music clear |
| Calls (15 s, twice) | `miccheck` PASS, −45 dBFS, 94–95% of samples, natural speech; 2,059 mouse reports during the second call |
| 5-minute call | PASS, 99% of samples |
| Bluetooth Off/On | headset microphone back 8 s after On; call PASS; mouse 1,127 reports |
| Service killed | restarted by Windows after 5 s; radio up 6.6 s later (controller reset from running firmware); microphone back 32 s after the kill; call PASS; mouse 1,058 reports |
| Sleep and wake (two cycles) | radio stopped before each sleep and back 3.4 s after each resume; the hands-free workaround ran once per resume; call PASS; mouse 485 reports |
| Discovery | 20 LE and 4 Classic devices (30 s scan) |
| Logs | no bugcheck, no Bluetooth or usbip warnings in the System log; in every session 0 stalls, 0 credit refusals, 0 rejected voice transfers (44,239 OUT and 41,707 IN transfers in the longest) |
| Diagnostics | `collect-diagnostics.cmd`: 11 files, no link keys |

**Audio under load.** During the 5-minute call the user heard crackle, and in music stutter, whenever the desktop lagged (for example Chrome starting). That session logged voice pacing up to 73.6 ms late. Reproduced with a 40 s call and three 4 s bursts of 16 busy processes:

| | v0.1.1 | v0.1.2 (MMCSS) |
|---|---|---|
| Worst pacing lateness | 111,661 µs | 1,811 µs |
| Microphone samples delivered | 85% | 98% |
| Heard | clear crackle and dropouts in the bursts | "much less, maybe one noticeable crackle … otherwise very consistent" |
| Music with the same bursts | stuttered | no stutter |

**The attested v0.1.3 build.** GitHub Actions built v0.1.3 (same source) on a `windows-2025` runner with Visual Studio 2026 (MSVC 14.51), where the host suites without firmware, the socket regressions and 60 s of fuzzing per target (1,697,869 and 888,032 inputs) passed. `gh attestation verify` on the downloaded zip showed SLSA v1 provenance from `build.yml` at `refs/tags/v0.1.3` on a GitHub-hosted runner; the programs inside the zip and the installed `deckbt-usbip.exe` verified too. Installed over v0.1.2 in 14 s with no restart. The same 40 s call with three load bursts: PASS, 100% of samples, worst pacing lateness 1,918 µs, no stalls or rejected transfers, heard as clean; 313 mouse reports in 10 s.

## v0.1.1

v0.1.1 fixes the findings of a static review of v0.1.0 ([CHANGELOG.md](../CHANGELOG.md)). Each fix has a regression that fails on v0.1.0:

| Fix | Regression | On v0.1.0 | On v0.1.1 |
|---|---|---|---|
| Unacknowledged host wake fails the start | `qca_backend_selftest`, controller that never answers `WAKE_IND` | start returned success, bridge ready, no hand-back | `ERROR_TIMEOUT` after 10 tries, no writer, bridge not ready, controller back in ROM at 115200, next start succeeds |
| Oversize isochronous packets rejected | `usbip_device_selftest`, 18-byte packets at alternate setting 2 (17) | both directions accepted (5 checks failed) | `-EINVAL`; 17-byte packets accepted and completed |
| | `fuzz_usbip_device` packet-size invariant | abort within 2 s against v0.1.0's `usbip_device.c` | 930,374 inputs, no findings |
| Bus ID escaped in the log | `usbip_selftest.py`, bus ID `1-1\r\n  999.000 forged\'` | the log gained a line `  999.000 forged\'' refused (status 1)` | one line, `import of '1-1\x0D\x0A  999.000 forged\x5C\x27' refused` |
| Foreign `DeckBtService` left alone | throwaway harness: `install.ps1` and `uninstall.ps1` functions against a dummy service | (the takeover deleted it) | refused before any change; uninstall leaves it registered; our own path, in any letter case, still accepted |

The pacing-timer changes (created before the server starts; a failed arm waits instead of spinning) have no failure injection: `CreateWaitableTimerExW` and `SetWaitableTimer` did not fail in any run.

On the device, the v0.1.1 code (commit `468d2bb`, differing from the release only in version files and documentation) was installed over v0.1.0 with its release-style package: `install.ps1` exit 0 in 11.6 s with no restart; `steady: host wake acknowledged after 1 WAKE_IND`, bridge ready 3,406 ms after start, `attach: usbip-win2 port 1`; the Generic Bluetooth Adapter and the paired devices came back. No headset was connected during this run, so voice was not exercised on v0.1.1. The isochronous check is consistent with the traffic recorded earlier from `BTHUSB`: a v0.1.0 voice session at alternate setting 6 (63 bytes) carried 1,504 OUT transfers with 94,752 bytes, exactly 63 bytes per transfer, the one-packet transfers `BTHUSB` sends for wideband voice; the check accepts those.

## Results on the device

### Install

First install on a clean system, from the release package.

| Step | Result |
|---|---|
| Starting state | DeckBtService and usbip-win2 uninstalled, then a restart. The stock radio on its stock driver. |
| First run of `install.ps1` | Downloaded usbip-win2 0.9.8.1 from its GitHub release, checked its SHA-256 and installed it silently. Blocked the stock driver, published the UART, installed the programs and registered the service. Asked for a restart (usbip-win2's installer requests one). 32.7 s. |
| After the restart | The service started by itself 8 s after boot; the radio was up 3.5 s later. |
| Second run of `install.ps1` | Confirmed the installation in 12.2 s. |
| Result | Shokz music and headset microphone both active. `miccheck` PASS at −36.1 dBFS; BLE mouse 398 reports. |
| Audio endpoints | New endpoint IDs, because usbip-win2 was reinstalled (expected). |

A first install therefore needs exactly one restart.

Script checks on the release package:

| Check | Result |
|---|---|
| Parser | Windows PowerShell 5.1 parser: 0 errors in the scripts |
| `-DryRun` | `install.ps1 -DryRun` and `uninstall.ps1 -DryRun` changed nothing (before/after snapshots) |
| Refusals | a junction in place of the data folder, a data folder pre-created by a non-administrator, and 32-bit PowerShell are refused |

### Upgrade

| Check | Result |
|---|---|
| `install.ps1` over an installed copy | exit 0 in 9.8 s |
| New service binary in place of the old | stop 0.53 s; bridge ready 3.42 s after start; log folder repaired (owner Administrators; SYSTEM and Administrators full control, Users read) |

### Uninstall, stock Bluetooth, reinstall

| Check | Result |
|---|---|
| `uninstall.ps1` | exit 0 in 12.6 s, no restart. Service and program folder removed, UART setting removed, deny entries and policy flags removed (the flags did not exist before the install), stock `QcBluetooth` bound again (problem 0). The Shokz reconnected on the stock radio, with music only. |
| `install.ps1` from stock Bluetooth | exit 0 in 22.6 s, no restart (continuing from the record of an interrupted run, see [below](#installer-blocked-while-the-stock-radio-streamed)). Radio devnode driverless, UART published, service attached. |
| After the reinstall | `miccheck` PASS at −44.4 dBFS; BLE mouse 1,622 reports; the same audio endpoint IDs as before the uninstall |

### Automatic start at boot

| Check | Result |
|---|---|
| Restart after the first install | Service started 8 s after boot; radio up 3.5 s later |
| Normal Windows restart (`shutdown /r`) | Started by the service manager 8 s after kernel boot; bridge ready 3.78 s later, attached in low-latency receive mode. Shokz music and hands-free active and the mouse connected, without the hands-free restart. |
| Shutdown side of that restart | usbip-win2's scheduled task `USBip Detach All On Reboot Or Shutdown` detached the adapter 1.1 s after the restart request. The service disconnected both links cleanly (109 ms), re-attached, and was stopped 19 s later with a clean detach. |
| Boot with usbip-win2 0.9.8.0 | Controller ready 3.46 s after service start; `BTHUSB` event 18 about 12 s after kernel boot; the mouse reconnected |

### Bluetooth Off/On

usbip-win2 0.9.8.1, Shokz playing music.

| Check | Result |
|---|---|
| Traced Off/On | Music endpoint active again without user action. A2DP `DISCOVER` → `START` each answered once. All 2,212 ACL-out, 81 control, 2,343 event and 91 ACL-in transfers succeeded; no device-descriptor reads; unlinks, missed unlinks, lost replies, ordering holds and inbound drops all 0. |
| Three further Off/On cycles | Music endpoint active 7.1–7.6 s after the On request (including about 1 s of script start-up); the same counters all 0 |
| With clean disconnect and the hands-free restart | Music active 7.1 s after On, hands-free restored; the hands-free restart was not needed; transport counters 0 |
| New service binary | Music and hands-free restored 3.1 s after On |

### Service restart and recovery

| Check | Result |
|---|---|
| Service restart | Stop 0.62 s (with clean disconnect of two links); hands-free 9.6 s and music 10.1 s after start; the hands-free restart was not needed |
| Same, before clean disconnect | Stop 0.59 s; bridge ready 3.36 s after start; headset link 10.0 s, music 11.0 s after start |
| Graceful stop/start, 3 cycles (0.9.8.0) | Stop 0.81–1.08 s; radio back 3.97–4.08 s after start, the old device's removal observed each time |
| Forced process termination, 4 in a row (0.9.8.0) | See the table below |

| Kill | New process after | Radio and all four Bluetooth enumerators ready after |
|---|---|---|
| 1 | 5.25 s | 12.42 s |
| 2 | 15.31 s | 22.36 s |
| 3 | 60.31 s | 67.45 s |
| 4 | 60.33 s | 67.36 s |

The fourth kill shows the last recovery action repeating. `sc qfailureflag` reports recovery enabled for non-crash failures as well. A forced kill in low-latency receive mode: service restarted after 6.6 s, radio back 13.2 s after the kill.

### Sleep and resume

| Check | Result |
|---|---|
| Sleep with clean disconnect and hands-free restart, one cycle | Headset link 9.0 s after resume; the service logged `hands-free: … connected 5031 ms without RFCOMM; restarted its Hands-Free device`; music and hands-free both active 15.6 s after resume, without user action |
| Microphone after that sleep, speaking | `miccheck` PASS, −38.5 dBFS RMS, 179,200 of 192,000 samples (93%) |
| Mouse during that call | 624 reports in 15 s |
| Three consecutive S3 cycles (0.9.8.0) | Power-Troubleshooter: target and effective state 4 (S3), sleep-to-wake 4.39 s, 63.22 s and 63.24 s; Kernel-Power events 42/107 match. Radio ready 3.75–3.78 s after return from `SetSuspendState`; detach and hand-back before sleep 0.36–0.40 s |
| First S3 cycle (0.9.8.0) | S3 confirmed (Kernel-Power 42/107, firmware S3 resume count 1). Detach and hand-back 0.39 s before sleep. Controller up 3.44 s after the resume notification, attached at 3.50 s; radio and enumerators ready 6.0 s after resume (VirtBthUsb: 7–8 s) |

The wake timer was armed for 30 s each cycle; actual sleep lengths differed as listed. The previous AC wake-timer policy was restored afterwards. Before the hands-free restart existed, the hands-free profile did not return after sleep: see [Hands-free profile missing after sleep](#hands-free-profile-missing-after-sleep).

### Voice and microphone

| Check | Result |
|---|---|
| Speaking, low-latency receive mode (`miccheck.py --seconds 12`) | PASS: 191,520 of 192,000 samples, −41.0 dBFS RMS; 1,853 voice-out and 1,803 voice-in transfers, none rejected |
| After sleep | PASS, −38.5 dBFS |
| After uninstall and reinstall | PASS, −44.4 dBFS |
| After the first install | PASS, −36.1 dBFS |
| First voice link on the real radio (0.9.8.0, zero-copy mode) | `Setup_Synchronous_Connection` rewritten and answered; `BTHUSB` selected alternate setting 6 with `SET_INTERFACE`; 633 SCO packets from the controller, 632 isochronous frames delivered to Windows; 556 frames to the headset; pacing lateness at most 1.25 ms. Nobody spoke, so the level was not assessed. |

All voice links above used wideband (mSBC, alternate setting 6).

### Mouse

Razer Orochi V2 over Bluetooth LE, counted with `bt-hid-watch.ps1`.

| Check | Result |
|---|---|
| During a voice call | 1,121 reports in 15 s |
| During a call after sleep | 624 reports in 15 s |
| After uninstall and reinstall | 1,622 reports |
| After the first install | 398 reports |

### Discovery and pairings

| Check | Result |
|---|---|
| Discovery | `bt-scan.ps1`: 20 LE and 3 Classic endpoints, 20 distinct unpaired devices. Three 30.1 s scans later heard 19, 18 and 17 distinct unpaired devices, Classic and LE. |
| Pairings | Pairings carry over in both directions: the Shokz reconnected by itself within 2 s of the first attach, and on the stock radio after the uninstall |

### Stop and sleep robustness

External disconnects and adversarial clients, mostly measured with usbip-win2 0.9.8.0:

| Check | Result |
|---|---|
| External detach (`usbip detach`) | Radio and all four enumerators back in 5.45 s, same service process, with eight silent handshakes open; 10 more cycles 4.20–4.31 s; in low-latency mode, 3 cycles 5.60–5.65 s |
| Abrupt TCP reset | `SetTcpEntry(DELETE_TCB)` on the service's loopback connection: radio back in 4.19 s without a service restart |
| Detach, reconnect and Off/On stress | 6 cycles including 3 Off/On: radio back 4.30–4.67 s each; no credit refusals, no lost replies, 1 ordering hold, 0 hold timeouts; no bugcheck |
| Listener under load | `usbip list` answered in 0.031 s with seven silent clients connected |
| Import from user mode | Refused with `ST_NA`; the kernel import (process 4) enumerated normally |
| A complete session | 2,371 events, 62 ACL out, 100 ACL in; no read, write, queue or credit errors; 156 unlinks with 0 missed, lost or replayed replies; 0 ordering holds |
| Resource samples | Across a TCP loss and ten re-attachments, same process: handles 120 → 118, private bytes 2,793,472 → 2,744,320, working set 7,274,496 → 7,282,688. After three sleeps: 118 handles, 2,789,376 private bytes |

The resource samples are short process snapshots. They exclude usbip-win2's kernel allocations and do not establish long-run leak freedom.

Targeted tests of the stop, fault and log paths (not part of `selftest.cmd`):

- `usbip.exe` subprocess deadline, and cancellation of a pending `attach` by a stop;
- a controller fault refuses further imports and signals recovery;
- the watchdog ends the process with one `fatal:` log line;
- 5,600 log lines from four threads across a log rotation, none lost;
- a hard-linked log file and a junction in place of the log folder are refused; the folder is repaired to owner Administrators with a protected DACL.

### Emulated device and controller bring-up

The components were first checked separately.

**Synthetic controller** (`deckbt-usbip.exe --backend stub`, attached with `usbip.exe attach`, usbip-win2 0.9.8.0):

| Check | Result |
|---|---|
| `usbip list` / `attach` | Device listed as `0cf3:6390`, class E0/01/01; attached at high speed |
| Import connection owner | Process 4 (System): usbip-win2 connects from the kernel |
| Enumeration | `USB\VID_0CF3&PID_6390\DECKBT0001` under the usbip-win2 root hub, `service=BTHUSB`, started |
| Bluetooth stack | `BTH\MS_BTHBRB`, `BTH\MS_BTHLE`, `BTH\MS_RFCOMM`, `BTH\MS_BTHPAN` started |
| `BTHPORT` initialisation | 42 HCI commands, 0 stalls; no `BTHUSB` events 3, 5, 6, 31 or 34 (only informational event 18, link-key storage) |
| EP0 requests answered by the server | `GET_DESCRIPTOR` (device, configuration, strings), `SET_CONFIGURATION`, `SET_INTERFACE`, `GET_STATUS`; no port reset, no device-qualifier request |
| Detach and re-attach | Radio removed; back with both enumerators 1.2 s after `attach` |

**Isochronous transport through usbip-win2.** Measured with VirtBthUsb's WinUSB harness against its vendor-class test device (the same interface-1 geometry as the radio), served over USB/IP:

- All 432 transfer cells (alternate settings 1–6, packet counts 1–64, both directions, legal and illegal geometries) were classified as in VirtBthUsb's UdeCx measurement: 216 legal cells accepted, 216 illegal cells rejected with the same errors.
- Byte counts matched: 30,772 bytes in, 77,437 bytes out.
- usbip-win2 answers `QueryBusTime` itself (219 calls, status 0), so the isochronous path needs no frame-clock filter.
- Cancellation: a held IN transfer reached the server as `USBIP_CMD_UNLINK`, was answered `-ECONNRESET`, and the next transfer completed.
- usbip-win2 derives each OUT packet's length from the gap to the next packet's offset (`repack` in its `device_ioctl.cpp`).

**Controller from user mode** (`deckbt-uartprobe.exe`): CTS asserted without a wake pulse. The version request at 115200 baud was answered in 60 ms with `04 0E 12 01 00 FC 00 19 0C 13 00 00 00 E6 38 01 02 11 12 0C 40` (SoC `0x400C1211`, product `0x13`, patch `0x38E6`, ROM `0x0201`), byte-identical to the reply recorded from the stock driver's initialisation.

**Real radio** (`--backend uart`, usbip-win2 0.9.8.0):

| Check | Result |
|---|---|
| Firmware | Read from Valve's installed package (`qcbtuart.inf_amd64_*` in the DriverStore): rampatch 155,044 bytes, board `0x0309`, NVM `hpnv21g.309` |
| Bring-up | ROM at 115200, 3,000,000 baud, rampatch, NVM, `HCI_Reset` complete 3.46 s after start; the first in-band-sleep wake acknowledged |
| Windows stack | `BTHUSB` and all four Bluetooth enumerators started; `BTHPORT` initialised the controller (event 18 only) |
| In-band sleep | 92 controller wake indications, 92 acknowledged, none skipped for CTS low |
| Stop | Detach, then the controller was reset and answered at 115200 (ROM) |

## Issues found and resolved

### Headset audio lost after Bluetooth Off/On

**Symptom.** With music playing on the Shokz, switching Bluetooth off and on reconnected the headset's link (and the mouse), but its music endpoint stayed unplugged (DeviceState 8) until the headset was reconnected by hand. Reproduced on the first traced attempt: link up 7.8 s after On, endpoint still unplugged 37 s later.

**Cause.** usbip-win2 0.9.8.0 rejects an OUT transfer whose URB carries a bare `TransferBuffer` when it is submitted above `APC_LEVEL` (`drivers/ude/network.cpp`, `make_transfer_buffer_mdl`, `STATUS_MUTANT_NOT_OWNED`; upstream [issue #190](https://github.com/vadimgrn/usbip-win2/issues/190), fixed in 0.9.8.1). BTHPORT sends some L2CAP and AVDTP signalling and a few HCI commands that way. The ETW traces show the chain:

1. The URB fails in about 0.1 ms without reaching the server (UCX: `STATUS_INVALID_PARAMETER` / `USBD_STATUS_INVALID_PARAMETER`).
2. BTHUSB treats it as a device fault and asks for a reset. USBHUB3 logs a client request and a re-enumeration and re-reads the device descriptor: a 64-byte and an 18-byte `GET_DESCRIPTOR(device)` pair about every 77 ms during connection setup.
3. UDE purges every endpoint. Parked event and ACL reads are unlinked, and ACL writes the server had already passed to the controller complete as cancelled.
4. BTHUSB re-submits the same write IRPs with the same buffers after each reset. UCX shows one 16-byte write dispatched three times, cancelled each time. Every copy reaches the controller.
5. The headset receives signalling two to five times. It refuses duplicated L2CAP connection requests (result 7, source CID already allocated), re-configures the AVDTP channel after a duplicated configure request, and answers a duplicated AVDTP `OPEN` with `BAD_STATE`. BTHPORT then disconnects the AVDTP signalling channel; its retry is refused (result 4), so A2DP never opens.

During the reproduced toggle the service counted 42 missed unlinks, of which 16 were event or ACL replies and 26 were writes the controller had already accepted. UCX recorded 26 ACL-out and 3 control transfers failing with `INVALID_PARAMETER` and 25 ACL-out writes cancelled; USBHUB3 recorded 29 client requests. Traces of earlier successful reconnects contain the same duplicated responses; those recoveries succeeded despite them.

**Fix.** usbip-win2 0.9.8.1, which `install.ps1` installs and requires. The service's attach options and its parsing of `usbip` output are unchanged. Upgrading usbip-win2 replaces the emulated host controller, so the adapter's Bluetooth devices and audio endpoints are created again: pairings remain, but Windows assigns new audio endpoint IDs, and a saved default output device may need selecting again. Uninstalling 0.9.8.0 hung in its host-controller teardown when the device had been attached and detached in the same boot; after a restart, the upgrade completed while nothing was attached.

**Evidence.** The [Bluetooth Off/On](#bluetooth-offon) results: music returned by itself every time, with no failed transfers, device-descriptor reads or unlinks.

### Bugcheck 0x4E in usbip-win2 zero-copy receive

**Symptom.** During normal use the Deck bugchecked with `PFN_LIST_CORRUPT` (0x4E), arguments `0x9A`, PFN `0x1bcc77`, state 6 (active), reference count 2. Subcode `0x9A` means a driver freed a page that was still locked for I/O. The triage minidump's stack was zeroed, so it names no module. The service log shows steady state at the time: an authenticated ACL connection, then repeated `GET_DESCRIPTOR(device)` pairs (the resets described above). No detach, kill or sleep was in progress.

**Cause.** Code reading of usbip-win2 0.9.8.0 shows a mechanism that matches these arguments; the same code is unchanged in 0.9.8.1:

- `drivers/ude/wsk_receive_irp.cpp`, `recv_loop`: in the default zero-copy receive mode, `prepare_wsk_mdl` → `make_transfer_buffer_mdl` locks a URB's bare `TransferBuffer` with `MmProbeAndLockPages` and receives the payload into it.
- `recv_loop` then queues the URB's completion (`enqueue_for_completion` → `WdfDpcEnqueue`).
- The pages are unlocked only when the next header is received (`recv_usbip_header` → `ctx.mdl_buf.reset()`).
- The completion DPC can run first, and the upper driver can free the buffer before the unlock. A page freed while locked is exactly 0x4E/0x9A.
- The send path is not affected: it unlocks in `send_complete` before completion is published.

The dump cannot confirm this call path. The attribution rests on two facts: usbip-win2's were the only third-party kernel modules loaded, and this is the only completion-before-unlock ordering found in them.

**Fix.** The service attaches with `--receive-mode low-latency`. In that mode WSK event callbacks copy from usbip-win2's own ring buffer into the URB (`wsk_receive_events.cpp`, `ret_submit_urb`, `fill_isoc_data`), and no URB page is locked on the receive path. Upstream recommends this mode for small, high-frequency transfers. One behaviour differs: a reply the client rejects (for example an isochronous descriptor whose offset differs from the request) tears down the whole connection instead of failing one URB. The service then re-attaches.

**Evidence.** `usbip port` reports `mode: low-latency` after service start, after each external detach and after a forced kill. External detach, forced kill, Off/On and discovery all recovered in this mode ([Stop and sleep robustness](#stop-and-sleep-robustness)), and every voice and microphone result on usbip-win2 0.9.8.1 was measured in it.

### Bugcheck 0x7E in BTHport

**Symptom.** During an external-detach test, about 2 s after `Create_Connection` for a reconnecting Classic device, the Deck bugchecked with `SYSTEM_THREAD_EXCEPTION_NOT_HANDLED` (0x7E, access violation). The triage dump kept the faulting thread:

```
BTHport!L2CapCon_CheckRemoteConfigRequestForInvalidParameters+0x3f   mov r9,[rax+0B8h]   rax = 0
BTHport!L2CapInt_L2capCompleteRemoteConfigRequest+0xa3
BTHport!L2CA_ReconfigRsp+0xc1
BTHport!L2CapCon_ProcessNextSignalRequest+0x1aa
BTHport!L2CapInt_HandleSignalRequest+0x1bd
BTHport!BthDispatchBrb+0x337
BTHport!HCI_ThreadFunction+0x3ab
```

The first instruction of the function loads the L2CAP channel's field at `+0x3A0`, its ACL link object, which was NULL. Windows' Bluetooth driver then dereferenced it while handling a remote L2CAP configuration request.

**Cause.** Not established. The dump holds no HCI history, so it does not show why the link was NULL. Two server-side behaviours could produce packet sequences Windows does not expect:

1. **Replies lost to unlink races.** usbip-win2 completes a transfer as cancelled the moment it sends `CMD_UNLINK`, and drops any `RET_SUBMIT` that crossed it (`device_ioctl.cpp`, `send_cmd_unlink_and_complete`; `request_list.cpp`, `cancel`). An event or ACL packet the service had already sent for such a transfer never reached Windows. Earlier sessions logged 10–81 such unlinks, in bursts during Classic connection setup. The device stayed in D0 throughout (`DEVPKEY_Device_PowerData` polled every 25 ms), so selective suspend was not the source.
2. **Cross-endpoint reordering.** Events use interrupt endpoint 0x81 and ACL uses bulk endpoint 0x82. When one endpoint has no read waiting, a later packet on the other can overtake it: for example, `Disconnection_Complete` could reach Windows before the L2CAP Configure Request that preceded it. This is a possible contributor, not a sequence reconstructed from the dump.

A third link is also possible but unproven: the duplicated signalling caused by usbip-win2 issue #190 (above) includes duplicated Configure Requests, which drive exactly the remote-reconfiguration path in this stack.

**Fix (mitigations).**

- Event and ACL replies are kept in a 32-packet history per stream. An unlink that arrives after its reply was sent marks that packet lost, and it is delivered again ahead of newer queued data.
- The bridge stamps every event and ACL packet from one arrival counter. The device model sends the older packet first across the two endpoints. A packet waiting for a read on its endpoint holds the other endpoint for at most 20 ms. Delivering the blocking packet clears the hold, so each new blockage gets its own interval.
- Replay updates the retained packet's request number in place, so repeated cancellation keeps the original order within a stream.

**Evidence and remaining risk.** Regression tests cover replay order, the hold bound and the hold reset (`usbip_device_selftest`, and the redelivery socket test, which fails without the replay). Six detach, reconnect and Off/On cycles ran without a bugcheck, lost reply or hold timeout. With usbip-win2 0.9.8.1 the unlink bursts are gone: no unlinks were counted across the Off/On and restart tests, and a 23-minute streaming session recorded 0 unlinks, 0 ordering holds and 0 credit refusals. That shows the trigger is gone, not that the races are handled. The mitigations are bounded: the 20 ms hold still allows reordering after it expires, the history is finite, and replay cannot undo a newer packet Windows has already consumed. A crash-free run is not proof that this failure cannot recur.

### Hands-free profile missing after sleep

**Symptom.** After every sleep the Shokz music output returned only after 26–30 s, and its hands-free (microphone) endpoint never returned without a manual reconnect. The radio was back and attached 3.5 s after resume, and the headset link after 4.5–9.5 s. Bluetooth Off/On and service restarts restored both profiles every time.

**Ruled out.**

- *Link loss seen by the headset.* The service used to detach the adapter without disconnecting links, and the headset then reconnected by itself with A2DP only. With a clean disconnect before the detach, Windows initiated the reconnect after two sleeps of about 75–90 s, and hands-free still stayed down.
- *The transport.* The traces show every request answered once and every transfer successful; the service's unlink, ordering and drop counters stayed at 0.
- *BTAGService* (Bluetooth Audio Gateway Service): polled every 0.5 s through a sleep, it kept running in the same process. Restarting it did not open hands-free.
- *The lock screen.* Windows locks the session on sleep, but re-plugging the adapter while locked restored both profiles.

**Cause.** After resume Windows opened AVDTP and AVCTP on the new link but never sent an L2CAP connection request for RFCOMM, the channel the hands-free profile runs on. After an Off/On or a restart it sends one within about 0.1 s of the link. The music endpoint's 20 s delay contains no Bluetooth traffic and ends when Windows gives up on hands-free. Timing after resume decides the outcome: an adapter arriving 3.5 s after resume lost hands-free, while re-plugging it 25 s after the same resume restored both profiles. Restarting only the headset's Hands-Free AG device (`BTHENUM\{0000111E-…}`, with `pnputil /restart-device` or `DIF_PROPERTYCHANGE`) 13.8 s after resume opened hands-free 1.1 s later, and the music endpoint became active at the same moment; nothing else was interrupted. Removing and re-readying the devnode through cfgmgr32 instead left it in `CM_PROB_WILL_BE_REMOVED`.

**Fix.**

- *Clean disconnect.* Before any deliberate stop the bridge disconnects each open ACL link (reason 0x15, as BTHPORT does on Bluetooth Off). Measured 94–453 ms for two links (headset and mouse).
- *Hands-free restart.* A BR/EDR link up 5 s without an RFCOMM connection request gets its peer's Hands-Free AG device restarted once. See [ARCHITECTURE.md](ARCHITECTURE.md).

**Evidence.** [Sleep and resume](#sleep-and-resume): music and hands-free both active 15.6 s after resume without user action, and a microphone PASS after that sleep. After a service restart, an Off/On and a normal Windows restart, Windows opened RFCOMM itself and the service did not intervene.

### Installer blocked while the stock radio streamed

**Symptom.** Running `install.ps1` from stock Bluetooth while a headset was playing through the stock driver blocked for 16 minutes in `pnputil /remove-device` on the stock radio's device.

**Cause.** Removing the stock radio's devnode while it carries an audio stream blocks inside Plug and Play until the stream ends.

**Fix.** The installer first switches Bluetooth off through the Windows radio API, as the Settings toggle does, which closes every link and audio stream. The removal is bounded to 90 s; if it still does not finish, the installer asks for a restart and retries the removal after it. The change record lets a re-run continue an interrupted install.

**Evidence.** The next run, from the record of the interrupted one, finished with exit 0 in 22.6 s and no restart ([Uninstall, stock Bluetooth, reinstall](#uninstall-stock-bluetooth-reinstall)).

### Other defects fixed

Also fixed before release:

- **ACL credit accounting.** The first voice session had 3 ACL packets refused for lack of controller credit; Off/On sessions had 3–9. Windows resets the controller when Bluetooth is switched off, often with ACL packets in flight, and the bridge kept counting those as outstanding. It also ignored the controller's separate LE buffer pool (7 shared + 16 LE buffers on this controller). The pool is now `Read_Buffer_Size` plus `LE_Read_Buffer_Size`, refilled by a successful `HCI_Reset`. The later stress sessions and the 23-minute streaming session counted no refusals.
- **Stop could hang.** On Windows `shutdown()` sends FIN but does not wake a blocked `recv`, so an importer that never closed its end held up the service stop. Detach now waits 3 s, then cancels the session read.
- **Recovery ended after two crashes.** A two-action recovery policy left the service stopped after a third crash; the last action now repeats every 60 s.
- **External disconnect left Bluetooth absent.** An external detach or lost connection now resets the controller and re-attaches.
- **Silent clients blocked the listener**, and a user-mode import was accepted while no radio was imported. Handshakes now have fixed slots and an absolute deadline; user-mode imports are refused (only isolated console testing can allow them).
- **Socket inheritance.** A spawned `usbip.exe` inherited the service's sockets and kept the port occupied after the service died. Child processes now inherit only their output pipe.
- **Rejected isochronous replies** advertised descriptors that were never sent (found by fuzzing); they now advertise zero.
- **Stop and suspend hardening:** a spent fault signal could restart a healthy radio; a suspend could wait out a 60 s `attach`, past the 45 s watchdog; log rotation retried a blocked rename on every line; console lines ended in CR CR LF.
- **Installer and log folder.** The folder handle did not pin the log folder, because share modes need a data right. A security review of the scripts found eight more issues: a data folder pre-created by a non-administrator or replaced by a junction; a gap between hashing and installing the release files; deny-policy flags not restored when other entries remained; an entry written before it was recorded; non-atomic state writes; `sc delete` exit code 1072 (service marked for deletion) not handled; 32-bit PowerShell; the zip layout and privacy of the diagnostics archive.

## Inherited from VirtBthUsb

These results were measured with the [VirtBthUsb](https://github.com/ethanm0202/VirtBthUsb/blob/v1.0.1/docs/VERIFICATION.md) kernel driver, which drives the same controller through the same Windows Bluetooth stack. They concern the controller and the stack, not the kernel code, and are not re-measured here:

- Voice: mSBC in both directions, 2,530 of 2,530 and 2,607 of 2,607 packets delivered, `miccheck` PASS on AirPods Pro and Shokz OpenMeet, 23 Discord voice links without loss.
- 62 S3 resumes, with firmware reloaded and pairings present after each.
- Pairing, encryption and BLE input on the real controller.

## Not yet tested

- Other Steam Deck OLED units and other Windows builds
- Overnight sleep, calls across sleep, and more than one sleep cycle with the hands-free restart
- Hibernate and Fast Startup
- Long calls
- Narrowband voice (CVSD, alternate settings 1–5)
- Prevention of the recorded live UART wedge, and a demonstrated recovery without restarting Windows
- The installer upgrading an existing usbip-win2 0.9.8.0
- Battery impact
- Secure Boot on, Memory Integrity (HVCI) on, and sustained gameplay with kernel anti-cheat
