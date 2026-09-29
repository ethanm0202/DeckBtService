/*
 * usbip_device_selftest.c - event / ACL delivery order in src/service/usbip_device.c.
 *
 * Events (interrupt 0x81) and ACL data (bulk 0x82) reach Windows on different endpoints. Reordering
 * is a possible contributor to the observed BTHPORT NULL-link bugcheck, not a proven cause. A fake
 * transport reports arrival order; the checks drive UsbipDeviceHandle with USB/IP commands and
 * read the RET_SUBMITs the device sends, including the explicit 20 ms ordering limit.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../src/service/usbip_device.h"
#include "../src/service/usbip_proto.h"

static int g_Failures;

#define CHECK(cond, msg) \
    do { if (cond) { printf("  ok   %s\n", msg); } else { printf("  FAIL %s\n", msg); g_Failures++; } } while (0)

/* ------------------------------------------------------------------ fake ordered transport */

typedef struct {
    unsigned char Data[64];
    unsigned long Length;
    unsigned long Order;
} FAKE_PACKET;

typedef struct {
    FAKE_PACKET Items[16];
    unsigned int Count;
} FAKE_QUEUE;

static FAKE_QUEUE g_Queue[2];      /* [0] events, [1] ACL */
static unsigned long g_NextOrder;
static int g_Ordered = 1;

static FAKE_QUEUE *
QueueOf(HCI_STREAM Stream)
{
    return (Stream == HciStreamEvent) ? &g_Queue[0] : (Stream == HciStreamAcl) ? &g_Queue[1] : NULL;
}

static void
Arrive(HCI_STREAM Stream, unsigned char Tag)
{
    FAKE_QUEUE *q = QueueOf(Stream);
    FAKE_PACKET *p = &q->Items[q->Count++];

    memset(p->Data, Tag, 4);
    p->Length = 4;
    p->Order = g_NextOrder++;
}

static unsigned char FSubmit(HCI_TRANSPORT *t, const unsigned char *p, unsigned long l) { (void)t; (void)p; (void)l; return 1; }
static unsigned long FLast(const HCI_TRANSPORT *t) { (void)t; return 0; }
static void FReset(HCI_TRANSPORT *t) { (void)t; g_Queue[0].Count = g_Queue[1].Count = 0; }

static unsigned char
FHas(const HCI_TRANSPORT *t, HCI_STREAM s)
{
    FAKE_QUEUE *q = QueueOf(s);

    (void)t;
    return (unsigned char)(q != NULL && q->Count != 0);
}

static unsigned char
FPop(HCI_TRANSPORT *t, HCI_STREAM s, unsigned char *Buffer, unsigned long Capacity, unsigned long *Written)
{
    FAKE_QUEUE *q = QueueOf(s);

    (void)t;
    *Written = 0;
    if (q == NULL || q->Count == 0 || q->Items[0].Length > Capacity) {
        return 0;
    }
    memcpy(Buffer, q->Items[0].Data, q->Items[0].Length);
    *Written = q->Items[0].Length;
    memmove(&q->Items[0], &q->Items[1], (q->Count - 1u) * sizeof(q->Items[0]));
    q->Count--;
    return 1;
}

static unsigned char
FPeek(const HCI_TRANSPORT *t, HCI_STREAM s, unsigned long *Order)
{
    FAKE_QUEUE *q = QueueOf(s);

    (void)t;
    if (!g_Ordered || q == NULL || q->Count == 0) {
        return 0;
    }
    *Order = q->Items[0].Order;
    return 1;
}

static const HCI_TRANSPORT_OPS g_Ops = { FSubmit, FSubmit, FSubmit, FHas, FPop, FLast, FReset, FPeek };

/* ------------------------------------------------------------------ captured replies */

static struct {
    unsigned long Seqnum;
    unsigned char Tag;      /* first payload byte, 0 if empty */
    long          Status;
    unsigned long Actual;
} g_Replies[64];
static unsigned int g_ReplyCount;

static int
Capture(void *Context, const unsigned char *Data, unsigned long Length)
{
    (void)Context;
    if (UsbipGet32(Data + USBIP_HDR_COMMAND) == USBIP_RET_SUBMIT && g_ReplyCount < 64u) {
        g_Replies[g_ReplyCount].Seqnum = UsbipGet32(Data + USBIP_HDR_SEQNUM);
        g_Replies[g_ReplyCount].Tag = (Length > USBIP_URB_HEADER_SIZE) ? Data[USBIP_URB_HEADER_SIZE] : 0u;
        g_Replies[g_ReplyCount].Status = (long)UsbipGet32(Data + USBIP_HDR_STATUS);
        g_Replies[g_ReplyCount].Actual = UsbipGet32(Data + USBIP_HDR_ACTUAL);
        g_ReplyCount++;
    }
    return 1;
}

static void
Read(USBIP_DEVICE *Device, unsigned long Seqnum, unsigned long Ep, unsigned long long Now)
{
    unsigned char h[USBIP_URB_HEADER_SIZE] = { 0 };

    UsbipPut32(h + USBIP_HDR_COMMAND, USBIP_CMD_SUBMIT);
    UsbipPut32(h + USBIP_HDR_SEQNUM, Seqnum);
    UsbipPut32(h + USBIP_HDR_DIRECTION, USBIP_DIR_IN);
    UsbipPut32(h + USBIP_HDR_EP, Ep);
    UsbipPut32(h + USBIP_HDR_LENGTH, 64);
    UsbipPut32(h + USBIP_HDR_PACKETS, USBIP_NON_ISO_PACKETS);
    (void)UsbipDeviceHandle(Device, h, NULL, 0, Now);
}

static void
Unlink(USBIP_DEVICE *Device, unsigned long Seqnum, unsigned long Target, unsigned long long Now)
{
    unsigned char h[USBIP_URB_HEADER_SIZE] = { 0 };

    UsbipPut32(h + USBIP_HDR_COMMAND, USBIP_CMD_UNLINK);
    UsbipPut32(h + USBIP_HDR_SEQNUM, Seqnum);
    UsbipPut32(h + USBIP_HDR_UNLINK_SEQNUM, Target);
    (void)UsbipDeviceHandle(Device, h, NULL, 0, Now);
}

static int
Replied(unsigned int Index, unsigned long Seqnum, unsigned char Tag)
{
    return g_ReplyCount > Index && g_Replies[Index].Seqnum == Seqnum && g_Replies[Index].Tag == Tag;
}

/* An isochronous transfer of Length bytes on EP 0x03 / 0x83 with Packets (offset, length) descriptors. */
static void
IsoSubmit(USBIP_DEVICE *Device, unsigned long Seqnum, int In, unsigned long Length, const unsigned long *Descs,
          unsigned long Packets, unsigned long long Now)
{
    unsigned char h[USBIP_URB_HEADER_SIZE] = { 0 };
    unsigned char payload[64 + 4 * USBIP_ISO_DESC_SIZE] = { 0 };
    unsigned long data = In ? 0u : Length;   /* IN carries only the descriptors */

    UsbipPut32(h + USBIP_HDR_COMMAND, USBIP_CMD_SUBMIT);
    UsbipPut32(h + USBIP_HDR_SEQNUM, Seqnum);
    UsbipPut32(h + USBIP_HDR_DIRECTION, In ? USBIP_DIR_IN : USBIP_DIR_OUT);
    UsbipPut32(h + USBIP_HDR_EP, 3);
    UsbipPut32(h + USBIP_HDR_LENGTH, Length);
    UsbipPut32(h + USBIP_HDR_PACKETS, Packets);
    for (unsigned long i = 0; i < Packets; i++) {
        UsbipPut32(payload + data + i * USBIP_ISO_DESC_SIZE + 0, Descs[2 * i]);
        UsbipPut32(payload + data + i * USBIP_ISO_DESC_SIZE + 4, Descs[2 * i + 1]);
    }
    (void)UsbipDeviceHandle(Device, h, payload, data + Packets * USBIP_ISO_DESC_SIZE, Now);
}

#define EP_EVENT 1u
#define EP_ACL   2u
#define MS(n)    ((unsigned long long)(n) * 10000ull)

int
main(void)
{
    static USBIP_DEVICE device;
    HCI_TRANSPORT transport = { 0 };
    unsigned long long now = MS(1000);

    transport.Ops = &g_Ops;
    UsbipDeviceInit(&device, &transport, Capture, NULL);
    UsbipDeviceBeginSession(&device);

    printf("-- An event never overtakes earlier ACL data --\n");
    Arrive(HciStreamAcl, 0xA1);           /* e.g. an L2CAP Configure Request */
    Arrive(HciStreamEvent, 0xE1);         /* then Disconnection_Complete */
    Read(&device, 1, EP_EVENT, now);
    CHECK(g_ReplyCount == 0, "event read parked, ACL read missing: the event is held");
    CHECK(device.Stats.OrderHolds == 1, "one hold counted");
    Read(&device, 2, EP_ACL, now + MS(1));
    CHECK(Replied(0, 2, 0xA1) && Replied(1, 1, 0xE1), "ACL read arrives: ACL first, then the event");
    CHECK(device.HoldSince == 0, "hold released");

    printf("-- ACL never overtakes an earlier event --\n");
    g_ReplyCount = 0;
    Arrive(HciStreamEvent, 0xE2);         /* e.g. Connection_Complete */
    Arrive(HciStreamAcl, 0xA2);           /* then the first data on the new handle */
    Read(&device, 3, EP_ACL, now);
    CHECK(g_ReplyCount == 0, "ACL held behind the earlier event");
    Read(&device, 4, EP_EVENT, now);
    CHECK(Replied(0, 4, 0xE2) && Replied(1, 3, 0xA2), "event first, then ACL");

    printf("-- A hold is bounded --\n");
    g_ReplyCount = 0;
    Arrive(HciStreamEvent, 0xE3);
    Arrive(HciStreamAcl, 0xA3);
    Read(&device, 5, EP_ACL, now);
    CHECK(g_ReplyCount == 0, "ACL held");
    (void)UsbipDeviceTick(&device, now + MS(19));
    CHECK(g_ReplyCount == 0, "still held after 19 ms");
    CHECK(UsbipDeviceTick(&device, now + MS(19)) == now + USBIP_DEV_ORDER_HOLD, "tick asks to run when the hold expires");
    (void)UsbipDeviceTick(&device, now + MS(20));
    CHECK(Replied(0, 5, 0xA3), "delivered once the hold expires");
    CHECK(device.Stats.OrderHoldTimeouts == 1, "timeout counted");
    Read(&device, 6, EP_EVENT, now + MS(21));
    CHECK(Replied(1, 6, 0xE3), "the waiting event follows when its read arrives");

    printf("-- Both reads parked: controller order --\n");
    g_ReplyCount = 0;
    Read(&device, 7, EP_EVENT, now);
    Read(&device, 8, EP_ACL, now);
    Read(&device, 9, EP_ACL, now);
    Arrive(HciStreamAcl, 0xA4);
    Arrive(HciStreamEvent, 0xE4);
    Arrive(HciStreamAcl, 0xA5);
    UsbipDeviceDrain(&device, now);
    CHECK(Replied(0, 8, 0xA4) && Replied(1, 7, 0xE4) && Replied(2, 9, 0xA5), "A4, E4, A5 in arrival order");

    printf("-- A lost ACL reply keeps its place ahead of a later event --\n");
    g_ReplyCount = 0;
    Read(&device, 10, EP_ACL, now);
    Arrive(HciStreamAcl, 0xA6);
    UsbipDeviceDrain(&device, now);
    CHECK(Replied(0, 10, 0xA6), "ACL delivered to read 10");
    Arrive(HciStreamEvent, 0xE6);
    Unlink(&device, 11, 10, now);         /* the reply crossed the unlink: usbip-win2 dropped it */
    CHECK(device.Stats.RepliesLost == 1, "unlink after the reply marks it lost");
    Read(&device, 12, EP_EVENT, now);
    CHECK(g_ReplyCount == 1, "the later event waits for the lost ACL packet");
    Read(&device, 13, EP_ACL, now);
    CHECK(Replied(1, 13, 0xA6) && Replied(2, 12, 0xE6), "lost ACL redelivered first, then the event");
    CHECK(device.Stats.Redelivered == 1, "one redelivery");

    printf("-- Without order information streams are independent --\n");
    g_ReplyCount = 0;
    g_Ordered = 0;
    Arrive(HciStreamAcl, 0xA7);
    Arrive(HciStreamEvent, 0xE7);
    Read(&device, 14, EP_EVENT, now);
    CHECK(Replied(0, 14, 0xE7), "event delivered at once");
    Read(&device, 15, EP_ACL, now);
    CHECK(Replied(1, 15, 0xA7), "ACL delivered when its read arrives");

    printf("-- Repeated cancellation preserves original packet order --\n");
    UsbipDeviceBeginSession(&device);
    g_ReplyCount = 0;
    g_Ordered = 1;
    Arrive(HciStreamAcl, 0xA8);
    Arrive(HciStreamAcl, 0xA9);
    Read(&device, 100, EP_ACL, now);
    Read(&device, 101, EP_ACL, now);
    Unlink(&device, 102, 100, now);
    Read(&device, 103, EP_ACL, now);
    CHECK(Replied(2, 103, 0xA8), "first packet redelivered once");
    Unlink(&device, 104, 103, now);
    Unlink(&device, 105, 101, now);
    Read(&device, 106, EP_ACL, now);
    Read(&device, 107, EP_ACL, now);
    CHECK(Replied(3, 106, 0xA8) && Replied(4, 107, 0xA9),
          "both replies cancelled: first packet still precedes second after another replay");

    printf("-- Each distinct cross-endpoint blockage gets its own deadline --\n");
    UsbipDeviceBeginSession(&device);
    g_ReplyCount = 0;
    Arrive(HciStreamAcl, 0xAA);
    Arrive(HciStreamEvent, 0xEA);
    Arrive(HciStreamAcl, 0xAB);
    Arrive(HciStreamEvent, 0xEB);
    Read(&device, 110, EP_EVENT, now);
    Read(&device, 111, EP_EVENT, now);
    Read(&device, 112, EP_ACL, now + MS(19));
    CHECK(Replied(0, 112, 0xAA) && Replied(1, 110, 0xEA),
          "first blockage resolved by its ACL read");
    (void)UsbipDeviceTick(&device, now + MS(20));
    CHECK(g_ReplyCount == 2, "newer event does not inherit the previous packet's expired deadline");
    Read(&device, 113, EP_ACL, now + MS(21));
    CHECK(Replied(2, 113, 0xAB) && Replied(3, 111, 0xEB),
          "second ACL read releases the second event in order");

    printf("-- Isochronous packets must lie in order inside the transfer --\n");
    {
        static const unsigned long same[4]    = { 0, 17, 0, 17 };   /* both packets are the whole buffer */
        static const unsigned long reversed[4] = { 9, 8, 0, 9 };
        static const unsigned long apart[4]   = { 0, 9, 9, 8 };
        unsigned char h[USBIP_URB_HEADER_SIZE] = { 0 };
        const unsigned char alt2[8] = { 0x01, 0x0B, 2, 0, 1, 0, 0, 0 };   /* SET_INTERFACE 1, alt 2 */

        UsbipDeviceBeginSession(&device);
        g_ReplyCount = 0;
        UsbipPut32(h + USBIP_HDR_COMMAND, USBIP_CMD_SUBMIT);
        UsbipPut32(h + USBIP_HDR_SEQNUM, 120);
        UsbipPut32(h + USBIP_HDR_PACKETS, USBIP_NON_ISO_PACKETS);
        memcpy(h + USBIP_HDR_SETUP, alt2, sizeof(alt2));
        (void)UsbipDeviceHandle(&device, h, NULL, 0, now);
        CHECK(g_ReplyCount == 1 && g_Replies[0].Status == 0 && device.ScoAlt == 2, "alternate setting 2 selected");

        IsoSubmit(&device, 121, 0, 17, same, 2, now);
        CHECK(g_ReplyCount == 2 && g_Replies[1].Seqnum == 121 && g_Replies[1].Status == USBIP_EINVAL &&
              g_Replies[1].Actual == 0, "overlapping descriptors: -EINVAL, nothing transferred");
        IsoSubmit(&device, 122, 0, 17, reversed, 2, now);
        CHECK(g_ReplyCount == 3 && g_Replies[2].Seqnum == 122 && g_Replies[2].Status == USBIP_EINVAL,
              "descriptors out of order: -EINVAL");
        IsoSubmit(&device, 123, 0, 17, apart, 2, now);
        CHECK(g_ReplyCount == 3, "adjacent in-order descriptors are accepted and paced");
        (void)UsbipDeviceTick(&device, now + MS(1000));
        CHECK(g_ReplyCount == 4 && g_Replies[3].Seqnum == 123 && g_Replies[3].Status == 0 &&
              g_Replies[3].Actual == 17, "and complete with exactly the 17 bytes requested");
        CHECK(device.Stats.ScoRejectedUrbs == 2, "two transfers rejected");
    }

    printf("-- No isochronous packet may exceed the setting's wMaxPacketSize --\n");
    {
        static const unsigned long exact[2] = { 0, 17 };
        static const unsigned long over[2]  = { 0, 18 };   /* inside the transfer, one byte over alt 2 */
        unsigned long long t = now + MS(1100);

        g_ReplyCount = 0;
        IsoSubmit(&device, 130, 0, 18, over, 1, t);
        CHECK(g_ReplyCount == 1 && g_Replies[0].Seqnum == 130 && g_Replies[0].Status == USBIP_EINVAL,
              "OUT packet of 18 bytes at alternate setting 2 (17): -EINVAL");
        IsoSubmit(&device, 131, 1, 18, over, 1, t);
        CHECK(g_ReplyCount == 2 && g_Replies[1].Seqnum == 131 && g_Replies[1].Status == USBIP_EINVAL,
              "IN packet of 18 bytes: -EINVAL");
        IsoSubmit(&device, 132, 0, 17, exact, 1, t);
        IsoSubmit(&device, 133, 1, 17, exact, 1, t);
        CHECK(g_ReplyCount == 2, "packets of exactly 17 bytes are accepted and paced");
        (void)UsbipDeviceTick(&device, t + MS(1000));
        CHECK(g_ReplyCount == 4 && g_Replies[2].Status == 0 && g_Replies[3].Status == 0 &&
              ((g_Replies[2].Seqnum == 132 && g_Replies[3].Seqnum == 133) ||
               (g_Replies[2].Seqnum == 133 && g_Replies[3].Seqnum == 132)),
              "and both complete");
        CHECK(device.Stats.ScoRejectedUrbs == 4, "four transfers rejected in all");
    }

    if (g_Failures != 0) {
        printf("\nUSBIP DEVICE SELFTEST FAILED: %d assertion(s) failed\n", g_Failures);
        return 1;
    }
    printf("\nUSBIP DEVICE SELFTEST PASSED\n");
    return 0;
}
