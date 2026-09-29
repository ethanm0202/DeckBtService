/*
 * qca_backend_selftest.c - drives the real src/service/qca_backend.c through QcaBackendStart and
 * QcaBackendStop against a mock of every function in src/service/uart_win32.h. No device is
 * opened: the mock is a QCA2066 on the other end of the UART, simulated well enough for the whole
 * start path (identify ladder, baud switch, rampatch/NVM download with the firmware of the
 * installed vendor package, board ID, logging off, build info, HCI_Reset, host IBS wake) and for
 * the hand-back to ROM (SoC reset, identify at 115200).
 *
 * The controller answers only while the host UART runs at the controller's rate, and its answer
 * to the host's WAKE_IND in steady state is scripted per scenario.
 *
 * Build: tools\selftest.cmd
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../src/service/qca_backend.h"

static int g_fail = 0;

#define CHECK(cond, ...)                           \
    do {                                           \
        if (cond) { printf("  ok   "); }           \
        else      { printf("  FAIL "); g_fail++; } \
        printf(__VA_ARGS__); printf("\n");         \
    } while (0)

/* ---------------------------------------------------------------- mock controller */

typedef enum { ChipRom, ChipPatched, ChipSteady } CHIP_STATE;

typedef struct {
    CRITICAL_SECTION Lock;
    HANDLE     Readable;          /* manual reset: host-bound bytes or a cancel are waiting */
    BOOL       Open;
    ULONG      Opens;
    ULONG      Closes;
    ULONG      HostBaud;          /* last UartConfigure */
    ULONG      ChipBaud;          /* the controller's rate */
    CHIP_STATE State;

    UCHAR      Rx[4096];          /* controller -> host, not yet read */
    ULONG      RxLength;
    BOOL       CancelPending;

    UCHAR      Cmd[300];          /* host -> controller H4 command being received */
    ULONG      CmdLength;
    ULONG      CmdNeed;

    ULONG      FileRemaining;     /* TLV download: bytes of the current file still to come */
    UCHAR      FileMode;

    /* Script: 0 never acknowledges a host WAKE_IND; N acknowledges from the Nth on. */
    ULONG      AckFromWakeInd;

    /* Observations, zeroed per scenario. */
    ULONG      WakeIndRx;         /* host WAKE_INDs while running firmware after HCI_Reset */
    ULONG      WakeAckTx;
    ULONG      SocResets;
    ULONG      AnswersSinceReset; /* version requests answered since the last SoC reset */
    ULONG      LastAnswerBaud;
    ULONG      TlvSegments;
    ULONG      HciResets;
    ULONG      LostBytes;         /* written while the host ran at another rate */
} MOCK_CHIP;

static MOCK_CHIP g_Chip;

/* Recorded from the Steam Deck OLED controller (tools/identify_selftest.c). */
static const UCHAR kVersionReply[] = {
    0x04, 0x0E, 0x12, 0x01, 0x00, 0xFC, 0x00, 0x19, 0x0C,
    0x13, 0x00, 0x00, 0x00, 0xE6, 0x38, 0x01, 0x02, 0x11, 0x12, 0x0C, 0x40
};
static const UCHAR kBoardIdReply[] = { 0x04, 0x0E, 0x08, 0x01, 0x00, 0xFC, 0x00, 0x23, 0x02, 0x03, 0x09 };
/* QCA2066 Command Complete envelopes (tools/qca_fsm_selftest.c MockEdlCommandComplete). */
static const UCHAR kTlvAck[] = { 0x04, 0x0E, 0x05, 0x01, 0x00, 0xFC, 0x00, EDL_PATCH_TLV_REQ_CMD };
static const UCHAR kBuildInfoReply[] = {
    0x04, 0x0E, 0x09, 0x01, 0x00, 0xFC, 0x00, EDL_GET_BUILD_INFO_CMD, 0x03, 'Q', 'C', 'A'
};
static const UCHAR kLoggingOffReply[] = { 0x04, 0x0E, 0x05, 0x01, 0x17, 0xFC, 0x00, QCA_DISABLE_LOGGING_SUB_OP };

/* Caller holds the lock. */
static void ChipReply(const UCHAR *Data, ULONG Length)
{
    MOCK_CHIP *c = &g_Chip;

    if (c->RxLength + Length > sizeof(c->Rx)) {
        return;
    }
    memcpy(c->Rx + c->RxLength, Data, Length);
    c->RxLength += Length;
    (void)SetEvent(c->Readable);
}

static void ChipCommandComplete(USHORT Opcode)
{
    UCHAR reply[7] = { H4_PKT_EVENT, HCI_EV_COMMAND_COMPLETE, 4, 1, 0, 0, 0 };

    reply[4] = (UCHAR)(Opcode & 0xFFu);
    reply[5] = (UCHAR)(Opcode >> 8);
    ChipReply(reply, sizeof(reply));
}

static ULONG IndexToRate(UCHAR Index)
{
    static const ULONG rates[] = { 115200, 921600, 1000000, 2000000, 3000000, 3200000 };

    for (ULONG i = 0; i < ARRAYSIZE(rates); i++) {
        UCHAR index;
        if (QcaBaudRateToIndex(rates[i], &index) && index == Index) {
            return rates[i];
        }
    }
    return 0;
}

/* One TLV segment: acknowledged as btqca.c qca_tlv_send_segment expects. */
static void ChipTlvSegment(const UCHAR *Segment, ULONG Length)
{
    MOCK_CHIP *c = &g_Chip;
    BOOL ack;

    c->TlvSegments++;
    if (c->FileRemaining == 0 && Length >= 4) {
        /* A new file: the TLV header gives its size and, for the rampatch, the download mode. */
        c->FileRemaining = ((ULONG)Segment[1] | ((ULONG)Segment[2] << 8) | ((ULONG)Segment[3] << 16)) + 4u;
        c->FileMode = (Segment[0] == QCA_TLV_TYPE_PATCH && Length > 14) ? Segment[14] : QCA_SKIP_EVT_NONE;
        if (Segment[0] == QCA_TLV_TYPE_PATCH) {
            c->State = ChipPatched;
        }
    }
    c->FileRemaining -= min(Length, c->FileRemaining);
    ack = Length < QCA_MAX_SIZE_PER_TLV_SEGMENT || c->FileRemaining == 0 ||
          !(c->FileMode == QCA_SKIP_EVT_VSE_CC || c->FileMode == QCA_SKIP_EVT_VSE);
    if (ack) {
        ChipReply(kTlvAck, sizeof(kTlvAck));
    }
}

static void ChipCommand(const UCHAR *Cmd, ULONG Length)
{
    MOCK_CHIP *c = &g_Chip;
    USHORT opcode = (USHORT)(Cmd[1] | ((USHORT)Cmd[2] << 8));

    switch (opcode) {
    case EDL_PATCH_CMD_OPCODE:
        if (Length < 5) {
            return;
        }
        if (Cmd[4] == EDL_PATCH_VER_REQ_CMD) {
            c->AnswersSinceReset++;
            c->LastAnswerBaud = c->ChipBaud;
            ChipReply(kVersionReply, sizeof(kVersionReply));
        } else if (Cmd[4] == EDL_PATCH_TLV_REQ_CMD && Length >= 6u && Length == 6u + Cmd[5]) {
            ChipTlvSegment(Cmd + 6, Cmd[5]);
        } else if (Cmd[4] == EDL_GET_BID_REQ_CMD) {
            ChipReply(kBoardIdReply, sizeof(kBoardIdReply));
        } else if (Cmd[4] == EDL_GET_BUILD_INFO_CMD) {
            ChipReply(kBuildInfoReply, sizeof(kBuildInfoReply));
        }
        return;
    case QCA_BAUDRATE_CMD_OPCODE:
        /* No event (the vendor driver's trace); the controller switches at once. */
        if (Length == 5 && IndexToRate(Cmd[4]) != 0) {
            c->ChipBaud = IndexToRate(Cmd[4]);
        }
        return;
    case 0xFC40u:
        /* SoC reset: the controller restarts in ROM at 115200, nothing in flight survives. */
        c->SocResets++;
        c->AnswersSinceReset = 0;
        c->ChipBaud = 115200;
        c->State = ChipRom;
        c->FileRemaining = 0;
        return;
    case QCA_DISABLE_LOGGING:
        ChipReply(kLoggingOffReply, sizeof(kLoggingOffReply));
        return;
    case 0x0C03u:
        c->HciResets++;
        c->State = ChipSteady;
        ChipCommandComplete(opcode);
        return;
    default:
        ChipCommandComplete(opcode);
        return;
    }
}

static void ChipWakeInd(void)
{
    static const UCHAR wakeAck[] = { QCA_IBS_WAKE_ACK };
    MOCK_CHIP *c = &g_Chip;

    if (c->State != ChipSteady) {
        return;   /* ROM and bring-up ignore IBS */
    }
    c->WakeIndRx++;
    if (c->AckFromWakeInd != 0 && c->WakeIndRx >= c->AckFromWakeInd) {
        c->WakeAckTx++;
        ChipReply(wakeAck, sizeof(wakeAck));
    }
}

/* Host -> controller, one byte. Only commands and IBS bytes are sent by this test. */
static void ChipByte(UCHAR Byte)
{
    MOCK_CHIP *c = &g_Chip;

    if (c->CmdLength == 0) {
        if (Byte == QCA_IBS_WAKE_IND) {
            ChipWakeInd();
        } else if (Byte == H4_PKT_COMMAND) {
            c->Cmd[0] = Byte;
            c->CmdLength = 1;
            c->CmdNeed = 4;
        }
        return;
    }
    c->Cmd[c->CmdLength++] = Byte;
    if (c->CmdLength == 4) {
        c->CmdNeed = 4u + c->Cmd[3];
    }
    if (c->CmdLength == c->CmdNeed) {
        ChipCommand(c->Cmd, c->CmdLength);
        c->CmdLength = 0;
    }
}

static void MockInit(void)
{
    ZeroMemory(&g_Chip, sizeof(g_Chip));
    InitializeCriticalSection(&g_Chip.Lock);
    g_Chip.Readable = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_Chip.ChipBaud = 115200;
    g_Chip.State = ChipRom;
}

/* A new scenario: the controller keeps its rate and state, the script and counters change. */
static void MockScript(ULONG AckFromWakeInd)
{
    EnterCriticalSection(&g_Chip.Lock);
    g_Chip.AckFromWakeInd = AckFromWakeInd;
    g_Chip.WakeIndRx = 0;
    g_Chip.WakeAckTx = 0;
    g_Chip.SocResets = 0;
    g_Chip.AnswersSinceReset = 0;
    g_Chip.LastAnswerBaud = 0;
    g_Chip.TlvSegments = 0;
    g_Chip.HciResets = 0;
    g_Chip.LostBytes = 0;
    LeaveCriticalSection(&g_Chip.Lock);
}

/* ---------------------------------------------------------------- uart_win32.h, mocked */

DWORD UartFindInterface(const WCHAR *ControllerInstanceId, WCHAR *Path, DWORD PathChars,
                        WCHAR *PortName, DWORD PortNameChars)
{
    (void)ControllerInstanceId;
    if (wcscpy_s(Path, PathChars, L"\\\\?\\MOCK#UART") != 0) {
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (PortName != NULL && wcscpy_s(PortName, PortNameChars, L"MOCK") != 0) {
        return ERROR_INSUFFICIENT_BUFFER;
    }
    return ERROR_SUCCESS;
}

DWORD UartOpen(UART_PORT *Port, const WCHAR *Path)
{
    DWORD error = ERROR_SUCCESS;

    (void)Path;
    EnterCriticalSection(&g_Chip.Lock);
    if (g_Chip.Open) {
        error = ERROR_ACCESS_DENIED;   /* exclusive */
    } else {
        ZeroMemory(Port, sizeof(*Port));
        Port->Handle = (HANDLE)&g_Chip;
        g_Chip.Open = TRUE;
        g_Chip.Opens++;
        g_Chip.HostBaud = 0;
        g_Chip.RxLength = 0;
        g_Chip.CmdLength = 0;
        g_Chip.CancelPending = FALSE;
    }
    LeaveCriticalSection(&g_Chip.Lock);
    return error;
}

void UartClose(UART_PORT *Port)
{
    if (Port->Handle == NULL) {
        return;
    }
    EnterCriticalSection(&g_Chip.Lock);
    g_Chip.Open = FALSE;
    g_Chip.Closes++;
    (void)SetEvent(g_Chip.Readable);
    LeaveCriticalSection(&g_Chip.Lock);
    Port->Handle = NULL;
}

DWORD UartConfigure(UART_PORT *Port, ULONG BaudRate)
{
    (void)Port;
    EnterCriticalSection(&g_Chip.Lock);
    if (BaudRate != g_Chip.HostBaud) {
        g_Chip.RxLength = 0;     /* bytes in flight at the old rate arrive as garbage */
        g_Chip.CmdLength = 0;
    }
    g_Chip.HostBaud = BaudRate;
    LeaveCriticalSection(&g_Chip.Lock);
    return ERROR_SUCCESS;
}

DWORD UartSetTimeouts(UART_PORT *Port, ULONG ReadInterval, ULONG ReadTotalMultiplier,
                      ULONG ReadTotalConstant, ULONG WriteTotalConstant)
{
    (void)Port;
    (void)ReadInterval;
    (void)ReadTotalMultiplier;
    (void)ReadTotalConstant;
    (void)WriteTotalConstant;
    return ERROR_SUCCESS;
}

DWORD UartGetModemStatus(UART_PORT *Port, ULONG *Status)
{
    (void)Port;
    *Status = 0x10;   /* SERIAL_CTS_STATE */
    return ERROR_SUCCESS;
}

DWORD UartPurge(UART_PORT *Port)
{
    (void)Port;
    EnterCriticalSection(&g_Chip.Lock);
    g_Chip.RxLength = 0;
    LeaveCriticalSection(&g_Chip.Lock);
    return ERROR_SUCCESS;
}

DWORD UartWakeController(UART_PORT *Port, BOOL *CtsBefore, ULONG *Pulses)
{
    (void)Port;
    *CtsBefore = TRUE;
    *Pulses = 0;
    return ERROR_SUCCESS;
}

DWORD UartWrite(UART_PORT *Port, const void *Data, ULONG Length, ULONG TimeoutMs)
{
    const UCHAR *bytes = (const UCHAR *)Data;
    DWORD error = ERROR_SUCCESS;

    (void)Port;
    (void)TimeoutMs;
    EnterCriticalSection(&g_Chip.Lock);
    if (!g_Chip.Open) {
        error = ERROR_INVALID_HANDLE;
    } else if (g_Chip.HostBaud != g_Chip.ChipBaud) {
        g_Chip.LostBytes += Length;   /* the controller sees noise */
        g_Chip.CmdLength = 0;
    } else {
        for (ULONG i = 0; i < Length; i++) {
            ChipByte(bytes[i]);
        }
    }
    LeaveCriticalSection(&g_Chip.Lock);
    return error;
}

DWORD UartRead(UART_PORT *Port, void *Buffer, ULONG Capacity, ULONG *Read, ULONG TimeoutMs)
{
    ULONGLONG deadline = GetTickCount64() + TimeoutMs;

    (void)Port;
    *Read = 0;
    for (;;) {
        DWORD wait = INFINITE;

        EnterCriticalSection(&g_Chip.Lock);
        if (!g_Chip.Open) {
            LeaveCriticalSection(&g_Chip.Lock);
            return ERROR_INVALID_HANDLE;
        }
        if (g_Chip.RxLength != 0 || g_Chip.CancelPending) {
            ULONG n = min(Capacity, g_Chip.RxLength);

            memcpy(Buffer, g_Chip.Rx, n);
            memmove(g_Chip.Rx, g_Chip.Rx + n, g_Chip.RxLength - n);
            g_Chip.RxLength -= n;
            g_Chip.CancelPending = FALSE;
            if (g_Chip.RxLength == 0) {
                (void)ResetEvent(g_Chip.Readable);
            }
            LeaveCriticalSection(&g_Chip.Lock);
            *Read = n;
            return ERROR_SUCCESS;
        }
        (void)ResetEvent(g_Chip.Readable);
        LeaveCriticalSection(&g_Chip.Lock);
        if (TimeoutMs != INFINITE) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                return ERROR_SUCCESS;
            }
            wait = (DWORD)(deadline - now);
        }
        (void)WaitForSingleObject(g_Chip.Readable, wait);
    }
}

/* Ends a pending read, or the next one if none is pending yet. */
void UartCancelRead(UART_PORT *Port)
{
    (void)Port;
    EnterCriticalSection(&g_Chip.Lock);
    g_Chip.CancelPending = TRUE;
    (void)SetEvent(g_Chip.Readable);
    LeaveCriticalSection(&g_Chip.Lock);
}

/* ---------------------------------------------------------------- scenarios */

static CRITICAL_SECTION g_Lock;
static HCI_TRANSPORT g_Transport;
static QCA_BACKEND g_Qca;

static void BackendLog(const char *Format, ...)
{
    va_list args;

    va_start(args, Format);
    printf("       | ");
    vprintf(Format, args);
    printf("\n");
    va_end(args);
}

/* Stop, then the controller must be back in ROM at 115200 and the port closed. */
static void StopAndCheckHandback(void)
{
    QcaBackendStop(&g_Qca);
    CHECK(g_Chip.SocResets == 1, "stop resets the SoC once (got %lu)", g_Chip.SocResets);
    CHECK(g_Chip.State == ChipRom && g_Chip.ChipBaud == 115200 &&
          g_Chip.AnswersSinceReset >= 1 && g_Chip.LastAnswerBaud == 115200,
          "controller answers identify at %lu baud after the reset (ROM)", g_Chip.LastAnswerBaud);
    CHECK(g_Qca.Stats.HandbackBaud == 115200, "Stats.HandbackBaud == 115200 (got %lu)",
          g_Qca.Stats.HandbackBaud);
    CHECK(!g_Chip.Open && g_Chip.Opens == g_Chip.Closes && g_Qca.Reader == NULL && g_Qca.Writer == NULL,
          "port closed, reader and writer gone (opens %lu, closes %lu)", g_Chip.Opens, g_Chip.Closes);
}

/* The controller acknowledges from host WAKE_IND #AckFrom; start must succeed after AckFrom tries. */
static void ScenarioAcknowledged(ULONG AckFrom)
{
    DWORD error;
    ULONG wakeIndRx;
    ULONG segments;
    ULONG expectedSegments;

    MockScript(AckFrom);
    error = QcaBackendStart(&g_Qca, &g_Lock, &g_Transport);
    EnterCriticalSection(&g_Chip.Lock);
    wakeIndRx = g_Chip.WakeIndRx;
    segments = g_Chip.TlvSegments;
    LeaveCriticalSection(&g_Chip.Lock);
    expectedSegments = g_Qca.Fsm.PatchSegments + g_Qca.Fsm.NvmSegments;

    CHECK(error == ERROR_SUCCESS, "QcaBackendStart returns ERROR_SUCCESS (got %lu) in %lu ms",
          error, g_Qca.Stats.BringUpMs);
    CHECK(g_Chip.State == ChipSteady && g_Chip.HciResets == 1 && g_Chip.ChipBaud == 3000000 &&
          g_Chip.HostBaud == 3000000,
          "controller runs firmware at %lu baud, host UART at %lu", g_Chip.ChipBaud, g_Chip.HostBaud);
    CHECK(segments == expectedSegments && expectedSegments != 0,
          "controller received every firmware segment (%lu of %lu, NVM %s)", segments, expectedSegments,
          g_Qca.Fsm.SelectedNvmName);
    CHECK(g_Qca.Stats.IbsWakeTries == AckFrom && wakeIndRx == AckFrom,
          "host wake acknowledged after %lu WAKE_IND (controller received %lu, want %lu)",
          g_Qca.Stats.IbsWakeTries, wakeIndRx, AckFrom);
    CHECK(g_Qca.IbsTxAwake && g_Qca.Bridge.Ready && g_Qca.Writer != NULL,
          "bridge ready and writer running");
    if (error == ERROR_SUCCESS) {
        StopAndCheckHandback();
    }
}

int main(void)
{
    DWORD n;
    DWORD error;

    n = GetEnvironmentVariableW(L"QCA_FW_DIR", g_Qca.FirmwareDir, ARRAYSIZE(g_Qca.FirmwareDir));
    if ((n == 0 || n >= ARRAYSIZE(g_Qca.FirmwareDir)) &&
        QcaBackendFindVendorFirmware(g_Qca.FirmwareDir, ARRAYSIZE(g_Qca.FirmwareDir)) != ERROR_SUCCESS) {
        printf("FAIL no firmware: set QCA_FW_DIR or install the Qualcomm package\n");
        return 1;
    }
    wcscpy_s(g_Qca.Controller, ARRAYSIZE(g_Qca.Controller), L"ACPI\\AMDI0020\\4");
    g_Qca.Log = BackendLog;
    InitializeCriticalSection(&g_Lock);
    MockInit();

    printf("controller acknowledges the first WAKE_IND\n");
    ScenarioAcknowledged(1);

    printf("\ncontroller acknowledges only the third WAKE_IND\n");
    ScenarioAcknowledged(3);

    printf("\ncontroller never acknowledges a WAKE_IND (every write succeeds)\n");
    MockScript(0);
    error = QcaBackendStart(&g_Qca, &g_Lock, &g_Transport);
    CHECK(error == ERROR_TIMEOUT, "QcaBackendStart returns ERROR_TIMEOUT (got %lu)", error);
    /* The hand-back's own WAKE_INDs before the SoC reset reach the controller too. */
    CHECK(g_Qca.Stats.IbsWakeTries == 10 && g_Chip.WakeIndRx >= 10 && g_Chip.WakeAckTx == 0,
          "10 WAKE_IND tries written, none acknowledged (tries %lu, controller received %lu)",
          g_Qca.Stats.IbsWakeTries, g_Chip.WakeIndRx);
    CHECK(g_Qca.Writer == NULL, "no writer thread left behind");
    CHECK(!g_Qca.Bridge.Ready, "bridge not ready");
    CHECK(g_Chip.SocResets == 1 && g_Chip.State == ChipRom && g_Chip.LastAnswerBaud == 115200 &&
          g_Chip.AnswersSinceReset >= 1,
          "controller handed back: SoC reset %lu, answers identify at %lu baud", g_Chip.SocResets,
          g_Chip.LastAnswerBaud);
    CHECK(g_Qca.Stats.HandbackBaud == 115200, "Stats.HandbackBaud == 115200 (got %lu)",
          g_Qca.Stats.HandbackBaud);
    CHECK(!g_Chip.Open && g_Qca.Reader == NULL && g_Qca.Lock == NULL, "port closed and backend released");
    if (error == ERROR_SUCCESS) {
        QcaBackendStop(&g_Qca);   /* the regression: clean up so the restart below still runs */
    }

    printf("\nthe same backend starts again after the failed start\n");
    ScenarioAcknowledged(1);

    DeleteCriticalSection(&g_Lock);
    printf("\n%s\n", g_fail ? "QCA BACKEND SELFTEST FAILED" : "QCA BACKEND SELFTEST PASSED");
    return g_fail ? 1 : 0;
}
