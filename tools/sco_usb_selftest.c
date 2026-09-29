/*
 * sco_usb_selftest.c - host-side tests for src/common/sco_usb.c (SCO over USB isochronous).
 *
 *   1. Pacing: back-to-back transfers queue behind each other; an idle endpoint restarts at now.
 *   2. OUT: frames are released only at their time; HCI SCO packets reassemble across frames and
 *      transfers; a lost frame never produces a corrupt packet and the stream resynchronises.
 *   3. IN: controller packets of any size are re-cut into the USB geometry of the selected setting
 *      (alt 1-5: 3 x wMaxPacketSize per packet; alt 6: one 63-byte packet), payload order and the
 *      handle/flags field preserved, zero bytes when no whole packet is available.
 *
 * Nonzero exit on any failure; final "SCO USB SELFTEST PASSED".
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "../src/include/sco_usb.h"

static int g_fail = 0;

#define CHECK(cond, ...)                            \
    do {                                            \
        if (cond) { printf("  ok   "); }            \
        else      { printf("  FAIL "); g_fail++; }  \
        printf(__VA_ARGS__); printf("\n");          \
    } while (0)

#define MS(x) ((unsigned long long)(x) * SCO_USB_FRAME_100NS)

/* ---------------------------------------------------------------- harnesses */

typedef struct _EMITTED {
    unsigned long Count;
    unsigned long Length[64];
    unsigned char Data[64][SCO_USB_MAX_PACKET];
} EMITTED;

static void
Emit(void *Context, const unsigned char *Packet, unsigned long Length)
{
    EMITTED *e = (EMITTED *)Context;
    if (e->Count < 64u) {
        e->Length[e->Count] = Length;
        memcpy(e->Data[e->Count], Packet, Length);
    }
    e->Count++;
}

typedef struct _SOURCE {
    unsigned long Count;
    unsigned long Next;
    unsigned long Length[16];
    unsigned char Data[16][SCO_USB_MAX_PACKET];
} SOURCE;

static unsigned char
Pull(void *Context, unsigned char *Packet, unsigned long Capacity, unsigned long *Written)
{
    SOURCE *s = (SOURCE *)Context;
    if (s->Next >= s->Count || s->Length[s->Next] > Capacity) {
        *Written = 0u;
        return 0u;
    }
    memcpy(Packet, s->Data[s->Next], s->Length[s->Next]);
    *Written = s->Length[s->Next];
    s->Next++;
    return 1u;
}

/* Builds an HCI SCO packet: handle field, payload length, payload bytes Base, Base+1, ... */
static unsigned long
MakeSco(unsigned char *Out, unsigned short HandleField, unsigned int Payload, unsigned char Base)
{
    Out[0] = (unsigned char)(HandleField & 0xFFu);
    Out[1] = (unsigned char)(HandleField >> 8);
    Out[2] = (unsigned char)Payload;
    for (unsigned int i = 0; i < Payload; i++) {
        Out[3 + i] = (unsigned char)(Base + i);
    }
    return 3u + Payload;
}

static void
AddSource(SOURCE *s, unsigned short HandleField, unsigned int Payload, unsigned char Base)
{
    s->Length[s->Count] = MakeSco(s->Data[s->Count], HandleField, Payload, Base);
    s->Count++;
}

/* ---------------------------------------------------------------- tests */

static void
TestPacing(void)
{
    SCO_USB_CLOCK clock = { 0 };
    unsigned long long due;

    printf("pacing\n");
    due = ScoUsbSchedule(&clock, MS(100), MS(8));
    CHECK(due == MS(108), "first transfer of 8 frames completes 8 ms after submission");
    due = ScoUsbSchedule(&clock, MS(101), MS(8));
    CHECK(due == MS(116), "a transfer queued behind it completes 8 ms after the first ends, not at 109 ms");
    due = ScoUsbSchedule(&clock, MS(102), MS(1));
    CHECK(due == MS(117), "a third queued transfer of 1 frame ends at 117 ms");
    due = ScoUsbSchedule(&clock, MS(500), MS(4));
    CHECK(due == MS(504), "after the endpoint idles, the next transfer starts at submission time");

    printf("OUT packet air time\n");
    CHECK(ScoUsbOutPacketSpan(63, 63) == SCO_USB_MSBC_FRAME_100NS, "alt 6: a full 63-byte packet is one mSBC frame, 7.5 ms");
    CHECK(ScoUsbOutPacketSpan(63, 0) == MS(1), "alt 6: an empty packet is just its 1 ms frame");
    CHECK(ScoUsbOutPacketSpan(17, 17) == MS(1) && ScoUsbOutPacketSpan(49, 49) == MS(1),
          "alts 1-5: a full packet every frame is the voice rate, 1 ms each");
    {
        /* BTHUSB sends one-packet 63-byte transfers back to back. Each packet represents
         * 7.5 ms of mSBC audio, so 133 frames span one second of voice. */
        SCO_USB_CLOCK out = { 0 };
        unsigned long long end = 0;
        for (int k = 0; k < 133; k++) {
            end = ScoUsbSchedule(&out, MS(0), ScoUsbOutPacketSpan(63, 63));
        }
        CHECK(end == 133ull * SCO_USB_MSBC_FRAME_100NS, "133 back-to-back alt-6 frames complete over 997.5 ms, not 133 ms");
    }
}

static void
TestOutReassembly(void)
{
    static SCO_USB_OUT out;
    EMITTED e;
    unsigned char pkt[SCO_USB_MAX_PACKET];
    unsigned long len = MakeSco(pkt, 0x0006, 48, 0x40);   /* 51 bytes = 3 x 17 */
    unsigned long n;

    printf("OUT reassembly and release timing\n");
    ScoUsbOutReset(&out);
    memset(&e, 0, sizeof(e));
    ScoUsbOutPush(&out, MS(1), pkt, 17);
    ScoUsbOutPush(&out, MS(2), pkt + 17, 17);
    ScoUsbOutPush(&out, MS(3), pkt + 34, 17);
    n = ScoUsbOutRelease(&out, MS(2), Emit, &e);
    CHECK(n == 0 && e.Count == 0, "two of three frames due: nothing reaches the controller yet");
    CHECK(out.Count == 1, "the undue third frame is still queued");
    n = ScoUsbOutRelease(&out, MS(3), Emit, &e);
    CHECK(n == 1 && e.Count == 1, "third frame due: exactly one packet emitted");
    CHECK(e.Length[0] == len && memcmp(e.Data[0], pkt, len) == 0, "emitted packet is byte-identical (51 bytes)");

    /* Two packets inside one 63-byte frame, the second completing in the next frame. */
    {
        unsigned char a[SCO_USB_MAX_PACKET], b[SCO_USB_MAX_PACKET], stream[128];
        unsigned long la = MakeSco(a, 0x0006, 10, 0x10);
        unsigned long lb = MakeSco(b, 0x0006, 60, 0x60);
        memcpy(stream, a, la);
        memcpy(stream + la, b, lb);
        memset(&e, 0, sizeof(e));
        ScoUsbOutPush(&out, MS(4), stream, 63);
        ScoUsbOutPush(&out, MS(5), stream + 63, la + lb - 63);
        (void)ScoUsbOutRelease(&out, MS(4), Emit, &e);
        CHECK(e.Count == 1 && e.Length[0] == la && memcmp(e.Data[0], a, la) == 0,
              "a short packet completing mid-frame is emitted with that frame");
        (void)ScoUsbOutRelease(&out, MS(5), Emit, &e);
        CHECK(e.Count == 2 && e.Length[1] == lb && memcmp(e.Data[1], b, lb) == 0,
              "the packet started in the same frame completes from the next frame");
    }

    memset(&e, 0, sizeof(e));
    ScoUsbOutPush(&out, MS(6), pkt, 0);
    CHECK(out.Count == 0, "a zero-length isochronous packet queues nothing");
    ScoUsbOutPush(&out, MS(7), pkt, 70);
    CHECK(out.Count == 2 && out.Ring[out.Head].Length == SCO_USB_MAX_ISO_PACKET,
          "an oversize packet is split into 63-byte frames with the same release time");
}

static void
TestOutResync(void)
{
    static SCO_USB_OUT out;
    static EMITTED e;
    unsigned char pkt[SCO_USB_MAX_PACKET];
    unsigned long long t = 0;
    int intact = 1;

    printf("OUT ring overflow and resynchronisation\n");
    ScoUsbOutReset(&out);
    memset(&e, 0, sizeof(e));
    /* Payload bytes 0xA0..0xAF never look like handle 0x006 at a frame start. */
    (void)MakeSco(pkt, 0x0006, 48, 0xA0);
    for (int f = 0; f < 3; f++) {
        ScoUsbOutPush(&out, MS(++t), pkt + 17 * f, 17);
    }
    (void)ScoUsbOutRelease(&out, MS(t), Emit, &e);
    CHECK(e.Count == 1 && out.LastHandle == 0x006, "stream established on handle 0x006");

    /* 43 packets = 129 frames into a 128-frame ring: the oldest frame (packet 1's header) is lost. */
    for (int p = 1; p <= 43; p++) {
        (void)MakeSco(pkt, 0x0006, 48, (unsigned char)(0xA0 + (p % 16)));
        for (int f = 0; f < 3; f++) {
            ScoUsbOutPush(&out, MS(++t), pkt + 17 * f, 17);
        }
    }
    CHECK(out.RingDrops == 1 && out.Count == SCO_USB_OUT_RING_SLOTS, "one frame dropped, ring stays full");
    (void)ScoUsbOutRelease(&out, MS(t), Emit, &e);
    CHECK(out.ResyncSkips == 2, "the two headless remainder frames of the damaged packet are skipped");
    CHECK(e.Count == 1 + 42, "the 42 undamaged packets after it are all delivered");
    for (unsigned long i = 1; i < e.Count && i < 64; i++) {
        unsigned char expect = (unsigned char)(0xA0 + ((i + 1) % 16));
        if (e.Length[i] != 51 || e.Data[i][0] != 0x06 || e.Data[i][2] != 48 || e.Data[i][3] != expect) {
            intact = 0;
        }
    }
    CHECK(intact, "every delivered packet is a whole original packet, in order");
}

static void
TestInGeometry(void)
{
    static SCO_USB_IN in;
    SOURCE src;
    unsigned char iso[64];
    unsigned char payload[240];
    unsigned long got;
    unsigned int pos = 0;
    int layout = 1;

    printf("IN payload size per alternate setting\n");
    CHECK(ScoUsbInPayloadSize(0) == 0, "alt 0 (0 bytes) carries no voice");
    CHECK(ScoUsbInPayloadSize(9) == 24 && ScoUsbInPayloadSize(17) == 48 && ScoUsbInPayloadSize(25) == 72 &&
          ScoUsbInPayloadSize(33) == 96 && ScoUsbInPayloadSize(49) == 144,
          "alts 1-5: 3 x wMaxPacketSize - 3 (24/48/72/96/144)");
    CHECK(ScoUsbInPayloadSize(63) == 60, "alt 6: 60 bytes, one packet per isochronous packet");

    printf("IN re-framing of a 240-byte controller packet at alt 2\n");
    ScoUsbInReset(&in);
    memset(&src, 0, sizeof(src));
    AddSource(&src, 0x0006, 240, 0x00);
    for (unsigned int i = 0; i < 240; i++) {
        payload[i] = (unsigned char)i;
    }
    for (int k = 0; k < 15; k++) {
        got = ScoUsbInFill(&in, 17, iso, 17, Pull, &src);
        if (got != 17) {
            layout = 0;
            break;
        }
        if (k % 3 == 0) {
            if (iso[0] != 0x06 || iso[1] != 0x00 || iso[2] != 48 || memcmp(iso + 3, payload + pos, 14) != 0) {
                layout = 0;
            }
            pos += 14;
        } else {
            if (memcmp(iso, payload + pos, 17) != 0) {
                layout = 0;
            }
            pos += 17;
        }
    }
    CHECK(layout && pos == 240, "15 full 17-byte packets: 5 x (header in packet 1 of 3, 48 payload bytes), order kept");
    CHECK(in.FramedPackets == 5 && in.SourcePackets == 1, "one controller packet became five host packets");
    got = ScoUsbInFill(&in, 17, iso, 17, Pull, &src);
    CHECK(got == 0, "no data left: the next frame carries zero bytes");

    printf("IN partial accumulation and handle flags\n");
    ScoUsbInReset(&in);
    memset(&src, 0, sizeof(src));
    AddSource(&src, 0x2006, 30, 0x00);
    got = ScoUsbInFill(&in, 17, iso, 17, Pull, &src);
    CHECK(got == 0 && in.FifoCount == 30, "30 of 48 payload bytes: nothing sent, bytes held");
    AddSource(&src, 0x2006, 30, 30);
    got = ScoUsbInFill(&in, 17, iso, 17, Pull, &src);
    CHECK(got == 17 && iso[0] == 0x06 && iso[1] == 0x20 && iso[2] == 48 && iso[3] == 0 && iso[16] == 13,
          "second packet completes 48 bytes: header keeps packet-status flags (06 20), payload starts at byte 0");
    CHECK(in.FifoCount == 12, "the 12 surplus bytes wait for the next packet");

    printf("IN alt 6 (mSBC)\n");
    ScoUsbInReset(&in);
    memset(&src, 0, sizeof(src));
    AddSource(&src, 0x0007, 60, 0x55);
    got = ScoUsbInFill(&in, 63, iso, 63, Pull, &src);
    CHECK(got == 63 && iso[2] == 60 && iso[3] == 0x55 && iso[62] == (unsigned char)(0x55 + 59),
          "a 60-byte controller packet is one 63-byte isochronous packet");

    printf("IN link change, bad source, short buffer\n");
    ScoUsbInReset(&in);
    memset(&src, 0, sizeof(src));
    AddSource(&src, 0x0006, 30, 0x00);
    AddSource(&src, 0x0009, 48, 0x80);
    got = ScoUsbInFill(&in, 17, iso, 17, Pull, &src);
    CHECK(got == 17 && iso[0] == 0x09 && iso[3] == 0x80 && in.DroppedBytes == 30,
          "a new handle discards the old link's 30 queued bytes; the packet carries only the new link");
    ScoUsbInReset(&in);
    memset(&src, 0, sizeof(src));
    AddSource(&src, 0x0006, 48, 0x00);
    src.Length[0] = 40;   /* header says 48, only 37 payload bytes present */
    AddSource(&src, 0x0006, 48, 0x10);
    got = ScoUsbInFill(&in, 17, iso, 17, Pull, &src);
    CHECK(got == 17 && in.SourceRejected == 1 && iso[3] == 0x10, "an inconsistent controller packet is rejected, not framed");
    got = ScoUsbInFill(&in, 17, iso, 5, Pull, &src);
    CHECK(got == 5 && iso[0] == 0x10 + 14, "a short host buffer takes 5 bytes; the packet continues at payload byte 14");
}

int
main(void)
{
    TestPacing();
    TestOutReassembly();
    TestOutResync();
    TestInGeometry();

    if (g_fail != 0) {
        printf("\nSCO USB SELFTEST FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    printf("\nSCO USB SELFTEST PASSED\n");
    return 0;
}
