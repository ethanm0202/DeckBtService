/*
 * fuzz_controller.c - libFuzzer target for everything that parses controller and host HCI traffic:
 * the H4 decoder (with in-band sleep bytes), the HCI bridge (event/ACL/SCO queues, ACL credits,
 * readiness hold) and the voice-routing rewrite, driven the way src/service/qca_backend.c drives
 * them. Each input is a script of operations; the first byte of each step picks one.
 *
 *   tools\fuzz.cmd [seconds]
 */

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "../src/include/h4_codec.h"
#include "../src/include/hci_bridge.h"
#include "../src/include/sco_route.h"
#include "../src/include/qca_protocol.h"

static H4_DECODER g_Decoder;
static HCI_BRIDGE g_Bridge;
static HCI_TRANSPORT g_Transport;
static SCO_ROUTE g_Route;

static unsigned char
WireSend(void *Context, const unsigned char *Packet, unsigned long Length)
{
    unsigned char out[H4_MAX_PACKET_SIZE + 1];

    (void)Context;
    /* The backend frames every outbound packet; framing must never overflow its slot. */
    return (unsigned char)(H4EncodePacket(H4_PKT_COMMAND, Packet, Length, out, sizeof(out)) != 0 || Length == 0);
}

static const HCI_BRIDGE_WIRE_OPS g_Ops = { WireSend, WireSend, WireSend };

static unsigned char
OnIbs(void *Context, unsigned char Byte)
{
    (void)Context;
    return (unsigned char)(Byte == QCA_IBS_WAKE_IND || Byte == QCA_IBS_SLEEP_IND || Byte == QCA_IBS_WAKE_ACK);
}

static void
OnPacket(void *Context, unsigned char Type, const unsigned char *Payload, unsigned long Length)
{
    unsigned char *copy = (unsigned char *)malloc(Length ? Length : 1u);

    (void)Context;
    memcpy(copy, Payload, Length);   /* exact-size heap copy, so overreads are caught */
    if (Type == H4_PKT_EVENT) {
        if (Length == 6u) {
            (void)ScoRouteRestoreEvent(&g_Route, copy, Length);
        }
        (void)HciBridgeOnEvent(&g_Bridge, copy, Length);
    } else if (Type == H4_PKT_ACL) {
        (void)HciBridgeOnAcl(&g_Bridge, copy, Length);
    } else if (Type == H4_PKT_SCO) {
        (void)HciBridgeOnSco(&g_Bridge, copy, Length);
    }
    free(copy);
}

int
LLVMFuzzerTestOneInput(const unsigned char *Data, size_t Size)
{
    HCI_BRIDGE_WIRE wire = { &g_Ops, NULL };
    size_t at = 0;

    H4DecoderInit(&g_Decoder);
    HciBridgeInit(&g_Bridge, &wire);
    memset(&g_Transport, 0, sizeof(g_Transport));
    HciBridgeBindTransport(&g_Transport, &g_Bridge);
    ScoRouteReset(&g_Route);

    while (at + 2u <= Size) {
        unsigned char op = Data[at];
        unsigned long length = Data[at + 1u];
        unsigned char *chunk;

        at += 2u;
        if (length > Size - at) {
            length = (unsigned long)(Size - at);
        }
        chunk = (unsigned char *)malloc(length ? length : 1u);
        memcpy(chunk, Data + at, length);
        at += length;

        switch (op % 8u) {
        case 0:   /* bytes from the controller, in arbitrary chunks */
        case 1:
            H4DecoderFeedEx(&g_Decoder, chunk, length, OnPacket, OnIbs, NULL);
            break;
        case 2: { /* host command, through the voice-routing rewrite first */
            unsigned char enhanced[SCO_ROUTE_MAX_COMMAND];
            unsigned long n = ScoRouteRewriteCommand(&g_Route, chunk, length, enhanced, sizeof(enhanced));
            if (n > sizeof(enhanced)) {
                abort();
            }
            (void)HciTransportSubmitCommand(&g_Transport, n ? enhanced : chunk, n ? n : length);
            break;
        }
        case 3:
            (void)HciTransportSubmitAcl(&g_Transport, chunk, length);
            break;
        case 4:
            (void)HciTransportSubmitSco(&g_Transport, chunk, length);
            break;
        case 5: { /* host reads with a capacity taken from the input */
            unsigned char out[2048];
            unsigned long written = 0;
            unsigned long capacity = length ? (chunk[0] * 8u + 1u) : 0u;
            HCI_STREAM stream = (HCI_STREAM)(op / 8u % 3u);

            if (capacity > sizeof(out)) {
                capacity = sizeof(out);
            }
            if (HciTransportPopStream(&g_Transport, stream, out, capacity, &written) && written > capacity) {
                abort();
            }
            break;
        }
        case 6:
            HciBridgeSetReady(&g_Bridge, (unsigned char)(length & 1u));
            break;
        default:
            HciTransportReset(&g_Transport);   /* as qca_backend.c TReset */
            H4DecoderDiscardPacket(&g_Decoder);
            ScoRouteReset(&g_Route);
            break;
        }
        free(chunk);
    }
    return 0;
}
