/*
 * qca_init_fsm.h - Transport-agnostic Qualcomm QCA2066 bring-up state machine.
 *
 * Deliberately knows nothing about the OS, the UART or timers: the caller pumps it.
 *
 * Sequence derived from Linux qca_uart_setup() in drivers/bluetooth/btqca.c
 * (https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/btqca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137):
 *   1. EDL version request           (0xFC00 / 0x19)      at 115200
 *   2. set baud rate                 (0xFC48)             -> host then re-programs the UART
 *   3. stream the rampatch TLV       (0xFC00 / 0x1E)      hpbtfw21.tlv
 *   4. read board ID / select NVM    (0xFC00 / 0x23)      hpnv21[g][.309|.bin]
 *   5. stream the selected NVM TLV   (0xFC00 / 0x1E)
 *   6. disable SOC logging           (0xFC17 / 0x14, 0x00)
 *   7. get firmware build info       (0xFC00 / 0x20)
 *   8. HCI_Reset                     (0x0C03)
 *   9. HCI_Read_Local_Version        (0x1001)             proof of life
 */

#pragma once

#include "qca_protocol.h"

typedef enum _QCA_FSM_STATE {
    QcaFsmStateStart = 0,
    QcaFsmStateVersionRequest,
    QcaFsmStateSetBaudRate,
    QcaFsmStatePatchDownload,
    QcaFsmStateBoardIdRequest,
    QcaFsmStateNvmDownload,
    QcaFsmStateDisableLogging,
    QcaFsmStateBuildInfo,
    QcaFsmStateHciReset,
    QcaFsmStateReadLocalVersion,
    QcaFsmStateReady,
    QcaFsmStateFailed
} QCA_FSM_STATE;

typedef enum _QCA_FSM_ACTION {
    QcaFsmActionSend = 0,      /* write Buffer, then wait for an event before pumping again */
    QcaFsmActionSendNoWait,    /* write Buffer, pump again immediately (unacked TLV segment) */
    /*
     * Write the 0xFC48 command at the current rate; no reply is awaited. The transport stops its
     * reader, lets the controller switch, moves the host to *BaudRate, discards anything received
     * meanwhile, and restarts the reader before pumping again.
     */
    QcaFsmActionSendBaudAndSwitch,
    QcaFsmActionDone,
    QcaFsmActionFailed
} QCA_FSM_ACTION;

#define QCA_FSM_MAX_COMMAND (QCA_MAX_SIZE_PER_TLV_SEGMENT + 16u)
typedef struct _QCA_NVM_CANDIDATE {
    const char  *Name;   /* e.g. "hpnv21g.309" */
    const UCHAR *Data;
    ULONG        Size;
} QCA_NVM_CANDIDATE, *PQCA_NVM_CANDIDATE;


typedef struct _QCA_INIT_FSM {
    QCA_FSM_STATE  State;

    const UCHAR   *Patch;
    ULONG          PatchSize;
    UCHAR          PatchDownloadMode;
    ULONG          PatchSegments;
    ULONG          PatchIndex;

    const QCA_NVM_CANDIDATE *Candidates;
    ULONG          CandidateCount;

    const UCHAR   *Nvm;
    ULONG          NvmSize;
    UCHAR          NvmDownloadMode;
    ULONG          NvmSegments;
    ULONG          NvmIndex;
    ULONG          NvmBaudOffset;   /* file offset of HCI tag data[1], patched to OperBaudIndex */

    ULONG          OperBaudRate;
    UCHAR          OperBaudIndex;

    /* Selection state and breadcrumbs */
    QCA_SOC_VERSION SocVersion;
    USHORT         BoardId;
    BOOLEAN        BoardIdValid;
    char           SelectedNvmName[32];
    ULONG          SelectedNvmIndex;
    BOOLEAN        NvmFallbackUsed;

    /* Observability counters */
    ULONG          CommandsSent;
    ULONG          EventsHandled;
    ULONG          LastFailureState;
    ULONG          UnexpectedEvents;

    QCA_TLV_INFO   PatchInfo;
    QCA_TLV_INFO   NvmInfo;
} QCA_INIT_FSM, *PQCA_INIT_FSM;

/*
 * Validates both images and arms the machine. Returns FALSE (and sets State to Failed) if either
 * TLV fails validation or the requested baud rate is unsupported - the point being that a bad
 * image is rejected before a single byte reaches the chip.
 */
BOOLEAN QcaFsmInit(
    _Out_ PQCA_INIT_FSM Fsm,
    _In_reads_bytes_(PatchSize) const UCHAR *Patch,
    _In_ ULONG PatchSize,
    _In_reads_(CandidateCount) const QCA_NVM_CANDIDATE *Candidates,
    _In_ ULONG CandidateCount,
    _In_ ULONG OperBaudRate);

VOID QcaFsmOnBoardIdTimeout(_Inout_ PQCA_INIT_FSM Fsm);
VOID QcaFsmOnNvmFailure(_Inout_ PQCA_INIT_FSM Fsm);

/*
 * Produces the next operation. Send/SendNoWait return a wire command. SendBaudAndSwitch returns
 * both the 0xFC48 command and the exact host rate to select once it has drained; the machine has
 * already advanced, since no reply to 0xFC48 is awaited.
 */
QCA_FSM_ACTION QcaFsmNext(
    _Inout_ PQCA_INIT_FSM Fsm,
    _Out_writes_bytes_to_(QCA_FSM_MAX_COMMAND, *Length) UCHAR *Buffer,
    _Out_ ULONG *Length,
    _Out_ ULONG *BaudRate);

/*
 * Feeds exactly one HCI event (H4 byte included). EDL responses accept the existing
 * vendor envelope or the QCA2066 Command Complete envelope for 0xFC00; each state
 * validates its selector and payload. Rejected responses increment UnexpectedEvents
 * exactly once and otherwise leave the pending operation unchanged. Timeout/retry is
 * the caller's decision. TLV acknowledgements contain no segment sequence number, so
 * a duplicate of the same outstanding segment's ack cannot be distinguished here.
 */
BOOLEAN QcaFsmOnPacket(
    _Inout_ PQCA_INIT_FSM Fsm,
    _In_reads_bytes_(Length) const UCHAR *Packet,
    _In_ ULONG Length);

const char *QcaFsmStateName(_In_ QCA_FSM_STATE State);
