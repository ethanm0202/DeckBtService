/*
 * deckbt_usbip.c - DeckBtService: the USB/IP server for the Bluetooth radio, its lifecycle, and the
 * Windows service around it.
 *
 * Listens on 127.0.0.1 only. One imported session at a time; OP_REQ_DEVLIST is answered while a
 * session is active. Backends:
 *   stub - the synthetic HCI stub (src/common/hci_stub.c): Windows enumerates the emulated radio
 *          without touching the real controller;
 *   uart - the QCA2066 over its SerCx2-published UART (qca_backend.c): firmware bring-up first,
 *          then the radio is served. Needs administrator rights and nothing else owning the UART.
 *
 * Once the backend is up the program attaches its own device with usbip-win2's usbip.exe (unless
 * --no-attach). Before sleep it detaches and hands the controller back (the controller loses its
 * firmware in S3); after resume it brings it up and attaches again.
 *
 *   deckbt-usbip.exe [options]              run in the console; Ctrl+C stops
 *   deckbt-usbip.exe install [options]      register the DeckBtService service (LocalSystem, automatic)
 *   deckbt-usbip.exe uninstall              stop and remove it
 *
 * Threads: accept, one session thread reading URB commands, the pacing thread that drives
 * UsbipDeviceTick on a high-resolution waitable timer, the backend's reader and writer, and the
 * main (or service) thread that starts the radio and restarts it after resume. g_Lock is the
 * controller lock of hci_transport.h: it serialises every call into the device and the transport.
 * g_StateLock serialises radio start and stop.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <powrprof.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>
#include <aclapi.h>
#include <sddl.h>

#include "usbip_proto.h"
#include "usbip_device.h"
#include "qca_backend.h"
#include "handsfree.h"
#include "mmcss.h"
#include "../include/hci_stub.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

#define DEFAULT_PORT   3241u
#define DEFAULT_BUSID  "1-1"
#define BUS_NUM        1u
#define DEV_NUM        2u
#define START_RETRY_MS 3000u
#define CTS_START_ATTEMPTS 3u   /* repeated physical wake failures cannot be fixed by an SCM loop */
#define HANDSHAKE_MS   2000u
#define MAX_HANDSHAKES 8u
#define SESSION_CLOSE_MS 3000u   /* after FIN, how long the importer gets to close its end */
#define DISCONNECT_MS    1000u   /* stop/suspend: how long peers' links get to close cleanly */
#define CONNECTION_LOG_MS 10000u /* other clients' connections: at most one log line per interval */
#define HANDSFREE_GRACE_MS 5000u /* Windows opens RFCOMM within ~0.1 s of a headset link, if at all */
#define HANDSFREE_POLL_MS  1000u
#define LOG_ROTATE_BYTES (4u * 1024u * 1024u)
#define USBIP_QUERY_MS 3000u
#define USBIP_ATTACH_MS 60000u
#define PROCESS_KILL_MS 1000u
#define SESSION_CANCEL_MS 2000u
#define STOP_DEADLINE_MS 45000u
#define WATCHDOG_POLL_MS 100u
#define SERVICE_NAME         L"DeckBtService"
#define SERVICE_DISPLAY_NAME L"DeckBtService (Steam Deck Bluetooth)"

static CRITICAL_SECTION g_Lock;
static USBIP_DEVICE    *g_Device;
static HCI_TRANSPORT    g_Transport;
static HCI_STUB         g_Stub;
static SOCKET           g_Listener = INVALID_SOCKET;
static SOCKET           g_Session = INVALID_SOCKET;   /* under g_Lock */
static HANDLE           g_Kick;
static volatile LONG    g_Stop;
static unsigned short   g_Port = DEFAULT_PORT;
static char             g_BusId[USBIP_BUSID_SIZE] = DEFAULT_BUSID;
static HANDLE           g_LogFile = INVALID_HANDLE_VALUE;
static HANDLE           g_LogDirectory = INVALID_HANDLE_VALUE;
static SRWLOCK          g_LogLock = SRWLOCK_INIT;
static ULONGLONG        g_RotateRetryAt;        /* under g_LogLock: after a failed rename */
static PSECURITY_DESCRIPTOR g_LogSecurity;
static int              g_Quiet;
static WCHAR            g_StopFile[MAX_PATH];
static WCHAR            g_LogPath[MAX_PATH];
static WCHAR            g_UsbipExe[MAX_PATH];
static int              g_ServiceMode;
static int              g_NoAttach;
static int              g_AllowUserImport;       /* console stub testing only */
static int              g_AttachedPort;          /* usbip-win2 root-hub port; 0 = not attached */
static int              g_Ready;                 /* under g_Lock: imports accepted */
static int              g_Running;               /* under g_StateLock: backend up */
static int              g_Suspended;             /* under g_StateLock */
static SRWLOCK          g_StateLock = SRWLOCK_INIT;
static HANDLE           g_StopEvent;
static HANDLE           g_ResumeEvent;
static HANDLE           g_SuspendPending;       /* manual reset: a suspend waits for g_StateLock */
static HANDLE           g_Pacing;
static HANDLE           g_Acceptor;
static HANDLE           g_SessionThread;        /* joined before stopping the backend */
static ULONG            g_StartAttempts = 1;
static int              g_UseUart;
static QCA_BACKEND     *g_Qca;
static WCHAR            g_Controller[200] = L"ACPI\\AMDI0020\\4";
static WCHAR            g_FirmwareDir[MAX_PATH];
static LARGE_INTEGER    g_QpcFrequency;
static LARGE_INTEGER    g_QpcStart;
static char             g_ControllerFault[160]; /* under g_Lock */
static __declspec(align(8)) volatile LONG64 g_StopDeadline;
static __declspec(align(8)) volatile LONG64 g_SuspendDeadline;
static volatile LONG    g_Fatal;

static int OpenLogFile(void);
static int RotateLog(void);

/* ------------------------------------------------------------------ time and logging */

/* Monotonic time in 100 ns units, the unit src/common/sco_usb.c schedules in. */
static unsigned long long
Now100ns(void)
{
    LARGE_INTEGER counter;
    unsigned long long c;
    unsigned long long f = (unsigned long long)g_QpcFrequency.QuadPart;

    QueryPerformanceCounter(&counter);
    c = (unsigned long long)counter.QuadPart;
    return (c / f) * 10000000ull + ((c % f) * 10000000ull) / f;
}

static void
Log(const char *Format, ...)
{
    LARGE_INTEGER counter;
    double seconds;
    char line[1200];
    char message[1024];
    DWORD written;
    int length;
    va_list args;

    QueryPerformanceCounter(&counter);
    seconds = (double)(counter.QuadPart - g_QpcStart.QuadPart) / (double)g_QpcFrequency.QuadPart;
    va_start(args, Format);
    (void)vsnprintf(message, sizeof(message), Format, args);
    va_end(args);
    length = snprintf(line, sizeof(line), "%10.3f %s\r\n", seconds, message);
    AcquireSRWLockExclusive(&g_LogLock);
    if (!g_Quiet && length >= 2) {
        /* stdout is in text mode: it expands the newline itself. */
        (void)fwrite(line, 1, (size_t)length - 2u, stdout);
        (void)fputc('\n', stdout);
        fflush(stdout);
    }
    if (g_LogFile != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size;
        /* A reader without delete sharing blocks the rename: retry every 10 s, not every line. */
        if (GetFileSizeEx(g_LogFile, &size) && size.QuadPart + length > LOG_ROTATE_BYTES &&
            GetTickCount64() >= g_RotateRetryAt && !RotateLog()) {
            g_RotateRetryAt = GetTickCount64() + 10000u;
        }
        if (g_LogFile != INVALID_HANDLE_VALUE) {
            (void)WriteFile(g_LogFile, line, (DWORD)length, &written, NULL);
        }
    }
    ReleaseSRWLockExclusive(&g_LogLock);
}

/* Never wait on CRT/controller/log locks when a worker may be stuck holding them. */
static DWORD WINAPI
FatalLog(LPVOID Parameter)
{
    char line[256];
    DWORD written;
    DWORD length = (DWORD)snprintf(line, sizeof(line), "fatal: %s; terminating for SCM recovery\r\n",
                                  (const char *)Parameter);
    if (TryAcquireSRWLockExclusive(&g_LogLock)) {
        if (g_LogFile != INVALID_HANDLE_VALUE) {
            (void)WriteFile(g_LogFile, line, length, &written, NULL);
        }
        ReleaseSRWLockExclusive(&g_LogLock);
    }
    (void)WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, length, &written, NULL);
    return 0;
}

static void
FatalStall(const char *Why)
{
    if (InterlockedCompareExchange(&g_Fatal, 1, 0) == 0) {
        HANDLE logger = CreateThread(NULL, 0, FatalLog, (LPVOID)Why, 0, NULL);
        if (logger != NULL) {
            /* Even a blocked disk or redirected stderr must not stall fatal termination. */
            (void)WaitForSingleObject(logger, 100);
            CloseHandle(logger);
        }
    }
    (void)TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
}

static DWORD WINAPI
StopWatchdog(LPVOID Parameter)
{
    (void)Parameter;
    for (;;) {
        LONG64 now = (LONG64)GetTickCount64();
        LONG64 stop = InterlockedCompareExchange64(&g_StopDeadline, 0, 0);
        LONG64 suspend = InterlockedCompareExchange64(&g_SuspendDeadline, 0, 0);
        if ((stop != 0 && now >= stop) || (suspend != 0 && now >= suspend)) {
            FatalStall("radio stop/suspend exceeded 45 seconds");
        }
        Sleep(WATCHDOG_POLL_MS);
    }
}

static const char *
HexBytes(const unsigned char *Data, unsigned long Length, char *Out, size_t OutSize)
{
    size_t used = 0;

    Out[0] = '\0';
    for (unsigned long i = 0; i < Length && used + 4 < OutSize; i++) {
        used += (size_t)snprintf(Out + used, OutSize - used, "%02X ", Data[i]);
    }
    return Out;
}

/* Client-supplied text for one log line: printable ASCII as is; control bytes, \ and ' as \xHH. */
static const char *
EscapeForLog(const char *Text, char *Out, size_t OutSize)
{
    size_t used = 0;

    for (const unsigned char *p = (const unsigned char *)Text; *p != '\0' && used + 5u <= OutSize; p++) {
        if (*p >= 0x20u && *p < 0x7Fu && *p != '\\' && *p != '\'') {
            Out[used++] = (char)*p;
        } else {
            used += (size_t)snprintf(Out + used, OutSize - used, "\\x%02X", *p);
        }
    }
    Out[used] = '\0';
    return Out;
}

/* EP0 trace, under g_Lock. HCI commands are logged by opcode; standard requests in full. */
static void
TraceControl(void *Context, const unsigned char Setup[8], const unsigned char *Data,
             unsigned long Length, long Status)
{
    char setupHex[32];
    char dataHex[200];

    (void)Context;
    if (Setup[0] == 0x20 && Setup[1] == 0 && Data != NULL && Length >= 3) {
        Log("EP0 HCI cmd 0x%04X plen %u -> %s", (unsigned)(Data[0] | (Data[1] << 8)), Data[2],
            Status == 0 ? "ok" : "STALL");
        return;
    }
    Log("EP0 setup [%s] -> %s len %lu %s", HexBytes(Setup, 8, setupHex, sizeof(setupHex)),
        Status == 0 ? "ok" : "STALL", Length,
        (Data != NULL) ? HexBytes(Data, Length < 32 ? Length : 32, dataHex, sizeof(dataHex)) : "");
}

static void
LogStats(const char *Why)
{
    const USBIP_DEVICE_STATS *s = &g_Device->Stats;

    Log("stats (%s): ctl %lu hci %lu stalls %lu evt %lu aclOut %lu aclIn %lu alt %u altChanges %lu "
        "portResets %lu scoOut %lu/%luB hci %lu rej %lu scoIn %lu/%luB scoRejUrbs %lu unlink %lu/%lu missed "
        "(answered %lu, irrecoverable %lu; replies lost %lu, redelivered %lu) order holds %lu/%lu timed out maxLateUs %lu",
        Why, s->ControlRequests, s->HciCommands, s->Stalls, s->EventsIn, s->AclOut, s->AclIn,
        g_Device->ScoAlt, s->AltChanges, s->PortResets, s->ScoOutUrbs, s->ScoOutBytes, s->ScoOutHci,
        s->ScoOutRejected, s->ScoInUrbs, s->ScoInBytes, s->ScoRejectedUrbs, s->Unlinks,
        s->UnlinksMissed, s->UnlinksAnswered, s->UnlinksIrrecoverable, s->RepliesLost, s->Redelivered,
        s->OrderHolds, s->OrderHoldTimeouts, s->MaxLateUs);
    if (g_Qca != NULL) {
        const QCA_BACKEND_STATS *q = &g_Qca->Stats;
        const HCI_BRIDGE_COUNTERS *c = &g_Qca->Bridge.Counters;

        Log("uart (%s): rx %llu B tx %llu B, readErr %lu writeErr %lu txFull %lu, IBS wakeInd %lu sleepInd %lu "
            "ack %lu ackCtsLow %lu wakeAckGap %lu; bridge cmd %lu/%lu held %lu evt %lu/%lu vendorDropped %lu aclOut %lu "
            "aclIn %lu noCredit %lu (pool %u+%u LE, free %u/%u) scoOut %lu scoIn %lu; shutdown disconnects %lu "
            "refused %lu, commands withheld %lu",
            Why, q->BytesRead, q->BytesWritten, q->ReadErrors, q->WriteErrors, q->TxQueueFull,
            q->IbsWakeIndRx, q->IbsSleepIndRx, q->IbsWakeAckTx, q->IbsAckCtsLow,
            q->IbsWakeIndRx - q->IbsWakeAckTx,
            c->CommandsSentToWire, c->CommandsSubmitted, c->CommandsHeld, c->EventsQueued, c->EventsReceived,
            c->EventsSuppressedVendor, c->AclSentToWire, c->AclQueued, c->AclDroppedNoCredit,
            g_Qca->Bridge.TotalAclBuffers, g_Qca->Bridge.LeTotalAclBuffers, g_Qca->Bridge.AvailableAclCredits,
            g_Qca->Bridge.AvailableLeCredits,
            c->ScoSentToWire, c->ScoReceived, c->ShutdownDisconnects, c->ShutdownDisconnectsRefused,
            c->CommandsWithheld);
    }
}

/* ------------------------------------------------------------------ socket helpers */

static int
RecvAll(SOCKET Socket, void *Buffer, unsigned long Length)
{
    char *p = (char *)Buffer;

    while (Length != 0) {
        int chunk = (Length > 0x40000000ul) ? 0x40000000 : (int)Length;
        int got = recv(Socket, p, chunk, 0);

        if (got <= 0) {
            return 0;
        }
        p += got;
        Length -= (unsigned long)got;
    }
    return 1;
}

static int
SendAll(SOCKET Socket, const void *Buffer, unsigned long Length)
{
    const char *p = (const char *)Buffer;

    while (Length != 0) {
        int chunk = (Length > 0x40000000ul) ? 0x40000000 : (int)Length;
        int sent = send(Socket, p, chunk, 0);

        if (sent <= 0) {
            return 0;
        }
        p += sent;
        Length -= (unsigned long)sent;
    }
    return 1;
}

/* USBIP_DEVICE_SEND, under g_Lock. */
static int
DeviceSend(void *Context, const unsigned char *Data, unsigned long Length)
{
    (void)Context;
    if (g_Session == INVALID_SOCKET) {
        return 0;
    }
    if (!SendAll(g_Session, Data, Length)) {
        shutdown(g_Session, SD_BOTH);   /* wake the reader and enter the normal recovery path */
        return 0;
    }
    return 1;
}

/* The process owning the client end of a loopback connection (4 = System, i.e. a kernel client). */
static DWORD
PeerProcessId(const struct sockaddr_in *Peer)
{
    DWORD size = 0;
    DWORD pid = 0;
    PMIB_TCPTABLE_OWNER_PID table = NULL;
    DWORD result = GetExtendedTcpTable(NULL, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);

    for (unsigned int attempt = 0; result == ERROR_INSUFFICIENT_BUFFER && attempt < 4; attempt++) {
        DWORD capacity;
        free(table);
        if (size > MAXDWORD - 4096u) {
            return 0;
        }
        capacity = size + 4096u; /* connections can arrive between sizing and copying */
        table = (PMIB_TCPTABLE_OWNER_PID)malloc(capacity);
        if (table == NULL) {
            return 0;
        }
        size = capacity;
        result = GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
    }
    if (result == NO_ERROR && table != NULL) {
        for (DWORD i = 0; i < table->dwNumEntries; i++) {
            const MIB_TCPROW_OWNER_PID *row = &table->table[i];

            if (row->dwState == MIB_TCP_STATE_ESTAB &&
                (unsigned short)row->dwLocalPort == Peer->sin_port &&
                row->dwLocalAddr == Peer->sin_addr.s_addr &&
                row->dwRemoteAddr == htonl(INADDR_LOOPBACK) &&
                ntohs((unsigned short)row->dwRemotePort) == g_Port) {
                pid = row->dwOwningPid;
                break;
            }
        }
    }
    free(table);
    return pid;
}

/* ------------------------------------------------------------------ backend */

/* HCI_TRANSPORT_NOTIFY. The stub answers synchronously and never calls this; an asynchronous
 * backend calls it without g_Lock held. */
static void
TransportNotify(void *Context, HCI_STREAM Stream)
{
    int holding = 0;

    (void)Context;
    if (Stream == HciStreamSco) {
        return;   /* SCO IN is pulled by the pacing tick */
    }
    EnterCriticalSection(&g_Lock);
    if (g_Session != INVALID_SOCKET) {
        UsbipDeviceDrain(g_Device, Now100ns());
        holding = g_Device->HoldSince != 0;
    }
    LeaveCriticalSection(&g_Lock);
    if (holding) {
        (void)SetEvent(g_Kick);   /* the pacing tick releases the hold if no read arrives */
    }
}

/* Backend workers only publish a reason; lifecycle teardown must join them elsewhere. */
static void
ControllerFault(void *Context, const char *Why)
{
    (void)Context;
    EnterCriticalSection(&g_Lock);
    (void)snprintf(g_ControllerFault, sizeof(g_ControllerFault), "controller fault: %s", Why);
    g_Ready = 0;
    LeaveCriticalSection(&g_Lock);
    (void)SetEvent(g_ResumeEvent);
}

/* ------------------------------------------------------------------ threads */

/* Owns Parameter, the high-resolution timer ServerStart created. Runs until g_Stop. */
static DWORD WINAPI
PacingThread(LPVOID Parameter)
{
    HANDLE timer = (HANDLE)Parameter;
    HANDLE waits[2];
    HANDLE mmcss = MmcssEnter();
    int timerFailing = 0;

    if (mmcss == NULL) {
        Log("pacing: MMCSS registration failed (%lu); normal priority", GetLastError());
    }
    waits[0] = g_Kick;
    waits[1] = timer;
    while (!g_Stop) {
        unsigned long long next = 0;
        unsigned long long now;

        EnterCriticalSection(&g_Lock);
        now = Now100ns();
        if (g_Session != INVALID_SOCKET) {
            next = UsbipDeviceTick(g_Device, now);
        }
        LeaveCriticalSection(&g_Lock);

        if (next == 0) {
            (void)WaitForSingleObject(g_Kick, INFINITE);
        } else if (next > now) {
            LARGE_INTEGER due;

            due.QuadPart = -(LONGLONG)(next - now);
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
                timerFailing = 0;
                (void)WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            } else {
                /* Coarser pacing until the timer works again, never a busy loop. */
                if (!timerFailing) {
                    Log("pacing: SetWaitableTimer failed (%lu); waiting in whole milliseconds", GetLastError());
                    timerFailing = 1;
                }
                (void)WaitForSingleObject(g_Kick, (DWORD)((next - now + 9999u) / 10000u));
            }
        }
    }
    MmcssLeave(mmcss);
    CloseHandle(timer);
    return 0;
}

static DWORD WINAPI
SessionThread(LPVOID Parameter)
{
    SOCKET socket = (SOCKET)(ULONG_PTR)Parameter;
    unsigned char header[USBIP_URB_HEADER_SIZE];
    unsigned char *payload = (unsigned char *)malloc(1024u * 1024u + USBIP_MAX_ISO_PACKETS * USBIP_ISO_DESC_SIZE);
    unsigned long urbs = 0;
    const char *why = "connection closed";
    HANDLE mmcss = MmcssEnter();

    if (mmcss == NULL) {
        Log("session: MMCSS registration failed (%lu); normal priority", GetLastError());
    }
    if (payload == NULL) {
        why = "out of memory";
    }
    while (payload != NULL) {
        unsigned long length;
        int ok;

        if (!RecvAll(socket, header, sizeof(header))) {
            break;
        }
        length = UsbipCommandPayloadLength(header);
        if (length == 0xFFFFFFFFul) {
            why = "malformed command";
            break;
        }
        if (length != 0 && !RecvAll(socket, payload, length)) {
            break;
        }
        EnterCriticalSection(&g_Lock);
        ok = UsbipDeviceHandle(g_Device, header, payload, length, Now100ns());
        LeaveCriticalSection(&g_Lock);
        (void)SetEvent(g_Kick);
        urbs++;
        if (!ok) {
            why = g_Device->Poisoned
                ? "unrecoverable unlink: an event or ACL reply aged out of the replay history"
                : g_Device->Broken ? "send failed" : "protocol violation";
            break;
        }
    }

    EnterCriticalSection(&g_Lock);
    Log("session ended: %s after %lu commands", why, urbs);
    LogStats("session end");
    g_Session = INVALID_SOCKET;
    if (g_Ready && !g_NoAttach) {
        g_Ready = 0;   /* no new import until the backend has been reset */
        (void)SetEvent(g_ResumeEvent);
    }
    LeaveCriticalSection(&g_Lock);
    (void)SetEvent(g_Kick);
    shutdown(socket, SD_BOTH);
    closesocket(socket);
    free(payload);
    MmcssLeave(mmcss);
    return 0;
}

/*
 * Any local user can connect, so connections other than the kernel's imports are logged at most
 * once per CONNECTION_LOG_MS; otherwise a connection loop could flush the size-capped log. The
 * listener thread alone calls this. Returns 1 with the connections left out since the last line.
 */
static int
ConnectionLogDue(ULONG *Suppressed)
{
    static ULONGLONG next;
    static ULONG suppressed;
    ULONGLONG now = GetTickCount64();

    if (now < next) {
        suppressed++;
        return 0;
    }
    next = now + CONNECTION_LOG_MS;
    *Suppressed = suppressed;
    suppressed = 0;
    return 1;
}

/* One accepted connection: an OP request, answered, and for a successful import a session. */
static void
ServeConnection(SOCKET Socket, const struct sockaddr_in *Peer, DWORD Pid, const unsigned char *Op)
{
    unsigned char reply[USBIP_OP_HEADER_SIZE + 4u + USBIP_USB_DEVICE_SIZE + 2u * USBIP_USB_INTERFACE_SIZE];
    unsigned short version;
    unsigned short code;
    ULONG suppressed = 0;
    int logged = 1;

    version = UsbipGet16(Op);
    code = UsbipGet16(Op + 2);
    if (Pid == 4 && code == USBIP_OP_REQ_IMPORT) {
        Log("connection from 127.0.0.1:%u (pid %lu): op 0x%04X version 0x%04X",
            ntohs(Peer->sin_port), Pid, code, version);
    } else if (ConnectionLogDue(&suppressed)) {
        Log("connection from 127.0.0.1:%u (pid %lu): op 0x%04X version 0x%04X (%lu other connection(s) not logged)",
            ntohs(Peer->sin_port), Pid, code, version, suppressed);
    } else {
        logged = 0;
    }
    if (version != USBIP_VERSION || UsbipGet32(Op + 4) != USBIP_ST_OK) {
        closesocket(Socket);
        return;
    }

    if (code == USBIP_OP_REQ_DEVLIST) {
        unsigned long size;

        UsbipPut16(reply, USBIP_VERSION);
        UsbipPut16(reply + 2, USBIP_OP_REP_DEVLIST);
        UsbipPut32(reply + 4, USBIP_ST_OK);
        UsbipPut32(reply + 8, 1);
        size = UsbipDeviceDescribe(reply + 12, sizeof(reply) - 12u, g_BusId, BUS_NUM, DEV_NUM, 1);
        (void)SendAll(Socket, reply, 12u + size);
        closesocket(Socket);
        return;
    }

    if (code == USBIP_OP_REQ_IMPORT) {
        char busId[USBIP_BUSID_SIZE + 1];
        unsigned long status = USBIP_ST_OK;
        unsigned long size = 0;

        memcpy(busId, Op + USBIP_OP_HEADER_SIZE, USBIP_BUSID_SIZE);
        busId[USBIP_BUSID_SIZE] = '\0';

        EnterCriticalSection(&g_Lock);
        if (Pid != 4 && !g_AllowUserImport) {
            status = USBIP_ST_NA;
        } else if (strcmp(busId, g_BusId) != 0) {
            status = USBIP_ST_NODEV;
        } else if (!g_Ready) {
            status = USBIP_ST_NA;
        } else if (g_Session != INVALID_SOCKET) {
            status = USBIP_ST_DEV_BUSY;
        }
        UsbipPut16(reply, USBIP_VERSION);
        UsbipPut16(reply + 2, USBIP_OP_REP_IMPORT);
        UsbipPut32(reply + 4, status);
        if (status == USBIP_ST_OK) {
            size = UsbipDeviceDescribe(reply + 8, sizeof(reply) - 8u, g_BusId, BUS_NUM, DEV_NUM, 0);
        }
        if (!SendAll(Socket, reply, 8u + size) || status != USBIP_ST_OK) {
            LeaveCriticalSection(&g_Lock);
            if (logged) {
                char escaped[USBIP_BUSID_SIZE * 4u + 1u];

                Log("import of '%s' refused (status %lu)", EscapeForLog(busId, escaped, sizeof(escaped)), status);
            }
            closesocket(Socket);
            return;
        }
        {
            BOOL noDelay = TRUE;
            (void)setsockopt(Socket, IPPROTO_TCP, TCP_NODELAY, (const char *)&noDelay, sizeof(noDelay));
        }
        if (g_SessionThread != NULL) {
            CloseHandle(g_SessionThread);
        }
        g_Session = Socket;
        UsbipDeviceBeginSession(g_Device);
        g_SessionThread = CreateThread(NULL, 0, SessionThread, (LPVOID)(ULONG_PTR)Socket, 0, NULL);
        if (g_SessionThread == NULL) {
            g_Session = INVALID_SOCKET;
            if (!g_NoAttach) {
                g_Ready = 0;
                (void)SetEvent(g_ResumeEvent);
            }
            closesocket(Socket);
        } else {
            Log("imported '%s' (pid %lu); session started", busId, Pid);
        }
        LeaveCriticalSection(&g_Lock);
        return;
    }

    Log("unsupported op 0x%04X; closing", code);
    closesocket(Socket);
}

/* ------------------------------------------------------------------ usbip-win2 client */

/*
 * Runs usbip.exe with Arguments, capturing its output. The client's own driver then connects back
 * to this server (from the kernel) to import the device, so the accept thread must be running.
 */
static DWORD
RunUsbip(const WCHAR *Arguments, char *Output, DWORD OutputSize, DWORD *ExitCode,
         DWORD TimeoutMs, int Interruptible)
{
    WCHAR commandLine[1024];
    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
    STARTUPINFOEXW startup;
    SIZE_T attributeBytes = 0;
    BOOL created = FALSE;
    PROCESS_INFORMATION process = { 0 };
    HANDLE readPipe = NULL;
    HANDLE writePipe = NULL;
    DWORD used = 0;
    DWORD error = ERROR_SUCCESS;
    ULONGLONG deadline = GetTickCount64() + TimeoutMs;

    *ExitCode = (DWORD)-1;
    Output[0] = '\0';
    if (Interruptible && (WaitForSingleObject(g_StopEvent, 0) == WAIT_OBJECT_0 ||
                          WaitForSingleObject(g_SuspendPending, 0) == WAIT_OBJECT_0)) {
        return ERROR_CANCELLED;
    }
    if (_snwprintf_s(commandLine, ARRAYSIZE(commandLine), _TRUNCATE, L"\"%s\" %s", g_UsbipExe, Arguments) < 0 ||
        !CreatePipe(&readPipe, &writePipe, &inherit, 0)) {
        return ERROR_INVALID_PARAMETER;
    }
    (void)SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    ZeroMemory(&startup, sizeof(startup));
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = writePipe;
    startup.StartupInfo.hStdError = writePipe;
    (void)InitializeProcThreadAttributeList(NULL, 1, 0, &attributeBytes);
    startup.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attributeBytes);
    if (startup.lpAttributeList == NULL) {
        error = ERROR_NOT_ENOUGH_MEMORY;
    } else if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes)) {
        error = GetLastError();
    } else {
        /* Never lend sockets or the UART to usbip.exe: an orphan would keep the radio/port owned. */
        if (UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      &writePipe, sizeof(writePipe), NULL, NULL)) {
            created = CreateProcessW(NULL, commandLine, NULL, NULL, TRUE,
                                     CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                     NULL, NULL, &startup.StartupInfo, &process);
        }
        if (!created) {
            error = GetLastError();
        }
        DeleteProcThreadAttributeList(startup.lpAttributeList);
    }
    free(startup.lpAttributeList);
    if (!created) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return error;
    }
    CloseHandle(writePipe);
    for (;;) {
        DWORD available = 0;
        DWORD got = 0;
        char discard[256];
        DWORD capacity = OutputSize - used - 1u;
        DWORD exited = WaitForSingleObject(process.hProcess, 0);
        HANDLE waits[3] = { process.hProcess, g_StopEvent, g_SuspendPending };

        /* A stop, or a suspend waiting for g_StateLock, must not sit out a 60 s attach. */
        if (Interruptible && (WaitForSingleObject(g_StopEvent, 0) == WAIT_OBJECT_0 ||
                              WaitForSingleObject(g_SuspendPending, 0) == WAIT_OBJECT_0)) {
            error = ERROR_CANCELLED;
            (void)TerminateProcess(process.hProcess, error);
            (void)WaitForSingleObject(process.hProcess, PROCESS_KILL_MS);
            break;
        }

        if (PeekNamedPipe(readPipe, NULL, 0, NULL, &available, NULL) && available != 0) {
            DWORD count = available < (capacity ? capacity : sizeof(discard)) ?
                          available : (capacity ? capacity : (DWORD)sizeof(discard));
            if (!ReadFile(readPipe, capacity ? Output + used : discard, count, &got, NULL)) {
                break;
            }
            if (capacity) {
                used += got;
            }
        } else if (exited == WAIT_OBJECT_0) {
            break;
        } else {
            (void)WaitForMultipleObjects(Interruptible ? 3u : 1u, waits, FALSE, 20);
        }
        if (GetTickCount64() >= deadline) {
            (void)TerminateProcess(process.hProcess, ERROR_TIMEOUT);
            (void)WaitForSingleObject(process.hProcess, PROCESS_KILL_MS);
            error = ERROR_TIMEOUT;
            break;
        }
    }
    Output[used] = '\0';
    (void)GetExitCodeProcess(process.hProcess, ExitCode);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(readPipe);
    for (char *p = Output; *p != '\0'; p++) {
        if (*p == '\r' || *p == '\n') {
            *p = ' ';
        }
    }
    return error;
}

static int
SessionLive(void)
{
    int live;

    EnterCriticalSection(&g_Lock);
    live = g_Session != INVALID_SOCKET;
    LeaveCriticalSection(&g_Lock);
    return live;
}

/* The usbip-win2 root-hub port currently holding this server's device, from `usbip port`; 0 if none. */
static int
FindAttachedPort(int Interruptible)
{
    char output[4096];
    char url[64];
    DWORD exitCode;
    const char *hit;
    const char *port = NULL;
    size_t urlLength;

    if (RunUsbip(L"port", output, sizeof(output), &exitCode, USBIP_QUERY_MS, Interruptible) != ERROR_SUCCESS ||
        exitCode != 0) {
        return 0;
    }
    (void)snprintf(url, sizeof(url), "usbip://127.0.0.1:%u/%s", g_Port, g_BusId);
    urlLength = strlen(url);
    for (hit = strstr(output, url); hit != NULL; hit = strstr(hit + 1, url)) {
        if (hit[urlLength] == ' ' || hit[urlLength] == '\0') {
            break;
        }
    }
    if (hit == NULL) {
        return 0;
    }
    for (const char *p = strstr(output, "Port "); p != NULL && p < hit; p = strstr(p + 1, "Port ")) {
        port = p;
    }
    return port != NULL ? atoi(port + strlen("Port ")) : 0;
}

/*
 * Attaches this server's device through usbip-win2; records the root-hub port for the detach.
 *
 * Always low-latency receive mode. usbip-win2's default zero-copy mode (0.9.8.0 and 0.9.8.1,
 * drivers/ude/wsk_receive_irp.cpp recv_loop) locks a URB's bare TransferBuffer with
 * MmProbeAndLockPages, receives into it, queues the URB's completion, and only unlocks the pages
 * when the next header is received. The upper driver may free the buffer in between: a page freed
 * while still locked is bugcheck 0x4E PFN_LIST_CORRUPT (0x9A), seen in testing. Low-latency
 * mode copies from the driver's own ring buffer and never locks URB pages on the receive path. It
 * is also upstream's recommendation for small, frequent transfers such as HCI traffic.
 */
static DWORD
Attach(void)
{
    WCHAR arguments[160];
    char output[1024];
    DWORD exitCode;
    DWORD error;
    const char *port;

    /* --once: this service owns re-attachment; a failed attach must not leave usbip-win2 retrying. */
    (void)_snwprintf_s(arguments, ARRAYSIZE(arguments), _TRUNCATE,
                       L"--tcp-port %u attach -r 127.0.0.1 -b %S --receive-mode low-latency --once",
                       g_Port, g_BusId);
    error = RunUsbip(arguments, output, sizeof(output), &exitCode, USBIP_ATTACH_MS, 1);
    if (error == ERROR_CANCELLED) {
        return error;
    }
    port = strstr(output, "attached to port ");
    if (error == ERROR_SUCCESS && exitCode == 0 && port != NULL) {
        g_AttachedPort = atoi(port + strlen("attached to port "));
        Log("attach: usbip-win2 port %d", g_AttachedPort);
        return ERROR_SUCCESS;
    }
    /*
     * After an unexpected disconnect usbip-win2 retries the import by itself. If one of its attempts
     * won the race, this attach was refused as busy, but the device is attached: keep it.
     */
    if (SessionLive()) {
        g_AttachedPort = FindAttachedPort(1);
        if (WaitForSingleObject(g_StopEvent, 0) == WAIT_OBJECT_0) {
            return ERROR_CANCELLED;
        }
        Log("attach: usbip.exe reported failure (Win32 %lu, exit %ld), but usbip-win2 already imported "
            "the device; port %d", error, (long)exitCode, g_AttachedPort);
        return ERROR_SUCCESS;
    }
    Log("attach: failed (Win32 %lu, exit %ld): %s", error, (long)exitCode, output);
    return error != ERROR_SUCCESS ? error : ERROR_NOT_CONNECTED;
}

/* Detaches the device and waits for the session to end, so nothing reaches a stopping backend. */
static void
Detach(void)
{
    /*
     * Detach through usbip.exe: a connection closed by this end looks like a network failure to
     * usbip-win2, which then keeps retrying the import. A disconnected session has no port to
     * detach, and its old port may belong to someone else by now. With --no-attach the importer
     * is not usbip-win2's attachment of this server, so usbip.exe is never run.
     */
    if (!g_NoAttach && SessionLive()) {
        int port = g_AttachedPort > 0 ? g_AttachedPort : FindAttachedPort(0);

        if (port > 0) {
            WCHAR arguments[64];
            char output[1024];
            DWORD exitCode;
            DWORD error;

            (void)_snwprintf_s(arguments, ARRAYSIZE(arguments), _TRUNCATE, L"detach -p %d", port);
            error = RunUsbip(arguments, output, sizeof(output), &exitCode, USBIP_QUERY_MS, 0);
            Log("detach: port %d: %s (Win32 %lu, exit %ld)", port, output, error, (long)exitCode);
        }
    }
    g_AttachedPort = 0;
    EnterCriticalSection(&g_Lock);
    if (g_Session != INVALID_SOCKET) {
        shutdown(g_Session, SD_BOTH);
    }
    LeaveCriticalSection(&g_Lock);
    if (g_SessionThread != NULL) {
        /*
         * shutdown() sends FIN but does not wake a recv blocked in another thread; usbip-win2
         * answers the FIN by closing, a stuck or foreign importer may never do so. Cancel the
         * read instead. Under g_Lock the socket is still open: the session thread closes it only
         * after clearing g_Session under the same lock.
         */
        if (WaitForSingleObject(g_SessionThread, SESSION_CLOSE_MS) == WAIT_TIMEOUT) {
            EnterCriticalSection(&g_Lock);
            if (g_Session != INVALID_SOCKET) {
                Log("detach: importer did not close the connection; cancelling the session read");
                (void)CancelIoEx((HANDLE)g_Session, NULL);
            }
            LeaveCriticalSection(&g_Lock);
            if (WaitForSingleObject(g_SessionThread, SESSION_CANCEL_MS) != WAIT_OBJECT_0) {
                FatalStall("session thread did not stop after cancellation");
            }
        }
        else if (WaitForSingleObject(g_SessionThread, 0) != WAIT_OBJECT_0) {
            FatalStall("session thread join failed");
        }
        CloseHandle(g_SessionThread);
        g_SessionThread = NULL;
    }
}

/* ------------------------------------------------------------------ radio lifecycle */

/* Backend up, imports accepted, device attached. Under g_StateLock. */
static DWORD
RadioStart(void)
{
    DWORD error = ERROR_SUCCESS;

    if (g_Running) {
        return ERROR_SUCCESS;
    }
    /* A fault signalled by the previous backend, before or during its stop, is spent: consume
     * its resume signal too, or the lifecycle thread would restart this healthy radio. */
    EnterCriticalSection(&g_Lock);
    g_ControllerFault[0] = '\0';
    LeaveCriticalSection(&g_Lock);
    (void)ResetEvent(g_ResumeEvent);
    if (g_UseUart) {
        error = QcaBackendStart(g_Qca, &g_Lock, &g_Transport);
        if (error != ERROR_SUCCESS) {
            if (g_Qca->Stuck) {
                FatalStall("controller thread stuck during failed start");
            }
            Log("radio: controller bring-up failed (Win32 %lu)", error);
            return error;
        }
    }
    EnterCriticalSection(&g_Lock);
    g_Ready = g_ControllerFault[0] == '\0';
    LeaveCriticalSection(&g_Lock);
    g_Running = 1;
    if (!g_NoAttach && WaitForSingleObject(g_StopEvent, 0) != WAIT_OBJECT_0) {
        error = Attach();
    }
    return error;
}

/*
 * Links closed cleanly, device detached, imports refused, controller handed back. Under g_StateLock.
 * Without the clean disconnect a peer only sees its link vanish: a headset then reconnects by itself
 * after resume with some of its profiles (see hci_bridge.h, CLEAN SHUTDOWN).
 */
static void
RadioStop(const char *Why)
{
    LONG64 outerDeadline;

    if (!g_Running) {
        return;
    }
    outerDeadline = InterlockedCompareExchange64(&g_StopDeadline,
                                                 (LONG64)GetTickCount64() + STOP_DEADLINE_MS, 0);
    Log("radio: stopping (%s)", Why);
    EnterCriticalSection(&g_Lock);
    g_Ready = 0;
    LeaveCriticalSection(&g_Lock);
    if (g_UseUart) {
        ULONGLONG started = GetTickCount64();
        ULONG found;
        ULONG open = QcaBackendDisconnectAll(g_Qca, DISCONNECT_MS, &found);

        if (found != 0) {
            Log("radio: disconnected %lu of %lu link(s) in %llu ms", found - open, found,
                GetTickCount64() - started);
        }
    }
    Detach();
    if (g_UseUart) {
        QcaBackendStop(g_Qca);
        if (g_Qca->Stuck) {
            FatalStall("controller thread did not stop");
        }
    }
    g_Running = 0;
    Log("radio: stopped");
    if (outerDeadline == 0) {
        InterlockedExchange64(&g_StopDeadline, 0);
    }
}

/* Starts the radio, retrying: after a resume or at boot the UART and usbip-win2 may still be starting. */
static DWORD
RadioStartWithRetries(void)
{
    DWORD error = ERROR_SUCCESS;
    ULONG ctsFailures = 0;

    for (ULONG attempt = 1; attempt <= g_StartAttempts; attempt++) {
        if (WaitForSingleObject(g_StopEvent, 0) == WAIT_OBJECT_0) {
            return ERROR_SUCCESS;
        }
        AcquireSRWLockExclusive(&g_StateLock);
        error = g_Suspended ? ERROR_SUCCESS : RadioStart();
        if (error != ERROR_SUCCESS && g_Running) {
            RadioStop("start incomplete");
        }
        ReleaseSRWLockExclusive(&g_StateLock);
        if (WaitForSingleObject(g_StopEvent, 0) == WAIT_OBJECT_0) {
            return ERROR_SUCCESS; /* cancelling attach during intentional stop is not a failure */
        }
        if (error == ERROR_SUCCESS) {
            return ERROR_SUCCESS;
        }
        ctsFailures = g_UseUart && g_Qca->CtsUnresponsive ? ctsFailures + 1 : 0;
        if (ctsFailures >= CTS_START_ATTEMPTS) {
            return ERROR_DEVICE_HARDWARE_ERROR;
        }
        if (attempt < g_StartAttempts &&
            WaitForSingleObject(g_StopEvent, START_RETRY_MS) == WAIT_OBJECT_0) {
            return ERROR_SUCCESS;
        }
    }
    return error;
}

/* Suspend and resume. The controller loses its firmware in S3, so the radio is stopped before sleep
 * (device detached, controller handed back, UART closed) and started again after resume.
 * Stop's explicit waits: disconnect 1s + port/detach 2*(3s+1s kill) + session 3s+2s.
 * Backend handback and lock acquisition can stall too. The independent watchdog bounds the
 * ENTIRE suspend callback, including a concurrent bring-up, to 45s + <=100ms polling
 * + <=100ms fatal logging, plus OS scheduling/termination latency. SCM then recovers it. */
static ULONG CALLBACK
PowerCallback(PVOID Context, ULONG Type, PVOID Setting)
{
    (void)Context;
    (void)Setting;
    if (Type == PBT_APMSUSPEND) {
        InterlockedExchange64(&g_SuspendDeadline, (LONG64)GetTickCount64() + STOP_DEADLINE_MS);
        (void)SetEvent(g_SuspendPending);   /* cancels an attach holding g_StateLock */
        AcquireSRWLockExclusive(&g_StateLock);
        g_Suspended = 1;
        RadioStop("suspend");
        (void)ResetEvent(g_SuspendPending);
        ReleaseSRWLockExclusive(&g_StateLock);
        InterlockedExchange64(&g_SuspendDeadline, 0);
    } else if (Type == PBT_APMRESUMEAUTOMATIC || Type == PBT_APMRESUMESUSPEND) {
        AcquireSRWLockExclusive(&g_StateLock);
        if (g_Suspended) {
            g_Suspended = 0;
            Log("power: resumed");
            (void)SetEvent(g_ResumeEvent);
        }
        ReleaseSRWLockExclusive(&g_StateLock);
    }
    return ERROR_SUCCESS;
}

/* ------------------------------------------------------------------ hands-free profile */

typedef struct _HANDSFREE_WATCH {
    unsigned short Handle;
    unsigned char  Address[6];
    ULONGLONG      Since;           /* when the link was first seen */
    int            Settled;         /* RFCOMM seen, or the restart already done */
} HANDSFREE_WATCH;

static HANDSFREE_WATCH g_HandsFree[HCI_BRIDGE_MAX_LINKS];   /* Run's thread only */
static unsigned int    g_HandsFreeCount;

/*
 * A BR/EDR link up HANDSFREE_GRACE_MS without an RFCOMM connection request gets the Hands-Free
 * device of its peer restarted, once per link (handsfree.h). Peers without one (keyboards,
 * controllers, A2DP-only speakers) are not touched. Called from Run about once a second.
 */
static void
HandsFreeCheck(void)
{
    HCI_BRIDGE_LINK links[HCI_BRIDGE_MAX_LINKS];
    HANDSFREE_WATCH watches[HCI_BRIDGE_MAX_LINKS];
    HANDSFREE_WATCH restart[HCI_BRIDGE_MAX_LINKS];
    unsigned int count = 0;
    unsigned int watched = 0;
    unsigned int restarts = 0;
    ULONGLONG now = GetTickCount64();

    if (!g_UseUart) {
        return;
    }
    AcquireSRWLockShared(&g_StateLock);
    if (g_Running && !g_Suspended) {
        EnterCriticalSection(&g_Lock);
        count = HciBridgeGetLinks(&g_Qca->Bridge, links, ARRAYSIZE(links));
        LeaveCriticalSection(&g_Lock);
    }
    ReleaseSRWLockShared(&g_StateLock);

    /* Rebuilt from the open links each time: a closed link, or a stopped radio, drops its entry. */
    for (unsigned int i = 0; i < count; i++) {
        HANDSFREE_WATCH *watch = &watches[watched];
        unsigned int j;

        if (!links[i].Classic) {
            continue;
        }
        for (j = 0; j < g_HandsFreeCount; j++) {
            if (g_HandsFree[j].Handle == links[i].Handle &&
                memcmp(g_HandsFree[j].Address, links[i].Address, sizeof(links[i].Address)) == 0) {
                break;
            }
        }
        if (j < g_HandsFreeCount) {
            *watch = g_HandsFree[j];
        } else {
            watch->Handle = links[i].Handle;
            memcpy(watch->Address, links[i].Address, sizeof(watch->Address));
            watch->Since = now;
            watch->Settled = 0;
        }
        if (links[i].Rfcomm) {
            watch->Settled = 1;
        } else if (!watch->Settled && now - watch->Since >= HANDSFREE_GRACE_MS) {
            watch->Settled = 1;
            restart[restarts++] = *watch;
        }
        watched++;
    }
    memcpy(g_HandsFree, watches, watched * sizeof(watches[0]));
    g_HandsFreeCount = watched;

    for (unsigned int i = 0; i < restarts; i++) {
        const unsigned char *a = restart[i].Address;
        ULONG restarted = HandsFreeRestart(a);

        if (restarted != 0) {
            Log("hands-free: %02X:%02X:%02X:%02X:%02X:%02X connected %llu ms without RFCOMM; restarted its "
                "Hands-Free device", a[5], a[4], a[3], a[2], a[1], a[0], now - restart[i].Since);
        }
    }
}

/* ------------------------------------------------------------------ server */

/* Fixed-size pending handshakes: a silent/slow client never owns the accept thread. */
static DWORD WINAPI
AcceptThread(LPVOID Parameter)
{
    struct {
        SOCKET Socket;
        struct sockaddr_in Peer;
        DWORD Pid;
        ULONGLONG Deadline;
        unsigned long Used;
        unsigned char Op[USBIP_OP_HEADER_SIZE + USBIP_BUSID_SIZE];
    } pending[MAX_HANDSHAKES];

    (void)Parameter;
    for (unsigned int i = 0; i < MAX_HANDSHAKES; i++) {
        pending[i].Socket = INVALID_SOCKET;
    }
    while (WaitForSingleObject(g_StopEvent, 0) != WAIT_OBJECT_0) {
        fd_set reads;
        struct timeval timeout = { 0, 100000 };

        FD_ZERO(&reads);
        FD_SET(g_Listener, &reads);
        for (unsigned int i = 0; i < MAX_HANDSHAKES; i++) {
            if (pending[i].Socket != INVALID_SOCKET) {
                FD_SET(pending[i].Socket, &reads);
            }
        }
        if (select(0, &reads, NULL, NULL, &timeout) == SOCKET_ERROR) {
            break;
        }
        for (unsigned int i = 0; i < MAX_HANDSHAKES; i++) {
            SOCKET client = pending[i].Socket;
            unsigned long needed = USBIP_OP_HEADER_SIZE;
            int drop;

            if (client == INVALID_SOCKET) {
                continue;
            }
            drop = GetTickCount64() >= pending[i].Deadline;
            if (!drop && FD_ISSET(client, &reads)) {
                int got;
                if (pending[i].Used >= USBIP_OP_HEADER_SIZE &&
                    UsbipGet16(pending[i].Op + 2) == USBIP_OP_REQ_IMPORT) {
                    needed += USBIP_BUSID_SIZE;
                }
                got = recv(client, (char *)pending[i].Op + pending[i].Used,
                           (int)(needed - pending[i].Used), 0);
                if (got <= 0) {
                    drop = 1;
                } else {
                    pending[i].Used += (unsigned long)got;
                    if (UsbipGet16(pending[i].Op + 2) == USBIP_OP_REQ_IMPORT) {
                        needed = sizeof(pending[i].Op);
                    }
                    if (pending[i].Used == needed) {
                        ServeConnection(client, &pending[i].Peer, pending[i].Pid, pending[i].Op);
                        pending[i].Socket = INVALID_SOCKET;
                        continue;
                    }
                }
            }
            if (drop) {
                closesocket(client);
                pending[i].Socket = INVALID_SOCKET;
            }
        }
        /* Nonblocking listener: drain a bounded batch without starving pending handshakes. */
        for (unsigned int accepted = 0; accepted < 32u && FD_ISSET(g_Listener, &reads); accepted++) {
            struct sockaddr_in peer;
            int peerLength = sizeof(peer);
            SOCKET client = accept(g_Listener, (struct sockaddr *)&peer, &peerLength);
            DWORD pid;
            DWORD sendTimeout = HANDSHAKE_MS;
            unsigned int slot = MAX_HANDSHAKES;
            u_long blocking = 0;

            if (client == INVALID_SOCKET) {
                break;
            }
            if (ioctlsocket(client, FIONBIO, &blocking) != 0) {
                closesocket(client);
                continue;
            }
            pid = PeerProcessId(&peer);
            for (unsigned int i = 0; i < MAX_HANDSHAKES; i++) {
                if (pending[i].Socket == INVALID_SOCKET) {
                    slot = i;
                    break;
                }
            }
            /* Local clients cannot fill the pending slots and exclude the kernel importer. */
            if (slot == MAX_HANDSHAKES && pid == 4) {
                for (unsigned int i = 0; i < MAX_HANDSHAKES; i++) {
                    if (pending[i].Pid != 4) {
                        closesocket(pending[i].Socket);
                        slot = i;
                        break;
                    }
                }
            }
            if (slot == MAX_HANDSHAKES ||
                setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char *)&sendTimeout, sizeof(sendTimeout)) != 0) {
                closesocket(client);
                if (slot != MAX_HANDSHAKES) {
                    pending[slot].Socket = INVALID_SOCKET;
                }
                continue;
            }
            pending[slot].Socket = client;
            pending[slot].Peer = peer;
            pending[slot].Pid = pid;
            pending[slot].Deadline = GetTickCount64() + HANDSHAKE_MS;
            pending[slot].Used = 0;
            memset(pending[slot].Op, 0, sizeof(pending[slot].Op));
        }
    }
    for (unsigned int i = 0; i < MAX_HANDSHAKES; i++) {
        if (pending[i].Socket != INVALID_SOCKET) {
            closesocket(pending[i].Socket);
        }
    }
    return 0;
}

/* Everything that does not depend on the controller: listener, threads, device model. */
static DWORD
ServerStart(void)
{
    WSADATA wsa;
    struct sockaddr_in address;
    BOOL exclusive = TRUE;
    HANDLE timer;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return ERROR_NETWORK_UNREACHABLE;
    }
    if (g_UseUart) {
        if (g_FirmwareDir[0] == L'\0' &&
            QcaBackendFindVendorFirmware(g_FirmwareDir, ARRAYSIZE(g_FirmwareDir)) != ERROR_SUCCESS) {
            Log("no installed Qualcomm Bluetooth driver package (qcbtuart) with firmware found");
            return ERROR_FILE_NOT_FOUND;
        }
        wcscpy_s(g_Qca->Controller, ARRAYSIZE(g_Qca->Controller), g_Controller);
        wcscpy_s(g_Qca->FirmwareDir, ARRAYSIZE(g_Qca->FirmwareDir), g_FirmwareDir);
        g_Qca->Log = Log;
        g_Qca->OnFault = ControllerFault;
        g_Qca->FaultContext = NULL;
    } else {
        HciStubInit(&g_Stub);
        HciStubBindTransport(&g_Transport, &g_Stub);
    }
    g_Transport.Notify = TransportNotify;
    g_Transport.NotifyContext = NULL;
    UsbipDeviceInit(g_Device, &g_Transport, DeviceSend, NULL);
    g_Device->Trace = TraceControl;

    g_Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_Listener == INVALID_SOCKET) {
        return (DWORD)WSAGetLastError();
    }
    (void)setsockopt(g_Listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(g_Port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_Listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(g_Listener, SOMAXCONN) != 0) {
        DWORD error = (DWORD)WSAGetLastError();
        Log("bind/listen on 127.0.0.1:%u failed (%lu)", g_Port, error);
        return error;
    }
    {
        u_long nonblocking = 1;
        if (ioctlsocket(g_Listener, FIONBIO, &nonblocking) != 0) {
            return (DWORD)WSAGetLastError();
        }
    }
    /* Pacing drives SCO completions and the order-hold deadline: no timer, no start. */
    timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == NULL) {
        DWORD error = GetLastError();
        Log("pacing: CreateWaitableTimerExW failed (%lu)", error);
        return error;
    }
    g_Pacing = CreateThread(NULL, 0, PacingThread, timer, 0, NULL);
    if (g_Pacing == NULL) {
        DWORD error = GetLastError();
        CloseHandle(timer);
        return error;
    }
    g_Acceptor = CreateThread(NULL, 0, AcceptThread, NULL, 0, NULL);
    if (g_Acceptor == NULL) {
        return GetLastError();
    }
    Log("DeckBtService: %s backend, listening on 127.0.0.1:%u, busid %s", g_UseUart ? "uart" : "stub",
        g_Port, g_BusId);
    return ERROR_SUCCESS;
}

static void
ServerStop(void)
{
    InterlockedExchange(&g_Stop, 1);
    (void)SetEvent(g_StopEvent);
    if (g_Acceptor != NULL) {
        if (WaitForSingleObject(g_Acceptor, SESSION_CLOSE_MS) != WAIT_OBJECT_0) {
            FatalStall("accept thread did not stop");
        }
        CloseHandle(g_Acceptor);
        g_Acceptor = NULL;
    }
    if (g_Listener != INVALID_SOCKET) {
        closesocket(g_Listener);
        g_Listener = INVALID_SOCKET;
    }
    EnterCriticalSection(&g_Lock);
    LogStats("shutdown");
    LeaveCriticalSection(&g_Lock);
    (void)SetEvent(g_Kick);
    if (g_Pacing != NULL) {
        if (WaitForSingleObject(g_Pacing, SESSION_CANCEL_MS) != WAIT_OBJECT_0) {
            FatalStall("pacing thread did not stop");
        }
        CloseHandle(g_Pacing);
        g_Pacing = NULL;
    }
    WSACleanup();
}

/*
 * The whole run: server, radio, then suspend/resume until stopped. Reports progress through
 * Report (service status in service mode). Returns the Win32 exit code.
 */
static DWORD
Run(void (*Report)(DWORD State, DWORD WaitHintMs))
{
    HPOWERNOTIFY power = NULL;
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS subscribe = { PowerCallback, NULL };
    DWORD error = ServerStart();

    if (error == ERROR_SUCCESS &&
        PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK, &subscribe, (PHPOWERNOTIFY)&power) != ERROR_SUCCESS) {
        Log("power: suspend/resume notification not registered; sleep will leave the radio stopped");
        power = NULL;
    }
    if (error == ERROR_SUCCESS) {
        Report(SERVICE_RUNNING, 0);
        error = RadioStartWithRetries();
        if (error != ERROR_SUCCESS) {
            Log("radio: not started (Win32 %lu)", error);
        }
    }
    while (error == ERROR_SUCCESS) {
        HANDLE waits[2] = { g_StopEvent, g_ResumeEvent };
        DWORD wait = WaitForMultipleObjects(2, waits, FALSE, HANDSFREE_POLL_MS);

        if (wait == WAIT_OBJECT_0) {
            break;
        }
        if (wait == WAIT_TIMEOUT) {
            HandsFreeCheck();
            continue;
        }
        /* PRESHUTDOWN precedes SCM shutdown, not necessarily the User32 1074 detach task.
         * A detach before preshutdown may still cause one benign reattach; attach/port waits
         * are interrupted by the stop event. SM_SHUTTINGDOWN describes this session (0),
         * not all user sessions. Event-log delivery likewise has no ordering guarantee. */
        AcquireSRWLockExclusive(&g_StateLock);
        if (!g_Suspended) {
            char reason[sizeof(g_ControllerFault)];
            EnterCriticalSection(&g_Lock);
            strcpy_s(reason, sizeof(reason), g_ControllerFault);
            LeaveCriticalSection(&g_Lock);
            if (reason[0] != '\0' || !g_NoAttach) {
                RadioStop(reason[0] != '\0' ? reason : "connection lost");
            }
        }
        ReleaseSRWLockExclusive(&g_StateLock);
        error = RadioStartWithRetries();
        if (error != ERROR_SUCCESS) {
            Log("radio: not restarted after resume/disconnect (Win32 %lu)", error);
        }
    }
    if (error == ERROR_DEVICE_HARDWARE_ERROR && g_UseUart && g_Qca->CtsUnresponsive) {
        /*
         * Stay controllable but unavailable. Exiting with an error would make SCM repeat this
         * same failed wake/reset forever. Do not suppress ordinary boot or attach failures.
         */
        Log("controller unresponsive: restart Windows to recover Bluetooth; "
            "CTS stayed low after %u start attempts; automatic retries paused", CTS_START_ATTEMPTS);
        (void)WaitForSingleObject(g_StopEvent, INFINITE);
        error = ERROR_SUCCESS;   /* an intentional stop, not a successful radio start */
    }
    Report(SERVICE_STOP_PENDING, 20000);
    (void)InterlockedCompareExchange64(&g_StopDeadline, (LONG64)GetTickCount64() + STOP_DEADLINE_MS, 0);
    if (power != NULL) {
        (void)PowerUnregisterSuspendResumeNotification(power);
    }
    AcquireSRWLockExclusive(&g_StateLock);
    RadioStop("stop");
    ReleaseSRWLockExclusive(&g_StateLock);
    ServerStop();
    InterlockedExchange64(&g_StopDeadline, 0);
    return error;
}

/* ------------------------------------------------------------------ console and service entry */

static BOOL WINAPI
ConsoleHandler(DWORD Event)
{
    (void)Event;
    (void)InterlockedCompareExchange64(&g_StopDeadline, (LONG64)GetTickCount64() + STOP_DEADLINE_MS, 0);
    (void)SetEvent(g_StopEvent);
    return TRUE;
}

/* --stop-file: a copy started hidden (elevated, no console to Ctrl+C) stops when the file appears. */
static DWORD WINAPI
StopFileThread(LPVOID Parameter)
{
    (void)Parameter;
    while (WaitForSingleObject(g_StopEvent, 500) == WAIT_TIMEOUT) {
        if (GetFileAttributesW(g_StopFile) != INVALID_FILE_ATTRIBUTES) {
            (void)InterlockedCompareExchange64(&g_StopDeadline, (LONG64)GetTickCount64() + STOP_DEADLINE_MS, 0);
            Log("stop file present; shutting down");
            (void)SetEvent(g_StopEvent);
            break;
        }
    }
    return 0;
}

static void
ReportConsole(DWORD State, DWORD WaitHintMs)
{
    (void)State;
    (void)WaitHintMs;
}

static SERVICE_STATUS_HANDLE g_StatusHandle;
static SERVICE_STATUS        g_ServiceStatus;

static void
ReportService(DWORD State, DWORD WaitHintMs)
{
    static DWORD checkPoint = 1;

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = State;
    g_ServiceStatus.dwControlsAccepted = (State == SERVICE_RUNNING) ?
        (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PRESHUTDOWN) : 0;
    g_ServiceStatus.dwWaitHint = WaitHintMs;
    g_ServiceStatus.dwCheckPoint = (State == SERVICE_RUNNING || State == SERVICE_STOPPED) ? 0 : checkPoint++;
    (void)SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

static DWORD WINAPI
ServiceControl(DWORD Control, DWORD EventType, LPVOID EventData, LPVOID Context)
{
    (void)EventType;
    (void)EventData;
    (void)Context;
    switch (Control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
    case SERVICE_CONTROL_PRESHUTDOWN:
        InterlockedExchange64(&g_StopDeadline, (LONG64)GetTickCount64() + STOP_DEADLINE_MS);
        ReportService(SERVICE_STOP_PENDING, 20000);
        (void)SetEvent(g_StopEvent);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void WINAPI
ServiceMain(DWORD Argc, LPWSTR *Argv)
{
    DWORD error;

    (void)Argc;
    (void)Argv;
    g_StatusHandle = RegisterServiceCtrlHandlerExW(SERVICE_NAME, ServiceControl, NULL);
    if (g_StatusHandle == NULL) {
        return;
    }
    ReportService(SERVICE_START_PENDING, 10000);
    error = Run(ReportService);
    g_ServiceStatus.dwWin32ExitCode = error;
    ReportService(SERVICE_STOPPED, 0);
}

/* ------------------------------------------------------------------ install / uninstall */

/* Calculates the length of a quoted argument under Microsoft CRT rules. */
static size_t
QuotedArgLength(const WCHAR *Arg)
{
    size_t len = 0;
    BOOLEAN quote = (*Arg == L'\0' || wcspbrk(Arg, L" \t\"") != NULL);
    unsigned int backslashes = 0;

    if (!quote) {
        return wcslen(Arg);
    }
    len += 2; /* opening and closing quotes */
    for (const WCHAR *p = Arg; *p != L'\0'; p++) {
        if (*p == L'\\') {
            backslashes++;
        } else if (*p == L'"') {
            len += 2 * backslashes + 2; /* 2*N backslashes + 1 escape backslash + quote */
            backslashes = 0;
        } else {
            len += backslashes + 1;
            backslashes = 0;
        }
    }
    len += 2 * backslashes; /* trailing backslashes doubled */
    return len;
}

/* Appends one argument to a Windows command line, quoting and escaping per Microsoft CRT rules:
 * runs of backslashes before a quotation mark are doubled plus one, and trailing backslashes
 * before the closing quote are doubled so the closing quote is not swallowed as a literal. */
static BOOLEAN
AppendQuotedArg(WCHAR *Buffer, size_t Capacity, const WCHAR *Arg)
{
    size_t len = wcslen(Buffer);
    const WCHAR *p;
    BOOLEAN quote;
    unsigned int backslashes;

    if (len >= Capacity - 1) {
        return FALSE;
    }
    quote = (*Arg == L'\0' || wcspbrk(Arg, L" \t\"") != NULL);
    if (len > 0) {
        if (len >= Capacity - 1) {
            return FALSE;
        }
        Buffer[len++] = L' ';
        Buffer[len] = L'\0';
    }
    if (!quote) {
        while (*Arg != L'\0') {
            if (len >= Capacity - 1) {
                return FALSE;
            }
            Buffer[len++] = *Arg++;
        }
        Buffer[len] = L'\0';
        return TRUE;
    }
    if (len >= Capacity - 1) {
        return FALSE;
    }
    Buffer[len++] = L'"';
    backslashes = 0;
    for (p = Arg; *p != L'\0'; p++) {
        if (*p == L'\\') {
            backslashes++;
        } else if (*p == L'"') {
            for (unsigned int j = 0; j < 2 * backslashes + 1; j++) {
                if (len >= Capacity - 1) {
                    return FALSE;
                }
                Buffer[len++] = L'\\';
            }
            if (len >= Capacity - 1) {
                return FALSE;
            }
            Buffer[len++] = L'"';
            backslashes = 0;
        } else {
            for (unsigned int j = 0; j < backslashes; j++) {
                if (len >= Capacity - 1) {
                    return FALSE;
                }
                Buffer[len++] = L'\\';
            }
            if (len >= Capacity - 1) {
                return FALSE;
            }
            Buffer[len++] = *p;
            backslashes = 0;
        }
    }
    for (unsigned int j = 0; j < 2 * backslashes; j++) {
        if (len >= Capacity - 1) {
            return FALSE;
        }
        Buffer[len++] = L'\\';
    }
    if (len >= Capacity - 1) {
        return FALSE;
    }
    Buffer[len++] = L'"';
    Buffer[len] = L'\0';
    return TRUE;
}

/* Builds the service command line dynamically, checking against Windows SCM's 32,767-character limit.
 * Returns a malloc'd wide string that the caller must free(), or NULL if too long / OOM. */
static WCHAR *
BuildServiceCommandLine(const WCHAR *Exe, int Argc, wchar_t **Argv)
{
    size_t total = 0;
    WCHAR *buf;

    if (Exe == NULL) {
        return NULL;
    }
    /* The executable path is unconditionally enclosed in quotes: "\"" + Exe + "\" --service" */
    total = wcslen(Exe) + 2 + 1 + wcslen(L"--service");
    for (int i = 0; i < Argc; i++) {
        total += 1 + QuotedArgLength(Argv[i]);
    }
    if (total > 32767) {
        return NULL;
    }
    buf = (WCHAR *)malloc((total + 1) * sizeof(WCHAR));
    if (buf == NULL) {
        return NULL;
    }
    buf[0] = L'\0';
    if (_snwprintf_s(buf, total + 1, _TRUNCATE, L"\"%s\" --service", Exe) < 0) {
        free(buf);
        return NULL;
    }
    for (int i = 0; i < Argc; i++) {
        if (!AppendQuotedArg(buf, total + 1, Argv[i])) {
            free(buf);
            return NULL;
        }
    }
    return buf;
}

/* Registers the service to run this executable with the given options, started at boot and restarted on failure. */
static int
Install(int argc, wchar_t **argv)
{
    WCHAR exe[MAX_PATH];
    WCHAR *commandLine = NULL;
    SC_HANDLE scm;
    SC_HANDLE service;
    SERVICE_DESCRIPTIONW description = { L"Presents the Steam Deck OLED's Bluetooth controller to Windows as a USB "
                                         L"Bluetooth adapter through usbip-win2, so headset microphones work." };
    SC_ACTION actions[3] = { { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 15000 }, { SC_ACTION_RESTART, 60000 } };
    SERVICE_FAILURE_ACTIONSW failure = { 86400, NULL, NULL, 3, actions };
    SERVICE_FAILURE_ACTIONS_FLAG failureFlags = { TRUE };
    PWSTR programFiles = NULL;
    DWORD exeLength;
    int allowed = 0;

    exeLength = GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
    if (exeLength != 0 && exeLength < ARRAYSIZE(exe) &&
        SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_ProgramFiles, 0, NULL, &programFiles))) {
        size_t length = wcslen(programFiles);
        allowed = _wcsnicmp(exe, programFiles, length) == 0 && exe[length] == L'\\';
    }
    CoTaskMemFree(programFiles);
    if (!allowed) {
        fprintf(stderr, "install: refusing service registration outside Program Files; use packaging\\install.ps1\n");
        return 3; /* No SCM handle is opened before this security boundary. */
    }
    commandLine = BuildServiceCommandLine(exe, argc, argv);
    if (commandLine == NULL) {
        fprintf(stderr, "install: command line exceeds maximum length (32,767 characters) or out of memory\n");
        return 1;
    }
    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (scm == NULL) {
        fprintf(stderr, "install: cannot open the service manager (Win32 %lu); run elevated\n", GetLastError());
        free(commandLine);
        return 1;
    }
    service = CreateServiceW(scm, SERVICE_NAME, SERVICE_DISPLAY_NAME, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                             SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, commandLine, NULL, NULL, NULL, NULL, NULL);
    if (service == NULL) {
        fprintf(stderr, "install: CreateService failed (Win32 %lu)\n", GetLastError());
        CloseServiceHandle(scm);
        free(commandLine);
        return 1;
    }
    (void)ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure) ||
        !ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &failureFlags)) {
        fprintf(stderr, "install: recovery configuration failed (Win32 %lu)\n", GetLastError());
        (void)DeleteService(service);
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        free(commandLine);
        return 1;
    }
    printf("installed %ls: %ls\n", SERVICE_NAME, commandLine);
    free(commandLine);
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return 0;
}

static int
Uninstall(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE service;
    SERVICE_STATUS status;

    if (scm == NULL) {
        fprintf(stderr, "uninstall: cannot open the service manager (Win32 %lu); run elevated\n", GetLastError());
        return 1;
    }
    service = OpenServiceW(scm, SERVICE_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (service == NULL) {
        DWORD error = GetLastError();
        CloseServiceHandle(scm);
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            printf("uninstall: %ls is not installed\n", SERVICE_NAME);
            return 0;
        }
        fprintf(stderr, "uninstall: OpenService failed (Win32 %lu)\n", error);
        return 1;
    }
    if (ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        for (ULONG waited = 0; waited < 30000u && status.dwCurrentState != SERVICE_STOPPED; waited += 250u) {
            Sleep(250);
            if (!QueryServiceStatus(service, &status)) {
                break;
            }
        }
    }
    if (!QueryServiceStatus(service, &status) || status.dwCurrentState != SERVICE_STOPPED) {
        fprintf(stderr, "uninstall: service has not stopped; leaving its registration intact\n");
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return 1;
    }
    if (!DeleteService(service)) {
        fprintf(stderr, "uninstall: DeleteService failed (Win32 %lu)\n", GetLastError());
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return 1;
    }
    printf("uninstalled %ls\n", SERVICE_NAME);
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return 0;
}

/* ------------------------------------------------------------------ options */

static int
ParseArgs(int argc, wchar_t **argv)
{
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--port") == 0 && i + 1 < argc) {
            const wchar_t *text = argv[++i];
            size_t length = wcslen(text);
            long port;

            if (length == 0 || length > 5 || wcsspn(text, L"0123456789") != length) {
                return 0;
            }
            port = wcstol(text, NULL, 10);
            if (port < 1024 || port > 65535) {
                return 0;
            }
            g_Port = (unsigned short)port;
        } else if (wcscmp(argv[i], L"--busid") == 0 && i + 1 < argc) {
            /* It reaches the usbip.exe command line: ASCII characters only */
            static const wchar_t busIdChars[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-";
            const wchar_t *busid = argv[++i];
            size_t length = wcslen(busid);

            if (length == 0 || length >= sizeof(g_BusId) || wcsspn(busid, busIdChars) != length) {
                return 0;
            }
            for (size_t b = 0; b < length; b++) {
                g_BusId[b] = (char)busid[b];
            }
            g_BusId[length] = '\0';
        } else if (wcscmp(argv[i], L"--stop-file") == 0 && i + 1 < argc) {
            if (wcscpy_s(g_StopFile, ARRAYSIZE(g_StopFile), argv[++i]) != 0) {
                return 0;
            }
        } else if (wcscmp(argv[i], L"--log") == 0 && i + 1 < argc) {
            if (wcscpy_s(g_LogPath, ARRAYSIZE(g_LogPath), argv[++i]) != 0) {
                return 0;
            }
        } else if (wcscmp(argv[i], L"--quiet") == 0) {
            g_Quiet = 1;
        } else if (wcscmp(argv[i], L"--service") == 0) {
            g_ServiceMode = 1;
        } else if (wcscmp(argv[i], L"--no-attach") == 0) {
            g_NoAttach = 1;
        } else if (wcscmp(argv[i], L"--allow-user-import") == 0) {
            g_AllowUserImport = 1;
        } else if (wcscmp(argv[i], L"--usbip") == 0 && i + 1 < argc) {
            if (wcscpy_s(g_UsbipExe, ARRAYSIZE(g_UsbipExe), argv[++i]) != 0) {
                return 0;
            }
        } else if (wcscmp(argv[i], L"--backend") == 0 && i + 1 < argc) {
            ++i;
            if (wcscmp(argv[i], L"uart") == 0) {
                g_UseUart = 1;
            } else if (wcscmp(argv[i], L"stub") != 0) {
                return 0;
            }
        } else if (wcscmp(argv[i], L"--controller") == 0 && i + 1 < argc) {
            if (wcscpy_s(g_Controller, ARRAYSIZE(g_Controller), argv[++i]) != 0) {
                return 0;
            }
        } else if (wcscmp(argv[i], L"--firmware-dir") == 0 && i + 1 < argc) {
            if (wcscpy_s(g_FirmwareDir, ARRAYSIZE(g_FirmwareDir), argv[++i]) != 0) {
                return 0;
            }
        } else {
            return 0;
        }
    }
    return !(g_ServiceMode && g_LogPath[0] != L'\0') &&
           (!g_AllowUserImport || (!g_UseUart && !g_ServiceMode && g_NoAttach));
}

/* Handle-based repair avoids changing the target of a planted junction. Keep the directory
 * open without delete sharing for the process lifetime, so its secured path cannot be swapped.
 * Share modes bind only handles holding a data right, hence FILE_LIST_DIRECTORY on that handle.
 * Repair pre-existing files too: protected parent ACLs do not revoke explicit child grants. */
static int
SecureLogHandle(HANDLE Handle, int Directory)
{
    BY_HANDLE_FILE_INFORMATION info;
    PSID owner;
    PACL dacl;
    BOOL present, defaulted;

    if (!GetFileInformationByHandle(Handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != Directory ||
        (!Directory && info.nNumberOfLinks != 1)) {
        return 0;
    }
    if (g_LogSecurity == NULL) {
        return 1;
    }
    return GetSecurityDescriptorOwner(g_LogSecurity, &owner, &defaulted) &&
           GetSecurityDescriptorDacl(g_LogSecurity, &present, &dacl, &defaulted) && present &&
           SetSecurityInfo(Handle, SE_FILE_OBJECT,
                           OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                           owner, NULL, dacl, NULL) == ERROR_SUCCESS;
}

static int
OpenLogFile(void)
{
    SECURITY_ATTRIBUTES security = { sizeof(security), g_LogSecurity, FALSE };
    DWORD access = FILE_APPEND_DATA | FILE_READ_ATTRIBUTES;
    HANDLE file;

    if (g_LogSecurity != NULL) {
        access |= READ_CONTROL | WRITE_DAC | WRITE_OWNER;
    }
    file = CreateFileW(g_LogPath, access, FILE_SHARE_READ | FILE_SHARE_DELETE,
                       &security, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    if (!SecureLogHandle(file, 0)) {
        CloseHandle(file);
        return 0;
    }
    g_LogFile = file;
    return 1;
}

/* Called with g_LogLock held. Retain the old handle if rotation cannot complete. */
static int
RotateLog(void)
{
    WCHAR previous[MAX_PATH];
    HANDLE old = g_LogFile;
    DWORD attributes;

    if (_snwprintf_s(previous, ARRAYSIZE(previous), _TRUNCATE, L"%s.1", g_LogPath) < 0) {
        return 0;
    }
    attributes = GetFileAttributesW(previous);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        return 0;
    }
    if (!MoveFileExW(g_LogPath, previous, MOVEFILE_REPLACE_EXISTING)) {
        return 0;
    }
    if (!OpenLogFile()) {
        (void)MoveFileExW(previous, g_LogPath, MOVEFILE_REPLACE_EXISTING);
        return 0;
    }
    CloseHandle(old);
    return 1;
}

/* Service logs always use the secured known-folder location; --log is console-only. */
static int
OpenLog(void)
{
    if (g_ServiceMode) {
        WCHAR directory[MAX_PATH];
        PWSTR programData = NULL;
        SECURITY_ATTRIBUTES security = { sizeof(security), NULL, FALSE };

        if (g_LogPath[0] != L'\0' ||
            FAILED(SHGetKnownFolderPath(&FOLDERID_ProgramData, 0, NULL, &programData))) {
            return 0;
        }
        if (_snwprintf_s(directory, ARRAYSIZE(directory), _TRUNCATE, L"%s\\DeckBtService", programData) < 0) {
            CoTaskMemFree(programData);
            return 0;
        }
        CoTaskMemFree(programData);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FR;;;BU)",
                SDDL_REVISION_1, &g_LogSecurity, NULL)) {
            return 0;
        }
        security.lpSecurityDescriptor = g_LogSecurity;
        if (!CreateDirectoryW(directory, &security) && GetLastError() != ERROR_ALREADY_EXISTS) {
            return 0;
        }
        g_LogDirectory = CreateFileW(directory, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL |
                                     WRITE_DAC | WRITE_OWNER,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (g_LogDirectory == INVALID_HANDLE_VALUE || !SecureLogHandle(g_LogDirectory, 1)) {
            return 0;
        }
        if (_snwprintf_s(g_LogPath, ARRAYSIZE(g_LogPath), _TRUNCATE, L"%s\\deckbt-service.log", directory) < 0) {
            return 0;
        }
    }
    return g_LogPath[0] == L'\0' || OpenLogFile();
}

static void
Usage(void)
{
    fprintf(stderr,
            "usage: deckbt-usbip [options]            run in this console (Ctrl+C stops)\n"
            "       deckbt-usbip install [options]    register the DeckBtService service with these options\n"
            "       deckbt-usbip uninstall            stop and remove the service\n"
            "       deckbt-usbip --version            print the build identity\n"
            "options: --backend stub|uart  --controller ID  --firmware-dir DIR  --port N  --busid ID\n"
            "         --usbip PATH  --no-attach  --log FILE  --quiet  --stop-file FILE\n"
            "         --allow-user-import (console stub with --no-attach only)\n");
}

int
wmain(int argc, wchar_t **argv)
{
    PWSTR programFiles = NULL;

    if (argc == 2 && wcscmp(argv[1], L"--version") == 0) {
        printf("deckbt-usbip %s\n", DECKBT_VERSION);
        return 0;
    }

    if (argc >= 2 && wcscmp(argv[1], L"install") == 0) {
        g_ServiceMode = 1;
        if (!ParseArgs(argc - 1, argv + 1)) {
            Usage();
            return 2;
        }
        return Install(argc - 2, argv + 2);
    }
    if (argc >= 2 && wcscmp(argv[1], L"uninstall") == 0) {
        return Uninstall();
    }
    QueryPerformanceFrequency(&g_QpcFrequency);
    QueryPerformanceCounter(&g_QpcStart);
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_ProgramFiles, 0, NULL, &programFiles))) {
        (void)_snwprintf_s(g_UsbipExe, ARRAYSIZE(g_UsbipExe), _TRUNCATE, L"%s\\USBip\\usbip.exe", programFiles);
    }
    CoTaskMemFree(programFiles);
    if (!ParseArgs(argc, argv)) {
        Usage();
        return 2;
    }
    if (!OpenLog()) {
        fprintf(stderr, "cannot open the log file\n");
        return 1;
    }
    {
        SYSTEMTIME utc;
        GetSystemTime(&utc);
        Log("startup %s UTC %04u-%02u-%02uT%02u:%02u:%02u.%03uZ pid %lu",
            DECKBT_VERSION, utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond,
            utc.wMilliseconds, GetCurrentProcessId());
    }
    g_Device = (USBIP_DEVICE *)calloc(1, sizeof(USBIP_DEVICE));
    g_Qca = g_UseUart ? (QCA_BACKEND *)calloc(1, sizeof(QCA_BACKEND)) : NULL;
    InitializeCriticalSection(&g_Lock);
    g_Kick = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_ResumeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_SuspendPending = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_Device == NULL || (g_UseUart && g_Qca == NULL) || g_Kick == NULL || g_StopEvent == NULL ||
        g_ResumeEvent == NULL || g_SuspendPending == NULL) {
        return 1;
    }
    g_StartAttempts = g_ServiceMode ? 20u : 1u;
    {
        HANDLE watchdog = CreateThread(NULL, 0, StopWatchdog, NULL, 0, NULL);
        if (watchdog == NULL) {
            return 1;
        }
        CloseHandle(watchdog);
    }

    if (g_ServiceMode) {
        SERVICE_TABLE_ENTRYW table[] = { { (LPWSTR)SERVICE_NAME, ServiceMain }, { NULL, NULL } };

        g_Quiet = 1;
        return StartServiceCtrlDispatcherW(table) ? 0 : (int)GetLastError();
    }

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    if (g_StopFile[0] != L'\0') {
        HANDLE watcher = CreateThread(NULL, 0, StopFileThread, NULL, 0, NULL);
        if (watcher != NULL) {
            CloseHandle(watcher);
        }
    }
    return Run(ReportConsole) == ERROR_SUCCESS ? 0 : 1;
}
