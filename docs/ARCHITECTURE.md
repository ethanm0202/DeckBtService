# Architecture

DeckBtService is a user-mode program that presents the Steam Deck OLED's Bluetooth controller (a Qualcomm QCA2066 on a UART) to Windows as a USB Bluetooth adapter. It serves an emulated USB device over USB/IP on the loopback interface; the [usbip-win2](https://github.com/vadimgrn/usbip-win2) client attaches it, and Windows binds its inbox USB Bluetooth driver (`BTHUSB.SYS`) and Bluetooth stack (`BTHPORT.SYS`) to it. Behind the device, the program drives the controller over its UART: firmware loading, H4 framing, Qualcomm in-band sleep, and the HCI adjustments this controller needs. It contains no kernel code.

## Why a virtual USB device

The stock stack (`qcbtuart.sys` on `ACPI\QCOM2066`, then the BthX miniport `BthMini.sys`) expects synchronous voice (SCO/eSCO) to leave the HCI transport through a hardware path ("HCI bypass", also called offload), where audio goes from the controller to an audio device without passing through Windows. Whether the Deck's hardware provides such a path has not been established; in Windows it gives no working microphone. `BthMini.sys` fails any other SCO mode with `STATUS_DEVICE_CONFIGURATION_ERROR` (`0xC0000182`), so Hands-Free devices enumerate in offload mode (`_HCIBYPASS_`) and headset microphones do not work. Music (A2DP) is unaffected because it travels as ordinary data.

`BTHUSB.SYS`, the transport for USB Bluetooth adapters, carries voice in-band over isochronous endpoints. Through it, Hands-Free devices enumerate as ordinary `{0000111E-...}` devices with `BthHFEnum`/`BthHFAud`, and the headset microphone is a normal recording device. `BTHUSB.SYS` binds only to USB devices, so the controller has to appear as one.

Windows runs one Bluetooth radio at a time (`BTHUSB` event 6, "Only one active Bluetooth adapter is supported at a time"). The stock radio must therefore be off while DeckBtService runs, and it must not hold the UART.

## Why USB/IP and usbip-win2

A virtual USB device on Windows needs a kernel driver that emulates a USB host controller. VirtBthUsb, the predecessor of this project, wrote its own (KMDF with UdeCx, the USB Device Emulation class extension), which requires Windows test signing. usbip-win2 is an existing USB/IP client with Microsoft-signed drivers, also built on UDE: it plugs a device into Windows for every USB/IP import. DeckBtService is the USB/IP server for one emulated device, so all kernel code involved is Microsoft's or usbip-win2's, and none of it is modified.

## Overview

```
┌──────────────────────────────────────────────────────────────────┐
│ Windows Bluetooth stack (BTHPORT.SYS) and profile drivers        │
└─────────────────────────────────┬────────────────────────────────┘
┌─────────────────────────────────▼────────────────────────────────┐
│ Inbox USB Bluetooth transport (BTHUSB.SYS)                       │
│   EP0 control ............ HCI commands                          │
│   0x81 interrupt IN ...... HCI events                            │
│   0x02 / 0x82 bulk ....... ACL data                              │
│   0x03 / 0x83 isochronous  SCO voice, 1 ms frames                │
└─────────────────────────────────┬────────────────────────────────┘
                                  │ emulated device USB\VID_0CF3&PID_6390
┌─────────────────────────────────▼────────────────────────────────┐
│ usbip-win2, kernel mode (Microsoft-signed)                       │
│   root hub: USBHUB3.SYS + usbip2_filter.sys                      │
│   UDE host controller: usbip2_ude.sys                            │
└─────────────────────────────────┬────────────────────────────────┘
                                  │ USB/IP over loopback TCP, 127.0.0.1:3241
                                  │ (the connection is owned by process 4, System)
┌─────────────────────────────────▼────────────────────────────────┐
│ deckbt-usbip.exe, user mode (service DeckBtService, LocalSystem) │
│   USB/IP server: import check, session, unlinks, recovery        │
│   USB device model: descriptors, EP0, parked IN transfers,       │
│     arrival order across endpoints, SCO pacing timer             │
│   HCI bridge: readiness, ACL credits, voice routing,             │
│     clean disconnect, RFCOMM tracking for hands-free             │
│   UART backend: firmware bring-up, H4 framing, in-band sleep     │
└─────────────────────────────────┬────────────────────────────────┘
                                  │ H4 over the SerCx2 COM port interface,
                                  │ 3,000,000 baud, RTS/CTS
┌─────────────────────────────────▼────────────────────────────────┐
│ AMD UART ACPI\AMDI0020\4 (amduart.sys, SerCx2), kernel mode      │
└─────────────────────────────────┬────────────────────────────────┘
┌─────────────────────────────────▼────────────────────────────────┐
│ Qualcomm QCA2066 (firmware read from Valve's installed driver)   │
└──────────────────────────────────────────────────────────────────┘
```

`deckbt-usbip.exe` starts the attach itself: it runs `usbip.exe attach` for its own bus ID, and usbip-win2's kernel driver connects back to the server and imports the device. Every HCI packet then crosses the loopback connection as a USB/IP message:

| Traffic | Windows side | USB/IP | In the service | UART (H4 type) |
|---|---|---|---|---|
| HCI command | EP0 class request (bmRequestType `0x20`) | `CMD_SUBMIT` on EP0; the data stage is the command | held until the controller is ready; two commands adjusted (see [The HCI bridge](#the-hci-bridge)) | `0x01` |
| HCI event | interrupt IN `0x81` | `CMD_SUBMIT` parked until an event arrives, completed with `RET_SUBMIT` | vendor events dropped; ACL credits returned; stamped for arrival order | `0x04` |
| ACL data | bulk OUT `0x02`, bulk IN `0x82` | `CMD_SUBMIT` / `RET_SUBMIT`; IN transfers parked | ACL credits tracked per connection; stamped for arrival order | `0x02` |
| SCO voice | isochronous OUT `0x03`, IN `0x83`, interface 1 alternate settings 1-6 | `CMD_SUBMIT` with isochronous packet descriptors, completed by the pacing thread | re-cut between USB packet geometry and the controller's SCO packets; paced to air time | `0x03` |
| In-band sleep | none | none | wake indications acknowledged by the writer thread | single bytes `0xFE`, `0xFD`, `0xFC` |

`USBIP_CMD_UNLINK` cancels a parked or pending transfer; USB/IP device listing and import (`OP_REQ_DEVLIST`, `OP_REQ_IMPORT`) precede the session.

## Components

```
packaging/                 install.ps1, uninstall.ps1, collect-diagnostics.ps1
deckbt-usbip.exe
  ├─ USB/IP server       src/service/deckbt_usbip.c   (connections, threads, pacing timer, service, lifecycle)
  ├─ hands-free restart  src/service/handsfree.c      (see Lifecycle)
  ├─ USB device          src/service/usbip_device.c   (descriptors, EP0, endpoints, ordering, SCO pacing)
  │                      src/common/usb_descriptors.c, sco_usb.c
  ├─ HCI transport       src/include/hci_transport.h  (stream interface between device and backend)
  ├─ backend: stub       src/common/hci_stub.c        (synthetic controller, --backend stub)
  └─ backend: UART       src/service/qca_backend.c    (--backend uart)
        ├─ UART access   src/service/uart_win32.c     (SerCx2-published COM port interface)
        ├─ bring-up      src/common/qca_init_fsm.c, qca_tlv.c, qca_identify.c
        ├─ H4 framing    src/common/h4_codec.c
        └─ HCI bridge    src/common/hci_bridge.c, sco_route.c
deckbt-uartprobe.exe     src/service/uart_probe.c     (identifies the controller over the UART)
```

The synthetic controller answers `BTHPORT`'s initialisation without hardware; the host-side tests and the socket regression tests use it ([BUILD.md](BUILD.md)).

## The virtual USB device

A non-composite Bluetooth device (`bDeviceClass 0xE0`, subclass `0x01`, protocol `0x01`), VID `0x0CF3` PID `0x6390`, serial `DECKBT0001`. Inbox `bth.inf` matches its compatible ID `USB\Class_E0&SubClass_01&Prot_01`, so `BTHUSB.SYS` binds without any INF from this project.

| Endpoint | Type | Carries |
|---|---|---|
| EP0 | control | HCI commands (class request) |
| 0x81 | interrupt IN | HCI events |
| 0x02 / 0x82 | bulk OUT / IN | ACL data |
| 0x03 / 0x83 | isochronous OUT / IN, interface 1 alternate settings 0-6 | SCO voice, 0/9/17/25/33/49/63 bytes per packet |

UDE validates the device the same way whichever driver emulates it. These constraints were established with VirtBthUsb and hold under usbip-win2 (`src/include/usb_descriptors.h`):

1. **High speed.** The configuration descriptor is validated against high-speed rules; a full-speed-shaped descriptor fails with `CONFIGURATION_DESCRIPTOR_VALIDATION_FAILURE`. The device is announced as high speed (USB/IP speed 3).
2. **Bulk `wMaxPacketSize` 512**, the high-speed value, not a full-speed dongle's 64.
3. **Isochronous `bInterval` 4**: $2^{4-1} \times 125\,\mu\text{s} = 1\,\text{ms}$, the SCO frame.
4. **No interface association**, so `usbccgp.sys` is not inserted and `BTHUSB.SYS` owns the whole device.

### Requests the server answers

usbip-win2 forwards all EP0 traffic to the server, including the standard requests that UdeCx answered by itself for VirtBthUsb:

- `GET_DESCRIPTOR` for the device, configuration and string descriptors. Other types (device qualifier, BOS) are stalled; the device declares `bcdUSB` 2.00.
- `SET_CONFIGURATION`, `GET_CONFIGURATION`, `SET_INTERFACE`, `GET_INTERFACE`. usbip-win2's filter turns the function driver's `SELECT_CONFIGURATION` and `SELECT_INTERFACE` into these requests, so the SCO alternate setting arrives explicitly as `SET_INTERFACE(interface 1, setting n)`.
- `GET_STATUS` (bus powered, no remote wakeup), `CLEAR_FEATURE`/`SET_FEATURE` (remote wakeup is stalled; endpoint halt is accepted).
- A port reset arrives as `SET_PORT_FEATURE(PORT_RESET)` (bmRequestType `0x23`); it empties the transport and the voice stream.
- HCI commands: bmRequestType `0x20`, bRequest 0; the data stage is the command.

Anything else is stalled (`-EPIPE`). Interrupt and bulk IN transfers are parked until the backend has a packet. `USBIP_CMD_UNLINK` removes a parked transfer and is answered `-ECONNRESET`, or 0 when the transfer had already completed.

### Voice over the isochronous endpoints

`BTHUSB` learns the voice rate only from how fast isochronous transfers complete, and SCO has no host flow control. A transfer therefore completes only once the air time it represents has passed (`src/common/sco_usb.c`, `src/include/sco_usb.h`):

- Alternate settings 1-5: one packet is one 1 ms frame. Alternate setting 6 (63 bytes, used for wideband mSBC voice): each non-empty OUT packet is one 60-byte mSBC frame, 7.5 ms of voice.
- OUT bytes are queued with their due time (at most 128 ms ahead) and released to the controller at that time; HCI SCO packets are reassembled from the stream.
- The controller chooses its own SCO packet size. IN data is re-cut into the geometry a USB controller produces for the selected setting; frames with nothing to send carry zero bytes.

A pacing thread waits on a high-resolution waitable timer until the next transfer is due and completes it with `USBIP_RET_SUBMIT`. The timer is created before the server starts; a server that cannot create it does not start. If arming it ever fails, the thread logs once and waits in whole milliseconds until it works again, rather than spinning. Isochronous IN data is sent compacted (packets back to back), as usbip-win2 expects.

## The USB/IP server

- Listens on `127.0.0.1` only, port 3241 by default. Port 3240, the USB/IP default, is left to other software such as usbipd-win (WSL USB passthrough).
- Answers `OP_REQ_DEVLIST` and one `OP_REQ_IMPORT` at a time (bus ID `1-1`). A second import is refused as busy.
- Accepts imports only from the kernel (see [Security](#security)).
- Eight fixed pending-handshake slots, each with a two-second absolute deadline. Silent or trickling clients do not block other connections; a kernel client can evict a non-kernel pending handshake when all slots are full. Device listing remains available to user mode.
- Sends have a bounded timeout; a failed send closes the session and enters recovery.
- Isochronous requests are rejected (`-EINVAL`) when their packet descriptors overlap, run out of order or past the transfer, or describe a packet longer than the selected alternate setting's `wMaxPacketSize` (the most one 1 ms frame carries; the endpoints declare no additional transactions). Rejected isochronous requests advertise zero packet descriptors in the reply rather than a count with no corresponding bytes. A full voice-transfer queue completes the overflowing request at once with valid framing.
- A refused import logs the client's bus ID with control characters, `\` and `'` escaped as `\xHH`, so a local client cannot add lines to the service log.

## Reaching the controller from user mode

The controller's UART is an AMD UART controller (`ACPI\AMDI0020\4`, `amduart.sys`, a SerCx2 client). The stock driver reaches it through a resource-hub connection from its ACPI `_CRS`, which user mode cannot open. SerCx2 can instead publish the port as a `GUID_DEVINTERFACE_COMPORT` device interface when the controller's hardware key holds `SerCxFriendlyName` ([Microsoft Learn](https://learn.microsoft.com/windows-hardware/drivers/serports/device-interface-publication-sercx)). With `SerCxFriendlyName = "QCA2066"`:

- the interface appears after the controller restarts, with port name `QCA2066` and no COM number; `src/service/uart_win32.c` finds it by the controller's instance ID;
- only administrators and SYSTEM may open it;
- it starts unconfigured, so the program sets rate, 8N1, the RTS/CTS handshake and timeouts with the standard serial IOCTLs;
- the port is exclusive: the stock driver must not be holding it.

Microsoft documents the registry method as intended for development; the supported method is an ACPI `_DSD` property, which only the firmware vendor can add.

The controller's ACPI node (`\_SB.FUR4.QTBT`) defines only `_HID`, `_CID`, `_DDN`, `_STR`, `_CRS` and `_STA`, with no power methods or power resources, so leaving the stock driver's device without a driver does not make ACPI switch the controller off. Its `_CRS` holds the UART connection (115200 8N1, RTS/CTS) and one edge-triggered GPIO interrupt (pin 11) that is not marked wake-capable; nothing here uses that interrupt.

## The controller backend

`src/service/qca_backend.c` takes the controller from whatever state it is in to a working radio, then serves it. [QCA2066.md](QCA2066.md) has the controller specifics.

1. **Firmware.** The rampatch (a TLV file, Qualcomm's type-length-value firmware format) and the NVM configuration files are read from the newest installed Qualcomm package (`qcbtuart.inf_amd64_*` in the DriverStore) and validated before anything is sent.
2. **Reach ROM state.** Identify at 115200 baud (then 3,000,000 and 3,200,000). A controller left running firmware is reset (in-band wake, SoC reset) and must then answer at 115200.
3. **Bring-up.** 3,000,000 baud, rampatch, board ID, NVM, logging off, `HCI_Reset`. About 3.5 s from ROM.
4. **Steady state.** Reads complete on the first byte; the host wakes the controller with an in-band sleep (IBS) `WAKE_IND` until it answers `WAKE_ACK`, at most 10 times 100 ms apart; without an acknowledgement the start fails and the controller is handed back. The HCI bridge then becomes ready. The host never sends `SLEEP_IND`, so its transmit side stays awake for the session ([QCA2066.md](QCA2066.md#in-band-sleep)).
5. **Serve.** HCI traffic moves between the device and the UART in H4 framing (one packet-type byte before each HCI packet).
6. **Hand back.** On stop, the controller is reset to ROM at 115200, so the stock driver can take it again.

Threads:

- **Reader**: one read always pending. It feeds the H4 decoder under the controller lock and, after releasing the lock, tells the device which streams became readable.
- **Writer** (steady state): sends queued packets whole, and acknowledges controller `WAKE_IND` bytes between packets. Acknowledgements are never sent from the read path. While CTS is deasserted nothing is written, and the controller repeats an unanswered `WAKE_IND`.
- **Front-end calls** (`Submit*`) never block: packets are queued for the writer (32 slots).

The reader and writer, like the service's pacing and USB/IP session threads, join the MMCSS "Pro Audio" task, as the Windows audio engine's threads do (`src/service/mmcss.h`). At normal priority an application starting delayed them by up to 112 ms, heard as crackle in calls and stutter in music. A thread that MMCSS refuses logs it and runs at normal priority.

## The HCI bridge

`src/common/hci_bridge.c`:

- **Readiness.** A command that arrives before the controller is ready is held and sent once it is, rather than stalling EP0.
- **Vendor events.** Qualcomm vendor events (`0xFF`) are dropped; `BTHPORT` never requested them.
- **ACL credits.** The controller accepts only as many ACL packets as it has buffers. The pool is its `Read_Buffer_Size` count plus any separate `LE_Read_Buffer_Size` pool (7 + 16 on the test device). Packets in flight are tracked per connection handle and credited back from `Number_Of_Completed_Packets`, disconnections and a successful `HCI_Reset`.
- **Arrival order.** Events and ACL data use different endpoints, so without care a later packet on one could overtake an earlier one on the other. Every event and ACL packet is stamped from one counter, and the device model (`usbip_device.c`) delivers the older packet first. A packet whose endpoint has no read waiting holds the other endpoint for at most 20 ms; after that, reordering is possible. Each newly blocked packet starts a fresh hold. A 32-packet history per stream allows a reply to be sent again when usbip-win2 dropped it after a crossed `CMD_UNLINK`; the replay keeps the packet's original position and stamp. This cannot retract later packets Windows already consumed, and unlinks outside the retained history cannot recover the payload. It is a mitigation, not an end-to-end ordering guarantee ([VERIFICATION.md](VERIFICATION.md#bugcheck-0x7e-in-bthport)).
- **In-band sleep.** `0xFE` (sleep), `0xFD` (wake indication) and `0xFC` (wake acknowledgement) are removed from the HCI stream by the H4 decoder.
- **Link tracking.** The bridge records each BR/EDR link's handle and peer address, and whether an L2CAP connection request for RFCOMM has appeared on it, for the hands-free workaround below.

Two edits to HCI traffic, both following the stock driver or upstream Linux:

- **`Write_LE_Host_Support`.** `BTHPORT` sets `Simultaneous_LE_Host = 1`, which this controller rejects with `0x11`; the completion is reported as success, as the vendor driver does.
- **Voice routing** (`src/common/sco_route.c`). `BTHPORT`'s legacy `Setup_/Accept_Synchronous_Connection` (`0x0428`/`0x0429`) have no data-path field, and this controller's default route is the offload path, so a legacy setup gives a link that carries no voice over HCI. They are rewritten to `Enhanced_Setup_/Enhanced_Accept_Synchronous_Connection` (`0x043D`/`0x043E`) with HCI data paths; the answers get `BTHPORT`'s legacy opcode back.

**Clean disconnect.** Before a deliberate radio stop the bridge can take over the command channel: it sends `HCI_Disconnect` (reason 0x15, remote device powered off) for each open ACL link, one at a time, as BTHPORT does when Bluetooth is switched off. During this, BTHPORT's commands are withheld and the disconnect replies are not delivered to it, so BTHPORT sees the adapter removed with its links up, as it would for an unplugged dongle (`src/include/hci_bridge.h`).

## Lifecycle

`deckbt-usbip.exe` runs as the `DeckBtService` Windows service (LocalSystem, automatic start) or in a console. Both run the same sequence.

**Start.**

1. **Server.** Listener, accept and pacing threads, device model. The pacing thread's high-resolution timer is created first; without it the start fails. Imports are refused (`ST_NA`) until the radio is up.
2. **Radio.** Controller bring-up, then `usbip.exe attach --receive-mode low-latency` for its own bus ID; usbip-win2's driver connects back from the kernel and imports the device. The default zero-copy receive mode is avoided because it completes transfers before unlocking their memory pages ([VERIFICATION.md](VERIFICATION.md#bugcheck-0x4e-in-usbip-win2-zero-copy-receive)). The root-hub port reported by `attach` is kept for the detach. In the service, a failed start is retried every 3 s, up to 20 attempts, since at boot or right after resume the UART or usbip-win2 may not be ready yet.

**Sleep and resume.** The controller loses its firmware in S3. On `PBT_APMSUSPEND` (`PowerRegisterSuspendResumeNotification`) every open link is disconnected cleanly, then the device is detached, the session drains, the controller is handed back and the UART closed, all before the system sleeps. On resume the radio is started again: bring-up, attach. Windows sees the adapter unplugged and plugged back in.

**Stop.** Disconnect links cleanly, detach, hand back, close. The service waits up to 1 s for the links to close (typically 0.1-0.5 s). Without the clean disconnect, peers only saw their link vanish when the controller was reset. A normal stop takes about 0.5 s.

**Hands-free workaround.** After a resume, Windows creates the profile devices of the re-attached adapter, but for a headset that connects within the first seconds it never opens the hands-free profile: no RFCOMM connection, no microphone, and the music output appears only after about 20 s. Restarting the headset's Hands-Free AG device makes Windows open the profile at once. About once a second (`src/service/handsfree.c`) the service checks the links the bridge tracks. A BR/EDR link up 5 s without an RFCOMM connection request gets its peer's started `BTHENUM\{0000111E-…}` device restarted (`DIF_PROPERTYCHANGE`), once per link, logged as `hands-free: …`. When Windows opens the profile itself (within about 0.1 s), nothing happens; peers without a Hands-Free device are never touched. [VERIFICATION.md](VERIFICATION.md#hands-free-profile-missing-after-sleep) has the analysis.

**Lost session.** An unexpected session end (external detach, TCP loss) refuses further imports and wakes the lifecycle thread, which resets the backend and re-attaches. A deliberate stop or suspend suppresses that recovery signal. The session thread is joined before the backend is stopped. A lost session's saved root-hub port is discarded rather than detached, since another client might have acquired that port since.

**Process failure.** If the process dies without a stop, usbip-win2 loses the connection and unplugs the device. The service manager restarts the service after 5 s, then 15 s, then 60 s for every later failure; nonzero reported service exits also count as failures. The start sequence resets a controller left running firmware.

**Controller faults.** In steady state the backend reports a failed UART link once per start: 20 consecutive failed reads, or any failed write (a partial write can leave the controller's H4 parser mid-packet). The service then restarts the radio as for a lost session, logged as `radio: stopping (controller fault: …)`. The report comes from a backend thread with no lock held and only signals the lifecycle thread.

**Bounded stop.** Every wait on the stop and suspend path has a deadline. `usbip.exe port` and `detach` get 3 s and `attach` gets 60 s, plus 1 s to end the process. The session drain gets 3 s, then 2 s more after its read is cancelled (on Windows, `shutdown()` does not wake a blocked `recv`). A pending `attach` is cancelled when a stop, preshutdown or suspend arrives, so none of them waits out its deadline. A watchdog bounds each whole radio stop and the whole suspend callback, including waiting for a bring-up in progress, to 45 s. If a thread cannot be joined in time (the session thread, or a UART reader or writer the backend marks `Stuck`), nothing is freed under it: the service logs one `fatal:` line and ends its process, and the service manager's recovery takes over.

**Shutdown.** The service accepts `PRESHUTDOWN` and stops then. usbip-win2 installs a scheduled task that detaches all devices on the restart request (User32 event 1074), about 20 s before services are told. The service sees that as a lost session and re-attaches once; the pending `attach` is cancelled when the stop arrives.

Each service process logs its version, UTC start time and PID.

## Locking

One lock, the controller lock of `hci_transport.h`, serialises every call into the device model and the transport: USB/IP commands, pacing ticks, backend deliveries. The backend calls `Notify` only after releasing it. A second lock serialises radio start and stop between the main thread and the power callback. The log has its own lock and is never held while taking the others. The fatal path only tries that lock, so a thread stuck while writing cannot block process termination.

## Security

DeckBtService runs as LocalSystem and opens a TCP port, so its exposure is limited as follows.

- **Loopback only, kernel imports only.** The listener binds `127.0.0.1`. usbip-win2 connects from the kernel, so an import connection is owned by process 4 (System). The server looks up the connection's owner by its full loopback TCP tuple in the established state and refuses imports from any other or unidentified owner (the lookup is repeated only when the connection table grows between its size query and its read). This keeps local user-mode programs from importing the radio. It is not authentication of a particular kernel driver. Only isolated console testing can opt out (`--allow-user-import`, rejected for the UART backend and the service).
- **Bounded handshakes.** Pending handshakes have fixed slots and an absolute deadline, so local clients cannot exhaust the listener or lock out the kernel importer.
- **UART access.** The published COM port interface can be opened only by administrators and SYSTEM.
- **Child processes.** `usbip.exe` inherits only its output pipe through an explicit handle list, never the listener, session sockets or UART. Its output is drained after it exits.
- **Protected log folder.** The service log is `%ProgramData%\DeckBtService\deckbt-service.log`, rotated at 4 MiB with one `.1` kept. The service creates or repairs that folder with owner Administrators and a protected DACL (SYSTEM and Administrators full control, Users read), and refuses reparse points and hard-linked log files, so an unprivileged user cannot redirect its writes.
- **Program Files install.** A LocalSystem service binary must not live in a folder users can write to. `deckbt-usbip.exe install` registers the service only when its own path lies under the Program Files known folder: a path check that keeps it out of user folders, not a permission check. The permissions come from `install.ps1`, which gives `%ProgramFiles%\DeckBtService` a DACL of SYSTEM and Administrators full control, Users read and execute.

### Installer trust model

`packaging/install.ps1` (started by `install.cmd`, which asks for administrator rights) makes these changes and records each one, with its prior value, in `%ProgramData%\DeckBtService\install-state.json`:

1. checks the hardware (Steam Deck OLED only; the LCD model is refused), the release files against `SHA256SUMS`, and that no other program's service is named `DeckBtService`;
2. installs usbip-win2 0.9.8.1, downloaded from its GitHub release, if it is missing or older;
3. switches the stock Bluetooth transport off with a device-installation deny policy for `ACPI\QCOM2066`, after switching Bluetooth off so no link or audio stream holds the stock radio;
4. publishes the UART to user mode (`SerCxFriendlyName` on `ACPI\AMDI0020\4`);
5. copies the programs to `%ProgramFiles%\DeckBtService`;
6. registers and starts the service and waits until the radio is attached.

It exits 0 when installed and running, 3010 when Windows must restart first (then it is run again), 1 on failure. `uninstall.ps1` reverses only the recorded changes and restores the stock radio.

What the scripts trust and check:

- **Release files.** `SHA256SUMS` detects a damaged or incomplete download. It ships in the same zip, so it does not prove who built the files. The binaries are not code-signed. Since v0.1.3 the zip and both programs carry GitHub build-provenance attestations: `gh attestation verify` shows that the files were built by the project's workflow from a given commit.
- **Copies.** The release folder may be writable by other users. The programs are staged inside the protected install folder and those copies are checked against the hashes read earlier; nothing from the release folder is executed after that check.
- **usbip-win2.** The installer is downloaded over HTTPS from the project's GitHub release and must match a SHA-256 pinned in `install.ps1` before it runs. Its drivers carry Microsoft signatures.
- **Folders.** The data folder is created or repaired with the service's permissions before anything is read from or written to it, and a junction there is refused.
- **Deny policy.** Deny entries that DeckBtService did not add are left alone and their policy flags restored on uninstall; when enabling the policy would also enforce such entries, the installer logs a warning.
- **Service name.** A `DeckBtService` service that runs anything other than `%ProgramFiles%\DeckBtService\deckbt-usbip.exe` is refused before any change and never stopped, replaced or deleted, by the installer or the uninstaller: a replaced registration's account, start type, dependencies and recovery settings could not be restored.
- **Environment.** 64-bit PowerShell only; `-DryRun` prints every check and planned change without changing anything.

## Differences from stock Bluetooth

What behaves differently from the stock driver, beyond the microphone, and what has not been compared yet, is listed in [ROADMAP.md](ROADMAP.md).

## Design history: VirtBthUsb

DeckBtService reuses the controller bring-up, HCI bridge and voice framing of [VirtBthUsb](https://github.com/ethanm0202/VirtBthUsb), a kernel driver for the same purpose. The main differences:

| | VirtBthUsb (kernel driver) | DeckBtService |
|---|---|---|
| Emulated USB host | own KMDF driver with UdeCx | usbip-win2's signed UDE driver |
| USB requests | UdeCx callbacks; UdeCx answered standard requests itself | USB/IP commands; every request answered by the server |
| SCO alternate setting | inferred from UdeCx endpoint lists | explicit `SET_INTERFACE` |
| UART | resource-hub connection as the `ACPI\QCOM2066` function driver | SerCx2-published COM port interface |
| Firmware | copied into the driver package at build | read from the installed vendor package |
| Sleep | D0 exit/entry of the controller's device | suspend/resume notifications to the service |
| Test signing | required | not required |
