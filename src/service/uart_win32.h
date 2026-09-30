/*
 * uart_win32.h - the controller's UART from user mode, through the GUID_DEVINTERFACE_COMPORT
 * interface that SerCx2 publishes for a controller whose hardware key carries SerCxFriendlyName
 * (https://learn.microsoft.com/windows-hardware/drivers/serports/device-interface-publication-sercx).
 *
 * The published port has no COM name and starts unconfigured, so the serial IOCTLs here set
 * what the vendor driver configures on the same UART: 8N1, CTS handshake with RTS handshake,
 * XON/XOFF limits 0x800, manual RTS only while waking the controller.
 */

#pragma once

#include <windows.h>

typedef struct _UART_PORT {
    HANDLE     Handle;
    OVERLAPPED ReadOverlapped;
    OVERLAPPED WriteOverlapped;
    OVERLAPPED IoctlOverlapped;
    CRITICAL_SECTION IoctlLock;  /* control requests share one event, not one allocation per packet */
    DWORD      WriteTransferred; /* last write completion count, not a guarantee of wire delivery */
} UART_PORT;

/*
 * Finds the published interface of the UART controller ControllerInstanceId (for example
 * L"ACPI\\AMDI0020\\4"). Path receives the interface path for UartOpen; PortName (optional)
 * the DEVPKEY_DeviceInterface_Serial_PortName value. Returns ERROR_SUCCESS, ERROR_NOT_FOUND when
 * the controller publishes no interface, or another Win32 error.
 */
DWORD UartFindInterface(const WCHAR *ControllerInstanceId, WCHAR *Path, DWORD PathChars,
                        WCHAR *PortName, DWORD PortNameChars);

/* Opens the port exclusively for overlapped I/O. Win32 error on failure. */
DWORD UartOpen(UART_PORT *Port, const WCHAR *Path);
void  UartClose(UART_PORT *Port);

/* Rate, 8N1 and the hardware handshake. */
DWORD UartConfigure(UART_PORT *Port, ULONG BaudRate);

/* SERIAL_TIMEOUTS (bring-up: 20 ms interval; steady: complete on the first byte). */
DWORD UartSetTimeouts(UART_PORT *Port, ULONG ReadInterval, ULONG ReadTotalMultiplier,
                      ULONG ReadTotalConstant, ULONG WriteTotalConstant);

DWORD UartGetModemStatus(UART_PORT *Port, ULONG *Status);
DWORD UartPurge(UART_PORT *Port);

/* Quiesced startup/recovery only: RTS pulses until the controller asserts CTS.
 * Never use during steady RX: CTS can be low for TX flow control, while RTS controls RX.
 * *CtsBefore reports whether CTS was already asserted. ERROR_NOT_READY if it never asserts. */
DWORD UartWakeController(UART_PORT *Port, BOOL *CtsBefore, ULONG *Pulses);

/* Blocking helpers with a deadline. UartRead returns ERROR_SUCCESS with *Read = 0 on timeout. */
DWORD UartWrite(UART_PORT *Port, const void *Data, ULONG Length, ULONG TimeoutMs);
DWORD UartRead(UART_PORT *Port, void *Buffer, ULONG Capacity, ULONG *Read, ULONG TimeoutMs);

/* Ends a read pending on another thread; it returns what arrived so far (possibly nothing). */
void  UartCancelRead(UART_PORT *Port);
