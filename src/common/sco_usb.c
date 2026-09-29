/*
 * sco_usb.c - SCO voice framing and pacing between USB isochronous packets and HCI SCO packets.
 * Contract and rationale: src/include/sco_usb.h.
 */

#include <string.h>
#include "../include/sco_usb.h"

#define SCO_USB_NO_HANDLE 0xFFFFu

static unsigned short
ScoUsbHandleOf(const unsigned char *Header)
{
    return (unsigned short)((Header[0] | ((unsigned int)Header[1] << 8)) & 0x0FFFu);
}

unsigned long long
ScoUsbSchedule(SCO_USB_CLOCK *Clock, unsigned long long Now, unsigned long long Span)
{
    unsigned long long start = (Clock->NextFree > Now) ? Clock->NextFree : Now;

    Clock->NextFree = start + Span;
    return Clock->NextFree;
}

unsigned long long
ScoUsbOutPacketSpan(unsigned long MaxPacketSize, unsigned long Length)
{
    if (MaxPacketSize >= SCO_USB_MAX_ISO_PACKET && Length > SCO_USB_HEADER_SIZE) {
        return SCO_USB_MSBC_FRAME_100NS;   /* alt 6: one whole mSBC frame per packet */
    }
    return SCO_USB_FRAME_100NS;
}

/* ---------------------------------------------------------------- host -> controller */

void
ScoUsbOutReset(SCO_USB_OUT *Out)
{
    memset(Out, 0, sizeof(*Out));
    Out->LastHandle = SCO_USB_NO_HANDLE;
}

void
ScoUsbOutPush(SCO_USB_OUT *Out, unsigned long long Due, const unsigned char *Data, unsigned long Length)
{
    while (Length > 0u) {
        unsigned long n = (Length > SCO_USB_MAX_ISO_PACKET) ? SCO_USB_MAX_ISO_PACKET : Length;
        SCO_USB_OUT_CHUNK *slot;

        if (Out->Count == SCO_USB_OUT_RING_SLOTS) {
            /* Oldest frame lost: the packet being reassembled is no longer contiguous. */
            Out->Head = (Out->Head + 1u) % SCO_USB_OUT_RING_SLOTS;
            Out->Count--;
            Out->RingDrops++;
            Out->Have = 0u;
            Out->Resync = 1u;
        }
        slot = &Out->Ring[(Out->Head + Out->Count) % SCO_USB_OUT_RING_SLOTS];
        slot->Due = Due;
        slot->Length = (unsigned char)n;
        memcpy(slot->Data, Data, (size_t)n);
        Out->Count++;
        Data += n;
        Length -= n;
    }
}

unsigned long
ScoUsbOutRelease(SCO_USB_OUT *Out, unsigned long long Now, SCO_USB_EMIT Emit, void *Context)
{
    unsigned long emitted = 0u;

    while (Out->Count > 0u && Out->Ring[Out->Head].Due <= Now) {
        const SCO_USB_OUT_CHUNK *chunk = &Out->Ring[Out->Head];
        unsigned int i = 0u;

        if (Out->Resync) {
            if (Out->LastHandle != SCO_USB_NO_HANDLE &&
                (chunk->Length < 2u || ScoUsbHandleOf(chunk->Data) != Out->LastHandle)) {
                /* Not a packet start on the stream in progress: skip the whole frame. */
                Out->ResyncSkips++;
                i = chunk->Length;
            } else {
                Out->Resync = 0u;
            }
        }
        while (i < chunk->Length) {
            unsigned int need = (Out->Have < SCO_USB_HEADER_SIZE)
                              ? SCO_USB_HEADER_SIZE
                              : SCO_USB_HEADER_SIZE + Out->Packet[2];
            unsigned int take = need - Out->Have;

            if (take > (unsigned int)chunk->Length - i) {
                take = (unsigned int)chunk->Length - i;
            }
            memcpy(&Out->Packet[Out->Have], &chunk->Data[i], take);
            Out->Have += take;
            i += take;
            if (Out->Have >= SCO_USB_HEADER_SIZE &&
                Out->Have == SCO_USB_HEADER_SIZE + (unsigned int)Out->Packet[2]) {
                Out->LastHandle = ScoUsbHandleOf(Out->Packet);
                Emit(Context, Out->Packet, Out->Have);
                Out->Emitted++;
                emitted++;
                Out->Have = 0u;
            }
        }
        Out->Head = (Out->Head + 1u) % SCO_USB_OUT_RING_SLOTS;
        Out->Count--;
    }
    return emitted;
}

/* ---------------------------------------------------------------- controller -> host */

unsigned long
ScoUsbInPayloadSize(unsigned long MaxPacketSize)
{
    if (MaxPacketSize < 4u) {
        return 0u;                      /* alternate setting 0: no bandwidth, no voice */
    }
    if (MaxPacketSize >= SCO_USB_MAX_ISO_PACKET) {
        unsigned long payload = MaxPacketSize - SCO_USB_HEADER_SIZE;
        return (payload > 255u) ? 255u : payload;   /* one packet per isochronous packet */
    }
    return 3u * MaxPacketSize - SCO_USB_HEADER_SIZE; /* one packet per three isochronous packets */
}

void
ScoUsbInReset(SCO_USB_IN *In)
{
    memset(In, 0, sizeof(*In));
}

static void
ScoUsbInAccept(SCO_USB_IN *In, const unsigned char *Packet, unsigned long Length)
{
    unsigned short field;
    unsigned int payload;
    unsigned int i;

    if (Length < SCO_USB_HEADER_SIZE || Length != SCO_USB_HEADER_SIZE + (unsigned long)Packet[2]) {
        In->SourceRejected++;
        return;
    }
    field = (unsigned short)(Packet[0] | ((unsigned int)Packet[1] << 8));
    if (In->HaveHandle && (field & 0x0FFFu) != (In->HandleField & 0x0FFFu)) {
        /* A different link: its voice must not be spliced onto the previous one's. */
        In->DroppedBytes += In->FifoCount;
        In->FifoHead = 0u;
        In->FifoCount = 0u;
    }
    In->HandleField = field;
    In->HaveHandle = 1u;
    payload = Packet[2];
    In->SourcePackets++;
    In->LastSourceLength = payload;
    /* Fill pulls only while FifoCount < one payload (<= 255), so this never exceeds 510 bytes. */
    for (i = 0u; i < payload; i++) {
        In->Fifo[(In->FifoHead + In->FifoCount) % SCO_USB_IN_FIFO_BYTES] = Packet[SCO_USB_HEADER_SIZE + i];
        In->FifoCount++;
    }
}

unsigned long
ScoUsbInFill(SCO_USB_IN *In, unsigned long MaxPacketSize, unsigned char *Buffer, unsigned long Capacity,
             SCO_USB_PULL Pull, void *Context)
{
    unsigned long n;

    if (In->FrameOffset >= In->FrameLength) {
        unsigned long payload = ScoUsbInPayloadSize(MaxPacketSize);
        unsigned char source[SCO_USB_MAX_PACKET];
        unsigned int i;

        In->FrameLength = 0u;
        In->FrameOffset = 0u;
        if (payload == 0u) {
            return 0u;
        }
        while (In->FifoCount < payload) {
            unsigned long written = 0u;
            if (!Pull(Context, source, sizeof(source), &written)) {
                break;
            }
            ScoUsbInAccept(In, source, written);
        }
        if (In->FifoCount < payload) {
            return 0u;                  /* not a whole packet yet: this frame carries nothing */
        }
        In->Frame[0] = (unsigned char)(In->HandleField & 0xFFu);
        In->Frame[1] = (unsigned char)(In->HandleField >> 8);
        In->Frame[2] = (unsigned char)payload;
        for (i = 0u; i < payload; i++) {
            In->Frame[SCO_USB_HEADER_SIZE + i] = In->Fifo[In->FifoHead];
            In->FifoHead = (In->FifoHead + 1u) % SCO_USB_IN_FIFO_BYTES;
        }
        In->FifoCount -= payload;
        In->FrameLength = SCO_USB_HEADER_SIZE + payload;
        In->FramedPackets++;
    }
    n = In->FrameLength - In->FrameOffset;
    if (n > MaxPacketSize) {
        n = MaxPacketSize;
    }
    if (n > Capacity) {
        n = Capacity;
    }
    memcpy(Buffer, &In->Frame[In->FrameOffset], (size_t)n);
    In->FrameOffset += n;
    return n;
}
