/*
 * qca_backend.c - the QCA2066 controller as an HCI_TRANSPORT backend, from user mode.
 * See qca_backend.h. The sequence and timings are those of the vendor driver's bring-up, as
 * recorded on the Steam Deck OLED (docs/QCA2066.md).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qca_backend.h"
#include "mmcss.h"

#define INIT_BAUD               115200ul
#define OPER_BAUD               3000000ul
#define READ_BUFFER             2048u
#define READ_INTERVAL_MS        20u      /* QCA_UART_READ_INTERVAL_MS */
#define WRITE_TIMEOUT_MS        1500u    /* QCA_UART_WRITE_TIMEOUT_MS */
#define SEND_TIMEOUT_MS         5000u
#define BAUD_SETTLE_MS          300u     /* QCA_UART_BAUD_SETTLE_MS */
#define NVM_SETTLE_MS           10u      /* QCA_UART_NVM_SETTLE_MS */
#define IBS_WAKE_SETTLE_MS      10u      /* QCA_UART_IBS_WAKE_SETTLE_MS */
#define SOC_RESET_SETTLE_MS     200u     /* QCA_UART_SOC_RESET_SETTLE_MS */
#define IBS_WAKE_RETRANS_MS     100u     /* QCA_UART_IBS_WAKE_RETRANS_MS */
#define IBS_WAKE_TRIES          10u      /* QCA_UART_IBS_WAKE_TRIES */
#define STEADY_READ_IDLE_MS     1000u    /* QCA_UART_STEADY_READ_IDLE_MS */
#define IDENTIFY_REPLY_MS       1500u
#define FSM_REPLY_MS            2000u
#define QUIESCE_MS              2000u

static const char *const g_NvmNames[QCA_BACKEND_NVM_FILES] = {
    "hpnv21.bin", "hpnv21g.bin", "hpnv21.309", "hpnv21g.309"   /* g_QcaNvmFiles order */
};

#define LOG(B, ...) do { if ((B)->Log != NULL) { (B)->Log(__VA_ARGS__); } } while (0)

/* Workers only, outside both locks. Shutdown errors must not request another restart. */
static void
ReportFault(QCA_BACKEND *B, const char *Why, DWORD Error)
{
    /* Linearize the fault with queue admission, then notify with neither lock held. */
    EnterCriticalSection(&B->TxLock);
    if (B->Phase != 2 || B->Stop || B->WriterStop ||
        InterlockedCompareExchange(&B->Faulted, 1, 0) != 0) {
        LeaveCriticalSection(&B->TxLock);
        return;
    }
    LeaveCriticalSection(&B->TxLock);
    LOG(B, "%s (Win32 %lu)", Why, Error);
    if (B->OnFault != NULL && B->Phase == 2 && !B->Stop && !B->WriterStop) {
        B->OnFault(B->FaultContext, Why);
    }
}

/* ------------------------------------------------------------------ firmware */

static DWORD
LoadFile(const WCHAR *Directory, const char *Name, UCHAR **Data, ULONG *Size)
{
    WCHAR path[MAX_PATH];
    HANDLE file;
    LARGE_INTEGER size;
    DWORD read = 0;

    *Data = NULL;
    *Size = 0;
    if (_snwprintf_s(path, ARRAYSIZE(path), _TRUNCATE, L"%s\\%S", Directory, Name) < 0) {
        return ERROR_FILENAME_EXCED_RANGE;
    }
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024) {
        CloseHandle(file);
        return ERROR_BAD_LENGTH;
    }
    *Data = (UCHAR *)malloc((size_t)size.QuadPart);
    if (*Data == NULL) {
        CloseHandle(file);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    if (!ReadFile(file, *Data, (DWORD)size.QuadPart, &read, NULL) || read != (DWORD)size.QuadPart) {
        DWORD error = GetLastError();
        CloseHandle(file);
        free(*Data);
        *Data = NULL;
        return error != ERROR_SUCCESS ? error : ERROR_READ_FAULT;
    }
    CloseHandle(file);
    *Size = read;
    return ERROR_SUCCESS;
}

static void
FreeFirmware(QCA_BACKEND *B)
{
    free(B->Patch);
    B->Patch = NULL;
    for (ULONG i = 0; i < QCA_BACKEND_NVM_FILES; i++) {
        free(B->Nvm[i]);
        B->Nvm[i] = NULL;
    }
    B->CandidateCount = 0;
}

static DWORD
LoadFirmware(QCA_BACKEND *B)
{
    DWORD error = LoadFile(B->FirmwareDir, "hpbtfw21.tlv", &B->Patch, &B->PatchSize);

    if (error != ERROR_SUCCESS) {
        LOG(B, "firmware: hpbtfw21.tlv not loaded from %ls (Win32 %lu)", B->FirmwareDir, error);
        return error;
    }
    for (ULONG i = 0; i < QCA_BACKEND_NVM_FILES; i++) {
        UCHAR *data;
        ULONG size;

        if (LoadFile(B->FirmwareDir, g_NvmNames[i], &data, &size) == ERROR_SUCCESS) {
            B->Nvm[B->CandidateCount] = data;
            B->Candidates[B->CandidateCount].Name = g_NvmNames[i];
            B->Candidates[B->CandidateCount].Data = data;
            B->Candidates[B->CandidateCount].Size = size;
            B->CandidateCount++;
        }
    }
    if (B->CandidateCount == 0) {
        LOG(B, "firmware: no NVM file in %ls", B->FirmwareDir);
        return ERROR_FILE_NOT_FOUND;
    }
    LOG(B, "firmware: rampatch %lu bytes, %lu NVM candidate(s) from %ls",
        B->PatchSize, B->CandidateCount, B->FirmwareDir);
    return ERROR_SUCCESS;
}

DWORD
QcaBackendFindVendorFirmware(WCHAR *Directory, DWORD Chars)
{
    WCHAR root[MAX_PATH];
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW found;
    FILETIME newest = { 0, 0 };
    HANDLE search;
    DWORD result = ERROR_FILE_NOT_FOUND;

    if (GetEnvironmentVariableW(L"SystemRoot", root, ARRAYSIZE(root)) == 0) {
        return GetLastError();
    }
    (void)_snwprintf_s(pattern, ARRAYSIZE(pattern), _TRUNCATE,
                       L"%s\\System32\\DriverStore\\FileRepository\\qcbtuart.inf_amd64_*", root);
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    do {
        WCHAR candidate[MAX_PATH];
        WCHAR patch[MAX_PATH];

        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }
        (void)_snwprintf_s(candidate, ARRAYSIZE(candidate), _TRUNCATE,
                           L"%s\\System32\\DriverStore\\FileRepository\\%s", root, found.cFileName);
        (void)_snwprintf_s(patch, ARRAYSIZE(patch), _TRUNCATE, L"%s\\hpbtfw21.tlv", candidate);
        if (GetFileAttributesW(patch) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        if (result != ERROR_SUCCESS || CompareFileTime(&found.ftLastWriteTime, &newest) > 0) {
            if (wcscpy_s(Directory, Chars, candidate) == 0) {
                newest = found.ftLastWriteTime;
                result = ERROR_SUCCESS;
            }
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return result;
}

/* ------------------------------------------------------------------ inbound path */

/* IBS bytes between packets. Claimed only in steady state. */
static unsigned char
OnIbsByte(void *Context, unsigned char Byte)
{
    QCA_BACKEND *B = (QCA_BACKEND *)Context;

    if (B->Phase != 2) {
        return 0;
    }
    switch (Byte) {
    case QCA_IBS_WAKE_IND:
        /* Acknowledged by the writer between packets, never from the read path. */
        B->Stats.IbsWakeIndRx++;
        if (InterlockedExchange(&B->IbsAckPending, 1) != 0 &&
            InterlockedIncrement(&B->IbsAckRepeats) == 10) {
            InterlockedExchange(&B->IbsAckWarning, 1);
        }
        (void)SetEvent(B->TxEvent);
        return 1;
    case QCA_IBS_SLEEP_IND:
        B->Stats.IbsSleepIndRx++;
        return 1;
    case QCA_IBS_WAKE_ACK:
        InterlockedExchange(&B->IbsTxAwake, 1);
        (void)SetEvent(B->IbsAckEvent);
        return 1;
    default:
        return 0;
    }
}

/* One H4 packet, under the controller lock. */
static void
OnPacket(void *Context, unsigned char Type, const unsigned char *Payload, unsigned long Length)
{
    QCA_BACKEND *B = (QCA_BACKEND *)Context;
    UCHAR framed[HCI_BRIDGE_MAX_EVENT_SIZE + 1];

    if (B->Phase == 0 || B->Phase == 1) {
        if (!B->Waiting || Length + 1u > sizeof(framed)) {
            return;
        }
        framed[0] = Type;
        memcpy(framed + 1, Payload, Length);
        if (B->Phase == 0) {
            if (QcaIdentifyOnPacket(&B->Identify, framed, Length + 1u)) {
                B->Waiting = FALSE;
                B->Matched = TRUE;
                (void)SetEvent(B->ReplyEvent);
            }
            return;
        }
        if (B->Fsm.State == QcaFsmStateHciReset && Type == H4_PKT_EVENT) {
            B->HciResetLength = min(Length, (unsigned long)sizeof(B->HciReset));
            memcpy(B->HciReset, Payload, B->HciResetLength);
        }
        if (QcaFsmOnPacket(&B->Fsm, framed, Length + 1u)) {
            B->Waiting = FALSE;
            B->Matched = TRUE;
            (void)SetEvent(B->ReplyEvent);
        }
        return;
    }

    if (B->Phase == 2) {
        unsigned char queued = 0;
        HCI_STREAM stream = HciStreamEvent;

        if (Type == H4_PKT_EVENT) {
            UCHAR fixed[6];

            /* BTHPORT's Write_LE_Host_Support (Simultaneous_LE_Host = 1) is rejected with 0x11 by
             * this controller; report success so LE scanning starts (as the vendor driver does). */
            if (Length == sizeof(fixed) && Payload[0] == HCI_EV_COMMAND_COMPLETE && Payload[1] == 4u &&
                Payload[3] == (UCHAR)(HCI_OP_WRITE_LE_HOST_SUPPORTED & 0xFFu) &&
                Payload[4] == (UCHAR)(HCI_OP_WRITE_LE_HOST_SUPPORTED >> 8) &&
                Payload[5] == HCI_ERR_UNSUPPORTED_FEATURE) {
                memcpy(fixed, Payload, sizeof(fixed));
                fixed[5] = 0x00;
                Payload = fixed;
            }
            /* Rewritten voice setups are answered under BTHPORT's legacy opcode (sco_route.h). */
            if (Length == sizeof(fixed) && Payload != fixed) {
                memcpy(fixed, Payload, sizeof(fixed));
                if (ScoRouteRestoreEvent(&B->ScoRoute, fixed, sizeof(fixed))) {
                    Payload = fixed;
                }
            }
            stream = HciStreamEvent;
            queued = HciBridgeOnEvent(&B->Bridge, Payload, Length);
        } else if (Type == H4_PKT_ACL) {
            stream = HciStreamAcl;
            queued = HciBridgeOnAcl(&B->Bridge, Payload, Length);
        } else if (Type == H4_PKT_SCO) {
            stream = HciStreamSco;
            queued = HciBridgeOnSco(&B->Bridge, Payload, Length);
        }
        if (queued) {
            B->PendingNotifyMask |= 1ul << (ULONG)stream;
        }
    }
}

static DWORD WINAPI
ReaderThread(LPVOID Parameter)
{
    QCA_BACKEND *B = (QCA_BACKEND *)Parameter;
    UCHAR buffer[READ_BUFFER];
    ULONG failures = 0;
    HANDLE mmcss = MmcssEnter();

    if (mmcss == NULL) {
        LOG(B, "uart reader: MMCSS registration failed (%lu); normal priority", GetLastError());
    }
    while (!B->Stop) {
        ULONG got = 0;
        ULONG fed = 0;
        DWORD error;

        if (B->ReaderPause) {
            InterlockedExchange(&B->ReaderPaused, 1);
            (void)SetEvent(B->ReaderIdle);
            (void)WaitForSingleObject(B->ReaderResume, INFINITE);
            InterlockedExchange(&B->ReaderPaused, 0);
            continue;
        }
        error = UartRead(&B->Port, buffer, sizeof(buffer), &got, INFINITE);
        if (error != ERROR_SUCCESS) {
            B->Stats.ReadErrors++;
            if (failures < 20u) {
                failures++;
            }
            if (failures == 20u) {
                ReportFault(B, "UART read failing", error);
            }
            Sleep(10);
            continue;
        }
        failures = 0;
        if (got == 0) {
            continue;
        }
        B->Stats.BytesRead += got;
        /*
         * Steady state decodes no more packets than the host's queues can take (INBOUND
         * BACKPRESSURE in hci_bridge.h) and notifies between slices so the front end drains them.
         * While they stay full the rest waits here and nothing more is read, so the UART's
         * RTS/CTS flow control holds the controller. Other phases decode the whole read at once.
         */
        while (fed < got && !B->Stop) {
            ULONG mask;
            ULONG step;

            EnterCriticalSection(B->Lock);
            if (B->Phase == 2 && !B->WriterStop) {
                step = H4DecoderFeedLimited(&B->Decoder, buffer + fed, got - fed,
                                            HciBridgeInboundRoom(&B->Bridge), OnPacket, OnIbsByte, B);
            } else {
                H4DecoderFeedEx(&B->Decoder, buffer + fed, got - fed, OnPacket, OnIbsByte, B);
                step = got - fed;
            }
            fed += step;
            mask = B->PendingNotifyMask;
            B->PendingNotifyMask = 0;
            LeaveCriticalSection(B->Lock);
            if (InterlockedExchange(&B->IbsAckWarning, 0) != 0) {
                LOG(B, "IBS: controller repeated WAKE_IND 10 times while an acknowledgement was pending");
            }
            for (ULONG s = 0; s < (ULONG)HciStreamMax; s++) {
                if (mask & (1ul << s)) {
                    HciTransportNotify(B->Transport, (HCI_STREAM)s);
                }
            }
            if (step == 0) {
                Sleep(1);
            }
        }
    }
    MmcssLeave(mmcss);
    return 0;
}

/*
 * Parks the reader with no read pending. Idempotent. The cancel repeats: a
 * read issued just after one cancel would otherwise pend until the next byte.
 */
static DWORD
QuiesceReader(QCA_BACKEND *B)
{
    if (B->ReaderPaused) {
        return ERROR_SUCCESS;
    }
    (void)ResetEvent(B->ReaderIdle);
    InterlockedExchange(&B->ReaderPause, 1);
    for (ULONG waited = 0; waited < QUIESCE_MS; waited += 50u) {
        UartCancelRead(&B->Port);
        if (WaitForSingleObject(B->ReaderIdle, 50) == WAIT_OBJECT_0) {
            return ERROR_SUCCESS;
        }
    }
    return ERROR_TIMEOUT;
}

/* Restarts the reader at the current rate with a fresh decoder. */
static void
ResumeReader(QCA_BACKEND *B)
{
    EnterCriticalSection(B->Lock);
    H4DecoderInit(&B->Decoder);
    LeaveCriticalSection(B->Lock);
    if (B->ReaderPaused) {
        /* Cleared here, not by the reader, so a Quiesce right after this cannot see a stale park. */
        InterlockedExchange(&B->ReaderPaused, 0);
        InterlockedExchange(&B->ReaderPause, 0);
        (void)SetEvent(B->ReaderResume);
    }
}

/* ------------------------------------------------------------------ outbound path */

static unsigned char
Enqueue(QCA_BACKEND *B, UCHAR Type, const unsigned char *Packet, unsigned long Length)
{
    unsigned char ok = 0;

    EnterCriticalSection(&B->TxLock);
    if (B->Phase != 2 || B->Faulted || B->WriterStop || B->Stop || Length == 0) {
        LeaveCriticalSection(&B->TxLock);
        return 0;
    }
    if (B->TxCount < QCA_BACKEND_TX_SLOTS) {
        QCA_BACKEND_TX_SLOT *slot = &B->Tx[(B->TxHead + B->TxCount) % QCA_BACKEND_TX_SLOTS];

        slot->Length = H4EncodePacket(Type, Packet, Length, slot->Data, sizeof(slot->Data));
        if (slot->Length != 0) {
            B->TxCount++;
            ok = 1;
        }
    } else {
        B->Stats.TxQueueFull++;
    }
    LeaveCriticalSection(&B->TxLock);
    if (ok) {
        (void)SetEvent(B->TxEvent);
    }
    return ok;
}

static unsigned char
WireSendCommand(void *Context, const unsigned char *Packet, unsigned long Length)
{
    return Enqueue((QCA_BACKEND *)Context, H4_PKT_COMMAND, Packet, Length);
}

static unsigned char
WireSendAcl(void *Context, const unsigned char *Packet, unsigned long Length)
{
    return Enqueue((QCA_BACKEND *)Context, H4_PKT_ACL, Packet, Length);
}

static unsigned char
WireSendSco(void *Context, const unsigned char *Packet, unsigned long Length)
{
    return Enqueue((QCA_BACKEND *)Context, H4_PKT_SCO, Packet, Length);
}

static const HCI_BRIDGE_WIRE_OPS g_WireOps = { WireSendCommand, WireSendAcl, WireSendSco };

/* CTS gates TX; RTS gates RX. Do not pulse RTS for ordinary TX flow control.
 * This AMD UART rejects SERIAL_EV_CTS notifications, so poll only while CTS is low. */
static DWORD
WaitForCts(QCA_BACKEND *B, BOOL *WasLow)
{
    ULONGLONG deadline = 0;

    if (WasLow != NULL) {
        *WasLow = FALSE;
    }
    for (;;) {
        ULONG status;
        DWORD error;
        ULONGLONG now;

        if (B->Stop || B->WriterStop || B->Faulted) {
            return ERROR_OPERATION_ABORTED;
        }
        error = UartGetModemStatus(&B->Port, &status);
        if (error != ERROR_SUCCESS) {
            return error;
        }
        if (status & 0x10ul) {   /* SERIAL_CTS_STATE */
            return ERROR_SUCCESS;
        }
        now = GetTickCount64();
        if (deadline == 0) {
            deadline = now + WRITE_TIMEOUT_MS;
            if (WasLow != NULL) {
                *WasLow = TRUE;
            }
        } else if (now >= deadline) {
            return ERROR_NOT_READY;
        }
        (void)WaitForSingleObject(B->TxEvent, 1);   /* stop wakes the writer immediately */
    }
}

/* Only at packet boundaries. A failed CTS wait must not consume the pending ACK. */
static DWORD
ServiceIbs(QCA_BACKEND *B)
{
    static const UCHAR wakeAck[] = { QCA_IBS_WAKE_ACK };
    BOOL ctsWasLow;
    DWORD error;

    if (!B->IbsAckPending) {
        return ERROR_SUCCESS;
    }
    error = WaitForCts(B, &ctsWasLow);
    if (ctsWasLow) {
        B->Stats.IbsAckCtsLow++;
    }
    if (error != ERROR_SUCCESS) {
        return error;
    }
    if (B->Stop || B->WriterStop || B->Faulted) {
        return ERROR_OPERATION_ABORTED;
    }
    /* A new indication during the write must leave another ACK pending. */
    InterlockedExchange(&B->IbsAckPending, 0);
    error = UartWrite(&B->Port, wakeAck, sizeof(wakeAck), WRITE_TIMEOUT_MS);
    if (error == ERROR_SUCCESS) {
        B->Stats.IbsWakeAckTx++;
        B->Stats.BytesWritten++;
        InterlockedExchange(&B->IbsAckRepeats, 0);
    } else {
        InterlockedExchange(&B->IbsAckPending, 1);
        B->Stats.WriteErrors++;
    }
    return error;
}

static DWORD WINAPI
WriterThread(LPVOID Parameter)
{
    QCA_BACKEND *B = (QCA_BACKEND *)Parameter;
    QCA_BACKEND_TX_SLOT *slot;
    UCHAR packet[QCA_BACKEND_TX_SLOT_BYTES];
    HANDLE mmcss = MmcssEnter();

    if (mmcss == NULL) {
        LOG(B, "uart writer: MMCSS registration failed (%lu); normal priority", GetLastError());
    }
    while (!B->Stop && !B->WriterStop && !B->Faulted) {
        (void)WaitForSingleObject(B->TxEvent, 1000);
        while (!B->Stop && !B->WriterStop && !B->Faulted) {
            ULONG length = 0;
            DWORD error = ServiceIbs(B);   /* between packets, never inside one */

            if (error != ERROR_SUCCESS) {
                ReportFault(B, "UART IBS acknowledgement failed", error);
                goto finished;
            }
            EnterCriticalSection(&B->TxLock);
            if (B->TxCount != 0) {
                slot = &B->Tx[B->TxHead];
                length = slot->Length;
                memcpy(packet, slot->Data, length);
                B->TxHead = (B->TxHead + 1u) % QCA_BACKEND_TX_SLOTS;
                B->TxCount--;
            }
            LeaveCriticalSection(&B->TxLock);
            if (length == 0 || B->Stop || B->WriterStop || B->Faulted) {
                break;
            }
            error = WaitForCts(B, NULL);
            if (error == ERROR_SUCCESS) {
                /* A wake indication may have arrived while TX was blocked. ACK it first. */
                error = ServiceIbs(B);
            }
            if (error != ERROR_SUCCESS) {
                ReportFault(B, "UART CTS wait before data failed", error);
                goto finished;
            }
            if (B->Stop || B->WriterStop || B->Faulted) {
                break;
            }
            error = UartWrite(&B->Port, packet, length, SEND_TIMEOUT_MS);
            if (error == ERROR_SUCCESS) {
                B->Stats.BytesWritten += length;
            } else {
                B->Stats.WriteErrors++;
                LOG(B, "UART failed packet: H4 0x%02X, %lu bytes, driver completed %lu; not replaying",
                    packet[0], length, B->Port.WriteTransferred);
                ReportFault(B, "UART write failed", error);
                goto finished;   /* a prefix may be on the wire; no replay or later packet */
            }
        }
    }
finished:
    MmcssLeave(mmcss);
    return 0;
}

/* ------------------------------------------------------------------ bring-up */

static DWORD
SetRate(QCA_BACKEND *B, ULONG Baud)
{
    DWORD error = UartConfigure(&B->Port, Baud);

    if (error == ERROR_SUCCESS) {
        B->CurrentBaud = Baud;
    }
    return error;
}

/* Writes one bring-up command and, if Wait, waits for the reply OnPacket accepts. */
static DWORD
Exchange(QCA_BACKEND *B, const UCHAR *Command, ULONG Length, BOOLEAN Wait, ULONG TimeoutMs)
{
    DWORD error;

    EnterCriticalSection(B->Lock);
    B->Waiting = Wait;
    B->Matched = FALSE;
    (void)ResetEvent(B->ReplyEvent);
    LeaveCriticalSection(B->Lock);

    error = UartWrite(&B->Port, Command, Length, SEND_TIMEOUT_MS);
    if (error == ERROR_SUCCESS) {
        B->Stats.BytesWritten += Length;
    }
    if (error != ERROR_SUCCESS || !Wait) {
        return error;
    }
    if (WaitForSingleObject(B->ReplyEvent, TimeoutMs) != WAIT_OBJECT_0) {
        EnterCriticalSection(B->Lock);
        B->Waiting = FALSE;
        LeaveCriticalSection(B->Lock);
        return ERROR_TIMEOUT;
    }
    return ERROR_SUCCESS;
}

/* Identify ladder at 115200 / 3000000 / 3200000. Returns the answering rate or 0. */
static ULONG
ProbeRate(QCA_BACKEND *B)
{
    ULONG rate;

    B->Phase = 0;
    QcaIdentifyInit(&B->Identify);
    while (QcaIdentifyNextRate(&B->Identify, &rate)) {
        UCHAR request[8];
        ULONG length = QcaIdentifyBuildRequest(request, sizeof(request));
        BOOL ctsBefore;
        ULONG pulses;

        if (QuiesceReader(B) != ERROR_SUCCESS || SetRate(B, rate) != ERROR_SUCCESS) {
            continue;   /* a rate the host UART refuses is skipped without sending anything */
        }
        (void)UartSetTimeouts(&B->Port, READ_INTERVAL_MS, 0, 0, WRITE_TIMEOUT_MS);
        (void)UartPurge(&B->Port);
        if (UartWakeController(&B->Port, &ctsBefore, &pulses) != ERROR_SUCCESS) {
            continue;   /* no byte without observed CTS */
        }
        ResumeReader(B);
        if (Exchange(B, request, length, TRUE, IDENTIFY_REPLY_MS) == ERROR_SUCCESS) {
            const QCA_SOC_VERSION *v = &B->Identify.Version;

            LOG(B, "identify: answered at %lu baud; SoC 0x%08lX product 0x%lX patch 0x%04X rom 0x%04X",
                rate, v->SocId, v->ProductId, v->PatchVersion, v->RomVersionField);
            return rate;
        }
    }
    return 0;
}

/* IBS wake x3, SoC reset, settle. The controller restarts in ROM at 115200. */
static DWORD
ResetSoc(QCA_BACKEND *B, ULONG Baud)
{
    static const UCHAR ibsWake[] = { QCA_IBS_WAKE_IND, QCA_IBS_WAKE_IND, QCA_IBS_WAKE_IND };
    static const UCHAR socReset[] = { H4_PKT_COMMAND, 0x40, 0xFC, 0x00 };
    BOOL ctsBefore;
    ULONG pulses;
    DWORD error = QuiesceReader(B);

    if (error == ERROR_SUCCESS && B->CurrentBaud != Baud) {
        error = SetRate(B, Baud);
    }
    if (error == ERROR_SUCCESS) {
        error = UartWakeController(&B->Port, &ctsBefore, &pulses);
    }
    if (error == ERROR_SUCCESS) {
        error = UartWrite(&B->Port, ibsWake, sizeof(ibsWake), WRITE_TIMEOUT_MS);
    }
    if (error == ERROR_SUCCESS) {
        Sleep(IBS_WAKE_SETTLE_MS);
        error = UartWrite(&B->Port, socReset, sizeof(socReset), WRITE_TIMEOUT_MS);
    }
    if (error == ERROR_SUCCESS) {
        Sleep(SOC_RESET_SETTLE_MS);
    }
    return error;
}

/* ROM at 115200 whatever the controller was left running. */
static DWORD
EnsureRom(QCA_BACKEND *B)
{
    ULONG baud = ProbeRate(B);
    DWORD error;

    B->Stats.EntryBaud = baud;
    if (baud == INIT_BAUD) {
        return ERROR_SUCCESS;
    }
    B->Stats.EntryReset = 1;
    LOG(B, "ensure-rom: controller %s; resetting at %lu", baud ? "running firmware" : "silent",
        baud ? baud : OPER_BAUD);
    error = ResetSoc(B, baud != 0 ? baud : OPER_BAUD);
    if (error != ERROR_SUCCESS) {
        ULONG modem;
        B->CtsUnresponsive = error == ERROR_NOT_READY &&
            UartGetModemStatus(&B->Port, &modem) == ERROR_SUCCESS && !(modem & 0x10ul);
        return error;
    }
    return ProbeRate(B) == INIT_BAUD ? ERROR_SUCCESS : ERROR_NOT_READY;
}

/* 0xFC48 was sent: let the controller switch, follow it, drop what arrived meanwhile. */
static DWORD
SwitchBaud(QCA_BACKEND *B, ULONG Baud)
{
    DWORD error = QuiesceReader(B);

    if (error != ERROR_SUCCESS) {
        return error;
    }
    Sleep(BAUD_SETTLE_MS);
    error = SetRate(B, Baud);
    if (error == ERROR_SUCCESS) {
        error = UartPurge(&B->Port);
    }
    if (error == ERROR_SUCCESS) {
        ResumeReader(B);
    }
    return error;
}

/* Firmware download to HCI_Reset. */
static DWORD
BringUp(QCA_BACKEND *B)
{
    B->Phase = 1;
    ResumeReader(B);
    for (;;) {
        UCHAR command[QCA_FSM_MAX_COMMAND];
        ULONG length = 0;
        ULONG newBaud = 0;
        QCA_FSM_STATE before;
        QCA_FSM_ACTION action;
        BOOLEAN wait;
        DWORD error;

        EnterCriticalSection(B->Lock);
        before = B->Fsm.State;
        action = QcaFsmNext(&B->Fsm, command, &length, &newBaud);
        LeaveCriticalSection(B->Lock);

        if (action == QcaFsmActionDone) {
            return ERROR_SUCCESS;
        }
        if (action == QcaFsmActionFailed) {
            LOG(B, "bring-up: state machine failed in %s", QcaFsmStateName(before));
            return ERROR_GEN_FAILURE;
        }
        wait = (BOOLEAN)(action == QcaFsmActionSend);
        if (before == QcaFsmStateBoardIdRequest) {
            Sleep(NVM_SETTLE_MS);   /* patch just applied; a lost board ID would select the .bin */
        }
        error = Exchange(B, command, length, wait, FSM_REPLY_MS);
        if (error == ERROR_SUCCESS && action == QcaFsmActionSendBaudAndSwitch) {
            error = SwitchBaud(B, newBaud);
            if (error == ERROR_SUCCESS) {
                LOG(B, "bring-up: switched to %lu baud", newBaud);
            }
        }
        if (error != ERROR_SUCCESS) {
            if (before == QcaFsmStateBoardIdRequest) {
                /* Optional, as upstream: fall back to the .bin NVM. */
                EnterCriticalSection(B->Lock);
                QcaFsmOnBoardIdTimeout(&B->Fsm);
                LeaveCriticalSection(B->Lock);
                LOG(B, "bring-up: board ID unanswered (Win32 %lu); using the .bin NVM", error);
                continue;
            }
            if (before == QcaFsmStateNvmDownload && !B->Fsm.NvmFallbackUsed) {
                EnterCriticalSection(B->Lock);
                QcaFsmOnNvmFailure(&B->Fsm);
                LeaveCriticalSection(B->Lock);
                if (B->Fsm.State != QcaFsmStateFailed) {
                    LOG(B, "bring-up: NVM download failed (Win32 %lu); falling back", error);
                    continue;
                }
            }
            if (before == QcaFsmStateHciReset && B->HciResetLength >= 6 && B->HciReset[0] == HCI_EV_COMMAND_COMPLETE &&
                B->HciReset[3] == 0x03 && B->HciReset[4] == 0x0C) {
                /* The state machine accepts only status 0; any other surfaces here as a timeout. */
                LOG(B, "bring-up: HCI_Reset answered with status 0x%02X", B->HciReset[5]);
            }
            LOG(B, "bring-up: %s failed (Win32 %lu)", QcaFsmStateName(before), error);
            return error;
        }
        if (before == QcaFsmStateBoardIdRequest && B->Fsm.BoardIdValid) {
            LOG(B, "bring-up: board ID 0x%04X, NVM %s", B->Fsm.BoardId, B->Fsm.SelectedNvmName);
        }
        if (wait && before == QcaFsmStateHciReset) {
            /* Answered with status 0 (the only reply the state machine accepts). The driver stops here too. */
            return ERROR_SUCCESS;
        }
    }
}

/* Steady timeouts, host IBS wake, writer, bridge ready. */
static DWORD
EnterSteady(QCA_BACKEND *B)
{
    static const UCHAR wakeInd[] = { QCA_IBS_WAKE_IND };
    BOOL ctsBefore;
    ULONG pulses;
    DWORD error = QuiesceReader(B);

    /* Reads complete on the first byte so no WAKE_IND waits behind a read interval. */
    if (error == ERROR_SUCCESS) {
        error = UartSetTimeouts(&B->Port, 0xFFFFFFFFul, 0xFFFFFFFFul, STEADY_READ_IDLE_MS, WRITE_TIMEOUT_MS);
    }
    if (error != ERROR_SUCCESS) {
        return error;
    }
    EnterCriticalSection(B->Lock);
    B->Waiting = FALSE;
    B->Phase = 2;
    LeaveCriticalSection(B->Lock);
    ResumeReader(B);

    error = UartWakeController(&B->Port, &ctsBefore, &pulses);
    for (ULONG tries = 0; error == ERROR_SUCCESS && tries < IBS_WAKE_TRIES && !B->IbsTxAwake; tries++) {
        (void)ResetEvent(B->IbsAckEvent);
        error = UartWrite(&B->Port, wakeInd, sizeof(wakeInd), WRITE_TIMEOUT_MS);
        B->Stats.IbsWakeTries = tries + 1;
        if (error == ERROR_SUCCESS) {
            (void)WaitForSingleObject(B->IbsAckEvent, IBS_WAKE_RETRANS_MS);
            error = ServiceIbs(B);
        }
    }
    if (error != ERROR_SUCCESS) {
        return error;
    }
    if (!B->IbsTxAwake) {
        /* Every WAKE_IND was written but none acknowledged: the controller is not serving HCI. */
        LOG(B, "steady: host wake NOT acknowledged after %lu WAKE_IND", B->Stats.IbsWakeTries);
        return ERROR_TIMEOUT;
    }
    LOG(B, "steady: host wake acknowledged after %lu WAKE_IND", B->Stats.IbsWakeTries);

    B->Writer = CreateThread(NULL, 0, WriterThread, B, 0, NULL);
    if (B->Writer == NULL) {
        return GetLastError();
    }
    EnterCriticalSection(B->Lock);
    HciBridgeSetReady(&B->Bridge, 1);
    LeaveCriticalSection(B->Lock);
    return ERROR_SUCCESS;
}

/* ------------------------------------------------------------------ transport adapter */

static unsigned char
TSubmitCommand(HCI_TRANSPORT *Transport, const unsigned char *Packet, unsigned long Length)
{
    QCA_BACKEND *B = (QCA_BACKEND *)Transport->Context;
    UCHAR enhanced[SCO_ROUTE_MAX_COMMAND];
    unsigned long length = ScoRouteRewriteCommand(&B->ScoRoute, Packet, Length, enhanced, sizeof(enhanced));

    /* Voice links go on the HCI data path, not the controller's default offload route. */
    if (length != 0) {
        return HciTransportSubmitCommand(&B->BridgeTransport, enhanced, length);
    }
    return HciTransportSubmitCommand(&B->BridgeTransport, Packet, Length);
}

static unsigned char
TSubmitAcl(HCI_TRANSPORT *Transport, const unsigned char *Packet, unsigned long Length)
{
    return HciTransportSubmitAcl(&((QCA_BACKEND *)Transport->Context)->BridgeTransport, Packet, Length);
}

static unsigned char
TSubmitSco(HCI_TRANSPORT *Transport, const unsigned char *Packet, unsigned long Length)
{
    return HciTransportSubmitSco(&((QCA_BACKEND *)Transport->Context)->BridgeTransport, Packet, Length);
}

static unsigned char
THasStream(const HCI_TRANSPORT *Transport, HCI_STREAM Stream)
{
    return HciTransportHasStream(&((const QCA_BACKEND *)Transport->Context)->BridgeTransport, Stream);
}

static unsigned char
TPopStream(HCI_TRANSPORT *Transport, HCI_STREAM Stream, unsigned char *Buffer, unsigned long Capacity,
           unsigned long *Written)
{
    return HciTransportPopStream(&((QCA_BACKEND *)Transport->Context)->BridgeTransport, Stream, Buffer,
                                 Capacity, Written);
}

static unsigned long
TLastEventLength(const HCI_TRANSPORT *Transport)
{
    (void)Transport;
    return 0;   /* asynchronous backend */
}

/* Under the controller lock, which the reader also holds while decoding. */
static void
TReset(HCI_TRANSPORT *Transport)
{
    QCA_BACKEND *B = (QCA_BACKEND *)Transport->Context;

    /* Framing survives: the rest of a packet in progress is still on the wire (h4_codec.h). */
    H4DecoderDiscardPacket(&B->Decoder);
    B->PendingNotifyMask = 0;
    ScoRouteReset(&B->ScoRoute);
    HciTransportReset(&B->BridgeTransport);
}

static unsigned char
TPeekOrder(const HCI_TRANSPORT *Transport, HCI_STREAM Stream, unsigned long *Order)
{
    return HciTransportPeekOrder(&((const QCA_BACKEND *)Transport->Context)->BridgeTransport, Stream, Order);
}

static const HCI_TRANSPORT_OPS g_TransportOps = {
    TSubmitCommand, TSubmitAcl, TSubmitSco, THasStream, TPopStream, TLastEventLength, TReset, TPeekOrder
};

/* ------------------------------------------------------------------ lifecycle */

static void
StopThreads(QCA_BACKEND *B)
{
    InterlockedExchange(&B->Stop, 1);
    if (B->Writer != NULL) {
        (void)SetEvent(B->TxEvent);
        if (WaitForSingleObject(B->Writer, 10000) != WAIT_OBJECT_0) {
            InterlockedExchange(&B->Stuck, 1);
            LOG(B, "fatal: UART writer thread did not stop");
            return;
        }
        CloseHandle(B->Writer);
        B->Writer = NULL;
    }
    if (B->Reader != NULL) {
        DWORD result = WAIT_TIMEOUT;

        for (ULONG waited = 0; waited < 5000u; waited += 50u) {
            (void)SetEvent(B->ReaderResume);
            UartCancelRead(&B->Port);
            result = WaitForSingleObject(B->Reader, 50);
            if (result == WAIT_OBJECT_0) {
                break;
            }
        }
        if (result != WAIT_OBJECT_0) {
            InterlockedExchange(&B->Stuck, 1);
            LOG(B, "fatal: UART reader thread did not stop");
            return;
        }
        CloseHandle(B->Reader);
        B->Reader = NULL;
    }
}

static void
Release(QCA_BACKEND *B)
{
    HANDLE *events[] = { &B->ReplyEvent, &B->ReaderIdle, &B->ReaderResume, &B->TxEvent, &B->IbsAckEvent };

    if (B->Stuck) {
        return;   /* A live worker still owns the port, events and locks. */
    }
    UartClose(&B->Port);
    for (ULONG i = 0; i < ARRAYSIZE(events); i++) {
        if (*events[i] != NULL) {
            CloseHandle(*events[i]);
            *events[i] = NULL;
        }
    }
    DeleteCriticalSection(&B->TxLock);
    FreeFirmware(B);
    B->Lock = NULL;
}

/* Reset to ROM and confirm with the identify ladder. The reader must be alive. */
static void
Handback(QCA_BACKEND *B)
{
    DWORD error = ResetSoc(B, B->CurrentBaud);

    B->Stats.HandbackBaud = (error == ERROR_SUCCESS) ? ProbeRate(B) : 0;
    LOG(B, "handback: controller answers at %lu baud%s", B->Stats.HandbackBaud,
        B->Stats.HandbackBaud == INIT_BAUD ? " (ROM)" : " - NOT in ROM");
    (void)QuiesceReader(B);
    (void)SetRate(B, INIT_BAUD);
}

DWORD
QcaBackendStart(QCA_BACKEND *B, CRITICAL_SECTION *Lock, HCI_TRANSPORT *Transport)
{
    WCHAR path[512];
    WCHAR portName[64];
    HCI_BRIDGE_WIRE wire;
    ULONGLONG started = GetTickCount64();
    DWORD error;

    if (B->Stuck || B->Reader != NULL || B->Writer != NULL) {
        return ERROR_BUSY;
    }

    /* A backend may be started again after a stop (sleep): everything but the configuration is reset. */
    {
        WCHAR controller[ARRAYSIZE(B->Controller)];
        WCHAR firmwareDir[ARRAYSIZE(B->FirmwareDir)];
        QCA_BACKEND_LOG log = B->Log;
        QCA_BACKEND_FAULT onFault = B->OnFault;
        void *faultContext = B->FaultContext;

        memcpy(controller, B->Controller, sizeof(controller));
        memcpy(firmwareDir, B->FirmwareDir, sizeof(firmwareDir));
        ZeroMemory(B, sizeof(*B));
        memcpy(B->Controller, controller, sizeof(controller));
        memcpy(B->FirmwareDir, firmwareDir, sizeof(firmwareDir));
        B->Log = log;
        B->OnFault = onFault;
        B->FaultContext = faultContext;
    }
    B->Lock = Lock;
    B->Transport = Transport;
    InitializeCriticalSection(&B->TxLock);
    B->ReplyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    B->ReaderIdle = CreateEventW(NULL, FALSE, FALSE, NULL);
    B->ReaderResume = CreateEventW(NULL, FALSE, FALSE, NULL);
    B->TxEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    B->IbsAckEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!B->ReplyEvent || !B->ReaderIdle || !B->ReaderResume || !B->TxEvent || !B->IbsAckEvent) {
        error = GetLastError();
        Release(B);
        return error;
    }

    wire.Ops = &g_WireOps;
    wire.Context = B;
    HciBridgeInit(&B->Bridge, &wire);
    ZeroMemory(&B->BridgeTransport, sizeof(B->BridgeTransport));
    HciBridgeBindTransport(&B->BridgeTransport, &B->Bridge);
    ScoRouteReset(&B->ScoRoute);
    H4DecoderInit(&B->Decoder);
    Transport->Ops = &g_TransportOps;
    Transport->Context = B;
    Transport->Backend = HCI_BACKEND_UART;

    error = LoadFirmware(B);
    if (error == ERROR_SUCCESS && !QcaFsmInit(&B->Fsm, B->Patch, B->PatchSize, B->Candidates,
                                              B->CandidateCount, OPER_BAUD)) {
        LOG(B, "firmware: images rejected by the TLV checks");
        error = ERROR_INVALID_DATA;
    }
    if (error == ERROR_SUCCESS) {
        error = UartFindInterface(B->Controller, path, ARRAYSIZE(path), portName, ARRAYSIZE(portName));
        if (error != ERROR_SUCCESS) {
            LOG(B, "uart: no published interface for %ls (Win32 %lu)", B->Controller, error);
        }
    }
    if (error == ERROR_SUCCESS) {
        error = UartOpen(&B->Port, path);
        LOG(B, "uart: open %ls (%ls): Win32 %lu", path, portName, error);
    }
    if (error != ERROR_SUCCESS) {
        Release(B);
        return error;
    }

    /* Reader starts parked; every step below positions the line before resuming it. */
    B->ReaderPause = 1;
    B->Reader = CreateThread(NULL, 0, ReaderThread, B, 0, NULL);
    if (B->Reader == NULL || WaitForSingleObject(B->ReaderIdle, QUIESCE_MS) != WAIT_OBJECT_0) {
        error = (B->Reader == NULL) ? GetLastError() : ERROR_TIMEOUT;
        StopThreads(B);
        Release(B);
        return error;
    }

    error = EnsureRom(B);
    if (error == ERROR_SUCCESS) {
        (void)QuiesceReader(B);
        (void)UartSetTimeouts(&B->Port, READ_INTERVAL_MS, 0, 0, WRITE_TIMEOUT_MS);
        error = BringUp(B);
    }
    if (error == ERROR_SUCCESS) {
        LOG(B, "bring-up: HCI_Reset complete; NVM %s, %lu baud", B->Fsm.SelectedNvmName, B->CurrentBaud);
        error = EnterSteady(B);
    }
    B->Stats.BringUpMs = (ULONG)(GetTickCount64() - started);
    if (error != ERROR_SUCCESS) {
        LOG(B, "start failed (Win32 %lu) after %lu ms; handing the controller back", error, B->Stats.BringUpMs);
        QcaBackendStop(B);
        return error;
    }
    LOG(B, "steady: bridge ready %lu ms after start", B->Stats.BringUpMs);
    return ERROR_SUCCESS;
}

void
QcaBackendStop(QCA_BACKEND *B)
{
    HANDLE writer;

    if (B->Stuck || B->Lock == NULL) {
        return;
    }
    EnterCriticalSection(B->Lock);
    B->Phase = 3;
    HciBridgeSetReady(&B->Bridge, 0);
    LeaveCriticalSection(B->Lock);

    /* The writer goes first; the reader stays for the hand-back's identify ladder. */
    writer = B->Writer;
    if (writer != NULL) {
        InterlockedExchange(&B->WriterStop, 1);
        (void)SetEvent(B->TxEvent);
        if (WaitForSingleObject(writer, 10000) != WAIT_OBJECT_0) {
            InterlockedExchange(&B->Stuck, 1);
            LOG(B, "fatal: UART writer thread did not stop");
            return;
        }
        CloseHandle(writer);
        B->Writer = NULL;
    }
    if (B->Reader != NULL && B->Port.Handle != NULL) {
        Handback(B);
    }
    StopThreads(B);
    Release(B);
}

ULONG
QcaBackendDisconnectAll(QCA_BACKEND *B, ULONG TimeoutMs, ULONG *Found)
{
    ULONGLONG deadline = GetTickCount64() + TimeoutMs;
    ULONG open = 0;

    *Found = 0;
    if (B->Lock == NULL) {
        return 0;
    }
    EnterCriticalSection(B->Lock);
    if (B->Phase == 2 && B->Bridge.Ready) {
        *Found = HciBridgeBeginShutdown(&B->Bridge, HCI_ERR_REMOTE_POWER_OFF);
        open = *Found;
    }
    LeaveCriticalSection(B->Lock);
    /* The reader thread feeds the controller's replies to the bridge, which sends the next one. */
    while (open != 0 && GetTickCount64() < deadline) {
        Sleep(10);
        EnterCriticalSection(B->Lock);
        open = HciBridgeOpenLinks(&B->Bridge);
        LeaveCriticalSection(B->Lock);
    }
    return open;
}
