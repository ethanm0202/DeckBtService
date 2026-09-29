<#
    bt-hid-watch.ps1 - In-process Raw Input watcher for Bluetooth HID devices.

    Uses Windows Raw Input with RIDEV_INPUTSINK on a background HWND_MESSAGE thread
    to count input events per device across Generic Desktop mouse, keyboard, gamepad,
    and joystick.

    Dot-sourceable by another script to control lifetime and take snapshots:
        . (Join-Path $PSScriptRoot 'bt-hid-watch.ps1')
        $watcher = New-BtHidWatcher
        $watcher.Start()
        ...
        $snap = $watcher.Snapshot()
        $watcher.Stop()

    Standalone:
        powershell -File tools\bt-hid-watch.ps1 -Seconds 10
        Exits 0 if any Bluetooth HID input was seen, else 1.
#>
[CmdletBinding()]
param(
    [int] $Seconds = 0
)

$ErrorActionPreference = 'Stop'

if (-not ([System.Management.Automation.PSTypeName]'DeckBtRawInputWatcher').Type) {
    $csharp = @"
using System;
using System.Text;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

public class DeckBtRawInputWatcher {
    private const uint RIDEV_INPUTSINK = 0x00000100;
    private const uint RID_HEADER = 0x10000005;
    private const uint RIDI_DEVICENAME = 0x20000007;
    private const uint WM_INPUT = 0x00FF;
    private const uint WM_USER = 0x0400;
    private const uint WM_STOP = WM_USER + 1;

    [StructLayout(LayoutKind.Sequential)]
    public struct RAWINPUTDEVICE {
        public ushort usUsagePage;
        public ushort usUsage;
        public uint dwFlags;
        public IntPtr hwndTarget;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct RAWINPUTHEADER {
        public uint dwType;
        public uint dwSize;
        public IntPtr hDevice;
        public IntPtr wParam;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MSG {
        public IntPtr hwnd;
        public uint message;
        public IntPtr wParam;
        public IntPtr lParam;
        public uint time;
        public int pt_x;
        public int pt_y;
    }

    private delegate IntPtr WndProcDelegate(IntPtr hWnd, uint uMsg, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WNDCLASSEX {
        public uint cbSize;
        public uint style;
        public WndProcDelegate lpfnWndProc;
        public int cbClsExtra;
        public int cbWndExtra;
        public IntPtr hInstance;
        public IntPtr hIcon;
        public IntPtr hCursor;
        public IntPtr hbrBackground;
        public string lpszMenuName;
        public string lpszClassName;
        public IntPtr hIconSm;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool RegisterRawInputDevices(
        [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] RAWINPUTDEVICE[] pRawInputDevices,
        uint uiNumDevices,
        uint cbSize);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetRawInputData(IntPtr hRawInput, uint uiCommand, IntPtr pData, ref uint pcbSize, uint cbSizeHeader);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern uint GetRawInputDeviceInfo(IntPtr hDevice, uint uiCommand, StringBuilder pData, ref uint pcbSize);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern ushort RegisterClassEx(ref WNDCLASSEX lpwcx);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool UnregisterClass(string lpClassName, IntPtr hInstance);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateWindowEx(
        uint dwExStyle, string lpClassName, string lpWindowName, uint dwStyle,
        int x, int y, int nWidth, int nHeight, IntPtr hWndParent, IntPtr hMenu, IntPtr hInstance, IntPtr lpParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool DestroyWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern IntPtr DefWindowProc(IntPtr hWnd, uint uMsg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern int GetMessage(out MSG lpMsg, IntPtr hWnd, uint wMsgFilterMin, uint wMsgFilterMax);

    [DllImport("user32.dll")]
    private static extern bool TranslateMessage(ref MSG lpMsg);

    [DllImport("user32.dll")]
    private static extern IntPtr DispatchMessage(ref MSG lpMsg);

    [DllImport("user32.dll")]
    private static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern void PostQuitMessage(int nExitCode);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandle(string lpModuleName);

    private Thread _thread;
    private IntPtr _hwnd = IntPtr.Zero;
    private WndProcDelegate _wndProc;
    private string _startError;
    private readonly ManualResetEvent _ready = new ManualResetEvent(false);
    private readonly ConcurrentDictionary<IntPtr, string> _deviceNames = new ConcurrentDictionary<IntPtr, string>();
    private readonly ConcurrentDictionary<string, long> _counts = new ConcurrentDictionary<string, long>(StringComparer.OrdinalIgnoreCase);

    public static bool IsBluetoothHid(string name) {
        if (string.IsNullOrEmpty(name)) return false;
        return name.IndexOf("{00001812-0000-1000-8000-00805f9b34fb}", StringComparison.OrdinalIgnoreCase) >= 0 ||
               name.IndexOf("{00001124-0000-1000-8000-00805f9b34fb}", StringComparison.OrdinalIgnoreCase) >= 0;
    }

    /* Throws unless the window exists and raw input is registered: "no input" must never mean "not listening". */
    public void Start() {
        if (_thread != null) return;
        _ready.Reset();
        _startError = null;
        _thread = new Thread(ThreadProc) {
            IsBackground = true,
            Name = "DeckBtRawInputWatcherThread"
        };
        _thread.Start();
        if (!_ready.WaitOne(5000)) {
            throw new TimeoutException("RawInputWatcher thread did not signal ready within 5 seconds.");
        }
        if (_startError != null) {
            _thread.Join(1000);
            _thread = null;
            throw new InvalidOperationException("Raw input watcher failed: " + _startError);
        }
    }

    public void Stop() {
        if (_thread == null) return;
        if (_hwnd != IntPtr.Zero) {
            PostMessage(_hwnd, WM_STOP, IntPtr.Zero, IntPtr.Zero);
        }
        if (!_thread.Join(5000)) {
            _thread.Abort();
        }
        _thread = null;
        _hwnd = IntPtr.Zero;
    }

    public Dictionary<string, long> Snapshot() {
        return new Dictionary<string, long>(_counts, StringComparer.OrdinalIgnoreCase);
    }

    public long TotalBluetoothInput() {
        long total = 0;
        foreach (var kvp in _counts) {
            if (IsBluetoothHid(kvp.Key)) {
                total += kvp.Value;
            }
        }
        return total;
    }

    private void ThreadProc() {
        IntPtr hInstance = GetModuleHandle(null);
        string className = "DeckBtRawInputClass_" + Guid.NewGuid().ToString("N");
        _wndProc = CustomWndProc;

        WNDCLASSEX wc = new WNDCLASSEX {
            cbSize = (uint)Marshal.SizeOf(typeof(WNDCLASSEX)),
            style = 0,
            lpfnWndProc = _wndProc,
            cbClsExtra = 0,
            cbWndExtra = 0,
            hInstance = hInstance,
            hIcon = IntPtr.Zero,
            hCursor = IntPtr.Zero,
            hbrBackground = IntPtr.Zero,
            lpszMenuName = null,
            lpszClassName = className,
            hIconSm = IntPtr.Zero
        };

        ushort regResult = RegisterClassEx(ref wc);
        if (regResult == 0) {
            _startError = "RegisterClassEx error " + Marshal.GetLastWin32Error();
            _ready.Set();
            return;
        }

        IntPtr HWND_MESSAGE = new IntPtr(-3);
        _hwnd = CreateWindowEx(0, className, "DeckBtRawInputWindow", 0, 0, 0, 0, 0, HWND_MESSAGE, IntPtr.Zero, hInstance, IntPtr.Zero);
        if (_hwnd == IntPtr.Zero) {
            _startError = "CreateWindowEx error " + Marshal.GetLastWin32Error();
            UnregisterClass(className, hInstance);
            _ready.Set();
            return;
        }

        RAWINPUTDEVICE[] rids = new RAWINPUTDEVICE[] {
            new RAWINPUTDEVICE { usUsagePage = 0x01, usUsage = 0x02, dwFlags = RIDEV_INPUTSINK, hwndTarget = _hwnd }, // Mouse
            new RAWINPUTDEVICE { usUsagePage = 0x01, usUsage = 0x06, dwFlags = RIDEV_INPUTSINK, hwndTarget = _hwnd }, // Keyboard
            new RAWINPUTDEVICE { usUsagePage = 0x01, usUsage = 0x05, dwFlags = RIDEV_INPUTSINK, hwndTarget = _hwnd }, // Gamepad
            new RAWINPUTDEVICE { usUsagePage = 0x01, usUsage = 0x04, dwFlags = RIDEV_INPUTSINK, hwndTarget = _hwnd }  // Joystick
        };

        bool regOk = RegisterRawInputDevices(rids, (uint)rids.Length, (uint)Marshal.SizeOf(typeof(RAWINPUTDEVICE)));
        if (!regOk) {
            _startError = "RegisterRawInputDevices error " + Marshal.GetLastWin32Error();
        }
        _ready.Set();
        if (regOk) {
            MSG msg;
            while (GetMessage(out msg, IntPtr.Zero, 0, 0) > 0) {
                TranslateMessage(ref msg);
                DispatchMessage(ref msg);
            }
        }

        if (_hwnd != IntPtr.Zero) {
            DestroyWindow(_hwnd);
            _hwnd = IntPtr.Zero;
        }
        UnregisterClass(className, hInstance);
    }

    private IntPtr CustomWndProc(IntPtr hWnd, uint uMsg, IntPtr wParam, IntPtr lParam) {
        if (uMsg == WM_STOP) {
            PostQuitMessage(0);
            return IntPtr.Zero;
        }
        if (uMsg == WM_INPUT) {
            uint headerSize = (uint)Marshal.SizeOf(typeof(RAWINPUTHEADER));
            uint size = headerSize;
            IntPtr pHeader = Marshal.AllocHGlobal((int)headerSize);
            try {
                uint res = GetRawInputData(lParam, RID_HEADER, pHeader, ref size, headerSize);
                if (res != unchecked((uint)-1) && res >= headerSize) {
                    RAWINPUTHEADER header = (RAWINPUTHEADER)Marshal.PtrToStructure(pHeader, typeof(RAWINPUTHEADER));
                    IntPtr hDevice = header.hDevice;
                    string name;
                    if (!_deviceNames.TryGetValue(hDevice, out name)) {
                        uint nameLen = 0;
                        GetRawInputDeviceInfo(hDevice, RIDI_DEVICENAME, null, ref nameLen);
                        if (nameLen > 0) {
                            StringBuilder sb = new StringBuilder((int)nameLen + 1);
                            if (GetRawInputDeviceInfo(hDevice, RIDI_DEVICENAME, sb, ref nameLen) != unchecked((uint)-1)) {
                                name = sb.ToString();
                            }
                        }
                        if (string.IsNullOrEmpty(name)) {
                            name = "UnknownDevice_" + hDevice.ToString("X");
                        }
                        _deviceNames[hDevice] = name;
                    }
                    _counts.AddOrUpdate(name, 1, (k, old) => old + 1);
                }
            } finally {
                Marshal.FreeHGlobal(pHeader);
            }
        }
        return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
}
"@
    Add-Type -TypeDefinition $csharp
}

function New-BtHidWatcher {
    return [DeckBtRawInputWatcher]::new()
}

function Test-BtHidDevice([string] $Name) {
    return [DeckBtRawInputWatcher]::IsBluetoothHid($Name)
}

if ($Seconds -gt 0) {
    Write-Host "Monitoring raw input for ${Seconds}s..."
    $watcher = New-BtHidWatcher
    $watcher.Start()
    try {
        Start-Sleep -Seconds $Seconds
    } finally {
        $watcher.Stop()
    }
    $snap = $watcher.Snapshot()
    Write-Host ("Captured raw input from {0} device(s):" -f $snap.Count)
    $btTotal = 0
    foreach ($kv in $snap.GetEnumerator()) {
        $isBt = Test-BtHidDevice $kv.Key
        Write-Host ("  {0}: {1} (BT={2})" -f $kv.Key, $kv.Value, $isBt)
        if ($isBt) { $btTotal += $kv.Value }
    }
    if ($btTotal -gt 0) {
        Write-Host ("PASS: {0} input report(s) from Bluetooth HID device(s)." -f $btTotal) -ForegroundColor Green
        exit 0
    } else {
        Write-Host 'No Bluetooth HID input was observed.'
        exit 1
    }
}
