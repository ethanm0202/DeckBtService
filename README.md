# DeckBtService

Bluetooth headset microphones for Windows on the Steam Deck OLED.

On the Steam Deck OLED, Windows' stock Bluetooth driver plays music through a Bluetooth headset but never offers its microphone. DeckBtService fixes that: once installed, the headset's microphone shows up in Windows as a normal recording device, for calls, voice chat and recording. It runs as a background Windows service; everything else about Bluetooth keeps working as before, with the same paired devices.

It is for the Steam Deck **OLED** only. The Steam Deck LCD has a different Bluetooth radio, is not supported, and the installer refuses to run on it.

---

## Install

1. Download `DeckBtService-0.1.3.zip` from the [Releases](../../releases) page and extract it.
2. Double-click `install.cmd` and approve the administrator prompt.
3. If it asks you to restart Windows, restart, then double-click `install.cmd` again. A first install needs one restart.

Release zips are built by [GitHub Actions](.github/workflows/build.yml) from the tagged commit, and each has a signed build-provenance attestation. With the [GitHub CLI](https://cli.github.com/), check that a download was built there from this repository:

```
gh attestation verify DeckBtService-0.1.3.zip --repo ethanm0202/DeckBtService
```

[packaging/README-install.md](packaging/README-install.md) has the full steps, what the installer changes on your system, how to uninstall and how to report a problem. The same guide is in the zip.

---

## Status

Version 0.1.3 ([changes](CHANGELOG.md)). Tested on one Steam Deck OLED running Windows 11 25H2, with a Shokz OpenMeet headset and a Bluetooth mouse. Details and measurements: [docs/VERIFICATION.md](docs/VERIFICATION.md).

What works:

- The headset microphone appears as a normal recording device and carries your voice in calls, in wideband (mSBC) quality.
- Devices you have already paired stay paired and reconnect by themselves.
- Music, finding nearby devices, and Bluetooth mice work, including a mouse during a call.
- Bluetooth starts by itself at boot, about 8 seconds after Windows starts; the radio is ready about 3.5 seconds later.
- After sleep, the headset's music and microphone come back by themselves, about 15 seconds after waking. The same happens after switching Bluetooth off and on in Settings.
- If the service stops unexpectedly, Windows restarts it and Bluetooth returns.
- Running the installer again resumes an interrupted install or upgrades an existing one. The uninstaller puts the stock Bluetooth driver back; reinstalling afterwards needed no restart.

Known limitations:

- **Tested on one Deck.** Other units and other Windows builds have not been tried.
- **Not yet tested:** overnight sleep, long calls, calls that span a sleep, and battery life compared with the stock driver.
- **Not yet tested:** Memory Integrity (Core isolation), Secure Boot, and games with kernel anti-cheat. The test Deck had Memory Integrity and Secure Boot off.
- **Other Bluetooth uses** such as pairing new devices, game controllers, several devices at once or file transfer have not been tested systematically.
- **Older headsets** that only support narrowband call audio (no wideband) are supported in the code but untested.
- **Heavy load can still cause a rare crackle in calls.** The service's audio threads use Windows' audio scheduler, so opening apps no longer stutters music; under deliberate full-CPU bursts a call still had one audible crackle in three bursts.
- **Bluetooth depends on the service.** If the service restarts, Bluetooth disappears for a few seconds and devices reconnect. There is no fallback to the stock driver while it is installed.
- **Windows sees a new adapter,** named Generic Bluetooth Adapter. Your headset's audio devices may appear as new ones, so an app that remembered a specific microphone or speaker may need it picked again.
- **Two Windows crashes (blue screens)** were recorded while earlier builds were being tested. Both have mitigations in this release, but that is not proof they cannot happen again; see [docs/VERIFICATION.md](docs/VERIFICATION.md).
- **The programs are not code-signed,** so Windows may warn about an unknown publisher when you run them. Their origin can be checked with the build attestation above instead.

A detailed comparison with the stock driver is in [docs/ROADMAP.md](docs/ROADMAP.md).

---

## How it works

The Deck OLED's Bluetooth chip (a Qualcomm QCA2066) is wired to a serial port inside the Deck, not to USB. Windows' driver for that kind of Bluetooth connection only handles call audio through a dedicated audio path that the Deck doesn't have, so the microphone never appears. Windows' driver for USB Bluetooth adapters handles call audio itself, but it only works with USB devices.

DeckBtService bridges the two. It takes over the serial port, starts the Bluetooth chip with the firmware that Valve's driver already installed on your Deck, and presents the chip to Windows as a USB Bluetooth adapter. The virtual USB connection is provided by [usbip-win2](https://github.com/vadimgrn/usbip-win2), an open-source project with a Microsoft-signed driver. Windows then runs the adapter with its own built-in USB Bluetooth drivers, unmodified. DeckBtService has no kernel driver of its own and does not need Windows test mode.

```
┌────────────────────────────────────────────────┐
│ Windows Bluetooth stack (BTHPORT.SYS)          │
└───────────────────────┬────────────────────────┘
┌───────────────────────▼────────────────────────┐
│ Inbox USB transport driver (BTHUSB.SYS)        │
└───────────────────────┬────────────────────────┘
┌───────────────────────▼────────────────────────┐
│ usbip-win2 virtual USB host (signed)           │
└───────────────────────┬────────────────────────┘
                        │ USB/IP over 127.0.0.1
┌───────────────────────▼────────────────────────┐
│ DeckBtService (user-mode Windows service)      │
│  USB device: descriptors, EP0 commands,        │
│  interrupt events, bulk ACL, isochronous SCO   │
│  HCI bridge: H4 framing, voice routing, pacing │
│  controller bring-up: Qualcomm firmware load   │
└───────────────────────┬────────────────────────┘
                        │ UART published by Windows (SerCx2)
┌───────────────────────▼────────────────────────┐
│ Qualcomm QCA2066                               │
└────────────────────────────────────────────────┘
```

The service opens the controller's UART, loads its Qualcomm firmware, and translates between USB transfers on one side and H4-framed serial traffic on the other. Call audio needs the most care: `BTHUSB.SYS` expects voice to arrive at the pace of a real USB radio, so the service paces isochronous transfers and re-frames the controller's voice packets to match. It also handles sleep (disconnecting links cleanly before suspend and bringing the controller back after resume) and restarts the radio after a fault.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design and [docs/QCA2066.md](docs/QCA2066.md) for the controller bring-up.

---

## Requirements

- Steam Deck OLED, with Valve's Windows Bluetooth driver installed (part of Valve's Windows driver downloads for the Deck OLED). DeckBtService reads the Bluetooth firmware from it; no firmware is included here.
- Windows 11, 64-bit. Tested on 25H2.
- Administrator rights.
- An internet connection for the first install: the installer downloads usbip-win2 0.9.8.1 from its official GitHub release and checks it before running it. Version 0.9.8.0 is not suitable: it drops some Bluetooth data ([usbip-win2 issue #190](https://github.com/vadimgrn/usbip-win2/issues/190)), which stops headsets reconnecting properly.

## Uninstall

Double-click `uninstall.cmd` in the extracted folder. It removes DeckBtService and puts the stock Bluetooth driver back. Options are in [packaging/README-install.md](packaging/README-install.md).

---

## Building

Building is only needed to change the code; releases include ready-to-run programs. The build uses the Enterprise WDK (EWDK); host-side tests run without the Deck's hardware. See [docs/BUILD.md](docs/BUILD.md).

---

## Repository layout

| Path | Contents |
|---|---|
| `src/service/` | the program and service (`deckbt_usbip.c`): USB/IP server and virtual USB device (`usbip_device.c`), controller backend (`qca_backend.c`), user-mode UART access (`uart_win32.c`), hands-free restart after resume (`handsfree.c`), UART check (`uart_probe.c`) |
| `src/common/`, `src/include/` | platform-free logic: descriptors, H4 codec, HCI bridge, Qualcomm firmware parser and bring-up state machine, SCO framing and routing, synthetic HCI stub |
| `tools/` | build, packaging and test scripts, host-side test suites, fuzz targets, Bluetooth diagnostics, microphone check |
| `packaging/` | release files: install, uninstall and diagnostics scripts with their double-click launchers, and the user guide |
| `reference/` | expected descriptors and HCI exchanges of the synthetic stub |

---

## Documentation

- [packaging/README-install.md](packaging/README-install.md): install, uninstall, reporting a problem
- [docs/BUILD.md](docs/BUILD.md): building, tests, packaging, running it as a service or in a console
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): design and the Windows constraints behind it
- [docs/QCA2066.md](docs/QCA2066.md): controller bring-up, firmware selection, in-band sleep
- [docs/VERIFICATION.md](docs/VERIFICATION.md): what was tested and the results
- [docs/ROADMAP.md](docs/ROADMAP.md): what is done, what is next, and differences from stock Bluetooth
- [CHANGELOG.md](CHANGELOG.md): changes in each release

---

## History

DeckBtService follows [VirtBthUsb](https://github.com/ethanm0202/VirtBthUsb), a kernel driver that did the same job from inside the Windows kernel. It worked, including headset voice and sleep, but as a test-signed driver it only loaded with Windows test signing turned on, and games with kernel anti-cheat usually refuse to start while test signing is on. VirtBthUsb worked out the USB descriptor layout Windows accepts for an emulated Bluetooth adapter, the isochronous transfer timing for voice, and the controller's firmware bring-up and in-band sleep. DeckBtService moves that work into a user-mode service on top of usbip-win2's signed driver. The platform-free modules in `src/common/` come from VirtBthUsb v1.0.1 and are maintained here. VirtBthUsb is no longer developed.

---

## Credits

- **[usbip-win2](https://github.com/vadimgrn/usbip-win2)** by vadimgrn (BSD-2-Clause), with driver signing by [OSSign](https://github.com/OSSign). DeckBtService uses its unmodified, Microsoft-signed USB/IP client to attach the virtual adapter. It is downloaded from its own release by the installer and not redistributed here.
- **Linux Bluetooth drivers** ([`btqca.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/btqca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137), `btqca.h`, [`hci_qca.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/hci_qca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137), GPL-2.0): the reference for the Qualcomm vendor commands, the TLV firmware format, the NVM file-selection rule, in-band sleep, and choosing the HCI voice data path on the QCA2066. This project implements those protocols for Windows. It contains no Linux code.
- **USB/IP protocol**: [Linux kernel documentation](https://www.kernel.org/doc/html/latest/usb/usbip_protocol.html).
- **Microsoft Learn**: SerCx2 device interface publication, USB and Bluetooth driver documentation.
- **Bluetooth SIG**: Bluetooth Core Specification (HCI, USB transport, synchronous connections) and the Hands-Free Profile (mSBC).
- **Qualcomm and Valve**: the controller firmware, read from the Windows driver package already installed on the Deck. It is not redistributed.

---

## License

[MIT](LICENSE)

## Disclaimers

- Steam Deck is a trademark of Valve Corporation. Windows is a trademark of Microsoft Corporation. Qualcomm is a trademark of Qualcomm Incorporated. This is an independent project, not affiliated with or endorsed by Valve, Microsoft, Qualcomm or the usbip-win2 project.
- Protocol constants and vendor command values are used for interoperability.
- No firmware or other proprietary binaries are included in this repository.
- Tested on one device. Use it at your own risk.
