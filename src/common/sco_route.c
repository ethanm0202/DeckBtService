/*
 * sco_route.c - legacy synchronous-connection commands rewritten onto the HCI data path.
 * Contract and rationale: src/include/sco_route.h.
 */

#include <string.h>
#include "../include/sco_route.h"

/* Voice_Setting air coding (bits 0-1) and the host-side format the CVSD mapping requires. */
#define SCO_ROUTE_AIR_MASK          0x0003u
#define SCO_ROUTE_AIR_CVSD          0x0000u
#define SCO_ROUTE_AIR_TRANSPARENT   0x0003u
#define SCO_ROUTE_INPUT_MASK        0x03E0u   /* sample size, data format, input coding */
#define SCO_ROUTE_INPUT_LINEAR16    0x0060u   /* 16-bit, 2's complement, linear */

/* Coding_Format IDs (Assigned Numbers): u-law 0x00, A-law 0x01, CVSD 0x02, transparent 0x03, linear PCM 0x04. */
#define SCO_ROUTE_CODING_CVSD       0x02u
#define SCO_ROUTE_CODING_TRANSPARENT 0x03u
#define SCO_ROUTE_CODING_LINEAR     0x04u

static unsigned char *
ScoRoutePut16(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    return p + 2;
}

static unsigned char *
ScoRoutePut32(unsigned char *p, unsigned long v)
{
    p = ScoRoutePut16(p, v & 0xFFFFu);
    return ScoRoutePut16(p, (v >> 16) & 0xFFFFu);
}

/* Coding_Format: ID, then Company_ID and Vendor_Codec_ID, both zero for assigned formats. */
static unsigned char *
ScoRoutePutCoding(unsigned char *p, unsigned char id)
{
    p[0] = id;
    memset(p + 1, 0, 4);
    return p + 5;
}

void
ScoRouteReset(SCO_ROUTE *Route)
{
    memset(Route, 0, sizeof(*Route));
}

unsigned long
ScoRouteRewriteCommand(SCO_ROUTE *Route, const unsigned char *Command, unsigned long Length,
                       unsigned char *Out, unsigned long Capacity)
{
    unsigned long op;
    unsigned long idLength;          /* handle (2) for Setup, BD_ADDR (6) for Accept */
    unsigned long enhParams;
    unsigned long voice;
    const unsigned char *prm;
    unsigned char *p;
    int transparent;

    if (Length < 3u) {
        return 0;
    }
    op = (unsigned long)Command[0] | ((unsigned long)Command[1] << 8);
    if (op == SCO_ROUTE_OP_SETUP && Command[2] == 17u && Length == 20u) {
        idLength = 2u;
        enhParams = SCO_ROUTE_ENH_SETUP_PARAMS;
    } else if (op == SCO_ROUTE_OP_ACCEPT && Command[2] == 21u && Length == 24u) {
        idLength = 6u;
        enhParams = SCO_ROUTE_ENH_ACCEPT_PARAMS;
    } else {
        return 0;
    }
    if (Capacity < 3u + enhParams) {
        return 0;
    }

    /* Legacy layout after the identifier: Tx_BW(4) Rx_BW(4) Max_Latency(2) Voice_Setting(2)
     * Retransmission_Effort(1) Packet_Type(2). */
    prm = Command + 3;
    voice = (unsigned long)prm[idLength + 10u] | ((unsigned long)prm[idLength + 11u] << 8);
    if ((voice & SCO_ROUTE_AIR_MASK) == SCO_ROUTE_AIR_TRANSPARENT) {
        transparent = 1;
    } else if ((voice & SCO_ROUTE_AIR_MASK) == SCO_ROUTE_AIR_CVSD &&
               (voice & SCO_ROUTE_INPUT_MASK) == SCO_ROUTE_INPUT_LINEAR16) {
        transparent = 0;
    } else {
        return 0;   /* u-law, A-law, or a host format with no mapping here: leave it to the controller */
    }

    p = ScoRoutePut16(Out, op == SCO_ROUTE_OP_SETUP ? SCO_ROUTE_OP_ENH_SETUP : SCO_ROUTE_OP_ENH_ACCEPT);
    *p++ = (unsigned char)enhParams;
    memcpy(p, prm, idLength);                                   /* handle or BD_ADDR */
    p += idLength;
    memcpy(p, prm + idLength, 8u);                              /* Transmit/Receive_Bandwidth */
    p += 8u;
    p = ScoRoutePutCoding(p, transparent ? SCO_ROUTE_CODING_TRANSPARENT : SCO_ROUTE_CODING_CVSD);
    p = ScoRoutePutCoding(p, transparent ? SCO_ROUTE_CODING_TRANSPARENT : SCO_ROUTE_CODING_CVSD);
    p = ScoRoutePut16(p, 60u);                                  /* Transmit/Receive_Codec_Frame_Size */
    p = ScoRoutePut16(p, 60u);
    p = ScoRoutePut32(p, transparent ? 8000u : 16000u);         /* Input/Output_Bandwidth, bytes/s */
    p = ScoRoutePut32(p, transparent ? 8000u : 16000u);
    p = ScoRoutePutCoding(p, transparent ? SCO_ROUTE_CODING_TRANSPARENT : SCO_ROUTE_CODING_LINEAR);
    p = ScoRoutePutCoding(p, transparent ? SCO_ROUTE_CODING_TRANSPARENT : SCO_ROUTE_CODING_LINEAR);
    p = ScoRoutePut16(p, 16u);                                  /* Input/Output_Coded_Data_Size, bits */
    p = ScoRoutePut16(p, 16u);
    *p++ = 2u;                                                  /* Input/Output_PCM_Data_Format: 2's complement */
    *p++ = 2u;
    *p++ = 0u;                                                  /* Input/Output_PCM_Sample_Payload_MSB_Position */
    *p++ = 0u;
    *p++ = SCO_ROUTE_DATA_PATH_HCI;                             /* Input/Output_Data_Path */
    *p++ = SCO_ROUTE_DATA_PATH_HCI;
    *p++ = transparent ? 1u : 16u;                              /* Input/Output_Transport_Unit_Size */
    *p++ = transparent ? 1u : 16u;
    memcpy(p, prm + idLength + 8u, 2u);                         /* Max_Latency */
    p += 2u;
    memcpy(p, prm + idLength + 13u, 2u);                        /* Packet_Type */
    p += 2u;
    *p++ = prm[idLength + 12u];                                 /* Retransmission_Effort */

    if (op == SCO_ROUTE_OP_SETUP) {
        if (Route->SetupPending < 0xFFu) {
            Route->SetupPending++;
        }
    } else if (Route->AcceptPending < 0xFFu) {
        Route->AcceptPending++;
    }
    Route->Rewritten++;
    return (unsigned long)(p - Out);
}

unsigned char
ScoRouteRestoreEvent(SCO_ROUTE *Route, unsigned char *Event, unsigned long Length)
{
    unsigned long at;
    unsigned long op;

    if (Length >= 6u && Event[0] == 0x0Fu) {
        at = 4u;    /* Command Status: status, Num_HCI_Command_Packets, opcode */
    } else if (Length >= 6u && Event[0] == 0x0Eu) {
        at = 3u;    /* Command Complete: Num_HCI_Command_Packets, opcode, status */
    } else {
        return 0;
    }
    op = (unsigned long)Event[at] | ((unsigned long)Event[at + 1u] << 8);
    if (op == SCO_ROUTE_OP_ENH_SETUP && Route->SetupPending != 0) {
        Route->SetupPending--;
        op = SCO_ROUTE_OP_SETUP;
    } else if (op == SCO_ROUTE_OP_ENH_ACCEPT && Route->AcceptPending != 0) {
        Route->AcceptPending--;
        op = SCO_ROUTE_OP_ACCEPT;
    } else {
        return 0;
    }
    Event[at] = (unsigned char)(op & 0xFFu);
    Event[at + 1u] = (unsigned char)(op >> 8);
    Route->Restored++;
    return 1;
}
