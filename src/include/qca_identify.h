/*
 * qca_identify.h - Non-destructive identification probe for the Qualcomm controller.
 *
 * Steps through baud rates (115200, 3000000, 3200000) sending a read-only EDL
 * version request (01 00 FC 01 19) to detect whether the controller is in ROM or
 * running warm, without downloading firmware or altering controller baud settings.
 */

#pragma once
#include "qca_protocol.h"

#define QCA_IDENTIFY_MAX_RATES 4u

typedef struct _QCA_IDENTIFY {
    ULONG           Rates[QCA_IDENTIFY_MAX_RATES];
    ULONG           RateCount;      /* 3 after Init */
    ULONG           Index;          /* next ladder position */
    ULONG           Attempts;       /* rates actually handed out */
    BOOLEAN         Answered;
    BOOLEAN         Done;
    ULONG           AnsweredRate;   /* 0 unless Answered */
    QCA_SOC_VERSION Version;        /* zeroed unless Answered */
} QCA_IDENTIFY, *PQCA_IDENTIFY;

VOID    QcaIdentifyInit(_Out_ PQCA_IDENTIFY Id);
BOOLEAN QcaIdentifyNextRate(_Inout_ PQCA_IDENTIFY Id, _Out_ ULONG *Rate);
ULONG   QcaIdentifyBuildRequest(_Out_writes_bytes_to_(Capacity, return) UCHAR *Out, _In_ ULONG Capacity);
BOOLEAN QcaIdentifyOnPacket(_Inout_ PQCA_IDENTIFY Id, _In_reads_bytes_(Length) const UCHAR *Packet, _In_ ULONG Length);
