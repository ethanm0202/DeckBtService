/*
 * uart_win32.c - user-mode access to the controller's UART. See uart_win32.h.
 */

/* Lean windows.h keeps winioctl.h out, whose SERIAL_IOC_* macros collide with ntddser.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <devioctl.h>
#include <initguid.h>     /* first, so the keys and GUIDs below get storage here */
#include <devpropdef.h>   /* before ntddser.h, which then defines DEVPKEY_DeviceInterface_Serial_PortName */
#include <ntddser.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <stdlib.h>
#include <wchar.h>

#include "uart_win32.h"

#pragma comment(lib, "cfgmgr32.lib")

/* The vendor driver's serial settings on this UART (docs/QCA2066.md). */
#define UART_FLOW_LIMIT        0x800u
#define UART_IOCTL_TIMEOUT_MS  2000u
#define UART_WAKE_PULSES       5u
#define UART_WAKE_PULSE_MS     5u

/* ------------------------------------------------------------------ discovery */

DWORD
UartFindInterface(const WCHAR *ControllerInstanceId, WCHAR *Path, DWORD PathChars,
                  WCHAR *PortName, DWORD PortNameChars)
{
    ULONG listChars = 0;
    WCHAR *list = NULL;
    DWORD result = ERROR_NOT_FOUND;
    CONFIGRET cr;

    cr = CM_Get_Device_Interface_List_SizeW(&listChars, (LPGUID)&GUID_DEVINTERFACE_COMPORT, NULL,
                                            CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS) {
        return CM_MapCrToWin32Err(cr, ERROR_GEN_FAILURE);
    }
    list = (WCHAR *)calloc(listChars + 1u, sizeof(WCHAR));
    if (list == NULL) {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    cr = CM_Get_Device_Interface_ListW((LPGUID)&GUID_DEVINTERFACE_COMPORT, NULL, list, listChars,
                                       CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS) {
        free(list);
        return CM_MapCrToWin32Err(cr, ERROR_GEN_FAILURE);
    }

    for (const WCHAR *iface = list; *iface != L'\0'; iface += wcslen(iface) + 1u) {
        WCHAR instance[MAX_DEVICE_ID_LEN];
        ULONG size = sizeof(instance);
        DEVPROPTYPE type;

        cr = CM_Get_Device_Interface_PropertyW(iface, &DEVPKEY_Device_InstanceId, &type,
                                               (PBYTE)instance, &size, 0);
        if (cr != CR_SUCCESS || type != DEVPROP_TYPE_STRING ||
            _wcsicmp(instance, ControllerInstanceId) != 0) {
            continue;
        }
        if (wcscpy_s(Path, PathChars, iface) != 0) {
            result = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        if (PortName != NULL && PortNameChars != 0) {
            size = PortNameChars * (ULONG)sizeof(WCHAR);
            PortName[0] = L'\0';
            cr = CM_Get_Device_Interface_PropertyW(iface, &DEVPKEY_DeviceInterface_Serial_PortName,
                                                   &type, (PBYTE)PortName, &size, 0);
            if (cr != CR_SUCCESS || type != DEVPROP_TYPE_STRING) {
                PortName[0] = L'\0';
            }
        }
        result = ERROR_SUCCESS;
        break;
    }
    free(list);
    return result;
}

/* ------------------------------------------------------------------ I/O plumbing */

static DWORD
Complete(UART_PORT *Port, OVERLAPPED *Overlapped, BOOL Started, ULONG TimeoutMs, DWORD *Transferred)
{
    DWORD error = Started ? ERROR_SUCCESS : GetLastError();

    *Transferred = 0;
    if (!Started && error != ERROR_IO_PENDING) {
        return error;
    }
    if (WaitForSingleObject(Overlapped->hEvent, TimeoutMs) != WAIT_OBJECT_0) {
        (void)CancelIoEx(Port->Handle, Overlapped);
    }
    if (!GetOverlappedResult(Port->Handle, Overlapped, Transferred, TRUE)) {
        error = GetLastError();
        return (error == ERROR_OPERATION_ABORTED) ? ERROR_TIMEOUT : error;
    }
    return ERROR_SUCCESS;
}

/* Each IOCTL has its own OVERLAPPED: the reader, writer and control paths issue them concurrently. */
static DWORD
Ioctl(UART_PORT *Port, DWORD Code, void *In, DWORD InLength, void *Out, DWORD OutLength)
{
    OVERLAPPED overlapped;
    DWORD transferred;
    DWORD error;
    BOOL started;

    ZeroMemory(&overlapped, sizeof(overlapped));
    overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (overlapped.hEvent == NULL) {
        return GetLastError();
    }
    started = DeviceIoControl(Port->Handle, Code, In, InLength, Out, OutLength, NULL, &overlapped);
    error = Complete(Port, &overlapped, started, UART_IOCTL_TIMEOUT_MS, &transferred);
    CloseHandle(overlapped.hEvent);
    return error;
}

static DWORD
SetHandflow(UART_PORT *Port, BOOL ManualRts)
{
    SERIAL_HANDFLOW flow;

    ZeroMemory(&flow, sizeof(flow));
    flow.ControlHandShake = SERIAL_CTS_HANDSHAKE;
    flow.FlowReplace = ManualRts ? SERIAL_RTS_CONTROL : SERIAL_RTS_HANDSHAKE;
    flow.XonLimit = UART_FLOW_LIMIT;
    flow.XoffLimit = UART_FLOW_LIMIT;
    return Ioctl(Port, IOCTL_SERIAL_SET_HANDFLOW, &flow, sizeof(flow), NULL, 0);
}

/* ------------------------------------------------------------------ public */

DWORD
UartOpen(UART_PORT *Port, const WCHAR *Path)
{
    ZeroMemory(Port, sizeof(*Port));
    Port->Handle = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, NULL);
    if (Port->Handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        Port->Handle = NULL;
        return error;
    }
    Port->ReadOverlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Port->WriteOverlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Port->ReadOverlapped.hEvent == NULL || Port->WriteOverlapped.hEvent == NULL) {
        DWORD error = GetLastError();
        UartClose(Port);
        return error;
    }
    return ERROR_SUCCESS;
}

void
UartClose(UART_PORT *Port)
{
    if (Port->Handle != NULL) {
        (void)CancelIoEx(Port->Handle, NULL);
        CloseHandle(Port->Handle);
    }
    if (Port->ReadOverlapped.hEvent != NULL) {
        CloseHandle(Port->ReadOverlapped.hEvent);
    }
    if (Port->WriteOverlapped.hEvent != NULL) {
        CloseHandle(Port->WriteOverlapped.hEvent);
    }
    ZeroMemory(Port, sizeof(*Port));
}

void
UartCancelRead(UART_PORT *Port)
{
    (void)CancelIoEx(Port->Handle, &Port->ReadOverlapped);
}

DWORD
UartConfigure(UART_PORT *Port, ULONG BaudRate)
{
    SERIAL_BAUD_RATE baud;
    SERIAL_LINE_CONTROL line;
    DWORD error;

    baud.BaudRate = BaudRate;
    error = Ioctl(Port, IOCTL_SERIAL_SET_BAUD_RATE, &baud, sizeof(baud), NULL, 0);
    if (error != ERROR_SUCCESS) {
        return error;
    }
    line.WordLength = 8;
    line.Parity = NO_PARITY;
    line.StopBits = STOP_BIT_1;
    error = Ioctl(Port, IOCTL_SERIAL_SET_LINE_CONTROL, &line, sizeof(line), NULL, 0);
    if (error != ERROR_SUCCESS) {
        return error;
    }
    return SetHandflow(Port, FALSE);
}

DWORD
UartSetTimeouts(UART_PORT *Port, ULONG ReadInterval, ULONG ReadTotalMultiplier,
                ULONG ReadTotalConstant, ULONG WriteTotalConstant)
{
    SERIAL_TIMEOUTS timeouts;

    ZeroMemory(&timeouts, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = ReadInterval;
    timeouts.ReadTotalTimeoutMultiplier = ReadTotalMultiplier;
    timeouts.ReadTotalTimeoutConstant = ReadTotalConstant;
    timeouts.WriteTotalTimeoutConstant = WriteTotalConstant;
    return Ioctl(Port, IOCTL_SERIAL_SET_TIMEOUTS, &timeouts, sizeof(timeouts), NULL, 0);
}

DWORD
UartGetModemStatus(UART_PORT *Port, ULONG *Status)
{
    *Status = 0;
    return Ioctl(Port, IOCTL_SERIAL_GET_MODEMSTATUS, NULL, 0, Status, sizeof(*Status));
}

DWORD
UartPurge(UART_PORT *Port)
{
    ULONG mask = SERIAL_PURGE_TXABORT | SERIAL_PURGE_RXABORT | SERIAL_PURGE_TXCLEAR | SERIAL_PURGE_RXCLEAR;

    return Ioctl(Port, IOCTL_SERIAL_PURGE, &mask, sizeof(mask), NULL, 0);
}

DWORD
UartWakeController(UART_PORT *Port, BOOL *CtsBefore, ULONG *Pulses)
{
    ULONG status = 0;
    DWORD error;

    *CtsBefore = FALSE;
    *Pulses = 0;
    error = UartGetModemStatus(Port, &status);
    if (error != ERROR_SUCCESS) {
        return error;
    }
    if (status & SERIAL_CTS_STATE) {
        *CtsBefore = TRUE;
        return ERROR_SUCCESS;
    }

    error = SetHandflow(Port, TRUE);
    for (ULONG i = 0; error == ERROR_SUCCESS && i < UART_WAKE_PULSES; i++) {
        (*Pulses)++;
        error = Ioctl(Port, IOCTL_SERIAL_CLR_RTS, NULL, 0, NULL, 0);
        Sleep(UART_WAKE_PULSE_MS);
        if (error == ERROR_SUCCESS) {
            error = Ioctl(Port, IOCTL_SERIAL_SET_RTS, NULL, 0, NULL, 0);
        }
        Sleep(UART_WAKE_PULSE_MS);
        if (error == ERROR_SUCCESS) {
            error = UartGetModemStatus(Port, &status);
        }
        if (error == ERROR_SUCCESS && (status & SERIAL_CTS_STATE)) {
            break;
        }
    }
    /* RTS always ends asserted and back under the handshake. */
    (void)Ioctl(Port, IOCTL_SERIAL_SET_RTS, NULL, 0, NULL, 0);
    {
        DWORD restore = SetHandflow(Port, FALSE);
        if (error == ERROR_SUCCESS) {
            error = restore;
        }
    }
    if (error == ERROR_SUCCESS && !(status & SERIAL_CTS_STATE)) {
        error = ERROR_NOT_READY;
    }
    return error;
}

DWORD
UartWrite(UART_PORT *Port, const void *Data, ULONG Length, ULONG TimeoutMs)
{
    DWORD written;
    BOOL started;
    DWORD error;

    (void)ResetEvent(Port->WriteOverlapped.hEvent);
    started = WriteFile(Port->Handle, Data, Length, NULL, &Port->WriteOverlapped);
    error = Complete(Port, &Port->WriteOverlapped, started, TimeoutMs, &written);
    if (error == ERROR_SUCCESS && written != Length) {
        error = ERROR_WRITE_FAULT;
    }
    return error;
}

DWORD
UartRead(UART_PORT *Port, void *Buffer, ULONG Capacity, ULONG *Read, ULONG TimeoutMs)
{
    BOOL started;
    DWORD error;

    (void)ResetEvent(Port->ReadOverlapped.hEvent);
    started = ReadFile(Port->Handle, Buffer, Capacity, NULL, &Port->ReadOverlapped);
    error = Complete(Port, &Port->ReadOverlapped, started, TimeoutMs, Read);
    if (error == ERROR_TIMEOUT) {
        return ERROR_SUCCESS;   /* *Read holds what arrived before the deadline */
    }
    return error;
}
