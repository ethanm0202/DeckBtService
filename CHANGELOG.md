# Changelog

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
