# Roadmap

What DeckBtService does as of v0.1.3, what comes next, and how it differs from the stock Bluetooth driver. Test results are in [VERIFICATION.md](VERIFICATION.md).

## Done in v0.1.0

- [x] Headset microphones: Windows' inbox USB Bluetooth drivers (`BTHUSB`/`BTHPORT`) run the Deck's QCA2066 through a user-mode USB/IP server and the Microsoft-signed usbip-win2 client. Wideband (mSBC) voice flows both ways; no kernel driver of its own and no test signing.
- [x] Controller bring-up from user mode: the UART published through SerCx2, firmware read from the installed vendor package, in-band sleep.
- [x] HCI bridge: commands, events, ACL data with credit accounting (shared and LE buffer pools, refilled on `HCI_Reset`), voice routed over HCI, isochronous pacing and re-framing.
- [x] Windows service: starts at boot, restarts after a failure (5 s, 15 s, then 60 s), bounded stop and suspend with a watchdog, recovery from an external detach, a lost connection or a UART fault.
- [x] Sleep: links disconnected cleanly before suspend; controller brought up and re-attached after resume; a headset whose hands-free profile Windows skips after resume gets its Hands-Free device restarted ([VERIFICATION.md](VERIFICATION.md#hands-free-profile-missing-after-sleep)).
- [x] Headset music and microphone return after Bluetooth Off/On and a service restart, with usbip-win2 0.9.8.1 ([VERIFICATION.md](VERIFICATION.md#headset-audio-lost-after-bluetooth-offon)).
- [x] Transport mitigations: usbip-win2 `low-latency` receive mode ([0x4E bugcheck](VERIFICATION.md#bugcheck-0x4e-in-usbip-win2-zero-copy-receive)); bounded cross-endpoint ordering and in-order replay of replies lost to unlink races ([0x7E bugcheck](VERIFICATION.md#bugcheck-0x7e-in-bthport)). These mitigate the recorded crashes; they do not establish that the causes are gone.
- [x] Service security: USB/IP imports accepted only from the kernel client (PID 4), bounded handshakes, installation only from Program Files, an admin-owned log folder with rotation.
- [x] Release zip with double-click launchers for install, uninstall and diagnostics: OLED-only hardware check, hash-checked usbip-win2 0.9.8.1, stock transport blocked by a deny policy, UART publication, a change record, and an uninstall that restores stock Bluetooth. A first install on a clean system needs one restart; uninstall, reinstall from stock and upgrade need none ([VERIFICATION.md](VERIFICATION.md#install)).
- [x] Tests: 12 host-side suites and the reference check, socket regressions against the console server, and two libFuzzer/AddressSanitizer targets.

## Done in v0.1.1

- [x] Fixes from a static review ([CHANGELOG.md](../CHANGELOG.md)): a controller that never acknowledges the host in-band-sleep wake now fails the start instead of being reported ready; the voice pacing timer is required at start and a failed timer arm no longer spins; isochronous packets longer than the alternate setting's `wMaxPacketSize` are rejected; a refused import's bus ID is escaped in the log; the installer refuses, and the uninstaller leaves alone, a `DeckBtService` service that runs another program.
- [x] Tests: a 13th suite runs the real controller backend against a simulated controller; new regressions for each fix.

## Done in v0.1.2

- [x] Audio under load: the pacing, USB/IP session and UART threads run in the MMCSS "Pro Audio" class. Opening apps made music stutter and calls crackle (voice pacing up to 112 ms late); now pacing stays within 2 ms and music did not stutter ([VERIFICATION.md](VERIFICATION.md#v012)).

## Done in v0.1.3

- [x] Release engineering: GitHub Actions builds and tests every push; release zips are built there from the tag and carry signed build-provenance attestations (`gh attestation verify`).

## Next

- [ ] **Longer runs**: overnight sleep, calls that span a sleep, long calls, and long-run input quality. Connected devices are not proof of audio or input quality.
- [ ] **Other Decks and Windows builds**: only one Deck OLED on Windows 11 25H2 has been tested.
- [ ] **Memory Integrity, Secure Boot and games with kernel anti-cheat**, each verified separately.
- [ ] **Narrowband voice** (CVSD, alternate settings 1-5): implemented, not tested.
- [ ] **Stock parity beyond the microphone**: a controlled reconnect and new-pairing matrix, game controllers and other HID input, A2DP/AVRCP, several devices at once, GATT/RFCOMM/PAN, range and Wi-Fi coexistence, and latency and battery measurements against the stock driver. See [Differences from stock Bluetooth](#differences-from-stock-bluetooth).
- [ ] **Transport correctness under cancellation**: the bounded-replay limitations stay open. The unlink storms seen with usbip-win2 0.9.8.0 were its issue #190 resets; on 0.9.8.1 no unlinks were counted in the reconnect tests, which shows the trigger is gone, not that the remaining races are handled.
- [ ] **Real controller faults**: the UART fault path is covered by isolated tests only. A real fault, such as the UART controller being reset by a driver update, has not been observed.
- [ ] **Host in-band sleep**: the host never sends `SLEEP_IND`, so the controller sees it awake for the whole session ([QCA2066.md](QCA2066.md#in-band-sleep)). Measure the battery cost against the stock driver before changing it.
- [ ] **Code signing**: the programs are not Authenticode-signed, so Windows warns about an unknown publisher. Since v0.1.3 their origin is shown by GitHub build-provenance attestations instead.

## Open questions

- **The hands-free restart works around Windows behaviour.** It restarts a headset's Hands-Free AG device when no RFCOMM connection follows a new link within 5 s. A headset that opens RFCOMM later than that would have its hands-free profile restarted once; none has been observed.
- **Report upstream**: usbip-win2's zero-copy receive mode completes URBs before unlocking their pages; BTHPORT dereferences a NULL link when an L2CAP Configure Request arrives after the link's teardown. The dropped-write defect is already fixed upstream (usbip-win2 issue #190, fixed in 0.9.8.1).

## Differences from stock Bluetooth

Apart from the microphone, the aim is behaviour equivalent to the stock driver; that is not yet a verified property. Using the same Windows upper stack does not show that a different transport has the same reliability, latency or power use.

| Area | What changes, and the current evidence |
|---|---|
| Radio and firmware | Same QCA2066, antenna and installed Qualcomm firmware, and the same 3 Mbaud UART configuration as the vendor driver. No added range or bandwidth. Range, interference and Wi-Fi coexistence have not been compared with stock. |
| Windows transport and identity | `qcbtuart.sys` and `BthMini.sys` are replaced by `BTHUSB.SYS`, usbip-win2 and a LocalSystem service. Windows sees a Generic Bluetooth Adapter with a virtual USB identity, not the original ACPI radio. Audio endpoints get new IDs when the adapter's devices are re-created, for example when usbip-win2 is reinstalled or upgraded, so saved per-app device selections may need to be made again. |
| Bluetooth profiles | Windows still supplies the upper stack. Classic and LE discovery, existing-pairing reconnects, BLE mouse input, A2DP playback and hands-free voice have been exercised. Codec selection, AVRCP media controls, GATT applications, RFCOMM and PAN data transfer are not validated; drivers enumerating is not a functional test. LE Audio support is not established. |
| Pairing and security | Existing pairings carry over; Windows and the controller still perform Bluetooth authentication and encryption. New pairing, removal and re-pairing, reconnect after a peripheral sleeps or goes out of range, and several simultaneous devices have not been tested as a matrix. |
| Startup and recovery | Bluetooth depends on the service, the published UART, the vendor firmware package and usbip-win2. Bring-up takes about 3.5 s, plus enumeration and reconnect time. Service or transport recovery removes the adapter for a few seconds and disconnects devices. There is no fallback to the stock radio while installed. |
| Suspend and wake | S3 is handled by a clean disconnect of every link, then detach, firmware reload and re-attach. Music and microphone were back 15.6 s after waking in the measured cycle. Overnight sleep, hibernate and Fast Startup have not been tested. USB remote wake is not implemented; whether the stock driver supports wake-by-Bluetooth on the Deck has not been established either. |
| Power and performance | Extra user/kernel transitions, loopback TCP, queues and a background process replace the direct vendor transport. The service process used about 6.9 MiB working set and 2.7 MiB private memory, excluding driver memory. Battery use, input and audio latency, throughput and behaviour under gaming load have not been measured against stock. |
| Cancellation and ordering | A 32-packet history per input stream covers replies lost to usbip-win2 unlink races. Cross-endpoint ordering waits at most 20 ms, after which a later packet may overtake. A late cancellation cannot retract a newer packet Windows has already consumed. These mitigations do not prove lossless, globally ordered delivery. |
| Fault handling | Process and TCP-loss recovery have been exercised. A UART failure (20 failed reads in a row, or a failed write) restarts the radio; that path is exercised only by tests. Every stop and sleep wait is bounded; a thread that cannot be stopped ends the process for service recovery. Finite queues and an eight-handle ACL accounting table are implementation limits. |
| Installation and updates | The installer blocks the stock transport with a device-installation deny policy and publishes the UART through a registry value Microsoft documents for development; the uninstaller reverts both and restores the stock driver. The service loads firmware from the newest installed `qcbtuart.inf_amd64_*` package, so removing or changing Valve's Bluetooth driver package affects it. usbip-win2 must be 0.9.8.1 or later. Behaviour across Windows and driver updates is untested. |
| Trust and games | No test signing, and the kernel driver is Microsoft-signed, but it adds a third-party kernel driver and a privileged loopback service. Accepting only PID-4 imports identifies kernel traffic, not a particular driver. Secure Boot, Memory Integrity and individual anti-cheat games are untested. |

Two bugchecks were recorded on the test Deck before v0.1.0. Their mitigations and remaining uncertainty are described in [VERIFICATION.md](VERIFICATION.md); short successful runs are not proof that they cannot recur.
