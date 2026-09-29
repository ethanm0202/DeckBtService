/*
 * usbip_device.c - the DeckBtService Bluetooth radio behind a USB/IP connection.
 * See usbip_device.h for the contract.
 */

#include <string.h>

#include "usbip_device.h"
#include "usbip_proto.h"
#include "../include/usb_descriptors.h"

#define EP_CONTROL   0u
#define EP_EVENT     1u   /* 0x81 interrupt IN  */
#define EP_ACL       2u   /* 0x02 / 0x82 bulk   */
#define EP_SCO       3u   /* 0x03 / 0x83 isoch  */

#define BM_DIR_IN            0x80u
#define BM_TYPE_MASK         0x60u
#define BM_RECIPIENT_MASK    0x1Fu
#define BM_HCI_COMMAND       0x20u   /* host-to-device, class, device */
#define BM_PORT_FEATURE      0x23u   /* host-to-device, class, other (hub port) */

#define REQ_GET_STATUS        0x00u
#define REQ_CLEAR_FEATURE     0x01u
#define REQ_SET_FEATURE       0x03u
#define REQ_GET_DESCRIPTOR    0x06u
#define REQ_GET_CONFIGURATION 0x08u
#define REQ_SET_CONFIGURATION 0x09u
#define REQ_GET_INTERFACE     0x0Au
#define REQ_SET_INTERFACE     0x0Bu

#define DESC_DEVICE           0x01u
#define DESC_CONFIGURATION    0x02u
#define DESC_STRING           0x03u

#define FEATURE_DEVICE_REMOTE_WAKEUP 1u
#define FEATURE_PORT_RESET           4u

/* Upper bound on one received transfer; BTHUSB never comes close. */
#define MAX_TRANSFER_LENGTH   (1024ul * 1024ul)

/* ------------------------------------------------------------------ small helpers */

static unsigned long
MinUl(unsigned long a, unsigned long b)
{
    return (a < b) ? a : b;
}

static int
Transmit(USBIP_DEVICE *Device, unsigned long Length)
{
    if (Device->Broken) {
        return 0;
    }
    if (!Device->Send(Device->SendContext, Device->Tx, Length)) {
        Device->Broken = 1;
        return 0;
    }
    return 1;
}

/*
 * RET_SUBMIT: header, then Data (IN payload, possibly empty), then isochronous descriptors.
 * Actual is the transferred byte count reported to the client; for OUT it has no payload.
 */
static int
SendRetSubmit(USBIP_DEVICE *Device, unsigned long Seqnum, long Status, unsigned long Actual,
              const unsigned char *Data, unsigned long DataLength, long StartFrame,
              unsigned long Packets, const unsigned char *Iso, unsigned long IsoLength)
{
    unsigned char *tx = Device->Tx;

    memset(tx, 0, USBIP_URB_HEADER_SIZE);
    UsbipPut32(tx + USBIP_HDR_COMMAND, USBIP_RET_SUBMIT);
    UsbipPut32(tx + USBIP_HDR_SEQNUM, Seqnum);
    UsbipPut32(tx + USBIP_HDR_STATUS, (unsigned long)Status);
    UsbipPut32(tx + USBIP_HDR_ACTUAL, Actual);
    UsbipPut32(tx + USBIP_HDR_RET_START_FRAME, (unsigned long)StartFrame);
    UsbipPut32(tx + USBIP_HDR_RET_PACKETS, Packets);
    UsbipPut32(tx + USBIP_HDR_ERROR_COUNT, 0);
    if (DataLength != 0) {
        memcpy(tx + USBIP_URB_HEADER_SIZE, Data, DataLength);
    }
    if (IsoLength != 0) {
        memcpy(tx + USBIP_URB_HEADER_SIZE + DataLength, Iso, IsoLength);
    }
    return Transmit(Device, USBIP_URB_HEADER_SIZE + DataLength + IsoLength);
}

static int
SendSimple(USBIP_DEVICE *Device, unsigned long Seqnum, long Status, unsigned long Actual,
           const unsigned char *Data, unsigned long DataLength)
{
    return SendRetSubmit(Device, Seqnum, Status, Actual, Data, DataLength, 0,
                         USBIP_NON_ISO_PACKETS, NULL, 0);
}

static int
SendRetUnlink(USBIP_DEVICE *Device, unsigned long Seqnum, long Status)
{
    unsigned char *tx = Device->Tx;

    memset(tx, 0, USBIP_URB_HEADER_SIZE);
    UsbipPut32(tx + USBIP_HDR_COMMAND, USBIP_RET_UNLINK);
    UsbipPut32(tx + USBIP_HDR_SEQNUM, Seqnum);
    UsbipPut32(tx + USBIP_HDR_UNLINK_STATUS, (unsigned long)Status);
    return Transmit(Device, USBIP_URB_HEADER_SIZE);
}

/* ------------------------------------------------------------------ parked IN reads */

static int
ParkPush(USBIP_PARK_QUEUE *Queue, unsigned long Seqnum, unsigned long Capacity)
{
    if (Queue->Count >= USBIP_DEV_MAX_PARKED) {
        return 0;
    }
    Queue->Items[Queue->Count].Seqnum = Seqnum;
    Queue->Items[Queue->Count].Capacity = Capacity;
    Queue->Count++;
    return 1;
}

static void
ParkRemoveAt(USBIP_PARK_QUEUE *Queue, unsigned int Index)
{
    memmove(&Queue->Items[Index], &Queue->Items[Index + 1],
            (Queue->Count - Index - 1u) * sizeof(Queue->Items[0]));
    Queue->Count--;
}

static int
ParkRemove(USBIP_PARK_QUEUE *Queue, unsigned long Seqnum)
{
    for (unsigned int i = 0; i < Queue->Count; i++) {
        if (Queue->Items[i].Seqnum == Seqnum) {
            ParkRemoveAt(Queue, i);
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ sent event / ACL replies */

static USBIP_SENT_IN *
SentAt(USBIP_SENT_RING *Ring, unsigned int Index)
{
    return &Ring->Items[(Ring->Head + Index) % USBIP_DEV_SENT_HISTORY];
}

static void
SentClear(USBIP_SENT_RING *Ring)
{
    Ring->Head = 0;
    Ring->Count = 0;
    Ring->Lost = 0;
}

static void
SentRecord(USBIP_SENT_RING *Ring, unsigned long Seqnum, const unsigned char *Data, unsigned long Length,
           unsigned long Order, int Ordered)
{
    USBIP_SENT_IN *entry;

    if (Length == 0 || Length > USBIP_DEV_SENT_BYTES) {
        return;
    }
    if (Ring->Count == USBIP_DEV_SENT_HISTORY) {
        if (SentAt(Ring, 0)->Lost) {
            Ring->Lost--;
        }
        Ring->Head = (Ring->Head + 1u) % USBIP_DEV_SENT_HISTORY;
        Ring->Count--;
    }
    entry = SentAt(Ring, Ring->Count);
    entry->Seqnum = Seqnum;
    entry->Length = Length;
    entry->Order = Order;
    entry->Ordered = Ordered;
    entry->Lost = 0;
    memcpy(entry->Data, Data, Length);
    Ring->Count++;
}

static int
SentMarkLost(USBIP_SENT_RING *Ring, unsigned long Seqnum)
{
    for (unsigned int i = 0; i < Ring->Count; i++) {
        USBIP_SENT_IN *entry = SentAt(Ring, i);

        if (entry->Seqnum == Seqnum && !entry->Lost) {
            entry->Lost = 1;
            Ring->Lost++;
            return 1;
        }
    }
    return 0;
}

/* The oldest lost packet. Replays retain the packet's original position in this ring. */
static USBIP_SENT_IN *
SentOldestLost(USBIP_SENT_RING *Ring)
{
    for (unsigned int i = 0; Ring->Lost != 0 && i < Ring->Count; i++) {
        if (SentAt(Ring, i)->Lost) {
            return SentAt(Ring, i);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ parked isoch transfers */

/* Index 0 is the oldest parked transfer; Index == Count is the next free slot. */
static USBIP_ISO_URB *
IsoAt(USBIP_ISO_QUEUE *Queue, unsigned int Index)
{
    return &Queue->Items[(Queue->Head + Index) % USBIP_DEV_MAX_ISO_URBS];
}

static void
IsoPopFront(USBIP_ISO_QUEUE *Queue)
{
    Queue->Head = (Queue->Head + 1u) % USBIP_DEV_MAX_ISO_URBS;
    Queue->Count--;
}

/* Unlink of a transfer that is not the oldest: rare, so later entries are shifted down. */
static int
IsoRemove(USBIP_ISO_QUEUE *Queue, unsigned long Seqnum)
{
    for (unsigned int i = 0; i < Queue->Count; i++) {
        if (IsoAt(Queue, i)->Seqnum == Seqnum) {
            for (unsigned int j = i; j + 1u < Queue->Count; j++) {
                *IsoAt(Queue, j) = *IsoAt(Queue, j + 1u);
            }
            Queue->Count--;
            return 1;
        }
    }
    return 0;
}

/* Isochronous descriptors for a reply: requested geometry, per-packet actual length. */
static unsigned long
WriteIsoDescs(USBIP_DEVICE *Device, const USBIP_ISO_URB *Urb, const unsigned long *Actual)
{
    unsigned char *d = Device->IsoDesc;

    for (unsigned long i = 0; i < Urb->Packets; i++, d += USBIP_ISO_DESC_SIZE) {
        UsbipPut32(d + 0, Urb->Offset[i]);
        UsbipPut32(d + 4, Urb->Length[i]);
        UsbipPut32(d + 8, (Actual != NULL) ? Actual[i] : 0u);
        UsbipPut32(d + 12, 0);
    }
    return Urb->Packets * USBIP_ISO_DESC_SIZE;
}

/* ------------------------------------------------------------------ SCO */

static void
ScoEmit(void *Context, const unsigned char *Packet, unsigned long Length)
{
    USBIP_DEVICE *device = (USBIP_DEVICE *)Context;

    if (HciTransportSubmitSco(device->Transport, Packet, Length)) {
        device->Stats.ScoOutHci++;
    } else {
        device->Stats.ScoOutRejected++;
    }
}

static unsigned char
ScoPull(void *Context, unsigned char *Packet, unsigned long Capacity, unsigned long *Written)
{
    USBIP_DEVICE *device = (USBIP_DEVICE *)Context;

    return HciTransportPopStream(device->Transport, HciStreamSco, Packet, Capacity, Written);
}

static void
ScoResetStreams(USBIP_DEVICE *Device)
{
    ScoUsbOutReset(&Device->ScoOut);
    ScoUsbInReset(&Device->ScoIn);
    Device->ScoOutClock.NextFree = 0;
    Device->ScoInClock.NextFree = 0;
}

/* Ends the voice stream: parked transfers are answered as cancelled, clocks and framers restart. */
static void
ScoFlush(USBIP_DEVICE *Device)
{
    USBIP_ISO_QUEUE *queues[2] = { &Device->ScoOutQ, &Device->ScoInQ };

    for (unsigned int q = 0; q < 2u; q++) {
        while (queues[q]->Count != 0) {
            USBIP_ISO_URB *urb = IsoAt(queues[q], 0);
            unsigned long isoLength = WriteIsoDescs(Device, urb, NULL);

            (void)SendRetSubmit(Device, urb->Seqnum, USBIP_ECONNRESET, 0, NULL, 0, urb->StartFrame,
                                urb->Packets, Device->IsoDesc, isoLength);
            IsoPopFront(queues[q]);
        }
    }
    ScoResetStreams(Device);
}

static void
ScoCompleteIn(USBIP_DEVICE *Device, const USBIP_ISO_URB *Urb)
{
    unsigned long actual[USBIP_DEV_MAX_ISO_PACKETS];
    unsigned long total = 0;
    unsigned long isoLength;

    for (unsigned long i = 0; i < Urb->Packets; i++) {
        /* ScoSubmit accepted no packet longer than Urb->MaxPacket. */
        actual[i] = (Urb->Length[i] == 0) ? 0u
                  : ScoUsbInFill(&Device->ScoIn, Urb->MaxPacket, Device->Scratch + total, Urb->Length[i],
                                 ScoPull, Device);
        total += actual[i];
    }
    Device->Stats.ScoInBytes += total;
    isoLength = WriteIsoDescs(Device, Urb, actual);
    (void)SendRetSubmit(Device, Urb->Seqnum, 0, total, Device->Scratch, total, Urb->StartFrame,
                        Urb->Packets, Device->IsoDesc, isoLength);
}

static void
ScoCompleteOut(USBIP_DEVICE *Device, const USBIP_ISO_URB *Urb)
{
    unsigned long isoLength = WriteIsoDescs(Device, Urb, Urb->Length);

    (void)SendRetSubmit(Device, Urb->Seqnum, 0, Urb->Bytes, NULL, 0, Urb->StartFrame,
                        Urb->Packets, Device->IsoDesc, isoLength);
}

static void
NoteLate(USBIP_DEVICE *Device, unsigned long long Due, unsigned long long Now)
{
    unsigned long long lateUs = (Now > Due) ? (Now - Due) / 10u : 0u;

    if (lateUs > 0xFFFFFFFFull) {
        lateUs = 0xFFFFFFFFull;
    }
    if (lateUs > Device->Stats.MaxLateUs) {
        Device->Stats.MaxLateUs = (unsigned long)lateUs;
    }
}

/*
 * A transfer the device cannot take: failed with no isochronous descriptors (number_of_packets 0).
 * The client reads exactly number_of_packets descriptors after the header, so announcing the
 * request's count without sending them would desynchronise the connection.
 */
static void
ScoReject(USBIP_DEVICE *Device, unsigned long Seqnum)
{
    Device->Stats.ScoRejectedUrbs++;
    (void)SendRetSubmit(Device, Seqnum, USBIP_EINVAL, 0, NULL, 0, 0, 0, NULL, 0);
}

/*
 * A SCO transfer: validated, given its slot on the endpoint's frame clock and parked until that
 * slot ends, exactly as DeckBtScoAccept does. OUT bytes are queued now, tagged per packet. With
 * every slot taken the transfer is completed at once instead (IN with every packet empty), as the
 * kernel driver did when it could not park one: BTHUSB's stream is never held hostage.
 */
static void
ScoSubmit(USBIP_DEVICE *Device, unsigned long Seqnum, int In, unsigned long Length,
          unsigned long Packets, const unsigned char *Data, const unsigned char *Iso,
          unsigned long long Now)
{
    USBIP_ISO_QUEUE *queue = In ? &Device->ScoInQ : &Device->ScoOutQ;
    unsigned long maxPacket = DeckBtScoAltPacketSize[Device->ScoAlt];
    int parked = (queue->Count < USBIP_DEV_MAX_ISO_URBS);
    USBIP_ISO_URB *urb = parked ? IsoAt(queue, queue->Count) : &Device->IsoOverflow;

    if (Packets == 0 || Packets > USBIP_DEV_MAX_ISO_PACKETS || maxPacket == 0) {
        ScoReject(Device, Seqnum);
        return;
    }
    urb->Seqnum = Seqnum;
    urb->Packets = Packets;
    urb->Bytes = 0;
    urb->MaxPacket = maxPacket;
    urb->StartFrame = (long)((Now / SCO_USB_FRAME_100NS) & 0x7FFFFFFFull);
    /*
     * In order and apart, so the packets never total more than the transfer (or send bytes twice),
     * and none longer than the setting's wMaxPacketSize, the most one 1 ms frame carries.
     */
    for (unsigned long i = 0, end = 0; i < Packets; i++) {
        unsigned long offset = UsbipGet32(Iso + i * USBIP_ISO_DESC_SIZE + 0);
        unsigned long length = UsbipGet32(Iso + i * USBIP_ISO_DESC_SIZE + 4);

        if (offset < end || offset > Length || length > Length - offset || length > maxPacket) {
            ScoReject(Device, Seqnum);
            return;
        }
        urb->Offset[i] = offset;
        urb->Length[i] = length;
        end = offset + length;
    }

    if (!In) {
        unsigned long long span = 0;
        unsigned long long at;

        for (unsigned long i = 0; i < Packets; i++) {
            span += ScoUsbOutPacketSpan(maxPacket, urb->Length[i]);
            urb->Bytes += urb->Length[i];
        }
        urb->Due = ScoUsbSchedule(&Device->ScoOutClock, Now, span);
        at = urb->Due - span;
        for (unsigned long i = 0; i < Packets; i++) {
            at += ScoUsbOutPacketSpan(maxPacket, urb->Length[i]);
            ScoUsbOutPush(&Device->ScoOut, at, Data + urb->Offset[i], urb->Length[i]);
        }
        Device->Stats.ScoOutUrbs++;
        Device->Stats.ScoOutBytes += urb->Bytes;
    } else {
        urb->Due = ScoUsbSchedule(&Device->ScoInClock, Now, (unsigned long long)Packets * SCO_USB_FRAME_100NS);
        Device->Stats.ScoInUrbs++;
    }

    if (parked) {
        queue->Count++;
        return;
    }
    Device->Stats.ScoUnpacedUrbs++;
    if (In) {
        unsigned long isoLength = WriteIsoDescs(Device, urb, NULL);

        (void)SendRetSubmit(Device, Seqnum, 0, 0, NULL, 0, urb->StartFrame, Packets, Device->IsoDesc, isoLength);
    } else {
        ScoCompleteOut(Device, urb);
    }
}

/* ------------------------------------------------------------------ EP0 */

static void
TraceControl(USBIP_DEVICE *Device, const unsigned char *Setup, const unsigned char *Data,
             unsigned long Length, long Status)
{
    if (Device->Trace != NULL) {
        Device->Trace(Device->TraceContext, Setup, Data, Length, Status);
    }
}

/* Answers a GET_DESCRIPTOR into Scratch. Returns the reply length, or -1 to stall. */
static long
GetDescriptor(USBIP_DEVICE *Device, unsigned int Type, unsigned int Index, unsigned long Capacity)
{
    const unsigned char *source = NULL;
    unsigned long size = 0;

    switch (Type) {
    case DESC_DEVICE:
        source = DeckBtDeviceDescriptor;
        size = DeckBtDeviceDescriptorSize;
        break;
    case DESC_CONFIGURATION:
        if (Index == 0) {
            source = DeckBtConfigDescriptor;
            size = DeckBtConfigDescriptorSize;
        }
        break;
    case DESC_STRING:
        source = DeckBtGetStringDescriptor((UCHAR)Index, &size);
        break;
    default:
        break;   /* device qualifier, BOS, OS strings: not a dual-speed or USB 2.01 device */
    }
    if (source == NULL) {
        return -1;
    }
    size = MinUl(size, Capacity);
    memcpy(Device->Scratch, source, size);
    return (long)size;
}

static void
Control(USBIP_DEVICE *Device, unsigned long Seqnum, unsigned long Length,
        const unsigned char *Setup, const unsigned char *Data, unsigned long long Now)
{
    unsigned int bm = Setup[0];
    unsigned int request = Setup[1];
    unsigned int value = (unsigned int)Setup[2] | ((unsigned int)Setup[3] << 8);
    unsigned int index = (unsigned int)Setup[4] | ((unsigned int)Setup[5] << 8);
    unsigned long capacity = MinUl(Length, USBIP_DEV_SCRATCH_BYTES);
    long reply = 0;             /* IN bytes in Scratch; -1 = stall */
    int hciCommand = 0;

    Device->Stats.ControlRequests++;

    if ((bm & BM_TYPE_MASK) == 0u) {
        switch (request) {
        case REQ_GET_STATUS:
            /* Bus powered, no remote wakeup; endpoints are never halted. */
            Device->Scratch[0] = 0;
            Device->Scratch[1] = 0;
            reply = (long)MinUl(2u, capacity);
            break;
        case REQ_CLEAR_FEATURE:
        case REQ_SET_FEATURE:
            if ((bm & BM_RECIPIENT_MASK) == 0u && value == FEATURE_DEVICE_REMOTE_WAKEUP) {
                reply = -1;
            }
            break;
        case REQ_GET_DESCRIPTOR:
            reply = GetDescriptor(Device, value >> 8, value & 0xFFu, capacity);
            break;
        case REQ_GET_CONFIGURATION:
            Device->Scratch[0] = Device->Configuration;
            reply = (long)MinUl(1u, capacity);
            break;
        case REQ_SET_CONFIGURATION:
            if (value > 1u) {
                reply = -1;
                break;
            }
            ScoFlush(Device);
            Device->Configuration = (unsigned char)value;
            Device->ScoAlt = 0;
            break;
        case REQ_GET_INTERFACE:
            if (index == DECKBT_IFACE_HCI) {
                Device->Scratch[0] = 0;
            } else if (index == DECKBT_IFACE_SCO) {
                Device->Scratch[0] = Device->ScoAlt;
            } else {
                reply = -1;
                break;
            }
            reply = (long)MinUl(1u, capacity);
            break;
        case REQ_SET_INTERFACE:
            if (index == DECKBT_IFACE_HCI && value == 0u) {
                break;
            }
            if (index != DECKBT_IFACE_SCO || value > DECKBT_SCO_ALT_MAX) {
                reply = -1;
                break;
            }
            ScoFlush(Device);
            if (Device->ScoAlt != (unsigned char)value) {
                Device->ScoAlt = (unsigned char)value;
                Device->Stats.AltChanges++;
            }
            break;
        default:
            reply = -1;
            break;
        }
    } else if (bm == BM_PORT_FEATURE && request == REQ_SET_FEATURE && value == FEATURE_PORT_RESET) {
        /* Port reset: the host restarts the radio, as a real dongle's reset does. */
        HciTransportReset(Device->Transport);
        SentClear(&Device->EventSent);
        SentClear(&Device->AclSent);
        Device->HoldSince = 0;
        ScoFlush(Device);
        Device->Stats.PortResets++;
    } else if (bm == BM_HCI_COMMAND && request == 0u) {
        hciCommand = 1;
        Device->Stats.HciCommands++;
        if (!HciTransportSubmitCommand(Device->Transport, Data, Length)) {
            reply = -1;
        }
    } else {
        reply = -1;
    }

    if (reply < 0) {
        Device->Stats.Stalls++;
        TraceControl(Device, Setup, Data, (bm & BM_DIR_IN) ? 0u : Length, USBIP_EPIPE);
        (void)SendSimple(Device, Seqnum, USBIP_EPIPE, 0, NULL, 0);
        return;
    }
    if (bm & BM_DIR_IN) {
        TraceControl(Device, Setup, Device->Scratch, (unsigned long)reply, 0);
        (void)SendSimple(Device, Seqnum, 0, (unsigned long)reply, Device->Scratch, (unsigned long)reply);
    } else {
        TraceControl(Device, Setup, Data, Length, 0);
        (void)SendSimple(Device, Seqnum, 0, Length, NULL, 0);
    }
    if (hciCommand) {
        /* The event this command produced may already have a reader waiting. */
        UsbipDeviceDrain(Device, Now);
    }
}

/* ------------------------------------------------------------------ public */

void
UsbipDeviceInit(USBIP_DEVICE *Device, HCI_TRANSPORT *Transport, USBIP_DEVICE_SEND Send,
                void *SendContext)
{
    memset(Device, 0, sizeof(*Device));
    Device->Transport = Transport;
    Device->Send = Send;
    Device->SendContext = SendContext;
    ScoResetStreams(Device);
}

void
UsbipDeviceBeginSession(USBIP_DEVICE *Device)
{
    Device->Broken = 0;
    Device->Configuration = 0;
    Device->ScoAlt = 0;
    Device->EventIn.Count = 0;
    Device->AclIn.Count = 0;
    SentClear(&Device->EventSent);
    SentClear(&Device->AclSent);
    Device->ScoOutQ.Head = Device->ScoOutQ.Count = 0;
    Device->ScoInQ.Head = Device->ScoInQ.Count = 0;
    Device->HoldSince = 0;
    memset(&Device->Stats, 0, sizeof(Device->Stats));
    HciTransportReset(Device->Transport);
    ScoResetStreams(Device);
}

unsigned long
UsbipDeviceDescribe(unsigned char *Out, unsigned long Capacity, const char *BusId,
                    unsigned long BusNum, unsigned long DevNum, int WithInterfaces)
{
    static const char path[] = "/virtual/deckbtservice";
    unsigned long size = USBIP_USB_DEVICE_SIZE + (WithInterfaces ? 2u * USBIP_USB_INTERFACE_SIZE : 0u);
    unsigned char *p = Out;
    size_t busIdLength = strlen(BusId);

    if (Capacity < size || busIdLength >= USBIP_BUSID_SIZE) {
        return 0;
    }
    memset(Out, 0, size);
    memcpy(p, path, sizeof(path));
    p += USBIP_PATH_SIZE;
    memcpy(p, BusId, busIdLength);
    p += USBIP_BUSID_SIZE;
    UsbipPut32(p, BusNum);            p += 4;
    UsbipPut32(p, DevNum);            p += 4;
    UsbipPut32(p, USBIP_SPEED_HIGH);  p += 4;
    UsbipPut16(p, DECKBT_VENDOR_ID);  p += 2;
    UsbipPut16(p, DECKBT_PRODUCT_ID); p += 2;
    UsbipPut16(p, DECKBT_DEVICE_BCD); p += 2;
    *p++ = DECKBT_CLASS_WIRELESS;
    *p++ = DECKBT_SUBCLASS_RF;
    *p++ = DECKBT_PROTOCOL_BLUETOOTH;
    *p++ = 1;                          /* bConfigurationValue */
    *p++ = 1;                          /* bNumConfigurations  */
    *p++ = 2;                          /* bNumInterfaces      */
    if (WithInterfaces) {
        for (unsigned int i = 0; i < 2u; i++) {
            *p++ = DECKBT_CLASS_WIRELESS;
            *p++ = DECKBT_SUBCLASS_RF;
            *p++ = DECKBT_PROTOCOL_BLUETOOTH;
            *p++ = 0;
        }
    }
    return size;
}

unsigned long
UsbipCommandPayloadLength(const unsigned char Header[48])
{
    unsigned long command = UsbipGet32(Header + USBIP_HDR_COMMAND);
    unsigned long direction = UsbipGet32(Header + USBIP_HDR_DIRECTION);
    unsigned long length = UsbipGet32(Header + USBIP_HDR_LENGTH);
    unsigned long packets = UsbipGet32(Header + USBIP_HDR_PACKETS);
    unsigned long total;

    if (command == USBIP_CMD_UNLINK) {
        return 0;
    }
    if (command != USBIP_CMD_SUBMIT || direction > USBIP_DIR_IN || length > MAX_TRANSFER_LENGTH) {
        return 0xFFFFFFFFul;
    }
    total = (direction == USBIP_DIR_OUT) ? length : 0u;
    if (packets != USBIP_NON_ISO_PACKETS) {
        if (packets > USBIP_MAX_ISO_PACKETS) {
            return 0xFFFFFFFFul;
        }
        total += packets * USBIP_ISO_DESC_SIZE;
    }
    return total;
}

int
UsbipDeviceHandle(USBIP_DEVICE *Device, const unsigned char Header[48],
                  const unsigned char *Payload, unsigned long PayloadLength, unsigned long long Now)
{
    unsigned long command = UsbipGet32(Header + USBIP_HDR_COMMAND);
    unsigned long seqnum = UsbipGet32(Header + USBIP_HDR_SEQNUM);
    unsigned long direction = UsbipGet32(Header + USBIP_HDR_DIRECTION);
    unsigned long ep = UsbipGet32(Header + USBIP_HDR_EP);
    unsigned long length = UsbipGet32(Header + USBIP_HDR_LENGTH);
    unsigned long packets = UsbipGet32(Header + USBIP_HDR_PACKETS);
    const unsigned char *setup = Header + USBIP_HDR_SETUP;
    const unsigned char *data;
    const unsigned char *iso;
    int in = (direction == USBIP_DIR_IN);

    if (UsbipCommandPayloadLength(Header) != PayloadLength) {
        return 0;
    }

    if (command == USBIP_CMD_UNLINK) {
        unsigned long target = UsbipGet32(Header + USBIP_HDR_UNLINK_SEQNUM);
        int found = ParkRemove(&Device->EventIn, target) || ParkRemove(&Device->AclIn, target) ||
                    IsoRemove(&Device->ScoOutQ, target) || IsoRemove(&Device->ScoInQ, target);
        int lostEvent = 0;
        int lostAcl = 0;

        if (found) {
            Device->Stats.Unlinks++;
        } else {
            /* Already answered: the reply crossed this unlink and usbip-win2 dropped it. */
            Device->Stats.UnlinksMissed++;
            lostEvent = SentMarkLost(&Device->EventSent, target);
            lostAcl = !lostEvent && SentMarkLost(&Device->AclSent, target);
            if (lostEvent || lostAcl) {
                Device->Stats.RepliesLost++;
            }
        }
        (void)SendRetUnlink(Device, seqnum, found ? USBIP_ECONNRESET : 0);
        if (lostEvent || lostAcl) {
            UsbipDeviceDrain(Device, Now);
        }
        return !Device->Broken;
    }

    data = in ? NULL : Payload;
    iso = Payload + (in ? 0u : length);

    switch (ep) {
    case EP_CONTROL:
        if (in != ((setup[0] & BM_DIR_IN) != 0)) {
            return 0;
        }
        Control(Device, seqnum, length, setup, data, Now);
        break;

    case EP_EVENT:
        if (!in || !ParkPush(&Device->EventIn, seqnum, length)) {
            Device->Stats.Stalls++;
            (void)SendSimple(Device, seqnum, USBIP_EPIPE, 0, NULL, 0);
            break;
        }
        UsbipDeviceDrain(Device, Now);
        break;

    case EP_ACL:
        if (in) {
            if (!ParkPush(&Device->AclIn, seqnum, length)) {
                Device->Stats.Stalls++;
                (void)SendSimple(Device, seqnum, USBIP_EPIPE, 0, NULL, 0);
                break;
            }
            UsbipDeviceDrain(Device, Now);
        } else if (HciTransportSubmitAcl(Device->Transport, data, length)) {
            Device->Stats.AclOut++;
            (void)SendSimple(Device, seqnum, 0, length, NULL, 0);
        } else {
            Device->Stats.Stalls++;
            (void)SendSimple(Device, seqnum, USBIP_EPIPE, 0, NULL, 0);
        }
        break;

    case EP_SCO:
        if (packets == USBIP_NON_ISO_PACKETS) {
            return 0;
        }
        ScoSubmit(Device, seqnum, in, length, packets, data, iso, Now);
        break;

    default:
        Device->Stats.Stalls++;
        (void)SendSimple(Device, seqnum, USBIP_EPIPE, 0, NULL, 0);
        break;
    }
    return !Device->Broken;
}

/* ------------------------------------------------------------------ event and ACL delivery */

typedef struct _USBIP_IN_STREAM {
    HCI_STREAM        Stream;
    USBIP_PARK_QUEUE *Reads;
    USBIP_SENT_RING  *Sent;
} USBIP_IN_STREAM;

/* Whether In has a packet waiting (lost or queued), and its arrival order if the backend knows it. */
static int
InPending(USBIP_DEVICE *Device, const USBIP_IN_STREAM *In, unsigned long *Order, int *Ordered)
{
    const USBIP_SENT_IN *lost = SentOldestLost(In->Sent);

    if (lost != NULL) {
        *Order = lost->Order;
        *Ordered = lost->Ordered;
        return 1;
    }
    if (!HciTransportHasStream(Device->Transport, In->Stream)) {
        return 0;
    }
    *Ordered = HciTransportPeekOrder(Device->Transport, In->Stream, Order);
    return 1;
}

/* In's next packet, a lost one first (it preceded everything still queued), into its oldest read. */
static void
InDeliver(USBIP_DEVICE *Device, const USBIP_IN_STREAM *In)
{
    USBIP_PARKED read = In->Reads->Items[0];
    unsigned long capacity = MinUl(read.Capacity, USBIP_DEV_SCRATCH_BYTES);
    unsigned long written = 0;
    const unsigned char *data = Device->Scratch;
    unsigned long order = 0;
    int ordered = 0;
    USBIP_SENT_IN *lost = SentOldestLost(In->Sent);

    ParkRemoveAt(In->Reads, 0);
    if (lost != NULL) {
        /* One that does not fit this read stays owed, as a queued packet that does not fit does. */
        if (lost->Length <= capacity) {
            written = lost->Length;
            data = lost->Data;
            Device->Stats.Redelivered++;
        }
    } else {
        ordered = HciTransportPeekOrder(Device->Transport, In->Stream, &order);
        if (!HciTransportPopStream(Device->Transport, In->Stream, Device->Scratch, capacity, &written)) {
            written = 0;   /* did not fit this read; it stays queued for the next one */
        }
    }
    if (In->Stream == HciStreamEvent) {
        Device->Stats.EventsIn++;
    } else {
        Device->Stats.AclIn++;
    }
    (void)SendSimple(Device, read.Seqnum, 0, written, data, written);
    if (lost != NULL && written != 0) {
        /* A replay changes the request, not the packet's place in controller arrival order. */
        lost->Seqnum = read.Seqnum;
        lost->Lost = 0;
        In->Sent->Lost--;
    } else if (lost == NULL) {
        SentRecord(In->Sent, read.Seqnum, data, written, order, ordered);
    }
}

/* Starts or continues a hold; nonzero once it has lasted USBIP_DEV_ORDER_HOLD. */
static int
HoldExpired(USBIP_DEVICE *Device, unsigned long long Now)
{
    if (Device->HoldSince == 0) {
        Device->HoldSince = (Now != 0) ? Now : 1u;
        Device->Stats.OrderHolds++;
        return 0;
    }
    return Now >= Device->HoldSince && Now - Device->HoldSince >= USBIP_DEV_ORDER_HOLD;
}

void
UsbipDeviceDrain(USBIP_DEVICE *Device, unsigned long long Now)
{
    USBIP_IN_STREAM streams[2];
    int holding = 0;

    streams[0].Stream = HciStreamEvent;
    streams[0].Reads = &Device->EventIn;
    streams[0].Sent = &Device->EventSent;
    streams[1].Stream = HciStreamAcl;
    streams[1].Reads = &Device->AclIn;
    streams[1].Sent = &Device->AclSent;

    while (!Device->Broken) {
        unsigned long order[2] = { 0, 0 };
        int ordered[2] = { 0, 0 };
        int pending[2];
        int ready[2];
        int oldest = -1;   /* stream holding the older packet, when both orders are known */
        int pick;

        for (int s = 0; s < 2; s++) {
            pending[s] = InPending(Device, &streams[s], &order[s], &ordered[s]);
            ready[s] = pending[s] && streams[s].Reads->Count != 0;
        }
        if (!ready[0] && !ready[1]) {
            break;
        }
        if (pending[0] && pending[1] && ordered[0] && ordered[1]) {
            oldest = ((long)(order[1] - order[0]) < 0) ? 1 : 0;
        }
        if (ready[0] && ready[1]) {
            pick = (oldest >= 0) ? oldest : 0;
        } else {
            pick = ready[0] ? 0 : 1;
            /* The older packet waits for a read on the other endpoint: let it go first, for a while. */
            if (oldest == 1 - pick) {
                if (!HoldExpired(Device, Now)) {
                    holding = 1;
                    break;
                }
                Device->Stats.OrderHoldTimeouts++;
            }
        }
        if (oldest != 1 - pick) {
            /* The blocking packet has a read now. A different blockage gets a fresh deadline. */
            Device->HoldSince = 0;
        }
        InDeliver(Device, &streams[pick]);
    }
    if (!holding) {
        Device->HoldSince = 0;
    }
}

unsigned long long
UsbipDeviceTick(USBIP_DEVICE *Device, unsigned long long Now)
{
    unsigned long long next = 0;

    (void)ScoUsbOutRelease(&Device->ScoOut, Now, ScoEmit, Device);

    while (Device->ScoOutQ.Count != 0 && IsoAt(&Device->ScoOutQ, 0)->Due <= Now) {
        NoteLate(Device, IsoAt(&Device->ScoOutQ, 0)->Due, Now);
        ScoCompleteOut(Device, IsoAt(&Device->ScoOutQ, 0));
        IsoPopFront(&Device->ScoOutQ);
    }
    while (Device->ScoInQ.Count != 0 && IsoAt(&Device->ScoInQ, 0)->Due <= Now) {
        NoteLate(Device, IsoAt(&Device->ScoInQ, 0)->Due, Now);
        ScoCompleteIn(Device, IsoAt(&Device->ScoInQ, 0));
        IsoPopFront(&Device->ScoInQ);
    }

    if (Device->ScoOutQ.Count != 0) {
        next = IsoAt(&Device->ScoOutQ, 0)->Due;
    }
    if (Device->ScoInQ.Count != 0 && (next == 0 || IsoAt(&Device->ScoInQ, 0)->Due < next)) {
        next = IsoAt(&Device->ScoInQ, 0)->Due;
    }
    if (Device->ScoOut.Count != 0) {
        unsigned long long due = Device->ScoOut.Ring[Device->ScoOut.Head].Due;

        if (next == 0 || due < next) {
            next = due;
        }
    }
    if (Device->HoldSince != 0) {
        UsbipDeviceDrain(Device, Now);
        if (Device->HoldSince != 0) {
            unsigned long long due = Device->HoldSince + USBIP_DEV_ORDER_HOLD;

            if (next == 0 || due < next) {
                next = due;
            }
        }
    }
    return next;
}
