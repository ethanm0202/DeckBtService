/*
 * sco_usb.h - SCO voice between the radio's USB isochronous endpoints and HCI SCO packets.
 *
 * BTHUSB moves synchronous voice (SCO/eSCO, Hands-Free) over the SCO interface's isochronous
 * endpoints 0x03 (OUT) and 0x83 (IN), one packet per 1 ms frame at the selected alternate setting's
 * wMaxPacketSize (9/17/25/33/49/63 bytes, src/include/usb_descriptors.h). The UART carries complete
 * HCI SCO packets ([handle+flags LE16][length u8][payload], no H4 byte at this seam). This module
 * converts between the two and supplies the time base BTHUSB would otherwise get from a real bus.
 *
 * Pacing (ScoUsbSchedule, ScoUsbOutPacketSpan):
 * SCO has no host-side flow control, and BTHUSB measures audio pacing solely by the rate
 * at which isochronous transfers complete. Completing transfers immediately would cause
 * BTHUSB to transmit ahead of real time, leading to buffer overflow and dropped audio.
 * The virtual bus completes each transfer only after the air time represented by the
 * transfer has elapsed relative to the previous transfer on that endpoint.
 *
 * For alternate settings 1-5 (CVSD / linear PCM), each isochronous packet corresponds
 * to a 1 ms frame (e.g. alt setting 2: 16 bytes/ms = 8 kHz 16-bit voice).
 * For alternate setting 6 (63-byte packets, mSBC), each non-empty packet carries one
 * complete 60-byte mSBC frame representing 7.5 ms of audio. Pacing these transfers at
 * 1 ms would deliver frames at over six times the link's transmission rate (133.3 frames/sec),
 * causing buffer exhaustion and audio loss. Non-empty packets on alternate setting 6 are
 * therefore paced at 7.5 ms intervals (SCO_USB_MSBC_FRAME_100NS).
 *
 * Host -> Controller (SCO_USB_OUT):
 * Each OUT isochronous packet's bytes are queued with the time of the frame that carries them and
 * released at that time, so the controller receives voice at the air rate instead of in bursts the
 * size of an URB. Released bytes are a stream: HCI SCO packets are reassembled from their 3-byte
 * header however they were cut into isochronous packets. The ring drops its oldest frame when full
 * (bounded latency); after a drop, reassembly restarts only at a frame whose first bytes carry the
 * connection handle last seen, so a lost header cannot desynchronise the stream for good.
 *
 * Controller -> Host (SCO_USB_IN):
 * The controller chooses its own SCO packet size over the UART (up to 240 bytes). A USB
 * radio instead emits the geometry of the selected setting (Core Vol 4 Part B 2.1.1): for alternate
 * settings 1-5 one HCI SCO packet of 3 * wMaxPacketSize - 3 payload bytes spans exactly three
 * isochronous packets (alt 2: 3 + 48 = 51 = 3 x 17); at wMaxPacketSize 63 (alt 6, mSBC) one packet of
 * 60 payload bytes fills one isochronous packet. The IN framer re-cuts the controller's payload
 * stream into that geometry, keeping the controller's handle and packet-status flags, so BTHUSB
 * sees what a USB controller would send whether it parses by header or by packet boundary. A frame
 * with no complete packet to send carries zero bytes: gaps are lost, never invented. The framer pulls
 * a controller packet only when it lacks one packet's payload, so at most two controller packets wait
 * here; staleness beyond that is bounded by the bridge's overwrite-oldest SCO FIFO.
 */

#pragma once

#include <windows.h>

#define SCO_USB_HEADER_SIZE       3u
#define SCO_USB_MAX_PACKET        (SCO_USB_HEADER_SIZE + 255u)
/* Largest wMaxPacketSize on the SCO interface (alternate setting 6). */
#define SCO_USB_MAX_ISO_PACKET    63u
/* One USB frame (bInterval 4 at high speed = 8 microframes) in 100 ns units. */
#define SCO_USB_FRAME_100NS       10000ull
/* 128 frames = 128 ms of OUT voice queued ahead at most. */
#define SCO_USB_OUT_RING_SLOTS    128u
/* Controller payload waiting to be re-cut: about 30 ms of 16-bit CVSD. */
#define SCO_USB_IN_FIFO_BYTES     512u

/* Receives one complete HCI SCO packet (header included). */
typedef void (*SCO_USB_EMIT)(void *Context, const unsigned char *Packet, unsigned long Length);
/* Pops one complete controller SCO packet; returns 0 when none is queued. */
typedef unsigned char (*SCO_USB_PULL)(void *Context, unsigned char *Packet, unsigned long Capacity,
                                      unsigned long *Written);

typedef struct _SCO_USB_CLOCK {
    unsigned long long NextFree;   /* end of the last scheduled transfer on this endpoint */
} SCO_USB_CLOCK;

typedef struct _SCO_USB_OUT_CHUNK {
    unsigned long long Due;
    unsigned char      Length;
    unsigned char      Data[SCO_USB_MAX_ISO_PACKET];
} SCO_USB_OUT_CHUNK;

typedef struct _SCO_USB_OUT {
    SCO_USB_OUT_CHUNK Ring[SCO_USB_OUT_RING_SLOTS];
    unsigned int      Head;
    unsigned int      Count;
    unsigned char     Packet[SCO_USB_MAX_PACKET];
    unsigned int      Have;          /* bytes of Packet reassembled so far */
    unsigned char     Resync;        /* 1 after a drop: wait for a frame starting with LastHandle */
    unsigned short    LastHandle;    /* 12-bit handle of the last complete packet; 0xFFFF = none */
    unsigned long     Emitted;       /* complete packets handed to Emit */
    unsigned long     RingDrops;     /* frames dropped because the ring was full */
    unsigned long     ResyncSkips;   /* frames discarded while resynchronising */
} SCO_USB_OUT;

typedef struct _SCO_USB_IN {
    unsigned char  Fifo[SCO_USB_IN_FIFO_BYTES];
    unsigned int   FifoHead;
    unsigned int   FifoCount;
    unsigned short HandleField;      /* handle + packet-status flags of the latest source packet */
    unsigned char  HaveHandle;
    unsigned char  Frame[SCO_USB_MAX_PACKET];
    unsigned int   FrameLength;      /* packet being cut into isochronous packets */
    unsigned int   FrameOffset;
    unsigned long  SourcePackets;    /* controller packets accepted */
    unsigned long  SourceRejected;   /* controller packets with an inconsistent length */
    unsigned long  FramedPackets;    /* packets built for the host */
    unsigned long  DroppedBytes;     /* payload dropped on FIFO overflow or handle change */
    unsigned long  LastSourceLength; /* payload length of the latest controller packet */
} SCO_USB_IN;

/* mSBC: 120 samples at 16 kHz per 60-byte frame. */
#define SCO_USB_MSBC_FRAME_100NS  75000ull

/*
 * Returns the completion time of a transfer standing for Span (100 ns units) submitted at Now: it
 * starts where the endpoint's previous transfer ends, or at Now if the endpoint has been idle since.
 */
unsigned long long ScoUsbSchedule(_Inout_ SCO_USB_CLOCK *Clock, _In_ unsigned long long Now,
                                  _In_ unsigned long long Span);

/* Air time one OUT isochronous packet of Length bytes stands for on an endpoint of MaxPacketSize. */
unsigned long long ScoUsbOutPacketSpan(_In_ unsigned long MaxPacketSize, _In_ unsigned long Length);

void ScoUsbOutReset(_Out_ SCO_USB_OUT *Out);
/* Queues one OUT isochronous packet's bytes for release at Due. Zero-length packets queue nothing. */
void ScoUsbOutPush(_Inout_ SCO_USB_OUT *Out, _In_ unsigned long long Due,
                   _In_reads_bytes_(Length) const unsigned char *Data, _In_ unsigned long Length);
/* Releases every frame due at or before Now; returns the number of packets handed to Emit. */
unsigned long ScoUsbOutRelease(_Inout_ SCO_USB_OUT *Out, _In_ unsigned long long Now,
                               _In_ SCO_USB_EMIT Emit, _In_opt_ void *Context);

/* HCI SCO payload per packet for a wMaxPacketSize; 0 if the setting carries no voice. */
unsigned long ScoUsbInPayloadSize(_In_ unsigned long MaxPacketSize);
void ScoUsbInReset(_Out_ SCO_USB_IN *In);
/*
 * Fills one IN isochronous packet of at most Capacity bytes (Capacity <= MaxPacketSize), pulling
 * controller packets as needed. Returns the bytes written; 0 = nothing to send this frame.
 */
unsigned long ScoUsbInFill(_Inout_ SCO_USB_IN *In, _In_ unsigned long MaxPacketSize,
                           _Out_writes_bytes_to_(Capacity, return) unsigned char *Buffer,
                           _In_ unsigned long Capacity, _In_ SCO_USB_PULL Pull, _In_opt_ void *Context);
