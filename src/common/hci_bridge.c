/*
 * hci_bridge.c - transport-agnostic bridge between the Windows-facing USB front end
 * and a real Bluetooth controller reached over a wire.
 *
 * See src/include/hci_bridge.h for architecture, locking rules, and policy documentation.
 */

#include <string.h>
#include "../include/hci_bridge.h"

/* Forward declarations for HCI_TRANSPORT_OPS */
static unsigned char HciBridgeSubmitCommand(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length);

static unsigned char HciBridgeSubmitAcl(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length);

static unsigned char HciBridgeSubmitSco(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length);

static unsigned char HciBridgeHasStream(
    const HCI_TRANSPORT *Transport,
    HCI_STREAM Stream);

static unsigned char HciBridgePopStream(
    HCI_TRANSPORT *Transport,
    HCI_STREAM Stream,
    unsigned char *Buffer,
    unsigned long Capacity,
    unsigned long *Written);

static unsigned long HciBridgeLastEventLength(
    const HCI_TRANSPORT *Transport);

static void HciBridgeReset(
    HCI_TRANSPORT *Transport);

static unsigned char HciBridgePeekOrder(
    const HCI_TRANSPORT *Transport,
    HCI_STREAM Stream,
    unsigned long *Order);
static void HciBridgeFlushScoTransport(
    HCI_TRANSPORT *Transport);


/* Static vtable exposed to the front end */
static const HCI_TRANSPORT_OPS g_HciBridgeTransportOps = {
    HciBridgeSubmitCommand,
    HciBridgeSubmitAcl,
    HciBridgeSubmitSco,
    HciBridgeHasStream,
    HciBridgePopStream,
    HciBridgeLastEventLength,
    HciBridgeReset,
    HciBridgePeekOrder,
    HciBridgeFlushScoTransport
};

/* Internal helper: sum of Outstanding over InUse handle entries in one pool (1 = LE, 0 = BR/EDR) */
static unsigned short HciBridgeTotalInFlight(const HCI_BRIDGE *Bridge, unsigned char LePool)
{
    unsigned int i;
    unsigned short inFlight = 0;
    for (i = 0; i < HCI_BRIDGE_MAX_HANDLES; i++) {
        if (Bridge->Handles[i].InUse && Bridge->Handles[i].IsLe == LePool) {
            inFlight = (unsigned short)(inFlight + Bridge->Handles[i].Outstanding);
        }
    }
    return inFlight;
}

/* Internal helper: clamp one pool's free counter against its own ceiling (pool - inFlight) */
static void HciBridgeClampPoolCredits(HCI_BRIDGE *Bridge, unsigned char LePool)
{
    unsigned short inFlight;
    unsigned short ceiling;
    unsigned short pool;
    unsigned short *credits;

    if (LePool) {
        if (Bridge->LeTotalAclBuffers == 0) {
            Bridge->AvailableLeCredits = 0;
            return;
        }
        pool = Bridge->LeTotalAclBuffers;
        credits = &Bridge->AvailableLeCredits;
    } else {
        if (Bridge->TotalAclBuffers == 0) {
            Bridge->AvailableAclCredits = 0;
            return;
        }
        pool = Bridge->TotalAclBuffers;
        credits = &Bridge->AvailableAclCredits;
    }

    inFlight = HciBridgeTotalInFlight(Bridge, LePool);
    ceiling = (pool > inFlight) ? (unsigned short)(pool - inFlight) : 0;

    if (*credits > ceiling) {
        *credits = ceiling;
    }
}

/* Internal helper: clamp both pools against their own ceilings. */
static void HciBridgeClampCredits(HCI_BRIDGE *Bridge)
{
    HciBridgeClampPoolCredits(Bridge, 0);
    HciBridgeClampPoolCredits(Bridge, 1);
}

/* Nothing in flight, every controller buffer free: after HCI_Reset. */
static void HciBridgeRefillCredits(HCI_BRIDGE *Bridge)
{
    memset(Bridge->Handles, 0, sizeof(Bridge->Handles));
    Bridge->AvailableAclCredits = Bridge->TotalAclBuffers;
    Bridge->AvailableLeCredits = Bridge->LeTotalAclBuffers;
}

/* Internal helper: reserve a slot for Handle if not already present; returns slot index or -1 if full */
static int HciBridgeReserveHandleSlot(HCI_BRIDGE *Bridge, unsigned short Handle, unsigned char IsLe)
{
    unsigned int i;
    int firstFree = -1;

    for (i = 0; i < HCI_BRIDGE_MAX_HANDLES; i++) {
        if (Bridge->Handles[i].InUse) {
            if (Bridge->Handles[i].Handle == Handle) {
                /*
                 * Handle numbers are reused across connections. A slot with nothing
                 * outstanding attributes nothing, so retag it to this link's pool;
                 * with packets still in flight keep the pool they were sent on.
                 */
                if (Bridge->Handles[i].Outstanding == 0) {
                    Bridge->Handles[i].IsLe = IsLe;
                }
                return (int)i;
            }
        } else if (firstFree < 0) {
            firstFree = (int)i;
        }
    }

    if (firstFree >= 0) {
        Bridge->Handles[firstFree].InUse = 1;
        Bridge->Handles[firstFree].Handle = Handle;
        Bridge->Handles[firstFree].Outstanding = 0;
        Bridge->Handles[firstFree].IsLe = IsLe;
        return firstFree;
    }

    return -1; /* Table full (all 8 slots occupied by different active handles) */
}

/* Internal helper: record completed ACL packets for Handle */
static void HciBridgeRecordAclCompleted(
    HCI_BRIDGE *Bridge,
    unsigned short Handle,
    unsigned short Completed)
{
    unsigned int i;
    int matched = 0;

    for (i = 0; i < HCI_BRIDGE_MAX_HANDLES; i++) {
        if (Bridge->Handles[i].InUse && Bridge->Handles[i].Handle == Handle) {
            unsigned short actual = (Bridge->Handles[i].Outstanding >= Completed)
                                  ? Completed
                                  : Bridge->Handles[i].Outstanding;
            unsigned char isLe = Bridge->Handles[i].IsLe;
            Bridge->Handles[i].Outstanding = (unsigned short)(Bridge->Handles[i].Outstanding - actual);
            if (isLe) {
                Bridge->AvailableLeCredits = (unsigned short)(Bridge->AvailableLeCredits + actual);
            } else {
                Bridge->AvailableAclCredits = (unsigned short)(Bridge->AvailableAclCredits + actual);
            }
            matched = 1;
            break;
        }
    }

    /* If no handle matched, do NOT credit (ignores stale / rogue reports) */
    if (!matched) {
        return;
    }

    /* Clamp the credited pool against its own ceiling (pool - inFlight). */
    HciBridgeClampCredits(Bridge);
}

/* Internal helper: handle Disconnection_Complete (event 0x05) to restore outstanding credits */
static void HciBridgeRecordDisconnection(HCI_BRIDGE *Bridge, unsigned short Handle)
{
    unsigned int i;
    for (i = 0; i < HCI_BRIDGE_MAX_HANDLES; i++) {
        if (Bridge->Handles[i].InUse && Bridge->Handles[i].Handle == Handle) {
            unsigned short lostCredits = Bridge->Handles[i].Outstanding;
            unsigned char isLe = Bridge->Handles[i].IsLe;
            Bridge->Handles[i].Outstanding = 0;
            Bridge->Handles[i].InUse = 0;
            if (lostCredits > 0) {
                if (isLe) {
                    Bridge->AvailableLeCredits = (unsigned short)(Bridge->AvailableLeCredits + lostCredits);
                } else {
                    Bridge->AvailableAclCredits = (unsigned short)(Bridge->AvailableAclCredits + lostCredits);
                }
            }
            HciBridgeClampCredits(Bridge);
            break;
        }
    }
    /* Remove only queued outbound SCO packets matching the disconnected handle */
    if (Bridge->OutboundScoCount > 0) {
        HCI_BRIDGE_SCO_SLOT kept[HCI_BRIDGE_SCO_OUT_FIFO_DEPTH];
        unsigned int keptCount = 0;

        for (i = 0; i < Bridge->OutboundScoCount; i++) {
            unsigned int idx = (Bridge->OutboundScoHead + i) % HCI_BRIDGE_SCO_OUT_FIFO_DEPTH;
            unsigned short scoHandle = (unsigned short)(((unsigned short)Bridge->OutboundScoFifo[idx].Data[0] |
                                                         ((unsigned short)Bridge->OutboundScoFifo[idx].Data[1] << 8)) & 0x0FFFu);
            if (scoHandle != Handle) {
                kept[keptCount++] = Bridge->OutboundScoFifo[idx];
            }
        }
        if (keptCount != Bridge->OutboundScoCount) {
            for (i = 0; i < keptCount; i++) {
                Bridge->OutboundScoFifo[i] = kept[i];
            }
            Bridge->OutboundScoHead = 0;
            Bridge->OutboundScoTail = (unsigned char)keptCount;
            Bridge->OutboundScoCount = (unsigned char)keptCount;
        }
    }
}

/* ---------------------------------------------------------------- Open links (OPEN LINKS, CLEAN SHUTDOWN) */

static unsigned short HciBridgeHandleAt(const unsigned char *Field)
{
    return (unsigned short)(((unsigned short)Field[0] | ((unsigned short)Field[1] << 8)) & 0x0FFFu);
}

static int HciBridgeFindLink(const HCI_BRIDGE *Bridge, unsigned short Handle)
{
    unsigned int i;
    for (i = 0; i < Bridge->LinkCount; i++) {
        if (Bridge->Links[i].Handle == Handle) {
            return (int)i;
        }
    }
    return -1;
}

/* Address: the Connection_Complete BD_ADDR field for a BR/EDR link, NULL for LE. */
static void HciBridgeAddLink(HCI_BRIDGE *Bridge, unsigned short Handle, const unsigned char *Address)
{
    HCI_BRIDGE_LINK *link;

    if (HciBridgeFindLink(Bridge, Handle) >= 0 || Bridge->LinkCount >= HCI_BRIDGE_MAX_LINKS) {
        return;
    }
    link = &Bridge->Links[Bridge->LinkCount++];
    memset(link, 0, sizeof(*link));
    link->Handle = Handle;
    if (Address) {
        memcpy(link->Address, Address, sizeof(link->Address));
        link->Classic = 1;
    }
}

static void HciBridgeRemoveLink(HCI_BRIDGE *Bridge, unsigned short Handle)
{
    int i = HciBridgeFindLink(Bridge, Handle);

    if (i < 0) {
        return;
    }
    Bridge->LinkCount--;
    Bridge->Links[i] = Bridge->Links[Bridge->LinkCount];
}

/*
 * Marks the link of an ACL packet (either direction) carrying an L2CAP Connection Request for
 * RFCOMM: first fragment, signalling channel (CID 1), command code 0x02, PSM 0x0003.
 */
static void HciBridgeNoteRfcomm(HCI_BRIDGE *Bridge, const unsigned char *Packet, unsigned long Length)
{
    int i;

    if (Length < 16 || ((Packet[1] >> 4) & 0x3u) == 0x1u ||
        Packet[6] != 0x01u || Packet[7] != 0x00u || Packet[8] != 0x02u ||
        Packet[12] != 0x03u || Packet[13] != 0x00u) {
        return;
    }
    i = HciBridgeFindLink(Bridge, HciBridgeHandleAt(Packet));
    if (i >= 0) {
        Bridge->Links[i].Rfcomm = 1;
    }
}

/* A command reached the controller: it takes a credit and awaits its Command_Status/Complete. */
static void HciBridgeNoteHostCommand(HCI_BRIDGE *Bridge, unsigned short Opcode)
{
    if (Bridge->HostCommandsPending < 0xFFu) {
        Bridge->HostCommandsPending++;
    }
    if ((Opcode >> 10) == 0x3Fu && Bridge->HostVendorOpcodeCount < HCI_BRIDGE_MAX_HOST_VENDOR_CMDS) {
        Bridge->HostVendorOpcodes[Bridge->HostVendorOpcodeCount++] = Opcode;
    }
    if (Bridge->CommandCredits > 0u) {
        Bridge->CommandCredits--;
    }
}

/*
 * Sends HCI_Disconnect for the next link not yet asked, unless one is still awaiting its
 * Command_Status, a host command awaits its reply, or the controller has no command credit.
 */
static void HciBridgeSendNextDisconnect(HCI_BRIDGE *Bridge)
{
    unsigned int i;

    if (!Bridge->ShuttingDown || Bridge->DisconnectInFlight || Bridge->HostCommandsPending != 0u ||
        Bridge->CommandCredits == 0u) {
        return;
    }
    for (i = 0; i < Bridge->LinkCount; i++) {
        HCI_BRIDGE_LINK *link = &Bridge->Links[i];
        unsigned char command[6];

        if (link->DisconnectSent) {
            continue;
        }
        link->DisconnectSent = 1;
        command[0] = 0x06;   /* HCI_Disconnect, opcode 0x0406 */
        command[1] = 0x04;
        command[2] = 0x03;
        command[3] = (unsigned char)(link->Handle & 0xFFu);
        command[4] = (unsigned char)(link->Handle >> 8);
        command[5] = Bridge->ShutdownReason;
        if (Bridge->Wire.Ops && Bridge->Wire.Ops->SendCommand &&
            Bridge->Wire.Ops->SendCommand(Bridge->Wire.Context, command, (unsigned long)sizeof(command))) {
            Bridge->DisconnectInFlight = 1;
            Bridge->DisconnectHandle = link->Handle;
            Bridge->CommandCredits--;
            Bridge->Counters.ShutdownDisconnects++;
            return;
        }
        Bridge->Counters.CommandsFailedWire++;
    }
}


/* ---------------------------------------------------------------- Lifecycle */

void HciBridgeInit(
    _Out_ HCI_BRIDGE *Bridge,
    _In_ const HCI_BRIDGE_WIRE *Wire)
{
    if (!Bridge) {
        return;
    }
    memset(Bridge, 0, sizeof(*Bridge));
    if (Wire) {
        Bridge->Wire = *Wire;
    }
    Bridge->CommandCredits = 1;   /* a controller accepts one command before its first reply */
}

void HciBridgeSetReady(
    _Inout_ HCI_BRIDGE *Bridge,
    _In_ unsigned char Ready)
{
    unsigned char wasReady;
    if (!Bridge) {
        return;
    }

    wasReady = Bridge->Ready;
    Bridge->Ready = Ready;

    /* On transition from 0 -> 1, replay held command immediately (hold-then-replay) */
    if (!wasReady && Ready) {
        if (Bridge->HeldCommandPending) {
            Bridge->HeldCommandPending = 0;
            Bridge->Counters.CommandsReplayed++;
            if (Bridge->Wire.Ops && Bridge->Wire.Ops->SendCommand) {
                unsigned char ok = Bridge->Wire.Ops->SendCommand(
                    Bridge->Wire.Context,
                    Bridge->HeldCommand,
                    Bridge->HeldCommandLength);
                if (ok) {
                    Bridge->Counters.CommandsSentToWire++;
                    unsigned short opcode = (unsigned short)((unsigned short)Bridge->HeldCommand[0] |
                                                             ((unsigned short)Bridge->HeldCommand[1] << 8));
                    HciBridgeNoteHostCommand(Bridge, opcode);
                } else {
                    Bridge->Counters.CommandsFailedWire++;
                }
            }
        }
        /* Drain any pending outbound SCO packets */
        HciBridgeDrainOutboundSco(Bridge);
    }
}

void HciBridgeBindTransport(
    _Inout_ HCI_TRANSPORT *Transport,
    _Inout_ HCI_BRIDGE *Bridge)
{
    if (!Transport || !Bridge) {
        return;
    }
    Transport->Ops = &g_HciBridgeTransportOps;
    Transport->Context = Bridge;
    Transport->Backend = HCI_BACKEND_UART;
    Bridge->Transport = Transport;
}

unsigned int HciBridgeBeginShutdown(
    _Inout_ HCI_BRIDGE *Bridge,
    _In_ unsigned char Reason)
{
    if (!Bridge) {
        return 0;
    }
    Bridge->ShuttingDown = 1;
    Bridge->ShutdownReason = Reason;
    HciBridgeSendNextDisconnect(Bridge);
    return Bridge->LinkCount;
}

unsigned int HciBridgeOpenLinks(
    _In_ const HCI_BRIDGE *Bridge)
{
    return Bridge ? Bridge->LinkCount : 0u;
}

unsigned int HciBridgeInboundRoom(
    _In_ const HCI_BRIDGE *Bridge)
{
    unsigned int events;
    unsigned int acl;

    if (!Bridge) {
        return 0u;
    }
    events = HCI_BRIDGE_EVENT_FIFO_DEPTH - Bridge->EventCount;
    acl = HCI_BRIDGE_ACL_IN_FIFO_DEPTH - Bridge->AclInCount;
    return (events < acl) ? events : acl;
}

unsigned int HciBridgeGetLinks(
    _In_ const HCI_BRIDGE *Bridge,
    _Out_writes_(Capacity) HCI_BRIDGE_LINK *Links,
    _In_ unsigned int Capacity)
{
    unsigned int count;

    if (!Bridge || !Links) {
        return 0;
    }
    count = (Bridge->LinkCount < Capacity) ? Bridge->LinkCount : Capacity;
    memcpy(Links, Bridge->Links, count * sizeof(Links[0]));
    return count;
}

/* ------------------------------------------------------ HCI_TRANSPORT_OPS */

static unsigned char HciBridgeSubmitCommand(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length)
{
    HCI_BRIDGE *bridge;
    if (!Transport || !Transport->Context || !Packet) {
        return 0;
    }
    bridge = (HCI_BRIDGE *)Transport->Context;

    /* Validate packet framing: opcode (2) + plen (1) + parameters */
    if (Length < 3 || Length != (3u + (unsigned long)Packet[2]) ||
        Length > HCI_BRIDGE_MAX_COMMAND_SIZE) {
        bridge->Counters.CommandsDropped++;
        return 0;
    }

    bridge->Counters.CommandsSubmitted++;

    /* CLEAN SHUTDOWN: the adapter is removed before any reply; a STALL would make BTHUSB reset it. */
    if (bridge->ShuttingDown) {
        bridge->Counters.CommandsWithheld++;
        return 1;
    }

    /*
     * Readiness gating: hold-then-replay.
     *
     * Returning 0 here makes the front end stall EP0, which during enumeration puts
     * BTHUSB into an endless reset-and-retry cycle with no usable diagnostic. Accepting
     * and holding converts enumeration failure into a command timeout, leaving the device
     * enumerated and diagnostic counters readable.
     *
     * The slot is depth 1 because BTHUSB is stop-and-wait on EP0. If a second command
     * arrives before replay, the earlier held command is overwritten.
     */
    if (!bridge->Ready) {
        if (bridge->HeldCommandPending) {
            bridge->Counters.CommandsHeldOverwritten++;
        }
        memcpy(bridge->HeldCommand, Packet, Length);
        bridge->HeldCommandLength = Length;
        bridge->HeldCommandPending = 1;
        bridge->Counters.CommandsHeld++;
        /* TRUE: BTHUSB's EP0 transfer succeeds and it waits for the reply. */
        return 1;
    }

    /* Ready: forward directly to wire */
    if (bridge->Wire.Ops && bridge->Wire.Ops->SendCommand) {
        unsigned char ok = bridge->Wire.Ops->SendCommand(
            bridge->Wire.Context, Packet, Length);
        if (ok) {
            bridge->Counters.CommandsSentToWire++;
            unsigned short opcode = (unsigned short)((unsigned short)Packet[0] | ((unsigned short)Packet[1] << 8));
            HciBridgeNoteHostCommand(bridge, opcode);
            return 1;
        } else {
            bridge->Counters.CommandsFailedWire++;
            return 0;
        }
    }

    return 0;
}

static unsigned char HciBridgeSubmitAcl(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length)
{
    HCI_BRIDGE *bridge;
    unsigned short handle;
    if (!Transport || !Transport->Context || !Packet) {
        return 0;
    }
    bridge = (HCI_BRIDGE *)Transport->Context;

    /* Validate packet framing: handle/flags (2) + length (2) + payload */
    if (Length < 4 || Length != (4u + (unsigned long)Packet[2] + ((unsigned long)Packet[3] << 8)) ||
        Length > HCI_BRIDGE_MAX_ACL_SIZE) {
        return 0;
    }

    bridge->Counters.AclSubmitted++;
    HciBridgeNoteRfcomm(bridge, Packet, Length);

    if (!bridge->Ready) {
        bridge->Counters.AclOutDroppedNotReady++;
        return 0;
    }

    /* Pool of this link: LE uses the separate pool only when the controller reported one. */
    handle = (unsigned short)(((unsigned short)Packet[0] | ((unsigned short)Packet[1] << 8)) & 0x0FFFu);
    {
        /*
         * Ordering guarantee: the bridge processes a Connection_Complete in HciBridgeOnEvent
         * before the front end can deliver it to Windows, and Windows cannot submit ACL for a
         * handle before it saw its Connection_Complete, so HciBridgeFindLink(handle) is reliable
         * at submit time. A handle with no link entry (should not happen) conservatively uses
         * the BR/EDR pool.
         */
        int link = HciBridgeFindLink(bridge, handle);
        unsigned char isLe = (unsigned char)(link >= 0 && !bridge->Links[link].Classic &&
                                             bridge->LeTotalAclBuffers != 0);
        unsigned short credits = isLe ? bridge->AvailableLeCredits : bridge->AvailableAclCredits;

        if (credits == 0) {
            bridge->Counters.AclDroppedNoCredit++;
            return 0;
        }

        /* Reserve handle slot BEFORE consuming credit or sending to wire */
        {
            int slotIdx = HciBridgeReserveHandleSlot(bridge, handle, isLe);
            if (slotIdx < 0) {
                bridge->Counters.AclDroppedNoHandleSlot++;
                return 0;
            }

            if (bridge->Wire.Ops && bridge->Wire.Ops->SendAcl) {
                unsigned char ok = bridge->Wire.Ops->SendAcl(
                    bridge->Wire.Context, Packet, Length);
                if (ok) {
                    if (isLe) {
                        bridge->AvailableLeCredits--;
                    } else {
                        bridge->AvailableAclCredits--;
                    }
                    bridge->Counters.AclSentToWire++;
                    bridge->Handles[slotIdx].Outstanding++;
                    return 1;
                } else {
                    bridge->Counters.AclDroppedWireFailed++;
                    return 0;
                }
            }
        }
    }
    return 0;
}

static unsigned char HciBridgeSubmitSco(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length)
{
    HCI_BRIDGE *bridge;
    HCI_BRIDGE_SCO_SLOT *slot;
    if (!Transport || !Transport->Context || !Packet) {
        return 0;
    }
    bridge = (HCI_BRIDGE *)Transport->Context;

    /* Validate packet framing: handle/flags (2) + length (1) + payload */
    if (Length < 3 || Length != (3u + (unsigned long)Packet[2]) ||
        Length > HCI_BRIDGE_MAX_SCO_SIZE) {
        return 0;
    }

    bridge->Counters.ScoSubmitted++;

    if (!bridge->Ready) {
        bridge->Counters.ScoOutDroppedNotReady++;
        return 0;
    }

    /* Synchronous FIFO: overwrite-oldest policy */
    if (bridge->OutboundScoCount >= HCI_BRIDGE_SCO_OUT_FIFO_DEPTH) {
        bridge->OutboundScoHead = (bridge->OutboundScoHead + 1) % HCI_BRIDGE_SCO_OUT_FIFO_DEPTH;
        bridge->OutboundScoCount--;
        bridge->Counters.ScoOutboundOverwrites++;
    }

    slot = &bridge->OutboundScoFifo[bridge->OutboundScoTail];
    memcpy(slot->Data, Packet, Length);
    slot->Length = Length;
    bridge->OutboundScoTail = (bridge->OutboundScoTail + 1) % HCI_BRIDGE_SCO_OUT_FIFO_DEPTH;
    bridge->OutboundScoCount++;

    /* Attempt to drain to wire immediately */
    HciBridgeDrainOutboundSco(bridge);
    return 1;
}

void HciBridgeDrainOutboundSco(HCI_BRIDGE *Bridge)
{
    if (!Bridge || !Bridge->Wire.Ops || !Bridge->Wire.Ops->SendSco) {
        return;
    }

    while (Bridge->OutboundScoCount > 0) {
        HCI_BRIDGE_SCO_SLOT *slot = &Bridge->OutboundScoFifo[Bridge->OutboundScoHead];
        unsigned char ok = Bridge->Wire.Ops->SendSco(
            Bridge->Wire.Context, slot->Data, slot->Length);
        if (!ok) {
            /* Wire is busy or buffer is full; leave queued */
            break;
        }
        Bridge->Counters.ScoSentToWire++;
        Bridge->OutboundScoHead = (Bridge->OutboundScoHead + 1) % HCI_BRIDGE_SCO_OUT_FIFO_DEPTH;
        Bridge->OutboundScoCount--;
    }
}

static unsigned char HciBridgeHasStream(
    const HCI_TRANSPORT *Transport,
    HCI_STREAM Stream)
{
    const HCI_BRIDGE *bridge;
    if (!Transport || !Transport->Context) {
        return 0;
    }
    bridge = (const HCI_BRIDGE *)Transport->Context;

    switch (Stream) {
    case HciStreamEvent: return (bridge->EventCount > 0) ? (unsigned char)1 : (unsigned char)0;
    case HciStreamAcl:   return (bridge->AclInCount > 0) ? (unsigned char)1 : (unsigned char)0;
    case HciStreamSco:   return (bridge->ScoInCount > 0) ? (unsigned char)1 : (unsigned char)0;
    default:             return (unsigned char)0;
    }
}

static unsigned char HciBridgePeekOrder(
    const HCI_TRANSPORT *Transport,
    HCI_STREAM Stream,
    unsigned long *Order)
{
    const HCI_BRIDGE *bridge;
    if (!Transport || !Transport->Context || !Order) {
        return 0;
    }
    bridge = (const HCI_BRIDGE *)Transport->Context;

    if (Stream == HciStreamEvent && bridge->EventCount > 0) {
        *Order = bridge->EventFifo[bridge->EventHead].Order;
        return 1;
    }
    if (Stream == HciStreamAcl && bridge->AclInCount > 0) {
        *Order = bridge->AclInFifo[bridge->AclInHead].Order;
        return 1;
    }
    return 0;
}

static unsigned char HciBridgePopStream(
    HCI_TRANSPORT *Transport,
    HCI_STREAM Stream,
    unsigned char *Buffer,
    unsigned long Capacity,
    unsigned long *Written)
{
    HCI_BRIDGE *bridge;
    if (!Written) {
        return 0;
    }
    *Written = 0;
    if (!Transport || !Transport->Context || !Buffer) {
        return 0;
    }
    bridge = (HCI_BRIDGE *)Transport->Context;

    switch (Stream) {
    case HciStreamEvent: {
        HCI_BRIDGE_EVENT_SLOT *slot;
        if (bridge->EventCount == 0) {
            return 0;
        }
        slot = &bridge->EventFifo[bridge->EventHead];
        if (slot->Length > Capacity) {
            /* Too small: never partially fill, leave packet queued */
            return 0;
        }
        memcpy(Buffer, slot->Data, slot->Length);
        *Written = slot->Length;
        bridge->EventHead = (bridge->EventHead + 1) % HCI_BRIDGE_EVENT_FIFO_DEPTH;
        bridge->EventCount--;
        return 1;
    }

    case HciStreamAcl: {
        HCI_BRIDGE_ACL_SLOT *slot;
        if (bridge->AclInCount == 0) {
            return 0;
        }
        slot = &bridge->AclInFifo[bridge->AclInHead];
        if (slot->Length > Capacity) {
            return 0;
        }
        memcpy(Buffer, slot->Data, slot->Length);
        *Written = slot->Length;
        bridge->AclInHead = (bridge->AclInHead + 1) % HCI_BRIDGE_ACL_IN_FIFO_DEPTH;
        bridge->AclInCount--;
        return 1;
    }

    case HciStreamSco: {
        HCI_BRIDGE_SCO_SLOT *slot;
        if (bridge->ScoInCount == 0) {
            return 0;
        }
        slot = &bridge->ScoInFifo[bridge->ScoInHead];
        if (slot->Length > Capacity) {
            /*
             * Exception for HciStreamSco per hci_transport.h:
             * Synchronous packet that does not fit Capacity is DISCARDED, not left queued,
             * and PopStream returns FALSE. Isoch IN capacity is fixed by whichever SCO
             * alternate setting Windows selected, so an oversize packet left queued would
             * be re-read forever and wedge the synchronous stream. Stale audio is worthless.
             */
            bridge->ScoInHead = (bridge->ScoInHead + 1) % HCI_BRIDGE_SCO_FIFO_DEPTH;
            bridge->ScoInCount--;
            bridge->Counters.ScoInboundDiscardedOversize++;
            return 0;
        }
        memcpy(Buffer, slot->Data, slot->Length);
        *Written = slot->Length;
        bridge->ScoInHead = (bridge->ScoInHead + 1) % HCI_BRIDGE_SCO_FIFO_DEPTH;
        bridge->ScoInCount--;
        return 1;
    }

    default:
        return 0;
    }
}

/*
 * hci_transport.h defines LastEventLength only for HCI_BACKEND_STUB. This bridge is the
 * asynchronous UART backend: events arrive from the wire on a DPC, so any remembered length is
 * already stale by the time the front end reads it. A breadcrumb that lies is worse than one
 * that is absent, and breadcrumbs are the only debugging channel this hardware has. The front
 * end gets the true length from PopStream's *Written.
 */
static unsigned long HciBridgeLastEventLength(const HCI_TRANSPORT *Transport)
{
    UNREFERENCED_PARAMETER(Transport);
    return 0;
}

static void HciBridgeReset(HCI_TRANSPORT *Transport)
{
    HCI_BRIDGE *bridge;
    if (!Transport || !Transport->Context) {
        return;
    }
    bridge = (HCI_BRIDGE *)Transport->Context;

    /* Drop all queued packets and partial state */
    bridge->EventHead = bridge->EventTail = bridge->EventCount = 0;
    bridge->AclInHead = bridge->AclInTail = bridge->AclInCount = 0;
    bridge->ScoInHead = bridge->ScoInTail = bridge->ScoInCount = 0;
    bridge->OutboundScoHead = bridge->OutboundScoTail = bridge->OutboundScoCount = 0;

    bridge->HeldCommandPending = 0;
    bridge->HeldCommandLength = 0;

    bridge->HostCommandsPending = 0;
    bridge->HostVendorOpcodeCount = 0;
    HciBridgeRefillCredits(bridge);

    bridge->Counters.Resets++;
}

void HciBridgeFlushSco(HCI_BRIDGE *Bridge)
{
    if (!Bridge) {
        return;
    }
    Bridge->OutboundScoHead = 0;
    Bridge->OutboundScoTail = 0;
    Bridge->OutboundScoCount = 0;
    Bridge->ScoInHead = 0;
    Bridge->ScoInTail = 0;
    Bridge->ScoInCount = 0;
}

static void HciBridgeFlushScoTransport(HCI_TRANSPORT *Transport)
{
    if (Transport != NULL && Transport->Context != NULL) {
        HciBridgeFlushSco((HCI_BRIDGE *)Transport->Context);
    }
}

/* ------------------------------------------------ Inbound Wire Feeds */

unsigned char HciBridgeOnEvent(
    _Inout_ HCI_BRIDGE *Bridge,
    _In_reads_bytes_(Length) const unsigned char *Packet,
    _In_ unsigned long Length)
{
    HCI_BRIDGE_EVENT_SLOT *slot;
    if (!Bridge || !Packet) {
        return 0;
    }

    /* Validate packet framing: event code (1) + plen (1) + payload */
    if (Length < 2 || Length != (2u + (unsigned long)Packet[1]) ||
        Length > HCI_BRIDGE_MAX_EVENT_SIZE) {
        return 0;
    }

    Bridge->Counters.EventsReceived++;

    /* Boot-chatter suppression: unconditionally drop vendor events (0xFF) */
    if (Packet[0] == 0xFFu) {
        Bridge->Counters.EventsSuppressedVendor++;
        return 0;
    }

    /* Snoop Command_Complete for HCI_Reset, Read_Buffer_Size and LE_Read_Buffer_Size */
    if (Packet[0] == 0x0Eu && Length >= 6) {
        unsigned short opcode = (unsigned short)((unsigned short)Packet[3] | ((unsigned short)Packet[4] << 8));
        unsigned char status = Packet[5];
        if (opcode == 0x0C03u && status == 0x00u) {
            HciBridgeRefillCredits(Bridge);
            Bridge->LinkCount = 0;   /* a reset ends every connection without Disconnection_Complete */
            Bridge->HostCommandsPending = 0;   /* and aborts every command still awaiting a reply */
            Bridge->HostVendorOpcodeCount = 0;
        } else if (opcode == 0x1005u && status == 0x00u && Length >= 13) {
            unsigned short totalAcl = (unsigned short)((unsigned short)Packet[9] | ((unsigned short)Packet[10] << 8));
            unsigned short wasTotal = Bridge->TotalAclBuffers;
            Bridge->TotalAclBuffers = totalAcl;
            if (wasTotal == 0) {
                HciBridgeRefillCredits(Bridge);
            }
            HciBridgeClampCredits(Bridge);
        } else if (opcode == 0x2002u && status == 0x00u && Length >= 9) {
            unsigned short leTotal = Packet[8];
            unsigned short wasLe = Bridge->LeTotalAclBuffers;
            Bridge->LeTotalAclBuffers = leTotal;
            if (wasLe == 0 && leTotal != 0) {
                Bridge->AvailableLeCredits = leTotal;
            }
            HciBridgeClampCredits(Bridge);
        }
    }

    /* Snoop for Number_Of_Completed_Packets (event 0x13) */
    if (Packet[0] == 0x13u && Length >= 3) {
        unsigned char numHandles = Packet[2];
        if (Length >= (unsigned long)(3 + numHandles * 4)) {
            unsigned char i;
            for (i = 0; i < numHandles; i++) {
                unsigned long off = 3 + (unsigned long)i * 4;
                unsigned short handle = (unsigned short)(((unsigned short)Packet[off] | ((unsigned short)Packet[off + 1] << 8)) & 0x0FFFu);
                unsigned short completed = (unsigned short)((unsigned short)Packet[off + 2] | ((unsigned short)Packet[off + 3] << 8));
                HciBridgeRecordAclCompleted(Bridge, handle, completed);
            }
        }
    }
    /* Snoop for Disconnection_Complete (event 0x05) */
    if (Packet[0] == 0x05u && Length >= 6) {
        unsigned char status = Packet[2];
        if (status == 0x00u) {
            unsigned short handle = (unsigned short)(((unsigned short)Packet[3] | ((unsigned short)Packet[4] << 8)) & 0x0FFFu);
            HciBridgeRecordDisconnection(Bridge, handle);
        }
    }
    /* Open ACL links: Connection_Complete (link type ACL), LE (Enhanced) Connection Complete */
    if (Packet[0] == 0x03u && Length >= 13 && Packet[2] == 0x00u && Packet[11] == 0x01u) {
        HciBridgeAddLink(Bridge, HciBridgeHandleAt(Packet + 3), Packet + 5);
    } else if (Packet[0] == 0x3Eu && Length >= 6 && Packet[3] == 0x00u &&
               (Packet[2] == 0x01u || Packet[2] == 0x0Au || Packet[2] == 0x29u)) {
        HciBridgeAddLink(Bridge, HciBridgeHandleAt(Packet + 4), NULL);
    } else if (Packet[0] == 0x05u && Length >= 6 && Packet[2] == 0x00u) {
        HciBridgeRemoveLink(Bridge, HciBridgeHandleAt(Packet + 3));
    }
    /*
     * Command flow control (CLEAN SHUTDOWN): every Command_Complete/Status carries the controller's
     * command credits and, unless its opcode is 0 (credits only), answers a command in flight. A
     * Command_Status for HCI_Disconnect is the bridge's own only if no host command was in flight.
     */
    if ((Packet[0] == 0x0Eu && Length >= 5) || (Packet[0] == 0x0Fu && Length >= 6)) {
        const unsigned char *reply = Packet + ((Packet[0] == 0x0Eu) ? 2 : 3);   /* ncmd, opcode */
        unsigned short opcode = (unsigned short)((unsigned short)reply[1] | ((unsigned short)reply[2] << 8));
        unsigned char ours = (unsigned char)(Packet[0] == 0x0Fu && opcode == 0x0406u &&
                                             Bridge->DisconnectInFlight && Bridge->HostCommandsPending == 0u);

        Bridge->CommandCredits = reply[0];

        /* Boot-chatter suppression: vendor opcodes (OGF 0x3F, e.g. 0xFC00, 0xFC17, 0xFC48) are
         * dropped unless the host sent a vendor command with this exact opcode. A late bring-up
         * completion must neither retire a host command nor reach the host. A matched completion
         * falls through to the ordinary accounting below and is forwarded. */
        if ((opcode >> 10) == 0x3Fu) {
            unsigned char match = 0;
            for (unsigned char v = 0; v < Bridge->HostVendorOpcodeCount; v++) {
                if (Bridge->HostVendorOpcodes[v] == opcode) {
                    for (unsigned char k = v; k + 1u < Bridge->HostVendorOpcodeCount; k++) {
                        Bridge->HostVendorOpcodes[k] = Bridge->HostVendorOpcodes[k + 1u];
                    }
                    Bridge->HostVendorOpcodeCount--;
                    match = 1;
                    break;
                }
            }
            if (!match) {
                Bridge->Counters.EventsSuppressedVendor++;
                return 0;
            }
        }
        if (ours) {
            Bridge->DisconnectInFlight = 0;
            if (Packet[2] != 0x00u) {
                Bridge->Counters.ShutdownDisconnectsRefused++;
                HciBridgeRemoveLink(Bridge, Bridge->DisconnectHandle);
            }
        } else if (opcode != 0x0000u && Bridge->HostCommandsPending > 0u) {
            Bridge->HostCommandsPending--;
        }
        HciBridgeSendNextDisconnect(Bridge);
        if (ours) {
            Bridge->Counters.EventsSuppressedShutdown++;
            return 0;
        }
    }
    /* CLEAN SHUTDOWN: the host sees neither the injected disconnects' replies nor their results. */
    if (Bridge->ShuttingDown && Packet[0] == 0x05u) {
        Bridge->Counters.EventsSuppressedShutdown++;
        return 0;
    }
    /*
     * Boot-chatter suppression:
     * Unconditionally drop Command_Complete (0x0E) or Command_Status (0x0F)
     * with vendor opcodes (OGF 0x3F, e.g. 0xFC00, 0xFC17, 0xFC48).
     * Late vendor completions (such as the 0xFC48 baud-switch reply) arrive after the
     * host has progressed and must be dropped so BTHPORT does not see a completion for
     * an unissued command (compare upstream Linux hci_qca.c:
     * https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/hci_qca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137).
     */


    if (!Bridge->Ready) {
        Bridge->Counters.EventsDroppedNotReady++;
        return 0;
    }

    if (Bridge->EventCount >= HCI_BRIDGE_EVENT_FIFO_DEPTH) {
        Bridge->Counters.EventsDroppedFifoFull++;
        return 0;
    }

    slot = &Bridge->EventFifo[Bridge->EventTail];
    memcpy(slot->Data, Packet, Length);
    slot->Length = Length;
    slot->Order = Bridge->NextOrder++;
    Bridge->EventTail = (Bridge->EventTail + 1) % HCI_BRIDGE_EVENT_FIFO_DEPTH;
    Bridge->EventCount++;
    Bridge->Counters.EventsQueued++;
    return 1;
}

unsigned char HciBridgeOnAcl(
    _Inout_ HCI_BRIDGE *Bridge,
    _In_reads_bytes_(Length) const unsigned char *Packet,
    _In_ unsigned long Length)
{
    HCI_BRIDGE_ACL_SLOT *slot;
    if (!Bridge || !Packet) {
        return 0;
    }

    /* Validate packet framing: handle/flags (2) + len (2) + payload */
    if (Length < 4 || Length != (4u + (unsigned long)Packet[2] + ((unsigned long)Packet[3] << 8)) ||
        Length > HCI_BRIDGE_MAX_ACL_SIZE) {
        return 0;
    }

    Bridge->Counters.AclReceived++;
    HciBridgeNoteRfcomm(Bridge, Packet, Length);

    if (!Bridge->Ready) {
        Bridge->Counters.AclInDroppedNotReady++;
        return 0;
    }

    if (Bridge->AclInCount >= HCI_BRIDGE_ACL_IN_FIFO_DEPTH) {
        Bridge->Counters.AclDroppedFifoFull++;
        return 0;
    }

    slot = &Bridge->AclInFifo[Bridge->AclInTail];
    memcpy(slot->Data, Packet, Length);
    slot->Length = Length;
    slot->Order = Bridge->NextOrder++;
    Bridge->AclInTail = (Bridge->AclInTail + 1) % HCI_BRIDGE_ACL_IN_FIFO_DEPTH;
    Bridge->AclInCount++;
    Bridge->Counters.AclQueued++;
    return 1;
}

unsigned char HciBridgeOnSco(
    _Inout_ HCI_BRIDGE *Bridge,
    _In_reads_bytes_(Length) const unsigned char *Packet,
    _In_ unsigned long Length)
{
    HCI_BRIDGE_SCO_SLOT *slot;
    if (!Bridge || !Packet) {
        return 0;
    }

    /* Validate packet framing: handle/flags (2) + len (1) + payload */
    if (Length < 3 || Length != (3u + (unsigned long)Packet[2]) ||
        Length > HCI_BRIDGE_MAX_SCO_SIZE) {
        return 0;
    }

    Bridge->Counters.ScoReceived++;

    if (!Bridge->Ready) {
        Bridge->Counters.ScoDroppedNotReady++;
        return 0;
    }

    /* Synchronous FIFO: overwrite-oldest policy */
    if (Bridge->ScoInCount >= HCI_BRIDGE_SCO_FIFO_DEPTH) {
        Bridge->ScoInHead = (Bridge->ScoInHead + 1) % HCI_BRIDGE_SCO_FIFO_DEPTH;
        Bridge->ScoInCount--;
        Bridge->Counters.ScoInboundOverwrites++;
    }

    slot = &Bridge->ScoInFifo[Bridge->ScoInTail];
    memcpy(slot->Data, Packet, Length);
    slot->Length = Length;
    Bridge->ScoInTail = (Bridge->ScoInTail + 1) % HCI_BRIDGE_SCO_FIFO_DEPTH;
    Bridge->ScoInCount++;
    Bridge->Counters.ScoQueued++;
    return 1;
}

unsigned char HciBridgeOnInbound(
    _Inout_ HCI_BRIDGE *Bridge,
    _In_ HCI_STREAM Stream,
    _In_reads_bytes_(Length) const unsigned char *Packet,
    _In_ unsigned long Length)
{
    switch (Stream) {
    case HciStreamEvent:
        return HciBridgeOnEvent(Bridge, Packet, Length);
    case HciStreamAcl:
        return HciBridgeOnAcl(Bridge, Packet, Length);
    case HciStreamSco:
        return HciBridgeOnSco(Bridge, Packet, Length);
    default:
        return 0;
    }
}
