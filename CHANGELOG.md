# Changelog

## 0.1.4 (2026-09-30)

**UART fault handling.** Before a steady-state data packet or in-band-sleep acknowledgement, the writer checks CTS. If it is low, the writer waits passively for up to the 1,500 ms readiness deadline, without changing RTS or the UART's automatic flow control. A failed wait no longer discards a pending acknowledgement. Pending acknowledgements go before the next data packet, never inside one.

A failed or partial write now ends the steady writer instead of sending the remaining queue into a possibly incomplete H4 packet. New submissions are refused after the fault. The log records the failed packet's type and length, the driver's completion count, and repeated wake indications while an acknowledgement is pending.

**Unresponsive controller.** After three consecutive starts where physical wake fails and CTS is still low, automatic retries pause. The service logs `controller unresponsive: restart Windows to recover Bluetooth` and stays available for an explicit stop, rather than causing an endless Windows service-recovery loop. Other startup failures retain their normal retry policy.

**HCI bridge & SCO stream hygiene.** A vendor-specific command completion (OGF 0x3F) reaches the host and retires a host command only if the host sent a vendor command with that exact opcode; otherwise it is treated as late bring-up chatter and dropped without touching `HostCommandsPending`, so it can neither complete the wrong command nor release a shutdown disconnect while a real host command is in flight. Outbound and inbound SCO FIFOs are flushed when a voice stream ends, including through the production QCA transport. A disconnect removes only that handle's queued SCO packets. When the UART writer frees a transmit slot it retries SCO packets that were waiting for one.

**Service registration & Unicode CLI.** Switched the service and UART probe entry points to native `wmain` and wide-character argument parsing, eliminating ANSI code-page (`CP_ACP`) path mangling for paths containing non-ASCII characters. Service registration (`deckbt-usbip install`) now pre-computes exact required argument lengths, quotes and escapes per Microsoft CRT rules (doubling trailing backslashes and backslashes before quotes), bounds-checks against the 32,767-character Windows SCM limit, and fails explicitly rather than silently truncating configuration lines.

**Limits of this change.** One v0.1.3 session lost Bluetooth and needed a Windows restart. Its initiating cause is still unknown. These changes fix demonstrated writer defects; they are not proof that the hardware failure cannot recur. CTS can fall after a pre-write check, and no non-reboot recovery for the recorded wedge has been demonstrated.

**Tests.** Backend regressions cover temporary low CTS before data or an acknowledgement, acknowledgement priority after flow control clears, stopping during the wait, partial writes, and a controller that never raises CTS. Three cases failed on the original backend and passed with the first fix. A stricter flow-control fixture then exposed eight failed assertions in an initial RTS-pulsing approach; all pass with passive waiting. A new lifecycle suite exercises bounded retries, a live but unavailable loopback server, ordinary startup errors, and stopping during recovery.

## 0.1.3 (2026-09-29)

Same source code as 0.1.2; the programs are now compiled on GitHub's Windows runner (Visual Studio 2026, MSVC 14.51) instead of locally with the EWDK.

**Verifiable builds.** Release zips are now built by GitHub Actions from the tagged commit, on a GitHub-hosted Windows runner, after the build, the host test suites (except the five that need Valve's firmware), the socket regressions and a fuzz run pass. The zip and both programs carry signed build-provenance attestations. Check a download with `gh attestation verify DeckBtService-0.1.3.zip --repo ethanm0202/DeckBtService`. Every push to `main` runs the same build and tests.

Upgrading: extract the new zip and run its `install.cmd`; no restart is needed.

## 0.1.2 (2026-09-29)

**Audio under load.** Opening an app or any other burst of CPU load made music on a Bluetooth headset stutter and calls crackle. The service's audio threads (voice pacing, the USB/IP session, the UART reader and writer) ran at normal priority, and the voice pacing was measured running up to 112 ms late. They now join Windows' Multimedia Class Scheduler (MMCSS) "Pro Audio" task, as Windows' own audio engine does. Under the same load, pacing stayed within 1.8 ms, the microphone delivered 98% of its audio instead of 85%, and music did not stutter. A thread that MMCSS refuses logs it and keeps running at normal priority.

Upgrading: extract the new zip and run its `install.cmd`; no restart is needed.

## 0.1.1 (2026-09-29)

Fixes for the findings of a static code and security review of v0.1.0. None was a memory-safety or privilege-escalation defect. Upgrading: extract the new zip and run its `install.cmd`; no restart is needed.

**Controller start**

- A controller that never answered the host's in-band-sleep wake (`WAKE_IND`, up to 10 tries 100 ms apart) was still reported ready, so Windows could start using a radio that would not respond. The start now fails with `ERROR_TIMEOUT`, the controller is handed back to ROM, and the service's normal start retry takes over. On the test Deck the first wake is always acknowledged, so a healthy start is unchanged.

**Service health**

- The pacing thread, which completes voice transfers and ends cross-endpoint ordering holds, created its high-resolution timer itself and exited if that failed while the service kept running. The timer is now created before the server starts, and a server without it does not start (the service exits and Windows restarts it).
- If arming the timer failed, the pacing loop could spin. It now logs once and waits in whole milliseconds until the timer works again.
- If the pacing thread could not be created, the accept thread was left running unjoined. Thread creation failures now stop the start cleanly.

**USB/IP**

- Isochronous (voice) transfers whose packet descriptors claim more bytes than the selected alternate setting's `wMaxPacketSize` are rejected with `-EINVAL`, as a real USB endpoint cannot carry them. Windows' USB driver stack does not permit such packets, and the wideband transfers recorded from Windows' Bluetooth driver are one 63-byte packet each. The USB/IP fuzz target now checks this invariant.
- A local program could put line breaks into the bus ID of a refused import request and make the service log show forged lines. Client-supplied bus IDs are now logged with control characters, `\` and `'` escaped as `\xHH`.

**Installer**

- If a service named `DeckBtService` already existed and ran another program, `install.ps1` deleted it and registered its own, and `uninstall.ps1` could not restore it. `install.ps1` now refuses to install before changing anything, and `uninstall.ps1` removes the service only when it runs `%ProgramFiles%\DeckBtService\deckbt-usbip.exe`.

**Documentation**

- The host never sends the in-band-sleep `SLEEP_IND`, so the controller sees the host awake for the whole session; this and its unmeasured battery cost are now documented ([docs/QCA2066.md](docs/QCA2066.md#in-band-sleep)).

**Tests**

- New `qca_backend_selftest`: the real controller backend against a simulated QCA2066 behind a mocked UART, with the host wake acknowledged on the first or third try or never, and a restart after a failed start.
- New regressions in `usbip_device_selftest` (oversize isochronous packets, both directions) and `tools/usbip_selftest.py` (forged log lines). Each fails on v0.1.0 and passes on v0.1.1.

## 0.1.0 (2026-09-28)

First public release. See [docs/VERIFICATION.md](docs/VERIFICATION.md) for what was tested.
