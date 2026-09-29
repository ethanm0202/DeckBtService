# Installing DeckBtService

DeckBtService makes Bluetooth headset microphones work under Windows on the **Steam Deck OLED**. It does not support the Steam Deck LCD, which has a different Bluetooth radio; the installer stops with a message if you run it there.

## Before you start

- Valve's Windows Bluetooth driver for the Deck OLED must be installed. If Bluetooth works on your Deck today, it is. DeckBtService loads the Bluetooth firmware from it.
- The first install needs an internet connection: it downloads [usbip-win2](https://github.com/vadimgrn/usbip-win2), which DeckBtService relies on.
- Finish any calls or music first. The installer switches Bluetooth off for a moment. Your paired devices stay paired and reconnect afterwards.

## Install

1. Download `DeckBtService-0.1.0.zip` from the project's [Releases page](https://github.com/ethanm0202/DeckBtService/releases).
2. Right-click the zip, choose **Extract All**, and extract it to your Downloads folder. This creates a folder `DeckBtService-0.1.0`. Keep it somewhere only you can write to, such as Downloads or Documents, not a shared folder.
3. Open the folder and double-click **`install.cmd`**. Approve the administrator prompt.
4. Wait for the window to report the result. It stays open until you close it.
5. If it says **Restart Windows**, restart, then double-click `install.cmd` again. A first install needs one restart, because usbip-win2's own installer asks for one. The second run confirms that Bluetooth is up.

When it finishes, Windows lists a **Generic Bluetooth Adapter**, and your paired devices reconnect. Your headset's microphone appears under **Input** in Sound settings and can be picked in any app. Because Windows sees a new adapter, an app that remembered a specific microphone or speaker may need it selected again.

Keep the extracted folder: `uninstall.cmd` and `collect-diagnostics.cmd` are in it. To update later, extract the newer release and double-click its `install.cmd`.

### Security warnings

The programs are not code-signed. When you open `install.cmd` or the other launchers, Windows may show **Windows protected your PC** or warn about an **unknown publisher**. This is expected: choose **More info**, then **Run anyway** (or **Run**). The installer checks every file in the folder against the list in `SHA256SUMS` before installing it, and checks usbip-win2's installer against a fixed SHA-256 before running it.

## What the installer changes

- Installs [usbip-win2](https://github.com/vadimgrn/usbip-win2) 0.9.8.1 if it is missing or older. Its installer is downloaded from the official GitHub release and checked against a pinned SHA-256 before it runs. usbip-win2 is not included in this download. It provides the Microsoft-signed virtual USB driver that DeckBtService attaches its Bluetooth adapter through.
- Switches off the stock Bluetooth driver (`qcbtuart.sys` on `ACPI\QCOM2066`) with a Windows device-installation deny policy. Windows allows only one Bluetooth radio at a time, and the stock driver would otherwise hold the radio's serial line.
- Makes the radio's serial line available to the service (`SerCxFriendlyName` = `QCA2066` on `ACPI\AMDI0020\4`).
- Copies the programs to `C:\Program Files\DeckBtService` and registers the `DeckBtService` service, which starts automatically with Windows. If another program already has a service named `DeckBtService`, the installer stops before changing anything and leaves that service alone; the uninstaller never removes it either.

Everything it changes is recorded in `C:\ProgramData\DeckBtService\install-state.json`, so the uninstaller reverses only those changes. The install log is `C:\ProgramData\DeckBtService\install.log`; the service's own log is `deckbt-service.log` in the same folder.

## Uninstall

Double-click **`uninstall.cmd`** in the extracted folder and approve the administrator prompt. It removes the service and the program folder, removes the deny-policy entries and the `SerCxFriendlyName` value the installer added, and puts the stock Qualcomm driver back on the radio. If it asks you to restart Windows, do so; Bluetooth then runs on the stock driver again. Headset microphones stop working on the stock driver, as before.

By default usbip-win2 and the logs are kept. For more options, open PowerShell as administrator (Start menu, type `PowerShell`, choose **Run as administrator**), go to the extracted folder, and run:

```powershell
powershell -ExecutionPolicy Bypass -File .\uninstall.ps1 -RemoveUsbip -RemoveLogs
```

- `-RemoveUsbip` also uninstalls usbip-win2, if DeckBtService's installer installed it.
- `-RemoveLogs` also deletes the DeckBtService logs in `C:\ProgramData\DeckBtService`; other files there are kept.
- `-DryRun` shows what it would do without changing anything. `install.ps1` accepts `-DryRun` too.

## Reporting a problem

Double-click **`collect-diagnostics.cmd`**. It saves `DeckBtService-diagnostics-<date>.zip` on your Desktop. Attach it to a new issue on the project's [Issues page](https://github.com/ethanm0202/DeckBtService/issues), with a short description of what happened.

The zip contains the service logs, the service and driver configuration, device states and the names and addresses of your paired Bluetooth devices. User profile paths in the logs (`C:\Users\<you>`) are replaced with `%USERPROFILE%`, but other file and folder paths are kept and may still show your Windows account name. It does not contain Bluetooth pairing keys.
