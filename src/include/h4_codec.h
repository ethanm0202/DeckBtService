/*
 * h4_codec.h - H4 packet framing codec for Bluetooth UART transport.
 *
 * The UART transport speaks H4: every packet on the wire is prefixed with a one-byte packet type.
 * USB does not: endpoints separate the streams (EP 0x81 interrupt IN = events, EP 0x02/0x82 bulk =
 * ACL, EP 0x03/0x83 isoch = SCO, EP0 = commands).
 *
 * This codec provides:
 *   1. Decoding: reassembles incoming H4 byte streams (which arrive in arbitrary chunks from the
 *      UART read pump) into discrete HCI packets.
 *   2. Encoding: prefixes outbound HCI packets with the appropriate H4 type byte.
 *   3. Resynchronisation: discards invalid framing bytes and rejects oversized packets without
 *      wedging the decoder, maintaining observability counters for diagnostics.
 *
 * Framing and the transport seam:
 * Buffers crossing the HCI_TRANSPORT seam (src/include/hci_transport.h) carry no H4 packet-type
 * prefix. When this decoder emits a packet via H4_PACKET_CALLBACK, the `Payload` pointer points
 * to the packet header (opcode/handle/event code + length) and excludes the H4 type byte:
 *   - Command (0x01): [Opcode LE16 (2B)] [Param Length (1B)] [Params...]
 *   - ACL     (0x02): [Handle+Flags LE16 (2B)] [Data Length LE16 (2B)] [Data...]
 *   - SCO     (0x03): [Handle+Flags LE16 (2B)] [Data Length (1B)] [Data...]
 *   - Event   (0x04): [Event Code (1B)] [Param Length (1B)] [Params...]
 *
 * `Length` is the total length of the packet header plus its payload (excluding H4 type byte).
 *
 * Standard packet framing (Bluetooth Core Specification Vol 4 Part A; upstream Linux hci_qca.c / btqca.c:
 * https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/hci_qca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137):
 *   - H4_PKT_COMMAND (0x01): HCI_COMMAND_PKT; Header: 3 bytes (Opcode LE16 + plen u8).
 *   - H4_PKT_ACL     (0x02): HCI_ACLDATA_PKT; Header: 4 bytes (Handle+flags LE16 + length LE16).
 *   - H4_PKT_SCO     (0x03): HCI_SCODATA_PKT; Header: 3 bytes (Handle+flags LE16 + length u8).
 *   - H4_PKT_EVENT   (0x04): HCI_EVENT_PKT;   Header: 2 bytes (Event code u8 + plen u8).
 *
 * Buffer sizing:
 * The internal accumulation buffer is fixed-size (no allocation).
 * Sized from the maximum packet the controller can emit, reported by HCI_Read_Buffer_Size:
 *   - ACL Data Packet Length: 1021 bytes payload (0x03FD) + 4-byte header = 1025 bytes.
 *   - SCO Data Packet Length: 255 bytes payload (0xFF) + 3-byte header = 258 bytes.
 *   - Event Parameter Length: 255 bytes payload (0xFF) + 2-byte header = 257 bytes.
 *   - Command Param Length:   255 bytes payload (0xFF) + 3-byte header = 258 bytes.
 *
 * Maximum legal packet across all streams is ACL: 1025 bytes (H4_MAX_PACKET_SIZE).
 * Any declared ACL length exceeding 1021 bytes is a framing error / oversized packet and is
 * rejected, counted in OversizedRejections, and resynchronised.
 */

#pragma once

#include <windows.h>

/* ---------------------------------------------------------------- H4 packet types */
#ifndef H4_PKT_COMMAND
#define H4_PKT_COMMAND              0x01u
#define H4_PKT_ACL                  0x02u
#define H4_PKT_SCO                  0x03u
#define H4_PKT_EVENT                0x04u
#endif

/* ---------------------------------------------------------------- Header lengths */
#define H4_COMMAND_HDR_SIZE         3u    /* Opcode LE16 (2B) + Parameter Length u8 (1B) */
#define H4_ACL_HDR_SIZE             4u    /* Handle+Flags LE16 (2B) + Data Length LE16 (2B) */
#define H4_SCO_HDR_SIZE             3u    /* Handle+Flags LE16 (2B) + Data Length u8 (1B) */
#define H4_EVENT_HDR_SIZE           2u    /* Event Code u8 (1B) + Parameter Length u8 (1B) */
#define H4_MAX_HDR_SIZE             4u    /* Maximum header size across all types */

/* ---------------------------------------------------------------- Max payload sizes */
#define H4_MAX_COMMAND_PAYLOAD_SIZE 255u  /* u8 plen */
#define H4_MAX_EVENT_PAYLOAD_SIZE   255u  /* u8 plen */
#define H4_MAX_SCO_PAYLOAD_SIZE     255u  /* HCI_Read_Buffer_Size Synchronous Data Packet Length */
#define H4_MAX_ACL_PAYLOAD_SIZE     1021u /* HCI_Read_Buffer_Size ACL Data Packet Length (hci_stub.c) */

/*
 * Maximum legal packet size (header + payload, excluding H4 type byte) across all packet types.
 * Derived from the maximum ACL packet (4-byte header + 1021-byte payload = 1025 bytes).
 */
#define H4_MAX_PACKET_SIZE          (H4_ACL_HDR_SIZE + H4_MAX_ACL_PAYLOAD_SIZE) /* 1025u */

/*
 * Decoder states.
 */
typedef enum _H4_DECODER_STATE {
    H4StateIdle = 0,     /* Awaiting H4 packet type byte */
    H4StateHeader,       /* Accumulating packet header bytes */
    H4StatePayload       /* Accumulating packet payload bytes */
} H4_DECODER_STATE;

/*
 * Packet emission callback.
 *
 * Parameters:
 *   Context: Caller-supplied opaque context pointer.
 *   Type:    H4 packet type (H4_PKT_COMMAND, H4_PKT_ACL, H4_PKT_SCO, H4_PKT_EVENT).
 *   Payload: Points to packet data starting at the packet header (excludes H4 type byte).
 *            Contains the complete packet header and payload data.
 *   Length:  Total packet length in bytes (header + payload, excluding H4 type byte).
 */
typedef void (*H4_PACKET_CALLBACK)(void *Context,
                                   unsigned char Type,
                                   const unsigned char *Payload,
                                   unsigned long Length);

/*
 * Out-of-band byte callback, consulted only between packets (decoder Idle) for a byte that is
 * not an H4 packet type. Returns nonzero to claim it (for example a Qualcomm in-band sleep byte,
 * QCA_IBS_* in upstream hci_qca.c); an unclaimed byte is discarded and counted in Desynchronised.
 * A byte inside a packet is always packet data and never reaches this callback.
 */
typedef unsigned char (*H4_OOB_CALLBACK)(void *Context, unsigned char Byte);

/*
 * H4_DECODER struct.
 *
 * Fixed-size internal accumulation buffer (no dynamic allocation).
 * Sized to H4_MAX_PACKET_SIZE (1025 bytes).
 */
typedef struct _H4_DECODER {
    H4_DECODER_STATE State;
    unsigned char    CurrentType;
    unsigned long    HeaderLength;
    unsigned long    PayloadLength;
    unsigned long    BufferLength;
    unsigned char    Discard;             /* the packet in progress is consumed but not emitted */
    unsigned char    Buffer[H4_MAX_PACKET_SIZE];

    /* Observability counters (following qca_init_fsm.h conventions) */
    unsigned long    BytesConsumed;       /* Total raw bytes fed into the decoder */
    unsigned long    CommandsEmitted;     /* Complete command packets emitted */
    unsigned long    AclPacketsEmitted;   /* Complete ACL data packets emitted */
    unsigned long    ScoPacketsEmitted;   /* Complete SCO data packets emitted */
    unsigned long    EventsEmitted;       /* Complete event packets emitted */
    unsigned long    Desynchronised;      /* Total bytes discarded due to framing / sync errors */
    unsigned long    OversizedRejections; /* Count of packets rejected for oversized / illegal length */
    unsigned long    PacketsDiscarded;    /* Packets consumed without emission (H4DecoderDiscardPacket) */
} H4_DECODER;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialises the decoder and clears all state and counters to zero.
 */
void H4DecoderInit(H4_DECODER *Decoder);

/*
 * Discards the packet in progress, if any: its remaining bytes are still consumed, so the decoder
 * stays in step with the byte stream, but the packet is not emitted. Forgetting the framing
 * instead would read the rest of that packet as new packets (payload bytes 0x01-0x04 taken as
 * H4 packet types). Between packets this has no effect.
 */
void H4DecoderDiscardPacket(H4_DECODER *Decoder);

/*
 * Consumes a chunk of received bytes and emits zero or more complete packets via Callback.
 *
 * The callback is invoked synchronously during the feed call as soon as each packet completes.
 * The payload buffer passed to Callback is owned by the decoder and valid only for the duration
 * of the callback invocation.
 */
void H4DecoderFeed(H4_DECODER *Decoder,
                   const unsigned char *Data,
                   unsigned long Length,
                   H4_PACKET_CALLBACK Callback,
                   void *Context);

/* H4DecoderFeed plus an optional out-of-band byte callback (NULL behaves exactly as Feed). */
void H4DecoderFeedEx(H4_DECODER *Decoder,
                     const unsigned char *Data,
                     unsigned long Length,
                     H4_PACKET_CALLBACK Callback,
                     H4_OOB_CALLBACK OutOfBand,
                     void *Context);

/*
 * H4DecoderFeedEx that stops right after the MaxPackets-th emitted packet, so a caller with room
 * for only MaxPackets can hand the rest over later. Returns the bytes consumed; the caller feeds
 * the remainder next. MaxPackets 0 consumes nothing.
 */
unsigned long H4DecoderFeedLimited(H4_DECODER *Decoder,
                                   const unsigned char *Data,
                                   unsigned long Length,
                                   unsigned long MaxPackets,
                                   H4_PACKET_CALLBACK Callback,
                                   H4_OOB_CALLBACK OutOfBand,
                                   void *Context);

/*
 * Encodes an outbound packet by writing the H4 `Type` byte followed by `Packet`.
 *
 * Parameters:
 *   Type:         H4_PKT_COMMAND, H4_PKT_ACL, or H4_PKT_SCO.
 *   Packet:       Packet bytes to prefix (must start with packet header, no H4 prefix).
 *   PacketLength: Length of Packet in bytes.
 *   OutBuffer:    Caller-supplied destination buffer.
 *   OutCapacity:  Capacity of OutBuffer in bytes (must be >= 1 + PacketLength).
 *
 * Returns:
 *   Total bytes written (1 + PacketLength), or 0 if OutCapacity is insufficient or inputs invalid.
 *   Performs no dynamic allocation.
 */
unsigned long H4EncodePacket(unsigned char Type,
                             const unsigned char *Packet,
                             unsigned long PacketLength,
                             unsigned char *OutBuffer,
                             unsigned long OutCapacity);

#ifdef __cplusplus
}
#endif
