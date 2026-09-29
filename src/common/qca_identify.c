/*
 * qca_identify.c - Non-destructive identification probe implementation.
 *
 * Steps through baud rates sending an EDL version request to determine whether the
 * controller is in ROM (115200) or warm (3000000 / 3200000), without downloading
 * firmware or altering controller baud settings.
 *
 * Free of OS or transport dependencies, dynamic allocations, CRT string calls,
 * and floating point.
 */

#include "../include/qca_identify.h"

VOID
QcaIdentifyInit(
    _Out_ PQCA_IDENTIFY Id)
{
    if (Id == NULL) {
        return;
    }

    RtlZeroMemory(Id, sizeof(*Id));
    Id->Rates[0] = 115200ul;
    Id->Rates[1] = 3000000ul;
    Id->Rates[2] = 3200000ul;
    Id->RateCount = 3u;
    Id->Index = 0;
    Id->Attempts = 0;
    Id->Answered = FALSE;
    Id->Done = FALSE;
    Id->AnsweredRate = 0;
}

BOOLEAN
QcaIdentifyNextRate(
    _Inout_ PQCA_IDENTIFY Id,
    _Out_ ULONG *Rate)
{
    if (Id == NULL || Rate == NULL) {
        return FALSE;
    }

    if (Id->Answered) {
        Id->Done = TRUE;
        return FALSE;
    }

    if (Id->Done) {
        return FALSE;
    }

    if (Id->Index >= Id->RateCount) {
        Id->Done = TRUE;
        return FALSE;
    }

    *Rate = Id->Rates[Id->Index];
    Id->Index++;
    Id->Attempts++;
    return TRUE;
}

ULONG
QcaIdentifyBuildRequest(
    _Out_writes_bytes_to_(Capacity, return) UCHAR *Out,
    _In_ ULONG Capacity)
{
    if (Out == NULL || Capacity < 5u) {
        return 0;
    }

    /*
     * Exactly 01 00 FC 01 19:
     *   [0] 0x01: H4_PKT_COMMAND
     *   [1] 0x00: EDL_PATCH_CMD_OPCODE (low byte of 0xFC00)
     *   [2] 0xFC: EDL_PATCH_CMD_OPCODE (high byte of 0xFC00)
     *   [3] 0x01: parameter length (1 byte)
     *   [4] 0x19: EDL_PATCH_VER_REQ_CMD
     */
    Out[0] = H4_PKT_COMMAND;
    Out[1] = (UCHAR)(EDL_PATCH_CMD_OPCODE & 0xFFu);
    Out[2] = (UCHAR)((EDL_PATCH_CMD_OPCODE >> 8) & 0xFFu);
    Out[3] = 0x01u;
    Out[4] = EDL_PATCH_VER_REQ_CMD;

    return 5u;
}

BOOLEAN
QcaIdentifyOnPacket(
    _Inout_ PQCA_IDENTIFY Id,
    _In_reads_bytes_(Length) const UCHAR *Packet,
    _In_ ULONG Length)
{
    QCA_SOC_VERSION ver;

    if (Id == NULL || Packet == NULL) {
        return FALSE;
    }

    if (Id->Done || Id->Answered) {
        return FALSE;
    }

    if (Id->Attempts == 0 || Id->Attempts > Id->RateCount) {
        return FALSE;
    }

    if (!QcaParseVersionEvent(Packet, Length, &ver)) {
        return FALSE;
    }

    Id->Version = ver;
    Id->AnsweredRate = Id->Rates[Id->Attempts - 1];
    Id->Answered = TRUE;
    Id->Done = TRUE;

    return TRUE;
}
