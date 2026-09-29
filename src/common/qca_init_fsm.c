/*
 * qca_init_fsm.c - QCA2066 bring-up state machine. See qca_init_fsm.h for the sequence and its
 * source in Linux qca_uart_setup().
 */

#include "../include/qca_init_fsm.h"

/* Standard HCI opcodes used at the tail of bring-up. */
#define HCI_OP_RESET_LOCAL          0x0C03u
#define HCI_OP_READ_LOCAL_VER_LOCAL 0x1001u

static ULONG BuildHciCommand(USHORT Opcode, UCHAR *Out, ULONG Capacity)
{
    if (Capacity < 4u) {
        return 0;
    }
    Out[0] = H4_PKT_COMMAND;
    Out[1] = (UCHAR)(Opcode & 0xFFu);
    Out[2] = (UCHAR)((Opcode >> 8) & 0xFFu);
    Out[3] = 0;
    return 4u;
}

static ULONG BuildDisableLogging(UCHAR *Out, ULONG Capacity)
{
    if (Capacity < 6u) {
        return 0;
    }
    Out[0] = H4_PKT_COMMAND;
    Out[1] = (UCHAR)(QCA_DISABLE_LOGGING & 0xFFu);
    Out[2] = (UCHAR)((QCA_DISABLE_LOGGING >> 8) & 0xFFu);
    Out[3] = 2u;
    Out[4] = QCA_DISABLE_LOGGING_SUB_OP;
    Out[5] = 0;   /* btqca.c qca_disable_soc_logging: { 0x14, 0x00 } */
    return 6u;
}

static int QcaStrCaseCmp(const char *s1, const char *s2)
{
    while (*s1 && *s2) {
        char c1 = *s1;
        char c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') {
            c1 = (char)(c1 + ('a' - 'A'));
        }
        if (c2 >= 'A' && c2 <= 'Z') {
            c2 = (char)(c2 + ('a' - 'A'));
        }
        if (c1 != c2) {
            return (int)((unsigned char)c1 - (unsigned char)c2);
        }
        s1++;
        s2++;
    }
    return (int)((unsigned char)*s1 - (unsigned char)*s2);
}

static void QcaCopyString(char *dst, ULONG capacity, const char *src)
{
    ULONG i = 0;
    if (capacity == 0) {
        return;
    }
    while (i + 1 < capacity && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Makes candidate Index the image to download, including where its baud byte is patched. */
static BOOLEAN ArmNvm(PQCA_INIT_FSM Fsm, ULONG Index, const char *Name)
{
    const QCA_NVM_CANDIDATE *c = &Fsm->Candidates[Index];
    ULONG baudOffset;

    if (!QcaParseTlv(c->Data, c->Size, &Fsm->NvmInfo) || Fsm->NvmInfo.Type != QCA_TLV_TYPE_NVM ||
        !QcaFindNvmHciBaudOffset(c->Data, c->Size, &baudOffset)) {
        return FALSE;
    }
    QcaCopyString(Fsm->SelectedNvmName, sizeof(Fsm->SelectedNvmName), Name);
    Fsm->SelectedNvmIndex = Index;
    Fsm->Nvm             = c->Data;
    Fsm->NvmSize         = c->Size;
    Fsm->NvmBaudOffset   = baudOffset;
    Fsm->NvmDownloadMode = QCA_SKIP_EVT_NONE;
    Fsm->NvmSegments     = QcaTlvSegmentCount(Fsm->NvmSize);
    Fsm->NvmIndex        = 0;
    return TRUE;
}

static BOOLEAN SelectNvm(PQCA_INIT_FSM Fsm)
{
    char targetName[32];
    char altName[32];
    ULONG matchIndex = (ULONG)-1;
    ULONG i;
    const char *chosenName = NULL;

    if (Fsm == NULL || Fsm->Candidates == NULL || Fsm->CandidateCount == 0) {
        return FALSE;
    }

    if (!QcaBuildNvmFileName(Fsm->SocVersion.SocId, Fsm->SocVersion.RomVersion,
                             Fsm->BoardId, targetName, sizeof(targetName))) {
        return FALSE;
    }

    for (i = 0; i < Fsm->CandidateCount; i++) {
        if (Fsm->Candidates[i].Name != NULL &&
            QcaStrCaseCmp(Fsm->Candidates[i].Name, targetName) == 0) {
            matchIndex = i;
            chosenName = targetName;
            break;
        }
    }

    if (matchIndex == (ULONG)-1) {
        if (!QcaBuildAltNvmFileName(targetName, altName, sizeof(altName))) {
            return FALSE;
        }
        for (i = 0; i < Fsm->CandidateCount; i++) {
            if (Fsm->Candidates[i].Name != NULL &&
                QcaStrCaseCmp(Fsm->Candidates[i].Name, altName) == 0) {
                matchIndex = i;
                chosenName = altName;
                break;
            }
        }
        if (matchIndex == (ULONG)-1) {
            return FALSE;
        }
    }

    return ArmNvm(Fsm, matchIndex, chosenName);
}

VOID
QcaFsmOnBoardIdTimeout(_Inout_ PQCA_INIT_FSM Fsm)
{
    if (Fsm == NULL || Fsm->State != QcaFsmStateBoardIdRequest) {
        return;
    }
    Fsm->BoardId = 0;
    Fsm->BoardIdValid = FALSE;
    if (!SelectNvm(Fsm)) {
        Fsm->LastFailureState = (ULONG)Fsm->State;
        Fsm->State = QcaFsmStateFailed;
        return;
    }
    Fsm->State = QcaFsmStateNvmDownload;
}

VOID
QcaFsmOnNvmFailure(_Inout_ PQCA_INIT_FSM Fsm)
{
    char altName[32];
    ULONG matchIndex = (ULONG)-1;
    ULONG i;

    if (Fsm == NULL) {
        return;
    }
    if (Fsm->NvmFallbackUsed) {
        Fsm->LastFailureState = (ULONG)Fsm->State;
        Fsm->State = QcaFsmStateFailed;
        return;
    }
    if (!QcaBuildAltNvmFileName(Fsm->SelectedNvmName, altName, sizeof(altName))) {
        Fsm->LastFailureState = (ULONG)Fsm->State;
        Fsm->State = QcaFsmStateFailed;
        return;
    }
    for (i = 0; i < Fsm->CandidateCount; i++) {
        if (Fsm->Candidates[i].Name != NULL &&
            QcaStrCaseCmp(Fsm->Candidates[i].Name, altName) == 0) {
            matchIndex = i;
            break;
        }
    }
    if (matchIndex == (ULONG)-1) {
        Fsm->LastFailureState = (ULONG)Fsm->State;
        Fsm->State = QcaFsmStateFailed;
        return;
    }
    if (!ArmNvm(Fsm, matchIndex, altName)) {
        Fsm->LastFailureState = (ULONG)Fsm->State;
        Fsm->State = QcaFsmStateFailed;
        return;
    }
    Fsm->NvmFallbackUsed = TRUE;
    Fsm->State           = QcaFsmStateNvmDownload;
}

BOOLEAN
QcaFsmInit(
    _Out_ PQCA_INIT_FSM Fsm,
    _In_reads_bytes_(PatchSize) const UCHAR *Patch,
    _In_ ULONG PatchSize,
    _In_reads_(CandidateCount) const QCA_NVM_CANDIDATE *Candidates,
    _In_ ULONG CandidateCount,
    _In_ ULONG OperBaudRate)
{
    UCHAR baudIndex = 0;
    ULONG i;

    RtlZeroMemory(Fsm, sizeof(*Fsm));
    Fsm->State = QcaFsmStateFailed;

    if (Patch == NULL || PatchSize == 0) {
        return FALSE;
    }
    if (Candidates == NULL || CandidateCount == 0) {
        return FALSE;
    }
    if (!QcaParseTlv(Patch, PatchSize, &Fsm->PatchInfo) ||
        Fsm->PatchInfo.Type != QCA_TLV_TYPE_PATCH) {
        return FALSE;
    }
    for (i = 0; i < CandidateCount; i++) {
        QCA_TLV_INFO info;
        ULONG baudOffset;
        if (Candidates[i].Name == NULL || Candidates[i].Data == NULL || Candidates[i].Size == 0) {
            return FALSE;
        }
        if (!QcaParseTlv(Candidates[i].Data, Candidates[i].Size, &info) ||
            info.Type != QCA_TLV_TYPE_NVM ||
            !QcaFindNvmHciBaudOffset(Candidates[i].Data, Candidates[i].Size, &baudOffset)) {
            return FALSE;
        }
    }
    if (!QcaBaudRateToIndex(OperBaudRate, &baudIndex)) {
        return FALSE;
    }

    Fsm->Patch             = Patch;
    Fsm->PatchSize         = PatchSize;
    Fsm->PatchDownloadMode = Fsm->PatchInfo.DownloadMode;
    Fsm->PatchSegments     = QcaTlvSegmentCount(PatchSize);
    Fsm->PatchIndex        = 0;

    Fsm->Candidates        = Candidates;
    Fsm->CandidateCount    = CandidateCount;

    Fsm->OperBaudRate      = OperBaudRate;
    Fsm->OperBaudIndex     = baudIndex;
    Fsm->State             = QcaFsmStateStart;
    return TRUE;
}

QCA_FSM_ACTION
QcaFsmNext(
    _Inout_ PQCA_INIT_FSM Fsm,
    _Out_writes_bytes_to_(QCA_FSM_MAX_COMMAND, *Length) UCHAR *Buffer,
    _Out_ ULONG *Length,
    _Out_ ULONG *BaudRate)
{
    ULONG n = 0;

    *Length = 0;
    *BaudRate = 0;

    switch (Fsm->State) {
    case QcaFsmStateStart:
        Fsm->State = QcaFsmStateVersionRequest;
        n = QcaBuildEdlCommand(EDL_PATCH_VER_REQ_CMD, Buffer, QCA_FSM_MAX_COMMAND);
        break;

    case QcaFsmStateSetBaudRate:
        n = QcaBuildBaudRateCommand(Fsm->OperBaudIndex, Buffer, QCA_FSM_MAX_COMMAND);
        if (n == 0) {
            Fsm->LastFailureState = (ULONG)Fsm->State;
            Fsm->State = QcaFsmStateFailed;
            return QcaFsmActionFailed;
        }
        *Length = n;
        *BaudRate = Fsm->OperBaudRate;
        Fsm->CommandsSent++;
        /*
         * No reply is awaited. For QCA2066, hci_qca.c qca_set_speed/qca_set_baudrate sends 0xFC48,
         * waits for it to drain, and switches the host baud rate. The controller switches rate
         * immediately upon processing the command; any reply arrives at the new rate while the
         * host is still at the old rate.
         */
        Fsm->State = QcaFsmStatePatchDownload;
        return QcaFsmActionSendBaudAndSwitch;

    case QcaFsmStatePatchDownload: {
        BOOLEAN ack = TRUE;
        n = QcaBuildTlvSegmentCommand(Fsm->Patch, Fsm->PatchSize, Fsm->PatchIndex,
                                      Fsm->PatchDownloadMode, Buffer, QCA_FSM_MAX_COMMAND, &ack);
        if (n == 0) {
            Fsm->LastFailureState = (ULONG)Fsm->State;
            Fsm->State = QcaFsmStateFailed;
            return QcaFsmActionFailed;
        }
        *Length = n;
        Fsm->CommandsSent++;
        if (!ack) {
            /* Unacknowledged segment: no event will arrive, so advance here. */
            Fsm->PatchIndex++;
            return QcaFsmActionSendNoWait;
        }
        return QcaFsmActionSend;
    }
    case QcaFsmStateBoardIdRequest:
        n = QcaBuildBoardIdCommand(Buffer, QCA_FSM_MAX_COMMAND);
        break;


    case QcaFsmStateNvmDownload: {
        BOOLEAN ack = TRUE;
        n = QcaBuildTlvSegmentCommand(Fsm->Nvm, Fsm->NvmSize, Fsm->NvmIndex,
                                      Fsm->NvmDownloadMode, Buffer, QCA_FSM_MAX_COMMAND, &ack);
        if (n == 0) {
            Fsm->LastFailureState = (ULONG)Fsm->State;
            Fsm->State = QcaFsmStateFailed;
            return QcaFsmActionFailed;
        }
        /*
         * The NVM HCI tag carries the controller's UART baud index (btqca.c). The files specify 17
         * (3.2 Mbaud) while the host runs OperBaudIndex; patch that byte on the wire to match
         * the operating baud rate.
         */
        {
            ULONG segStart = Fsm->NvmIndex * QCA_MAX_SIZE_PER_TLV_SEGMENT;
            if (Fsm->NvmBaudOffset >= segStart && Fsm->NvmBaudOffset - segStart < n - 6u) {
                Buffer[6u + (Fsm->NvmBaudOffset - segStart)] = Fsm->OperBaudIndex;
            }
        }
        *Length = n;
        Fsm->CommandsSent++;
        if (!ack) {
            Fsm->NvmIndex++;
            return QcaFsmActionSendNoWait;
        }
        return QcaFsmActionSend;
    }

    case QcaFsmStateDisableLogging:
        n = BuildDisableLogging(Buffer, QCA_FSM_MAX_COMMAND);
        break;

    case QcaFsmStateBuildInfo:
        n = QcaBuildEdlCommand(EDL_GET_BUILD_INFO_CMD, Buffer, QCA_FSM_MAX_COMMAND);
        break;

    case QcaFsmStateHciReset:
        n = BuildHciCommand(HCI_OP_RESET_LOCAL, Buffer, QCA_FSM_MAX_COMMAND);
        break;

    case QcaFsmStateReadLocalVersion:
        n = BuildHciCommand(HCI_OP_READ_LOCAL_VER_LOCAL, Buffer, QCA_FSM_MAX_COMMAND);
        break;

    case QcaFsmStateReady:
        return QcaFsmActionDone;

    case QcaFsmStateVersionRequest:
    case QcaFsmStateFailed:
    default:
        /*
         * VersionRequest lands here only if the caller pumps again without delivering the
         * response; treat it as a protocol error rather than silently resending.
         */
        Fsm->LastFailureState = (ULONG)Fsm->State;
        Fsm->State = QcaFsmStateFailed;
        return QcaFsmActionFailed;
    }

    if (n == 0) {
        Fsm->LastFailureState = (ULONG)Fsm->State;
        Fsm->State = QcaFsmStateFailed;
        return QcaFsmActionFailed;
    }

    *Length = n;
    Fsm->CommandsSent++;
    return QcaFsmActionSend;
}

/* btqca.c qca_tlv_send_segment: legacy VSE has a result; QCA2066 CC does not. */
static BOOLEAN IsTlvAcknowledgement(const UCHAR *Packet, ULONG Length)
{
    const UCHAR *data;
    ULONG dataLength;

    if (QcaParseEdlResponse(Packet, Length, EDL_TVL_DNLD_RES_EVT, &data, &dataLength)) {
        return Packet[1] == HCI_EV_VENDOR && dataLength == 1u && data[0] == 0;
    }
    return QcaParseEdlResponse(Packet, Length, EDL_PATCH_TLV_REQ_CMD, &data, &dataLength) &&
           Packet[1] == 0x0Eu && dataLength == 0;
}

/* btqca.c qca_read_fw_build_info: cresp/rtype, label length, then label bytes. */
static BOOLEAN IsBuildInfoResponse(const UCHAR *Packet, ULONG Length)
{
    const UCHAR *data;
    ULONG dataLength;

    return QcaParseEdlResponse(Packet, Length, EDL_GET_BUILD_INFO_CMD, &data, &dataLength) &&
           dataLength >= 1u && (ULONG)data[0] <= dataLength - 1u;
}

/* A Command_Complete is: H4 0x04, evt 0x0E, plen, ncmd, opcode lo, opcode hi, status... */
static BOOLEAN IsCommandComplete(const UCHAR *P, ULONG Length, USHORT *Opcode, UCHAR *Status)
{
    if (P == NULL || Opcode == NULL || Status == NULL ||
        Length < 7u || P[0] != H4_PKT_EVENT || P[1] != 0x0Eu ||
        P[2] < 4u || Length != 3u + (ULONG)P[2]) {
        return FALSE;
    }
    *Opcode = (USHORT)(P[4] | ((USHORT)P[5] << 8));
    *Status = P[6];
    return TRUE;
}

BOOLEAN
QcaFsmOnPacket(
    _Inout_ PQCA_INIT_FSM Fsm,
    _In_reads_bytes_(Length) const UCHAR *Packet,
    _In_ ULONG Length)
{
    USHORT opcode = 0;
    UCHAR  status = 0;

    switch (Fsm->State) {
    case QcaFsmStateVersionRequest:
        if (QcaParseVersionEvent(Packet, Length, &Fsm->SocVersion)) {
            Fsm->State = QcaFsmStateSetBaudRate;
            Fsm->EventsHandled++;
            return TRUE;
        }
        break;

    case QcaFsmStatePatchDownload:
        if (IsTlvAcknowledgement(Packet, Length)) {
            Fsm->EventsHandled++;
            Fsm->PatchIndex++;
            if (Fsm->PatchIndex >= Fsm->PatchSegments) {
                Fsm->State = QcaFsmStateBoardIdRequest;
            }
            return TRUE;
        }
        break; /* Patch download must not fall through to board-ID parsing. */

    case QcaFsmStateBoardIdRequest: {
        USHORT bid = 0;
        if (QcaParseBoardIdEvent(Packet, Length, &bid)) {
            Fsm->BoardId = bid;
            Fsm->BoardIdValid = TRUE;
            Fsm->EventsHandled++;
            if (!SelectNvm(Fsm)) {
                Fsm->LastFailureState = (ULONG)Fsm->State;
                Fsm->State = QcaFsmStateFailed;
                return FALSE;
            }
            Fsm->State = QcaFsmStateNvmDownload;
            return TRUE;
        }
        break;
    }
    case QcaFsmStateNvmDownload:
        if (IsTlvAcknowledgement(Packet, Length)) {
            Fsm->EventsHandled++;
            Fsm->NvmIndex++;
            if (Fsm->NvmIndex >= Fsm->NvmSegments) {
                Fsm->State = QcaFsmStateDisableLogging;
            }
            return TRUE;
        }
        break;

    case QcaFsmStateDisableLogging:
        /*
         * The QCA2066 echoes the sub-op: 04 0E 05 01 17 FC 00 14. Older parts end after the
         * status byte (length 7).
         */
        if (IsCommandComplete(Packet, Length, &opcode, &status) &&
            opcode == QCA_DISABLE_LOGGING && status == 0 &&
            (Length == 7u || (Length == 8u && Packet[7] == QCA_DISABLE_LOGGING_SUB_OP))) {
            Fsm->State = QcaFsmStateBuildInfo;
            Fsm->EventsHandled++;
            return TRUE;
        }
        break;

    case QcaFsmStateBuildInfo:
        if (IsBuildInfoResponse(Packet, Length)) {
            Fsm->State = QcaFsmStateHciReset;
            Fsm->EventsHandled++;
            return TRUE;
        }
        break;

    case QcaFsmStateHciReset:
        if (IsCommandComplete(Packet, Length, &opcode, &status) &&
            opcode == HCI_OP_RESET_LOCAL && status == 0 && Length == 7u) {
            Fsm->State = QcaFsmStateReadLocalVersion;
            Fsm->EventsHandled++;
            return TRUE;
        }
        break;

    case QcaFsmStateReadLocalVersion:
        if (IsCommandComplete(Packet, Length, &opcode, &status) &&
            opcode == HCI_OP_READ_LOCAL_VER_LOCAL && status == 0 && Length == 15u) {
            Fsm->State = QcaFsmStateReady;
            Fsm->EventsHandled++;
            return TRUE;
        }
        break;

    default:
        break;
    }

    Fsm->UnexpectedEvents++;
    return FALSE;
}

const char *
QcaFsmStateName(_In_ QCA_FSM_STATE State)
{
    switch (State) {
    case QcaFsmStateStart:            return "Start";
    case QcaFsmStateVersionRequest:   return "VersionRequest";
    case QcaFsmStateSetBaudRate:      return "SetBaudRate";
    case QcaFsmStatePatchDownload:    return "PatchDownload";
    case QcaFsmStateBoardIdRequest:   return "BoardIdRequest";
    case QcaFsmStateNvmDownload:      return "NvmDownload";
    case QcaFsmStateDisableLogging:   return "DisableLogging";
    case QcaFsmStateBuildInfo:        return "BuildInfo";
    case QcaFsmStateHciReset:         return "HciReset";
    case QcaFsmStateReadLocalVersion: return "ReadLocalVersion";
    case QcaFsmStateReady:            return "Ready";
    case QcaFsmStateFailed:           return "Failed";
    default:                          return "?";
    }
}
