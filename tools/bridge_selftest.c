/*
 * bridge_selftest.c - standalone host-side test suite for src/common/hci_bridge.c.
 *
 * Drives the transport-agnostic HCI bridge through the HCI_TRANSPORT vtable against an
 * in-process mock controller, validating all 8 acceptance criteria:
 *   1. Readiness gating (hold-then-replay policy)
 *   2. Boot-chatter suppression (vendor event 0xFF dropped, standard event forwarded)
 *   3. ACL credit bookkeeping (Read_Buffer_Size, exhaustion, restoration, multi-handle)
 *   4. Synchronous (SCO) FIFO overwrite-oldest (inbound and outbound)
 *   5. PopStream buffer sizing (undersized capacity leaves packet queued)
 *   6. Reset empties all streams and clears partial state
 *   7. Stream ordering and interleaving (FIFO preservation across all 3 streams)
 *      7c. Clean shutdown; 7d. inbound backpressure (a burst larger than the queues)
 *   8. Exact diagnostics counter verification
 *
 * Build: tools\selftest.cmd
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/include/hci_bridge.h"
#include "../src/include/h4_codec.h"

static int g_fail = 0;

#define CHECK(cond, ...)                            \
    do {                                            \
        if (cond) { printf("  ok   "); }            \
        else      { printf("  FAIL "); g_fail++; }  \
        printf(__VA_ARGS__); printf("\n");          \
    } while (0)

/* ---------------------------------------------------------------- Mock Wire */

typedef struct _MOCK_CONTROLLER {
    /* Commands received */
    unsigned long CommandsReceived;
    unsigned char LastCommand[HCI_BRIDGE_MAX_COMMAND_SIZE];
    unsigned long LastCommandLength;

    /* ACL received */
    unsigned long AclReceived;
    unsigned char LastAcl[HCI_BRIDGE_MAX_ACL_SIZE];
    unsigned long LastAclLength;

    /* SCO received */
    unsigned long ScoReceived;
    unsigned char LastSco[HCI_BRIDGE_MAX_SCO_SIZE];
    unsigned long LastScoLength;

    /* SCO history log to verify overwrite-oldest in outbound direction */
    unsigned long ScoHistoryCount;
    unsigned char ScoHistory[32][HCI_BRIDGE_MAX_SCO_SIZE];
    unsigned long ScoHistoryLength[32];

    /* Knobs to simulate wire refusals / flow control */
    unsigned char RefuseCommand;
    unsigned char RefuseAcl;
    unsigned char RefuseSco;
} MOCK_CONTROLLER;

static unsigned char MockSendCommand(void *Context, const unsigned char *Packet, unsigned long Length)
{
    MOCK_CONTROLLER *mock = (MOCK_CONTROLLER *)Context;
    if (mock->RefuseCommand) {
        return 0;
    }
    mock->CommandsReceived++;
    memcpy(mock->LastCommand, Packet, Length);
    mock->LastCommandLength = Length;
    return 1;
}

static unsigned char MockSendAcl(void *Context, const unsigned char *Packet, unsigned long Length)
{
    MOCK_CONTROLLER *mock = (MOCK_CONTROLLER *)Context;
    if (mock->RefuseAcl) {
        return 0;
    }
    mock->AclReceived++;
    memcpy(mock->LastAcl, Packet, Length);
    mock->LastAclLength = Length;
    return 1;
}

static unsigned char MockSendSco(void *Context, const unsigned char *Packet, unsigned long Length)
{
    MOCK_CONTROLLER *mock = (MOCK_CONTROLLER *)Context;
    if (mock->RefuseSco) {
        return 0;
    }
    mock->ScoReceived++;
    memcpy(mock->LastSco, Packet, Length);
    mock->LastScoLength = Length;
    if (mock->ScoHistoryCount < 32) {
        memcpy(mock->ScoHistory[mock->ScoHistoryCount], Packet, Length);
        mock->ScoHistoryLength[mock->ScoHistoryCount] = Length;
        mock->ScoHistoryCount++;
    }
    return 1;
}

/* Mirrors qca_backend.c OnPacket in steady state for events and ACL. */
static void BurstPacket(void *Context, unsigned char Type, const unsigned char *Payload, unsigned long Length)
{
    HCI_BRIDGE *bridge = (HCI_BRIDGE *)Context;

    if (Type == H4_PKT_EVENT) {
        (void)HciBridgeOnEvent(bridge, Payload, Length);
    } else if (Type == H4_PKT_ACL) {
        (void)HciBridgeOnAcl(bridge, Payload, Length);
    }
}

/*
 * Feeds one UART read the way qca_backend.c ReaderThread does: slices of at most
 * HciBridgeInboundRoom packets. When there is no room the reader waits; here a slow host then
 * takes one packet of Stream. Returns the number of waits.
 */
static unsigned long BurstFeed(H4_DECODER *Decoder, HCI_BRIDGE *Bridge, HCI_TRANSPORT *Transport,
                               const unsigned char *Data, unsigned long Length, HCI_STREAM Stream,
                               unsigned char *Out, unsigned long *OutCount, unsigned long *LastLength)
{
    unsigned long fed = 0;
    unsigned long waits = 0;
    unsigned long written;

    while (fed < Length && waits < 100u) {
        unsigned long step = H4DecoderFeedLimited(Decoder, Data + fed, Length - fed,
                                                  HciBridgeInboundRoom(Bridge), BurstPacket, NULL, Bridge);
        fed += step;
        if (step == 0) {
            waits++;
            if (HciTransportPopStream(Transport, Stream, Out, HCI_BRIDGE_MAX_ACL_SIZE, &written)) {
                (*OutCount)++;
                *LastLength = written;
            }
        }
    }
    return waits;
}

/* ---------------------------------------------------------------- Test Harness */

int main(void)
{
    HCI_BRIDGE bridge;
    HCI_TRANSPORT transport;
    HCI_BRIDGE_WIRE wire;
    MOCK_CONTROLLER mock;
    HCI_BRIDGE_WIRE_OPS wireOps;

    unsigned char popBuf[2048];
    unsigned long written = 0;
    unsigned char ok = 0;
    unsigned int i;

    /* Expected counter tracking */
    unsigned long expCommandsSubmitted = 0;
    unsigned long expCommandsHeld = 0;
    unsigned long expCommandsReplayed = 0;
    unsigned long expCommandsSentToWire = 0;
    unsigned long expCommandsFailedWire = 0;
    unsigned long expCommandsDropped = 0;

    unsigned long expAclSubmitted = 0;
    unsigned long expAclSentToWire = 0;
    unsigned long expAclOutDroppedNotReady = 0;
    unsigned long expAclDroppedNoCredit = 0;
    unsigned long expAclDroppedWireFailed = 0;

    unsigned long expScoSubmitted = 0;
    unsigned long expScoSentToWire = 0;
    unsigned long expScoOutboundOverwrites = 0;
    unsigned long expScoInboundOverwrites = 0;

    unsigned long expEventsReceived = 0;
    unsigned long expEventsQueued = 0;
    unsigned long expEventsSuppressedVendor = 0;
    unsigned long expEventsDroppedNotReady = 0;
    unsigned long expEventsDroppedFifoFull = 0;

    unsigned long expAclReceived = 0;
    unsigned long expAclQueued = 0;
    unsigned long expAclInDroppedNotReady = 0;
    unsigned long expAclDroppedFifoFull = 0;

    unsigned long expScoReceived = 0;
    unsigned long expScoQueued = 0;
    unsigned long expScoDroppedNotReady = 0;
    unsigned long expScoInboundDiscardedOversize = 0;

    unsigned long expResets = 0;

    printf("=== Bridge Self-Test: QCA2066 HCI Bridge ===\n\n");

    /* Initialise mock and bridge */
    memset(&mock, 0, sizeof(mock));
    wireOps.SendCommand = MockSendCommand;
    wireOps.SendAcl     = MockSendAcl;
    wireOps.SendSco     = MockSendSco;
    wire.Ops            = &wireOps;
    wire.Context        = &mock;

    memset(&transport, 0, sizeof(transport));
    HciBridgeInit(&bridge, &wire);
    HciBridgeBindTransport(&transport, &bridge);


    /* =========================================================================
     * 1. Readiness Gating (Hold-then-replay)
     * ========================================================================= */
    printf("\n-- Criterion 1: Readiness Gating (Hold-Then-Replay) --\n");

    CHECK(bridge.Ready == 0, "Bridge starts in Ready=0");

    /* Host submits HCI_Reset (opcode 0x0C03, plen 0) before Ready */
    {
        const unsigned char cmdReset[3] = { 0x03, 0x0C, 0x00 };
        ok = HciTransportSubmitCommand(&transport, cmdReset, sizeof(cmdReset));
        expCommandsSubmitted++;
        expCommandsHeld++;

        CHECK(ok == 1, "SubmitCommand before Ready returns 1 (accepted for EP0)");
        CHECK(mock.CommandsReceived == 0, "Command did not reach wire before Ready");
        CHECK(bridge.HeldCommandPending == 1, "Command held in holding slot");
        CHECK(bridge.HeldCommandLength == 3, "Held command length == 3");
        CHECK(memcmp(bridge.HeldCommand, cmdReset, 3) == 0, "Held command content matches");
    }

    /* Firmware download completes; bridge transition Ready 0 -> 1 */
    HciBridgeSetReady(&bridge, 1);
    expCommandsReplayed++;
    expCommandsSentToWire++;

    CHECK(bridge.Ready == 1, "Bridge is now Ready=1");
    CHECK(mock.CommandsReceived == 1, "Held command replayed to wire upon Ready=1");
    CHECK(mock.LastCommandLength == 3, "Replayed command length == 3");
    {
        const unsigned char cmdReset[3] = { 0x03, 0x0C, 0x00 };
        CHECK(memcmp(mock.LastCommand, cmdReset, 3) == 0, "Replayed command content matches HCI_Reset");
    }
    CHECK(bridge.HeldCommandPending == 0, "Held command slot cleared");

    /* Subsequent command after Ready forwards directly to wire */
    {
        const unsigned char cmdReadVer[3] = { 0x01, 0x10, 0x00 };
        ok = HciTransportSubmitCommand(&transport, cmdReadVer, sizeof(cmdReadVer));
        expCommandsSubmitted++;
        expCommandsSentToWire++;

        CHECK(ok == 1, "SubmitCommand after Ready returns 1");
        CHECK(mock.CommandsReceived == 2, "Second command reached wire directly");
        CHECK(mock.LastCommandLength == 3, "Second command length == 3");
        CHECK(memcmp(mock.LastCommand, cmdReadVer, 3) == 0, "Second command content matches Read_Local_Version");
    }

    /* =========================================================================
     * 2. Boot-Chatter Suppression
     * ========================================================================= */
    printf("\n-- Criterion 2: Boot-Chatter Suppression --\n");

    /* Mock injects a vendor event (0xFF, plen 3) */
    {
        const unsigned char vendorEvt[5] = { 0xFF, 0x03, 0xAA, 0xBB, 0xCC };
        ok = HciBridgeOnEvent(&bridge, vendorEvt, sizeof(vendorEvt));
        expEventsReceived++;
        expEventsSuppressedVendor++;

        CHECK(ok == 0, "HciBridgeOnEvent returns 0 for vendor event (0xFF)");
        CHECK(HciTransportHasStream(&transport, HciStreamEvent) == 0,
              "Vendor event does not appear on HciStreamEvent");
        CHECK(bridge.Counters.EventsSuppressedVendor == 1,
              "EventsSuppressedVendor counter incremented to 1");
    }

    /*
     * Boot chatter is not only event code 0xFF. Upstream hci_qca.c
     * (https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/hci_qca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137)
     * notes that this silicon family answers the 0xFC48 baud switch with a vendor event that
     * lands after the host has moved on, where it can be misinterpreted as a response to a later
     * command. A Command_Complete or Command_Status carrying a vendor opcode is the same hazard
     * wearing a standard event code: BTHPORT would see a completion for a command it never issued.
     * Everything in OGF 0x3F (opcode >= 0xFC00) must be consumed internally.
     */
    {
        const unsigned char vendorComplete[7] = {
            0x0E, 0x05,             /* Command_Complete, plen 5 */
            0x01,                   /* Num_HCI_Command_Packets */
            0x48, 0xFC,             /* Opcode 0xFC48 - QCA baud switch */
            0x00, 0x00
        };
        const unsigned char vendorStatus[6] = {
            0x0F, 0x04,             /* Command_Status, plen 4 */
            0x00,                   /* Status */
            0x01,                   /* Num_HCI_Command_Packets */
            0x00, 0xFC              /* Opcode 0xFC00 - EDL patch download */
        };

        bridge.HostCommandsPending = 1;
        ok = HciBridgeOnEvent(&bridge, vendorComplete, sizeof(vendorComplete));
        expEventsReceived++;
        expEventsSuppressedVendor++;
        CHECK(ok == 0, "Command_Complete for vendor opcode 0xFC48 is suppressed");
        CHECK(bridge.HostCommandsPending == 1,
              "Late vendor Command_Complete does not decrement HostCommandsPending");
        CHECK(HciTransportHasStream(&transport, HciStreamEvent) == 0,
              "Vendor Command_Complete does not reach the host event stream");

        ok = HciBridgeOnEvent(&bridge, vendorStatus, sizeof(vendorStatus));
        expEventsReceived++;
        expEventsSuppressedVendor++;
        CHECK(ok == 0, "Command_Status for vendor opcode 0xFC00 is suppressed");
        CHECK(bridge.HostCommandsPending == 1,
              "Late vendor Command_Status does not decrement HostCommandsPending");
        CHECK(HciTransportHasStream(&transport, HciStreamEvent) == 0,
              "Vendor Command_Status does not reach the host event stream");
        bridge.HostCommandsPending = 0;

        /*
         * A host vendor command is matched to its completion by opcode, not by count: a late
         * boot completion for a different vendor opcode must not retire it or reach the host.
         */
        {
            const unsigned char ordinaryCmd[3] = { 0x01, 0x10, 0x00 };   /* Read_Local_Version */
            const unsigned char hostVendorCmd[3] = { 0x17, 0xFC, 0x00 };
            const unsigned char fc17Complete[6] = { 0x0E, 0x04, 0x01, 0x17, 0xFC, 0x00 };

            ok = HciTransportSubmitCommand(&transport, ordinaryCmd, sizeof(ordinaryCmd));
            expCommandsSubmitted++;
            expCommandsSentToWire++;
            CHECK(ok == 1, "Host submits an ordinary command (Read_Local_Version)");
            ok = HciTransportSubmitCommand(&transport, hostVendorCmd, sizeof(hostVendorCmd));
            expCommandsSubmitted++;
            expCommandsSentToWire++;
            CHECK(ok == 1, "Host submits vendor command (0xFC17)");
            CHECK(bridge.HostCommandsPending == 2 && bridge.HostVendorOpcodeCount == 1,
                  "Two host commands pending, one of them vendor 0xFC17");

            ok = HciBridgeOnEvent(&bridge, vendorComplete, sizeof(vendorComplete));
            expEventsReceived++;
            expEventsSuppressedVendor++;
            CHECK(ok == 0, "Late boot 0xFC48 completion is suppressed while host 0xFC17 is pending");
            CHECK(bridge.HostCommandsPending == 2 && bridge.HostVendorOpcodeCount == 1,
                  "Late 0xFC48 completion does not retire the host 0xFC17 command");
            CHECK(HciTransportHasStream(&transport, HciStreamEvent) == 0,
                  "Late 0xFC48 completion does not reach the host");

            ok = HciBridgeOnEvent(&bridge, fc17Complete, sizeof(fc17Complete));
            expEventsReceived++;
            expEventsQueued++;
            CHECK(ok == 1, "0xFC17 completion is forwarded to the host");
            CHECK(bridge.HostCommandsPending == 1 && bridge.HostVendorOpcodeCount == 0,
                  "0xFC17 completion retires exactly one host command, leaving the ordinary one pending");
            CHECK(HciTransportHasStream(&transport, HciStreamEvent) == 1,
                  "Host event stream receives the 0xFC17 completion");
            HciTransportPopStream(&transport, HciStreamEvent, popBuf, sizeof(popBuf), &written);
            bridge.HostCommandsPending = 0;
        }
    }


    /* Mock injects ordinary Command_Complete (0x0E, plen 4) for HCI_Reset */
    {
        const unsigned char cmdComplete[6] = { 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00 };
        ok = HciBridgeOnEvent(&bridge, cmdComplete, sizeof(cmdComplete));
        expEventsReceived++;
        expEventsQueued++;

        CHECK(ok == 1, "HciBridgeOnEvent returns 1 for Command_Complete (0x0E)");
        CHECK(HciTransportHasStream(&transport, HciStreamEvent) == 1,
              "Command_Complete does appear on HciStreamEvent");
        /*
         * hci_transport.h defines LastEventLength only for HCI_BACKEND_STUB; an asynchronous
         * backend must report 0 rather than a value that is already stale. A queued event is
         * exactly the case where a reintroduced "remember the last length" field would show up.
         */
        CHECK(HciTransportLastEventLength(&transport) == 0,
              "LastEventLength stays 0 for the async backend even with an event queued");

        /* Pop and verify content */
        written = 0;
        ok = HciTransportPopStream(&transport, HciStreamEvent, popBuf, sizeof(popBuf), &written);
        CHECK(ok == 1, "PopStream succeeds for Command_Complete");
        CHECK(written == 6, "PopStream written == 6");
        CHECK(memcmp(popBuf, cmdComplete, 6) == 0, "Popped event byte-identical to injected event");
        CHECK(HciTransportHasStream(&transport, HciStreamEvent) == 0, "Event stream now empty");
    }

    /* =========================================================================
     * 3. ACL Credit Bookkeeping
     * ========================================================================= */
    printf("\n-- Criterion 3: ACL Credit Bookkeeping --\n");

    /*
     * Controller reports Read_Buffer_Size Command_Complete:
     * 4 ACL buffers (N = 4), 1021 ACL length, 8 SCO buffers, 255 SCO length
     */
    {
        const unsigned char readBufSize[13] = {
            0x0E, 0x0B,             /* Event 0x0E, plen 11 */
            0x01,                   /* Num_HCI_Command_Packets: 1 */
            0x05, 0x10,             /* Opcode: 0x1005 (Read_Buffer_Size) */
            0x00,                   /* Status: Success */
            0xFD, 0x03,             /* ACL Data Packet Length: 1021 */
            0xFF,                   /* Synchronous Data Packet Length: 255 */
            0x04, 0x00,             /* Total Num ACL Data Packets: 4 */
            0x08, 0x00              /* Total Num Synchronous Data Packets: 8 */
        };
        ok = HciBridgeOnEvent(&bridge, readBufSize, sizeof(readBufSize));
        expEventsReceived++;
        expEventsQueued++;

        CHECK(ok == 1, "Read_Buffer_Size event accepted and queued");
        CHECK(bridge.TotalAclBuffers == 4, "TotalAclBuffers learned == 4");
        CHECK(bridge.AvailableAclCredits == 4, "AvailableAclCredits initialised to 4");

        /* Drain event stream */
        HciTransportPopStream(&transport, HciStreamEvent, popBuf, sizeof(popBuf), &written);
    }

    /* Single handle test: submit 4 ACL packets for Handle 0x0040 */
    {
        const unsigned char aclPkt40[6] = { 0x40, 0x20, 0x02, 0x00, 0x11, 0x22 };
        unsigned long mockAclBefore = mock.AclReceived;

        for (i = 0; i < 4; i++) {
            ok = HciTransportSubmitAcl(&transport, aclPkt40, sizeof(aclPkt40));
            expAclSubmitted++;
            expAclSentToWire++;
            CHECK(ok == 1, "SubmitAcl packet %u/4 succeeds", i + 1);
            CHECK(bridge.AvailableAclCredits == (unsigned short)(3 - i),
                  "Credits decremented to %u", 3 - i);
        }

        CHECK(mock.AclReceived == mockAclBefore + 4, "Mock received all 4 ACL packets");
        CHECK(bridge.AvailableAclCredits == 0, "Credits now exhausted (0)");

        /* N+1st SubmitAcl must return 0 */
        ok = HciTransportSubmitAcl(&transport, aclPkt40, sizeof(aclPkt40));
        expAclSubmitted++;
        expAclDroppedNoCredit++;

        CHECK(ok == 0, "N+1st SubmitAcl returns 0 (credit exhausted)");
        CHECK(bridge.Counters.AclDroppedNoCredit == 1, "AclDroppedNoCredit == 1");
    }

    /* Controller sends Number_Of_Completed_Packets (0x13) restoring 1 credit on 0x0040 */
    {
        const unsigned char numCompl1[7] = {
            0x13, 0x05,             /* Event 0x13, plen 5 */
            0x01,                   /* Num_Handles: 1 */
            0x40, 0x00,             /* Handle: 0x0040 */
            0x01, 0x00              /* Num_Completed: 1 */
        };
        ok = HciBridgeOnEvent(&bridge, numCompl1, sizeof(numCompl1));
        expEventsReceived++;
        expEventsQueued++;

        CHECK(ok == 1, "Number_Of_Completed_Packets accepted");
        CHECK(bridge.AvailableAclCredits == 1, "AvailableAclCredits restored to 1");

        /* Next SubmitAcl now succeeds */
        {
            const unsigned char aclPkt40[6] = { 0x40, 0x20, 0x02, 0x00, 0x33, 0x44 };
            ok = HciTransportSubmitAcl(&transport, aclPkt40, sizeof(aclPkt40));
            expAclSubmitted++;
            expAclSentToWire++;
            CHECK(ok == 1, "SubmitAcl after credit restoration succeeds");
            CHECK(bridge.AvailableAclCredits == 0, "Credits exhausted again (0)");
        }

        /* Drain event stream */
        HciTransportPopStream(&transport, HciStreamEvent, popBuf, sizeof(popBuf), &written);
    }

    /*
     * Multi-handle credit accounting.
     *
     * Entering here handle 0x0040 holds all 4 buffers. To exercise two handles legitimately,
     * first free two of them, put those two in flight on 0x0041, and only then report both.
     * Reporting completions for a handle that never sent anything is not a legitimate input and
     * is tested separately below - crediting it would let the host overcommit real silicon.
     */
    {
        const unsigned char numCompl40[7] = {
            0x13, 0x05,
            0x01,
            0x40, 0x00,             /* Handle 0x0040 */
            0x02, 0x00              /* 2 completed */
        };
        const unsigned char numComplBoth[11] = {
            0x13, 0x09,
            0x02,                   /* Num_Handles: 2 */
            0x40, 0x00, 0x02, 0x00, /* Handle 0x0040: 2 completed */
            0x41, 0x00, 0x02, 0x00  /* Handle 0x0041: 2 completed */
        };
        const unsigned char numComplGhost[7] = {
            0x13, 0x05,
            0x01,
            0x99, 0x00,             /* Handle 0x0099 - never submitted on */
            0x02, 0x00              /* claims 2 completed */
        };
        const unsigned char aclPkt40[6] = { 0x40, 0x20, 0x02, 0x00, 0x55, 0x66 };
        const unsigned char aclPkt41[6] = { 0x41, 0x20, 0x02, 0x00, 0x77, 0x88 };

        /* Free two of 0x0040's buffers so a second handle can take them. */
        ok = HciBridgeOnEvent(&bridge, numCompl40, sizeof(numCompl40));
        expEventsReceived++; expEventsQueued++;
        CHECK(ok == 1, "Number_Of_Completed_Packets for 0x0040 accepted");
        CHECK(bridge.AvailableAclCredits == 2, "Two buffers freed on 0x0040 (credits 2)");
        HciTransportPopStream(&transport, HciStreamEvent, popBuf, sizeof(popBuf), &written);

        ok = HciTransportSubmitAcl(&transport, aclPkt41, sizeof(aclPkt41));
        expAclSubmitted++; expAclSentToWire++;
        CHECK(ok == 1, "Handle 0x0041 takes a freed buffer");
        ok = HciTransportSubmitAcl(&transport, aclPkt41, sizeof(aclPkt41));
        expAclSubmitted++; expAclSentToWire++;
        CHECK(ok == 1, "Handle 0x0041 takes the second freed buffer");
        CHECK(bridge.AvailableAclCredits == 0,
              "Pool exhausted with 2 in flight on each of two handles");

        /*
         * Regression: credit over-return. A Number_Of_Completed_Packets naming a handle with
         * nothing outstanding - a stale event for a closed handle, or a rogue one - must mint
         * no credit at all. Clamping to TotalAclBuffers is not enough: the other handle still
         * occupies real controller buffers, so any credit granted here is an overcommit that
         * real silicon answers with a dropped or corrupted packet.
         */
        ok = HciBridgeOnEvent(&bridge, numComplGhost, sizeof(numComplGhost));
        expEventsReceived++; expEventsQueued++;
        CHECK(ok == 1, "Number_Of_Completed_Packets for an unknown handle is still forwarded");
        CHECK(bridge.AvailableAclCredits == 0,
              "Completions for a handle with nothing outstanding mint no credit");
        HciTransportPopStream(&transport, HciStreamEvent, popBuf, sizeof(popBuf), &written);

        /* The genuine two-handle report returns every buffer. */
        ok = HciBridgeOnEvent(&bridge, numComplBoth, sizeof(numComplBoth));
        expEventsReceived++; expEventsQueued++;
        CHECK(ok == 1, "Two-handle Number_Of_Completed_Packets accepted");
        CHECK(bridge.AvailableAclCredits == 4, "Credits restored to total pool (4)");
        HciTransportPopStream(&transport, HciStreamEvent, popBuf, sizeof(popBuf), &written);

        ok = HciTransportSubmitAcl(&transport, aclPkt40, sizeof(aclPkt40));
        expAclSubmitted++; expAclSentToWire++;
        CHECK(ok == 1, "Handle 0x0040 packet 1/2 succeeds");
        ok = HciTransportSubmitAcl(&transport, aclPkt40, sizeof(aclPkt40));
        expAclSubmitted++; expAclSentToWire++;
        CHECK(ok == 1, "Handle 0x0040 packet 2/2 succeeds");
        ok = HciTransportSubmitAcl(&transport, aclPkt41, sizeof(aclPkt41));
        expAclSubmitted++; expAclSentToWire++;
        CHECK(ok == 1, "Handle 0x0041 packet 1/2 succeeds");
        ok = HciTransportSubmitAcl(&transport, aclPkt41, sizeof(aclPkt41));
        expAclSubmitted++; expAclSentToWire++;
        CHECK(ok == 1, "Handle 0x0041 packet 2/2 succeeds");

        CHECK(bridge.AvailableAclCredits == 0, "Credits exhausted across both handles (0)");

        /* 5th packet across both handles fails */
        ok = HciTransportSubmitAcl(&transport, aclPkt40, sizeof(aclPkt40));
        expAclSubmitted++; expAclDroppedNoCredit++;
        CHECK(ok == 0, "5th SubmitAcl fails across both handles");
    }
    /* Disconnection credit recovery test: handle 0x0042 disconnects without any event 0x13 */
    {
        const unsigned char aclPkt42[6] = { 0x42, 0x20, 0x02, 0x00, 0x99, 0xAA };
        const unsigned char aclPkt43[6] = { 0x43, 0x20, 0x02, 0x00, 0xBB, 0xCC };
        const unsigned char disconnEvt[6] = {
            0x05, 0x04,             /* Event 0x05 (Disconnection_Complete), plen 4 */
            0x00,                   /* Status: Success */
            0x42, 0x00,             /* Handle: 0x0042 */
            0x13                    /* Reason: Remote User Terminated Connection (0x13) */
        };

        /*
         * Refill to the full pool. Both handles still hold two buffers each, so the refill must
         * name both: a report of 4 on 0x0040 alone would be a lie the bridge is required to
         * refuse, since 0x0041's two buffers are genuinely still occupied.
         */
        const unsigned char numComplRefill[11] = {
            0x13, 0x09,
            0x02,
            0x40, 0x00, 0x02, 0x00,
            0x41, 0x00, 0x02, 0x00
        };
        HciBridgeOnEvent(&bridge, numComplRefill, sizeof(numComplRefill));
        expEventsReceived++; expEventsQueued++;
        HciTransportPopStream(&transport, HciStreamEvent, popBuf, sizeof(popBuf), &written);

        CHECK(bridge.AvailableAclCredits == 4, "Credits refilled to 4 for disconnection test");

        /* Submit 4 packets on handle 0x0042 to exhaust credits */
        for (i = 0; i < 4; i++) {
            ok = HciTransportSubmitAcl(&transport, aclPkt42, sizeof(aclPkt42));
            expAclSubmitted++; expAclSentToWire++;
            CHECK(ok == 1, "Handle 0x0042 packet %u/4 submitted", i + 1);
        }
        CHECK(bridge.AvailableAclCredits == 0, "Credits exhausted on handle 0x0042 (0)");

        /* 5th packet fails */
        ok = HciTransportSubmitAcl(&transport, aclPkt42, sizeof(aclPkt42));
        expAclSubmitted++; expAclDroppedNoCredit++;
        CHECK(ok == 0, "SubmitAcl fails when exhausted");

        /*
         * Handle 0x0042 disconnects without any Number_Of_Completed_Packets (event 0x13).
         * Controller sends Disconnection_Complete (0x05). Bridge must restore all 4
         * outstanding credits to the pool and free the slot.
         */
        ok = HciBridgeOnEvent(&bridge, disconnEvt, sizeof(disconnEvt));
        expEventsReceived++; expEventsQueued++;
        CHECK(ok == 1, "Disconnection_Complete event queued");
        CHECK(bridge.AvailableAclCredits == 4,
              "AvailableAclCredits fully restored to 4 upon Disconnection_Complete without event 0x13");

        /* Drain disconnection event */
        HciTransportPopStream(&transport, HciStreamEvent, popBuf, sizeof(popBuf), &written);

        /* Prove handle slot is reusable and credits are immediately usable on handle 0x0043 */
        for (i = 0; i < 4; i++) {
            ok = HciTransportSubmitAcl(&transport, aclPkt43, sizeof(aclPkt43));
            expAclSubmitted++; expAclSentToWire++;
            CHECK(ok == 1, "Handle 0x0043 packet %u/4 succeeds using restored credits", i + 1);
        }
        CHECK(bridge.AvailableAclCredits == 0, "Credits exhausted on new handle 0x0043");

        /* Clean up: refill credits for subsequent tests */
        {
            const unsigned char numComplClean[7] = { 0x13, 0x05, 0x01, 0x43, 0x00, 0x04, 0x00 };
            HciBridgeOnEvent(&bridge, numComplClean, sizeof(numComplClean));
            expEventsReceived++; expEventsQueued++;
            HciTransportPopStream(&transport, HciStreamEvent, popBuf, sizeof(popBuf), &written);
        }
    }
    /*
     * HCI_Reset recovery: Windows resets the controller when Bluetooth is switched off, with ACL
     * still in flight. The reset frees every controller buffer but reports no completions and no
     * disconnections, so the bridge must refill on the reset's Command_Complete.
     */
    {
        const unsigned char aclPkt44[6] = { 0x44, 0x20, 0x02, 0x00, 0x01, 0x02 };
        const unsigned char resetComplete[6] = { 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00 };
        const unsigned char staleCompl[7] = { 0x13, 0x05, 0x01, 0x44, 0x00, 0x03, 0x00 };
        const unsigned char leBufferSize[9] = { 0x0E, 0x07, 0x01, 0x02, 0x20, 0x00, 0xFB, 0x00, 0x04 };

        CHECK(bridge.AvailableAclCredits == 4, "Pool full before the reset test");
        for (i = 0; i < 3; i++) {
            ok = HciTransportSubmitAcl(&transport, aclPkt44, sizeof(aclPkt44));
            expAclSubmitted++; expAclSentToWire++;
            CHECK(ok == 1, "Handle 0x0044 packet %u/3 in flight before the reset", i + 1);
        }
        CHECK(bridge.AvailableAclCredits == 1, "Three buffers in flight (credits 1)");

        ok = HciBridgeOnEvent(&bridge, resetComplete, sizeof(resetComplete));
        expEventsReceived++; expEventsQueued++;
        CHECK(ok == 1, "HCI_Reset Command_Complete forwarded to the host");
        CHECK(bridge.AvailableAclCredits == 4, "HCI_Reset refills the pool with nothing reported complete");
        HciTransportPopStream(&transport, HciStreamEvent, popBuf, sizeof(popBuf), &written);

        ok = HciBridgeOnEvent(&bridge, staleCompl, sizeof(staleCompl));
        expEventsReceived++; expEventsQueued++;
        CHECK(bridge.AvailableAclCredits == 4, "Completions for pre-reset packets mint no extra credit");
        HciTransportPopStream(&transport, HciStreamEvent, popBuf, sizeof(popBuf), &written);

        /* A separate LE pool: the host may fill both pools at once, so none of it may be refused. */
        ok = HciBridgeOnEvent(&bridge, leBufferSize, sizeof(leBufferSize));
        expEventsReceived++; expEventsQueued++;
        CHECK(bridge.LeTotalAclBuffers == 4, "LE pool learned from LE_Read_Buffer_Size (4)");
        CHECK(bridge.AvailableAclCredits == 8, "Shared and LE pools both usable (credits 8)");
        HciTransportPopStream(&transport, HciStreamEvent, popBuf, sizeof(popBuf), &written);

        for (i = 0; i < 8; i++) {
            ok = HciTransportSubmitAcl(&transport, aclPkt44, sizeof(aclPkt44));
            expAclSubmitted++; expAclSentToWire++;
            CHECK(ok == 1, "Packet %u/8 across both pools accepted", i + 1);
        }
        ok = HciTransportSubmitAcl(&transport, aclPkt44, sizeof(aclPkt44));
        expAclSubmitted++; expAclDroppedNoCredit++;
        CHECK(ok == 0, "A ninth packet exceeds both pools and is refused");

        ok = HciBridgeOnEvent(&bridge, resetComplete, sizeof(resetComplete));
        expEventsReceived++; expEventsQueued++;
        CHECK(bridge.AvailableAclCredits == 8, "HCI_Reset refills both pools");
        HciTransportPopStream(&transport, HciStreamEvent, popBuf, sizeof(popBuf), &written);
    }


    /* =========================================================================
     * 4. SCO Overwrite-Oldest (Inbound and Outbound)
     * ========================================================================= */
    printf("\n-- Criterion 4: Synchronous (SCO) FIFO Overwrite-Oldest --\n");

    /* Part A: Inbound SCO overwrite */
    {
        /* Capacity is HCI_BRIDGE_SCO_FIFO_DEPTH = 8. Inject 10 packets (payload index 0..9) */
        for (i = 0; i < 10; i++) {
            unsigned char scoPkt[4] = { 0x50, 0x00, 0x01, (unsigned char)i };
            ok = HciBridgeOnSco(&bridge, scoPkt, sizeof(scoPkt));
            expScoReceived++;
            expScoQueued++;
            CHECK(ok == 1, "Inbound SCO packet %u injected", i);
        }
        expScoInboundOverwrites += 2;

        CHECK(bridge.Counters.ScoInboundOverwrites == 2,
              "ScoInboundOverwrites == 2 after injecting 10 into depth-8 FIFO");
        CHECK(bridge.ScoInCount == 8, "Inbound FIFO count is exactly capacity (8)");

        /*
         * Pop all 8 packets: oldest 2 packets (0, 1) must have been dropped;
         * surviving packets must be 2, 3, 4, 5, 6, 7, 8, 9.
         */
        for (i = 2; i < 10; i++) {
            written = 0;
            ok = HciTransportPopStream(&transport, HciStreamSco, popBuf, sizeof(popBuf), &written);
            CHECK(ok == 1, "PopStream on HciStreamSco succeeds for packet %u", i);
            CHECK(written == 4, "Popped SCO length == 4");
            CHECK(popBuf[3] == (unsigned char)i,
                  "Surviving packet matches newest payload index %u (oldest overwritten)", i);
        }

        CHECK(HciTransportHasStream(&transport, HciStreamSco) == 0,
              "Inbound SCO FIFO completely empty after 8 pops");
    }

    /* Part B: Outbound SCO overwrite */
    {
        /* Simulate busy/flow-controlled wire: mock refuses SCO */
        mock.RefuseSco = 1;
        mock.ScoHistoryCount = 0;

        /* Submit 10 outbound SCO packets (payload index 0..9) */
        for (i = 0; i < 10; i++) {
            unsigned char scoPkt[4] = { 0x50, 0x00, 0x01, (unsigned char)i };
            ok = HciTransportSubmitSco(&transport, scoPkt, sizeof(scoPkt));
            expScoSubmitted++;
            CHECK(ok == 1, "SubmitSco outbound packet %u accepted into FIFO", i);
        }
        expScoOutboundOverwrites += 2;

        CHECK(bridge.Counters.ScoOutboundOverwrites == 2,
              "ScoOutboundOverwrites == 2 after submitting 10 into depth-8 FIFO");
        CHECK(bridge.OutboundScoCount == 8, "Outbound FIFO count is exactly capacity (8)");
        CHECK(mock.ScoReceived == 0, "No SCO packets reached mock while wire refused");

        /* Now wire clears: allow SCO and drain */
        mock.RefuseSco = 0;
        HciBridgeDrainOutboundSco(&bridge);
        expScoSentToWire += 8;

        CHECK(mock.ScoReceived == 8, "All 8 surviving SCO packets reached mock");
        CHECK(bridge.OutboundScoCount == 0, "Outbound SCO FIFO drained to 0");

        /* Verify that oldest packets (0, 1) were overwritten and newest (2..9) survived */
        for (i = 0; i < 8; i++) {
            unsigned char expectedIdx = (unsigned char)(i + 2);
            CHECK(mock.ScoHistory[i][3] == expectedIdx,
                  "Outbound mock received surviving packet %u with payload index %u", i, expectedIdx);
        }
    }

    /* Part C: Outbound SCO flush prevents stale packet leakage across call boundaries */
    {
        unsigned char stalePkt[4] = { 0x50, 0x00, 0x01, 0xAA };
        unsigned char freshPkt[4] = { 0x51, 0x00, 0x01, 0xBB };

        /* Simulate final packet of previous call queued while wire refused */
        mock.RefuseSco = 1;
        ok = HciTransportSubmitSco(&transport, stalePkt, sizeof(stalePkt));
        expScoSubmitted++;
        CHECK(ok == 1, "Final SCO packet accepted into FIFO while wire busy");
        CHECK(bridge.OutboundScoCount == 1, "Stale packet remains queued in bridge SCO FIFO");

        /* Call ends: voice stream flushed */
        HciTransportFlushSco(&transport);
        CHECK(bridge.OutboundScoCount == 0, "FlushSco empties outbound SCO FIFO immediately");

        /* New call begins on wire */
        mock.RefuseSco = 0;
        ok = HciTransportSubmitSco(&transport, freshPkt, sizeof(freshPkt));
        expScoSubmitted++;
        expScoSentToWire++;
        CHECK(ok == 1, "Fresh SCO packet submitted on new call");
        CHECK(mock.LastScoLength == sizeof(freshPkt) && mock.LastSco[3] == 0xBB,
              "Wire received fresh packet (0xBB); stale packet (0xAA) from previous call was discarded");
        CHECK(bridge.OutboundScoCount == 0, "Outbound SCO FIFO drained cleanly");
    }

    /* Part D: Multi-handle SCO disconnection purges only the disconnected handle's queued audio */
    {
        unsigned char pkt40[4] = { 0x40, 0x00, 0x01, 0x40 };
        unsigned char pkt41[4] = { 0x41, 0x00, 0x01, 0x41 };
        const unsigned char discEvt40[6] = { 0x05, 0x04, 0x00, 0x40, 0x00, 0x16 };

        mock.RefuseSco = 1;
        ok = HciTransportSubmitSco(&transport, pkt40, sizeof(pkt40));
        expScoSubmitted++;
        CHECK(ok == 1, "Queue SCO packet for handle 0x0040");
        ok = HciTransportSubmitSco(&transport, pkt41, sizeof(pkt41));
        expScoSubmitted++;
        CHECK(ok == 1, "Queue SCO packet for handle 0x0041");
        CHECK(bridge.OutboundScoCount == 2, "Two packets queued across different handles in Outbound SCO FIFO");

        /* Disconnect handle 0x0040 */
        ok = HciBridgeOnEvent(&bridge, discEvt40, sizeof(discEvt40));
        expEventsReceived++;
        expEventsQueued++;
        CHECK(ok == 1, "Disconnection_Complete for handle 0x0040 processed");
        CHECK(bridge.OutboundScoCount == 1, "Outbound SCO FIFO has 1 packet left (handle 0x0041 preserved, not wiped)");

        HciTransportPopStream(&transport, HciStreamEvent, popBuf, sizeof(popBuf), &written);

        /* Wire clears: drain remaining SCO */
        mock.RefuseSco = 0;
        HciBridgeDrainOutboundSco(&bridge);
        expScoSentToWire++;
        CHECK(mock.LastScoLength == sizeof(pkt41) && mock.LastSco[0] == 0x41,
              "Wire received handle 0x0041 packet; handle 0x0040 packet was cleanly discarded");
        CHECK(bridge.OutboundScoCount == 0, "Outbound SCO FIFO fully drained");
    }

    /* =========================================================================
     * 5. PopStream Partial Buffer Handling
     * ========================================================================= */
    printf("\n-- Criterion 5: PopStream Undersized Buffer Semantics --\n");

    {
        const unsigned char testEvt[10] = { 0x1B, 0x08, 1, 2, 3, 4, 5, 6, 7, 8 };
        ok = HciBridgeOnEvent(&bridge, testEvt, sizeof(testEvt));
        expEventsReceived++;
        expEventsQueued++;
        CHECK(ok == 1, "Test event (len 10) queued");

        /* Pop with capacity 9 (one byte too small) */
        written = 999;
        ok = HciTransportPopStream(&transport, HciStreamEvent, popBuf, 9, &written);
        CHECK(ok == 0, "PopStream with Capacity=9 (needed 10) returns 0");
        CHECK(written == 0, "Written is set to 0 on failure");
        CHECK(HciTransportHasStream(&transport, HciStreamEvent) == 1,
              "Packet remains queued after undersized pop attempt");

        /* Pop with exact capacity 10 */
        written = 0;
        ok = HciTransportPopStream(&transport, HciStreamEvent, popBuf, 10, &written);
        CHECK(ok == 1, "PopStream with Capacity=10 succeeds");
        CHECK(written == 10, "Written is exactly 10");
        CHECK(memcmp(popBuf, testEvt, 10) == 0, "Packet content matches untouched");
        CHECK(HciTransportHasStream(&transport, HciStreamEvent) == 0, "Stream empty after successful pop");
    }
    /* Part B: HciStreamSco Exception: Oversized packet is discarded, not left queued */
    {
        const unsigned char scoA[10] = { 0x50, 0x00, 0x07, 1, 2, 3, 4, 5, 6, 7 }; /* Length 10 */
        const unsigned char scoB[4]  = { 0x50, 0x00, 0x01, 0xBB };                   /* Length 4 */

        ok = HciBridgeOnSco(&bridge, scoA, sizeof(scoA));
        expScoReceived++; expScoQueued++;
        CHECK(ok == 1, "SCO packet A (len 10) queued");

        ok = HciBridgeOnSco(&bridge, scoB, sizeof(scoB));
        expScoReceived++; expScoQueued++;
        CHECK(ok == 1, "SCO packet B (len 4) queued");

        /* Pop with capacity 9 (smaller than A) */
        written = 999;
        ok = HciTransportPopStream(&transport, HciStreamSco, popBuf, 9, &written);
        expScoInboundDiscardedOversize++;

        CHECK(ok == 0, "PopStream on HciStreamSco with Capacity=9 returns 0 (oversized)");
        CHECK(written == 0, "Written is set to 0 for discarded SCO packet");
        CHECK(bridge.Counters.ScoInboundDiscardedOversize == 1,
              "ScoInboundDiscardedOversize incremented to 1");

        /*
         * Packet A was discarded, so the stream is not wedged.
         * The next PopStream with capacity 4 returns packet B.
         */
        written = 0;
        ok = HciTransportPopStream(&transport, HciStreamSco, popBuf, 4, &written);
        CHECK(ok == 1, "PopStream on HciStreamSco with Capacity=4 succeeds for packet B");
        CHECK(written == 4, "Written is exactly 4");
        CHECK(memcmp(popBuf, scoB, 4) == 0, "Popped packet is packet B (packet A was discarded, not wedged)");
        CHECK(HciTransportHasStream(&transport, HciStreamSco) == 0, "SCO stream empty after draining B");
    }


    /* =========================================================================
     * 6. Reset Empties All Streams and Clears Partial State
     * ========================================================================= */
    printf("\n-- Criterion 6: Reset Empties All Streams --\n");

    {
        const unsigned char evt[6] = { 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00 };
        const unsigned char acl[6] = { 0x40, 0x20, 0x02, 0x00, 0x11, 0x22 };
        const unsigned char sco[4] = { 0x50, 0x00, 0x01, 0x99 };

        HciBridgeOnEvent(&bridge, evt, sizeof(evt));
        expEventsReceived++; expEventsQueued++;

        HciBridgeOnAcl(&bridge, acl, sizeof(acl));
        expAclReceived++; expAclQueued++;

        HciBridgeOnSco(&bridge, sco, sizeof(sco));
        expScoReceived++; expScoQueued++;

        CHECK(HciTransportHasStream(&transport, HciStreamEvent) == 1, "Event stream has data");
        CHECK(HciTransportHasStream(&transport, HciStreamAcl) == 1, "ACL stream has data");
        CHECK(HciTransportHasStream(&transport, HciStreamSco) == 1, "SCO stream has data");

        /* Perform Reset */
        HciTransportReset(&transport);
        expResets++;

        CHECK(HciTransportHasStream(&transport, HciStreamEvent) == 0, "Event stream empty after Reset");
        CHECK(HciTransportHasStream(&transport, HciStreamAcl) == 0, "ACL stream empty after Reset");
        CHECK(HciTransportHasStream(&transport, HciStreamSco) == 0, "SCO stream empty after Reset");
        CHECK(bridge.HeldCommandPending == 0, "Held command cleared after Reset");
        CHECK(bridge.AvailableAclCredits == bridge.TotalAclBuffers + bridge.LeTotalAclBuffers,
              "Credits restored to the whole pool after Reset");
    }

    /* =========================================================================
     * 7. Stream Ordering and Interleaving
     * ========================================================================= */
    printf("\n-- Criterion 7: Stream Ordering and Interleaving --\n");

    {
        /* Inject 3 items for each stream, interleaved */
        for (i = 1; i <= 3; i++) {
            unsigned char evtPkt[6] = { 0x0E, 0x04, 0x01, 0x00, 0x00, (unsigned char)i };
            unsigned char aclPkt[6] = { 0x40, 0x20, 0x02, 0x00, 0x00, (unsigned char)i };
            unsigned char scoPkt[4] = { 0x50, 0x00, 0x01, (unsigned char)i };

            HciBridgeOnEvent(&bridge, evtPkt, sizeof(evtPkt));
            expEventsReceived++; expEventsQueued++;

            HciBridgeOnAcl(&bridge, aclPkt, sizeof(aclPkt));
            expAclReceived++; expAclQueued++;

            HciBridgeOnSco(&bridge, scoPkt, sizeof(scoPkt));
            expScoReceived++; expScoQueued++;
        }

        /* Pop all events in sequence */
        for (i = 1; i <= 3; i++) {
            written = 0;
            ok = HciTransportPopStream(&transport, HciStreamEvent, popBuf, sizeof(popBuf), &written);
            CHECK(ok == 1, "PopStream Event %u succeeds", i);
            CHECK(written == 6 && popBuf[5] == (unsigned char)i,
                  "Event %u payload matches FIFO order with no corruption", i);
        }
        CHECK(HciTransportHasStream(&transport, HciStreamEvent) == 0, "All events drained");

        /* Pop all ACL packets in sequence */
        for (i = 1; i <= 3; i++) {
            written = 0;
            ok = HciTransportPopStream(&transport, HciStreamAcl, popBuf, sizeof(popBuf), &written);
            CHECK(ok == 1, "PopStream ACL %u succeeds", i);
            CHECK(written == 6 && popBuf[5] == (unsigned char)i,
                  "ACL %u payload matches FIFO order with no corruption", i);
        }
        CHECK(HciTransportHasStream(&transport, HciStreamAcl) == 0, "All ACL drained");

        /* Pop all SCO packets in sequence */
        for (i = 1; i <= 3; i++) {
            written = 0;
            ok = HciTransportPopStream(&transport, HciStreamSco, popBuf, sizeof(popBuf), &written);
            CHECK(ok == 1, "PopStream SCO %u succeeds", i);
            CHECK(written == 4 && popBuf[3] == (unsigned char)i,
                  "SCO %u payload matches FIFO order with no corruption", i);
        }
        CHECK(HciTransportHasStream(&transport, HciStreamSco) == 0, "All SCO drained");
    }

    /* =========================================================================
     * 7b. Handle table exhaustion must refuse, never leak a credit
     *
     * Credits are returned only against a tracked handle. If a submission on an untrackable
     * handle were still allowed onto the wire, its credit could never come back and the pool
     * would bleed one buffer per such packet until ACL stopped forever. Refusing is
     * recoverable; leaking is not. Uses its own bridge so the counter totals asserted in
     * criterion 8 stay untouched.
     * ========================================================================= */
    printf("\n-- Criterion 7b: Handle Table Exhaustion --\n");

    {
        HCI_BRIDGE      slotBridge;
        HCI_TRANSPORT   slotTransport;
        MOCK_CONTROLLER slotMock;
        HCI_BRIDGE_WIRE slotWire;
        /* Read_Buffer_Size Command_Complete advertising 32 ACL buffers, comfortably more
         * than HCI_BRIDGE_MAX_HANDLES so the handle table runs out before the credits do. */
        const unsigned char bufSize[15] = {
            0x0E, 0x0D, 0x01, 0x05, 0x10, 0x00,
            0xFD, 0x03,             /* ACL packet length 1021 */
            0xFF,                   /* SCO packet length 255  */
            0x20, 0x00,             /* Total ACL buffers = 32 */
            0x08, 0x00,             /* Total SCO buffers      */
            0x00, 0x00
        };
        unsigned short creditsBefore;

        memset(&slotMock, 0, sizeof(slotMock));
        memset(&slotTransport, 0, sizeof(slotTransport));
        slotWire.Ops     = &wireOps;
        slotWire.Context = &slotMock;
        HciBridgeInit(&slotBridge, &slotWire);
        HciBridgeBindTransport(&slotTransport, &slotBridge);
        HciBridgeSetReady(&slotBridge, 1);

        HciBridgeOnEvent(&slotBridge, bufSize, sizeof(bufSize));
        CHECK(slotBridge.TotalAclBuffers == 32, "Slot test learned 32 ACL buffers");

        for (i = 0; i < HCI_BRIDGE_MAX_HANDLES; i++) {
            unsigned char acl[6] = { 0x00, 0x20, 0x02, 0x00, 0xDE, 0xAD };
            acl[0] = (unsigned char)(0x50 + i);
            ok = HciTransportSubmitAcl(&slotTransport, acl, sizeof(acl));
            CHECK(ok == 1, "Handle 0x%04X occupies a tracking slot", 0x50 + i);
        }

        creditsBefore = slotBridge.AvailableAclCredits;
        CHECK(creditsBefore == 24, "Eight buffers in flight, 24 credits left");

        {
            const unsigned char acl9[6] = { 0x60, 0x20, 0x02, 0x00, 0xBE, 0xEF };
            unsigned long wireBefore = slotMock.AclReceived;

            ok = HciTransportSubmitAcl(&slotTransport, acl9, sizeof(acl9));
            CHECK(ok == 0, "A ninth handle is refused rather than sent untracked");
            CHECK(slotMock.AclReceived == wireBefore,
                  "The refused packet never reached the wire");
            CHECK(slotBridge.AvailableAclCredits == creditsBefore,
                  "Refusal consumed no credit, so nothing can leak");
            CHECK(slotBridge.Counters.AclDroppedNoHandleSlot == 1,
                  "AclDroppedNoHandleSlot records exactly the refused packet");
        }

        /* Freeing a handle makes its slot reusable: the refusal is recoverable. */
        {
            const unsigned char disconn[6] = { 0x05, 0x04, 0x00, 0x50, 0x00, 0x13 };
            const unsigned char acl9[6] = { 0x60, 0x20, 0x02, 0x00, 0xBE, 0xEF };

            HciBridgeOnEvent(&slotBridge, disconn, sizeof(disconn));
            ok = HciTransportSubmitAcl(&slotTransport, acl9, sizeof(acl9));
            CHECK(ok == 1, "The ninth handle succeeds once a slot is freed by disconnection");
        }
    }

    /* =========================================================================
     * 7c. Clean shutdown disconnects every link before the controller is handed back
     *
     * Before S3 the adapter is detached and the controller reset. Without a disconnect a headset
     * only sees its link vanish; it then reconnects on its own with some profiles (the microphone
     * profile did not return after sleep). The shutdown ends each ACL link with HCI_Disconnect,
     * one command at a time, and keeps its own replies and BTHPORT's commands apart.
     * Uses its own bridge so the counter totals asserted in criterion 8 stay untouched.
     * ========================================================================= */
    printf("\n-- Criterion 7c: Clean Shutdown --\n");

    {
        HCI_BRIDGE      sdBridge;
        HCI_TRANSPORT   sdTransport;
        MOCK_CONTROLLER sdMock;
        HCI_BRIDGE_WIRE sdWire;
        /* Connection_Complete: status, handle, BD_ADDR, link type (1 = ACL, 0 = SCO), encryption */
        const unsigned char connAcl[13]  = { 0x03, 0x0B, 0x00, 0x02, 0x00, 0xDB, 0x9C, 0x9F, 0xE1, 0xF5, 0xA8, 0x01, 0x00 };
        const unsigned char connSco[13]  = { 0x03, 0x0B, 0x00, 0x03, 0x00, 0xDB, 0x9C, 0x9F, 0xE1, 0xF5, 0xA8, 0x00, 0x00 };
        const unsigned char connFail[13] = { 0x03, 0x0B, 0x04, 0x05, 0x00, 0x6B, 0x2E, 0x6C, 0xF2, 0x8C, 0x70, 0x01, 0x00 };
        /* LE Connection Complete (subevent 0x01) on handle 0x0040 */
        const unsigned char connLe[21]   = { 0x3E, 0x13, 0x01, 0x00, 0x40, 0x00, 0x00, 0x01,
                                             0xC5, 0xB3, 0xF2, 0xEF, 0xD9, 0xDF, 0x06, 0x00, 0x00, 0x00, 0xF4, 0x01, 0x00 };
        const unsigned char readVer[3]   = { 0x01, 0x10, 0x00 };
        const unsigned char discStatus[6]  = { 0x0F, 0x04, 0x00, 0x01, 0x06, 0x04 };
        const unsigned char discUnknown[6] = { 0x0F, 0x04, 0x02, 0x01, 0x06, 0x04 };
        const unsigned char verComplete[6] = { 0x0E, 0x04, 0x01, 0x01, 0x10, 0x00 };
        const unsigned char disc2[6]     = { 0x05, 0x04, 0x00, 0x02, 0x00, 0x16 };
        const unsigned char disc40[6]    = { 0x05, 0x04, 0x00, 0x40, 0x00, 0x16 };
        const unsigned char expect2[6]   = { 0x06, 0x04, 0x03, 0x02, 0x00, 0x15 };
        const unsigned char expect40[6]  = { 0x06, 0x04, 0x03, 0x40, 0x00, 0x15 };
        const unsigned char resetDone[6] = { 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00 };
        unsigned int found;

        memset(&sdMock, 0, sizeof(sdMock));
        memset(&sdTransport, 0, sizeof(sdTransport));
        sdWire.Ops     = &wireOps;
        sdWire.Context = &sdMock;
        HciBridgeInit(&sdBridge, &sdWire);
        HciBridgeBindTransport(&sdTransport, &sdBridge);
        HciBridgeSetReady(&sdBridge, 1);

        CHECK(HciBridgeBeginShutdown(&sdBridge, 0x15) == 0 && sdMock.CommandsReceived == 0,
              "Shutdown with no links sends nothing");
        HciBridgeInit(&sdBridge, &sdWire);
        HciBridgeBindTransport(&sdTransport, &sdBridge);
        HciBridgeSetReady(&sdBridge, 1);

        HciBridgeOnEvent(&sdBridge, connAcl, sizeof(connAcl));
        HciBridgeOnEvent(&sdBridge, connSco, sizeof(connSco));
        HciBridgeOnEvent(&sdBridge, connFail, sizeof(connFail));
        HciBridgeOnEvent(&sdBridge, connLe, sizeof(connLe));
        CHECK(HciBridgeOpenLinks(&sdBridge) == 2,
              "Classic and LE ACL links tracked; a SCO link and a failed connection are not");

        HciBridgeOnEvent(&sdBridge, resetDone, sizeof(resetDone));
        CHECK(HciBridgeOpenLinks(&sdBridge) == 0, "A successful HCI_Reset ends every link");
        HciBridgeOnEvent(&sdBridge, connAcl, sizeof(connAcl));
        HciBridgeOnEvent(&sdBridge, connLe, sizeof(connLe));
        while (HciTransportPopStream(&sdTransport, HciStreamEvent, popBuf, sizeof(popBuf), &written)) {
        }

        /* Link information for the service's hands-free check (OPEN LINKS in hci_bridge.h) */
        {
            HCI_BRIDGE_LINK links[HCI_BRIDGE_MAX_LINKS];
            const unsigned char shokz[6] = { 0xDB, 0x9C, 0x9F, 0xE1, 0xF5, 0xA8 };
            /* L2CAP Connection Request on the signalling channel: handle, lengths, CID 1, code 2, PSM, SCID */
            const unsigned char connAvdtp[16]  = { 0x02, 0x20, 0x0C, 0x00, 0x08, 0x00, 0x01, 0x00,
                                                   0x02, 0x10, 0x04, 0x00, 0x19, 0x00, 0x40, 0x00 };
            const unsigned char contRfcomm[16] = { 0x02, 0x10, 0x0C, 0x00, 0x08, 0x00, 0x01, 0x00,
                                                   0x02, 0x11, 0x04, 0x00, 0x03, 0x00, 0x41, 0x00 };
            const unsigned char connRfcomm[16] = { 0x02, 0x20, 0x0C, 0x00, 0x08, 0x00, 0x01, 0x00,
                                                   0x02, 0x12, 0x04, 0x00, 0x03, 0x00, 0x41, 0x00 };
            const unsigned char leRfcomm[16]   = { 0x40, 0x20, 0x0C, 0x00, 0x08, 0x00, 0x01, 0x00,
                                                   0x02, 0x13, 0x04, 0x00, 0x03, 0x00, 0x42, 0x00 };
            unsigned int n = HciBridgeGetLinks(&sdBridge, links, HCI_BRIDGE_MAX_LINKS);
            int classic = (n == 2 && links[0].Handle == 0x0002) ? 0 : 1;

            CHECK(n == 2 && links[classic].Handle == 0x0002 && links[classic].Classic == 1 &&
                  memcmp(links[classic].Address, shokz, 6) == 0 && links[1 - classic].Classic == 0,
                  "A BR/EDR link keeps its peer address; an LE link is marked LE");

            (void)HciTransportSubmitAcl(&sdTransport, connAvdtp, sizeof(connAvdtp));
            (void)HciTransportSubmitAcl(&sdTransport, contRfcomm, sizeof(contRfcomm));
            n = HciBridgeGetLinks(&sdBridge, links, HCI_BRIDGE_MAX_LINKS);
            CHECK(links[classic].Rfcomm == 0,
                  "An AVDTP connection request and a continuation fragment do not count as RFCOMM");

            (void)HciTransportSubmitAcl(&sdTransport, connRfcomm, sizeof(connRfcomm));
            n = HciBridgeGetLinks(&sdBridge, links, HCI_BRIDGE_MAX_LINKS);
            CHECK(links[classic].Rfcomm == 1 && links[1 - classic].Rfcomm == 0,
                  "The host's RFCOMM connection request marks only its own link");

            (void)HciBridgeOnAcl(&sdBridge, leRfcomm, sizeof(leRfcomm));
            n = HciBridgeGetLinks(&sdBridge, links, HCI_BRIDGE_MAX_LINKS);
            CHECK(links[1 - classic].Rfcomm == 1, "A peer's RFCOMM connection request marks the link too");
            while (HciTransportPopStream(&sdTransport, HciStreamAcl, popBuf, sizeof(popBuf), &written)) {
            }
        }

        found = HciBridgeBeginShutdown(&sdBridge, 0x15);
        CHECK(found == 2, "Shutdown reports both open links");
        CHECK(sdMock.CommandsReceived == 1 && sdMock.LastCommandLength == 6 &&
              memcmp(sdMock.LastCommand, expect2, 6) == 0,
              "Exactly one HCI_Disconnect (0x0002, remote device powered off) is on the wire");

        ok = HciTransportSubmitCommand(&sdTransport, readVer, sizeof(readVer));
        CHECK(ok == 1 && sdMock.CommandsReceived == 1,
              "A host command during shutdown is accepted but withheld from the controller");

        ok = HciBridgeOnEvent(&sdBridge, verComplete, sizeof(verComplete));
        CHECK(ok == 1 && sdMock.CommandsReceived == 1,
              "A reply to a command the host sent earlier still reaches the host and releases nothing");
        HciTransportPopStream(&sdTransport, HciStreamEvent, popBuf, sizeof(popBuf), &written);

        ok = HciBridgeOnEvent(&sdBridge, discStatus, sizeof(discStatus));
        CHECK(ok == 0, "The injected disconnect's Command_Status is kept from the host");
        CHECK(sdMock.CommandsReceived == 2 && memcmp(sdMock.LastCommand, expect40, 6) == 0,
              "That Command_Status releases the HCI_Disconnect for the LE link");

        ok = HciBridgeOnEvent(&sdBridge, disc2, sizeof(disc2));
        CHECK(ok == 0 && HciBridgeOpenLinks(&sdBridge) == 1,
              "Disconnection_Complete during shutdown closes the link and is kept from the host");
        ok = HciBridgeOnEvent(&sdBridge, discStatus, sizeof(discStatus));
        CHECK(ok == 0 && sdMock.CommandsReceived == 2, "With no link left nothing more is sent");
        ok = HciBridgeOnEvent(&sdBridge, disc40, sizeof(disc40));
        CHECK(ok == 0 && HciBridgeOpenLinks(&sdBridge) == 0, "Every link is closed");
        CHECK(HciTransportHasStream(&sdTransport, HciStreamEvent) == 0,
              "The host received nothing from the shutdown");
        CHECK(sdBridge.Counters.CommandsWithheld == 1 && sdBridge.Counters.ShutdownDisconnects == 2,
              "Counters: one host command withheld, two disconnects sent");

        /* A link the controller no longer knows is refused; the shutdown must not wait for it. */
        HciBridgeInit(&sdBridge, &sdWire);
        HciBridgeBindTransport(&sdTransport, &sdBridge);
        HciBridgeSetReady(&sdBridge, 1);
        HciBridgeOnEvent(&sdBridge, connAcl, sizeof(connAcl));
        HciBridgeOnEvent(&sdBridge, connLe, sizeof(connLe));
        (void)HciBridgeBeginShutdown(&sdBridge, 0x15);
        ok = HciBridgeOnEvent(&sdBridge, discUnknown, sizeof(discUnknown));
        CHECK(ok == 0 && HciBridgeOpenLinks(&sdBridge) == 1 && sdBridge.Counters.ShutdownDisconnectsRefused == 1,
              "A refused disconnect removes its link");
        CHECK(memcmp(sdMock.LastCommand, expect40, 6) == 0, "and the shutdown moves on to the next link");

        /* The controller's command credits: a Command_Status granting none holds the next disconnect. */
        {
            const unsigned char noCredit[6] = { 0x0F, 0x04, 0x00, 0x00, 0x06, 0x04 };   /* Num_HCI_Command_Packets 0 */
            const unsigned char creditNop[5] = { 0x0E, 0x03, 0x01, 0x00, 0x00 };         /* opcode 0: credit only */
            const unsigned char hostDisc[6] = { 0x06, 0x04, 0x03, 0x02, 0x00, 0x13 };
            unsigned long sent;

            HciBridgeInit(&sdBridge, &sdWire);
            HciBridgeBindTransport(&sdTransport, &sdBridge);
            HciBridgeSetReady(&sdBridge, 1);
            HciBridgeOnEvent(&sdBridge, connAcl, sizeof(connAcl));
            HciBridgeOnEvent(&sdBridge, connLe, sizeof(connLe));
            while (HciTransportPopStream(&sdTransport, HciStreamEvent, popBuf, sizeof(popBuf), &written)) {
            }
            sent = sdMock.CommandsReceived;
            (void)HciBridgeBeginShutdown(&sdBridge, 0x15);
            CHECK(sdMock.CommandsReceived == sent + 1 && memcmp(sdMock.LastCommand, expect2, 6) == 0,
                  "Credits: the first disconnect goes out");
            ok = HciBridgeOnEvent(&sdBridge, noCredit, sizeof(noCredit));
            CHECK(ok == 0 && sdMock.CommandsReceived == sent + 1,
                  "Its Command_Status with Num_HCI_Command_Packets 0 does not release the next disconnect");
            ok = HciBridgeOnEvent(&sdBridge, creditNop, sizeof(creditNop));
            CHECK(ok == 1 && sdMock.CommandsReceived == sent + 2 && memcmp(sdMock.LastCommand, expect40, 6) == 0,
                  "A later command credit (opcode 0 Command_Complete) releases it");
            HciTransportPopStream(&sdTransport, HciStreamEvent, popBuf, sizeof(popBuf), &written);

            /* BTHPORT's own HCI_Disconnect in flight when the shutdown starts */
            HciBridgeInit(&sdBridge, &sdWire);
            HciBridgeBindTransport(&sdTransport, &sdBridge);
            HciBridgeSetReady(&sdBridge, 1);
            HciBridgeOnEvent(&sdBridge, connAcl, sizeof(connAcl));
            HciBridgeOnEvent(&sdBridge, connLe, sizeof(connLe));
            while (HciTransportPopStream(&sdTransport, HciStreamEvent, popBuf, sizeof(popBuf), &written)) {
            }
            ok = HciTransportSubmitCommand(&sdTransport, hostDisc, sizeof(hostDisc));
            sent = sdMock.CommandsReceived;
            (void)HciBridgeBeginShutdown(&sdBridge, 0x15);
            CHECK(ok == 1 && sdMock.CommandsReceived == sent,
                  "No disconnect is sent while a host command awaits its Command_Status");
            ok = HciBridgeOnEvent(&sdBridge, discStatus, sizeof(discStatus));
            CHECK(ok == 1 && HciTransportHasStream(&sdTransport, HciStreamEvent) == 1,
                  "The Command_Status for the host's own disconnect is the host's and reaches it");
            CHECK(sdMock.CommandsReceived == sent + 1 && memcmp(sdMock.LastCommand, expect2, 6) == 0,
                  "Only then does the first injected disconnect go out");
            ok = HciBridgeOnEvent(&sdBridge, discStatus, sizeof(discStatus));
            CHECK(ok == 0 && sdMock.CommandsReceived == sent + 2 && memcmp(sdMock.LastCommand, expect40, 6) == 0,
                  "Its own Command_Status is kept from the host and releases the next");
            HciTransportPopStream(&sdTransport, HciStreamEvent, popBuf, sizeof(popBuf), &written);
        }
    }

    /* =========================================================================
     * 7d. A burst larger than the inbound queues loses nothing
     *
     * One UART read can hold more packets than the event or ACL queue. The reader decodes only as
     * many packets as there is room for and waits for the host to drain the rest (INBOUND
     * BACKPRESSURE in hci_bridge.h), instead of dropping what does not fit.
     * ========================================================================= */
    printf("\n-- Criterion 7d: Inbound Backpressure --\n");

    {
        static unsigned char burst[2048];
        static unsigned char out[HCI_BRIDGE_MAX_ACL_SIZE];
        static HCI_BRIDGE bpBridge;
        HCI_TRANSPORT   bpTransport;
        MOCK_CONTROLLER bpMock;
        HCI_BRIDGE_WIRE bpWire;
        H4_DECODER      dec;
        /* Read_Local_Version Command_Complete */
        static const unsigned char verCc[15] = { 0x04, 0x0E, 0x0C, 0x01, 0x01, 0x10, 0x00, 0x0B, 0x00, 0x00,
                                                 0x0B, 0x1D, 0x00, 0x00, 0x00 };
        static const unsigned char acl[13] = { 0x02, 0x02, 0x20, 0x08, 0x00, 0x04, 0x00, 0x40, 0x00, 1, 2, 3, 4 };
        unsigned long used = 0;
        unsigned long popped = 0;
        unsigned long lastLength = 0;
        unsigned long waits;

        memset(&bpMock, 0, sizeof(bpMock));
        memset(&bpTransport, 0, sizeof(bpTransport));
        bpWire.Ops     = &wireOps;
        bpWire.Context = &bpMock;
        HciBridgeInit(&bpBridge, &bpWire);
        HciBridgeBindTransport(&bpTransport, &bpBridge);
        HciBridgeSetReady(&bpBridge, 1);
        H4DecoderInit(&dec);

        CHECK(HciBridgeInboundRoom(&bpBridge) == HCI_BRIDGE_ACL_IN_FIFO_DEPTH,
              "Empty queues: room is the smaller queue (%u)", HciBridgeInboundRoom(&bpBridge));

        /* 16 LE Advertising Reports (45 bytes each) and then a Command_Complete: 751 bytes */
        for (i = 0; i < 16u; i++) {
            burst[used++] = 0x04;
            burst[used++] = 0x3E;
            burst[used++] = 43;
            burst[used++] = 0x02;
            memset(burst + used, (int)i, 42);
            used += 42;
        }
        memcpy(burst + used, verCc, sizeof(verCc));
        used += sizeof(verCc);

        waits = BurstFeed(&dec, &bpBridge, &bpTransport, burst, used, HciStreamEvent, out, &popped, &lastLength);
        while (HciTransportPopStream(&bpTransport, HciStreamEvent, out, sizeof(out), &written)) {
            popped++;
            lastLength = written;
        }
        CHECK(used == 751u && bpBridge.Counters.EventsReceived == 17u && bpBridge.Counters.EventsQueued == 17u &&
              bpBridge.Counters.EventsDroppedFifoFull == 0u,
              "One 751-byte read of 17 events: all 17 queued, none dropped");
        CHECK(waits >= 1u, "The reader waited for the host instead of dropping (%lu wait(s))", waits);
        CHECK(popped == 17u && lastLength == sizeof(verCc) - 1u && out[0] == 0x0E && out[3] == 0x01 && out[4] == 0x10,
              "The host receives all 17, the Command_Complete last");

        used = 0;
        for (i = 0; i < 9u; i++) {
            memcpy(burst + used, acl, sizeof(acl));
            burst[used + 9] = (unsigned char)i;
            used += sizeof(acl);
        }
        popped = 0;
        waits = BurstFeed(&dec, &bpBridge, &bpTransport, burst, used, HciStreamAcl, out, &popped, &lastLength);
        while (HciTransportPopStream(&bpTransport, HciStreamAcl, out, sizeof(out), &written)) {
            popped++;
            lastLength = written;
        }
        CHECK(bpBridge.Counters.AclQueued == 9u && bpBridge.Counters.AclDroppedFifoFull == 0u && waits >= 1u,
              "One read of 9 ACL packets: all 9 queued after %lu wait(s), none dropped", waits);
        CHECK(popped == 9u && out[8] == 8u, "The host receives all 9, the last one last");
    }

    /* =========================================================================
     * 8. Exact Diagnostics Counters Verification
     * ========================================================================= */
    printf("\n-- Criterion 8: Exact Diagnostics Counters Agreement --\n");

    CHECK(bridge.Counters.CommandsSubmitted == expCommandsSubmitted,
          "CommandsSubmitted exact match: %lu == %lu", bridge.Counters.CommandsSubmitted, expCommandsSubmitted);
    CHECK(bridge.Counters.CommandsHeld == expCommandsHeld,
          "CommandsHeld exact match: %lu == %lu", bridge.Counters.CommandsHeld, expCommandsHeld);
    CHECK(bridge.Counters.CommandsReplayed == expCommandsReplayed,
          "CommandsReplayed exact match: %lu == %lu", bridge.Counters.CommandsReplayed, expCommandsReplayed);
    CHECK(bridge.Counters.CommandsSentToWire == expCommandsSentToWire,
          "CommandsSentToWire exact match: %lu == %lu", bridge.Counters.CommandsSentToWire, expCommandsSentToWire);
    CHECK(bridge.Counters.CommandsFailedWire == expCommandsFailedWire,
          "CommandsFailedWire exact match: %lu == %lu", bridge.Counters.CommandsFailedWire, expCommandsFailedWire);
    CHECK(bridge.Counters.CommandsDropped == expCommandsDropped,
          "CommandsDropped exact match: %lu == %lu", bridge.Counters.CommandsDropped, expCommandsDropped);

    CHECK(bridge.Counters.AclSubmitted == expAclSubmitted,
          "AclSubmitted exact match: %lu == %lu", bridge.Counters.AclSubmitted, expAclSubmitted);
    CHECK(bridge.Counters.AclSentToWire == expAclSentToWire,
          "AclSentToWire exact match: %lu == %lu", bridge.Counters.AclSentToWire, expAclSentToWire);
    CHECK(bridge.Counters.AclOutDroppedNotReady == expAclOutDroppedNotReady,
          "AclOutDroppedNotReady exact match: %lu == %lu", bridge.Counters.AclOutDroppedNotReady, expAclOutDroppedNotReady);
    CHECK(bridge.Counters.AclDroppedNoCredit == expAclDroppedNoCredit,
          "AclDroppedNoCredit exact match: %lu == %lu", bridge.Counters.AclDroppedNoCredit, expAclDroppedNoCredit);
    CHECK(bridge.Counters.AclDroppedWireFailed == expAclDroppedWireFailed,
          "AclDroppedWireFailed exact match: %lu == %lu", bridge.Counters.AclDroppedWireFailed, expAclDroppedWireFailed);

    CHECK(bridge.Counters.ScoSubmitted == expScoSubmitted,
          "ScoSubmitted exact match: %lu == %lu", bridge.Counters.ScoSubmitted, expScoSubmitted);
    CHECK(bridge.Counters.ScoSentToWire == expScoSentToWire,
          "ScoSentToWire exact match: %lu == %lu", bridge.Counters.ScoSentToWire, expScoSentToWire);
    CHECK(bridge.Counters.ScoOutboundOverwrites == expScoOutboundOverwrites,
          "ScoOutboundOverwrites exact match: %lu == %lu", bridge.Counters.ScoOutboundOverwrites, expScoOutboundOverwrites);
    CHECK(bridge.Counters.ScoInboundOverwrites == expScoInboundOverwrites,
          "ScoInboundOverwrites exact match: %lu == %lu", bridge.Counters.ScoInboundOverwrites, expScoInboundOverwrites);

    CHECK(bridge.Counters.EventsReceived == expEventsReceived,
          "EventsReceived exact match: %lu == %lu", bridge.Counters.EventsReceived, expEventsReceived);
    CHECK(bridge.Counters.EventsQueued == expEventsQueued,
          "EventsQueued exact match: %lu == %lu", bridge.Counters.EventsQueued, expEventsQueued);
    CHECK(bridge.Counters.EventsSuppressedVendor == expEventsSuppressedVendor,
          "EventsSuppressedVendor exact match: %lu == %lu", bridge.Counters.EventsSuppressedVendor, expEventsSuppressedVendor);
    CHECK(bridge.Counters.EventsDroppedNotReady == expEventsDroppedNotReady,
          "EventsDroppedNotReady exact match: %lu == %lu", bridge.Counters.EventsDroppedNotReady, expEventsDroppedNotReady);
    CHECK(bridge.Counters.EventsDroppedFifoFull == expEventsDroppedFifoFull,
          "EventsDroppedFifoFull exact match: %lu == %lu", bridge.Counters.EventsDroppedFifoFull, expEventsDroppedFifoFull);

    CHECK(bridge.Counters.AclReceived == expAclReceived,
          "AclReceived exact match: %lu == %lu", bridge.Counters.AclReceived, expAclReceived);
    CHECK(bridge.Counters.AclQueued == expAclQueued,
          "AclQueued exact match: %lu == %lu", bridge.Counters.AclQueued, expAclQueued);
    CHECK(bridge.Counters.AclInDroppedNotReady == expAclInDroppedNotReady,
          "AclInDroppedNotReady exact match: %lu == %lu", bridge.Counters.AclInDroppedNotReady, expAclInDroppedNotReady);
    CHECK(bridge.Counters.AclDroppedFifoFull == expAclDroppedFifoFull,
          "AclDroppedFifoFull exact match: %lu == %lu", bridge.Counters.AclDroppedFifoFull, expAclDroppedFifoFull);

    CHECK(bridge.Counters.ScoReceived == expScoReceived,
          "ScoReceived exact match: %lu == %lu", bridge.Counters.ScoReceived, expScoReceived);
    CHECK(bridge.Counters.ScoQueued == expScoQueued,
          "ScoQueued exact match: %lu == %lu", bridge.Counters.ScoQueued, expScoQueued);
    CHECK(bridge.Counters.ScoDroppedNotReady == expScoDroppedNotReady,
          "ScoDroppedNotReady exact match: %lu == %lu", bridge.Counters.ScoDroppedNotReady, expScoDroppedNotReady);
    CHECK(bridge.Counters.ScoInboundDiscardedOversize == expScoInboundDiscardedOversize,
          "ScoInboundDiscardedOversize exact match: %lu == %lu", bridge.Counters.ScoInboundDiscardedOversize, expScoInboundDiscardedOversize);

    CHECK(bridge.Counters.Resets == expResets,
          "Resets exact match: %lu == %lu", bridge.Counters.Resets, expResets);

    /* Final Result */
    printf("\n");
    if (g_fail > 0) {
        printf("BRIDGE SELFTEST FAILED: %d assertion(s) failed\n", g_fail);
        return 1;
    }

    printf("BRIDGE SELFTEST PASSED\n");
    return 0;
}
