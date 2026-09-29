/*
 * h4_codec.c - H4 packet framing codec implementation.
 *
 * Implements H4 byte-stream reassembly, packet encoding, resynchronisation,
 * and observability counters.
 */

#include "../include/h4_codec.h"

#include <string.h>

/*
 * Returns the expected header length (in bytes) for the given H4 packet type,
 * or 0 if the type is unknown/invalid.
 */
static unsigned long
H4GetHeaderLength(unsigned char Type)
{
    switch (Type) {
    case H4_PKT_COMMAND: return H4_COMMAND_HDR_SIZE; /* 3 */
    case H4_PKT_ACL:     return H4_ACL_HDR_SIZE;     /* 4 */
    case H4_PKT_SCO:     return H4_SCO_HDR_SIZE;     /* 3 */
    case H4_PKT_EVENT:   return H4_EVENT_HDR_SIZE;   /* 2 */
    default:             return 0u;
    }
}

/*
 * Returns the maximum legal payload length for the given H4 packet type.
 */
static unsigned long
H4GetMaxPayload(unsigned char Type)
{
    switch (Type) {
    case H4_PKT_COMMAND: return H4_MAX_COMMAND_PAYLOAD_SIZE; /* 255 */
    case H4_PKT_ACL:     return H4_MAX_ACL_PAYLOAD_SIZE;     /* 1021 */
    case H4_PKT_SCO:     return H4_MAX_SCO_PAYLOAD_SIZE;     /* 255 */
    case H4_PKT_EVENT:   return H4_MAX_EVENT_PAYLOAD_SIZE;   /* 255 */
    default:             return 0u;
    }
}

/*
 * Parses the payload length from a complete packet header buffer.
 */
static unsigned long
H4ParsePayloadLength(unsigned char Type, const unsigned char *Hdr)
{
    switch (Type) {
    case H4_PKT_COMMAND:
        /* [Opcode LE16 (2B)][Plen u8 (1B)] */
        return (unsigned long)Hdr[2];

    case H4_PKT_ACL:
        /* [Handle+Flags LE16 (2B)][Data Length LE16 (2B)] */
        return (unsigned long)Hdr[2] | ((unsigned long)Hdr[3] << 8);

    case H4_PKT_SCO:
        /* [Handle+Flags LE16 (2B)][Data Length u8 (1B)] */
        return (unsigned long)Hdr[2];

    case H4_PKT_EVENT:
        /* [Event Code u8 (1B)][Plen u8 (1B)] */
        return (unsigned long)Hdr[1];

    default:
        return 0u;
    }
}

/*
 * Records the emission of a complete packet in the observability counters.
 */
static void
H4RecordEmission(H4_DECODER *Decoder, unsigned char Type)
{
    switch (Type) {
    case H4_PKT_COMMAND: Decoder->CommandsEmitted++;   break;
    case H4_PKT_ACL:     Decoder->AclPacketsEmitted++; break;
    case H4_PKT_SCO:     Decoder->ScoPacketsEmitted++; break;
    case H4_PKT_EVENT:   Decoder->EventsEmitted++;     break;
    default:             break;
    }
}

/*
 * Hands a complete packet to the caller, unless it was discarded, and returns to Idle.
 * Returns 1 if the packet was emitted.
 */
static unsigned char
H4FinishPacket(H4_DECODER *Decoder, H4_PACKET_CALLBACK Callback, void *Context)
{
    unsigned char emitted = 0u;

    if (Decoder->Discard) {
        Decoder->PacketsDiscarded++;
    } else {
        H4RecordEmission(Decoder, Decoder->CurrentType);
        Callback(Context, Decoder->CurrentType, Decoder->Buffer, Decoder->BufferLength);
        emitted = 1u;
    }
    Decoder->State = H4StateIdle;
    Decoder->CurrentType = 0;
    Decoder->HeaderLength = 0u;
    Decoder->PayloadLength = 0u;
    Decoder->BufferLength = 0u;
    Decoder->Discard = 0u;
    return emitted;
}

/*
 * Processes a single byte through the decoder state machine. Returns 1 if a packet was emitted.
 */
static unsigned char
H4FeedByte(H4_DECODER *Decoder,
           unsigned char Byte,
           H4_PACKET_CALLBACK Callback,
           H4_OOB_CALLBACK OutOfBand,
           void *Context)
{
    unsigned char emitted = 0u;

    switch (Decoder->State) {
    case H4StateIdle: {
        unsigned long hdrLen = H4GetHeaderLength(Byte);
        if (hdrLen == 0u) {
            /* Not a packet type: out-of-band if claimed, otherwise discard and keep scanning */
            if (OutOfBand == NULL || OutOfBand(Context, Byte) == 0u) {
                Decoder->Desynchronised++;
            }
        } else {
            Decoder->CurrentType = Byte;
            Decoder->HeaderLength = hdrLen;
            Decoder->PayloadLength = 0u;
            Decoder->BufferLength = 0u;
            Decoder->State = H4StateHeader;
        }
        break;
    }

    case H4StateHeader: {
        Decoder->Buffer[Decoder->BufferLength++] = Byte;
        if (Decoder->BufferLength == Decoder->HeaderLength) {
            /* Header complete. Parse and validate declared payload length. */
            unsigned long payloadLen = H4ParsePayloadLength(Decoder->CurrentType, Decoder->Buffer);
            unsigned long maxPayload = H4GetMaxPayload(Decoder->CurrentType);

            if (payloadLen > maxPayload) {
                /*
                 * Framing error / oversized length. Reject and resynchronise.
                 * The packet cannot be legal: discard the type byte and the
                 * accumulated header bytes, count them in Desynchronised,
                 * and return to H4StateIdle so the next valid packet is received.
                 */
                Decoder->OversizedRejections++;
                Decoder->Desynchronised += 1u + Decoder->BufferLength;

                Decoder->State = H4StateIdle;
                Decoder->CurrentType = 0;
                Decoder->HeaderLength = 0u;
                Decoder->PayloadLength = 0u;
                Decoder->BufferLength = 0u;
                Decoder->Discard = 0u;
            } else {
                Decoder->PayloadLength = payloadLen;
                if (payloadLen == 0u) {
                    /* Zero-length payload: the packet is complete */
                    emitted = H4FinishPacket(Decoder, Callback, Context);
                } else {
                    Decoder->State = H4StatePayload;
                }
            }
        }
        break;
    }

    case H4StatePayload: {
        Decoder->Buffer[Decoder->BufferLength++] = Byte;
        if (Decoder->BufferLength == (Decoder->HeaderLength + Decoder->PayloadLength)) {
            emitted = H4FinishPacket(Decoder, Callback, Context);
        }
        break;
    }

    default:
        /* Unreachable state recovery */
        Decoder->State = H4StateIdle;
        Decoder->BufferLength = 0u;
        Decoder->Discard = 0u;
        break;
    }
    return emitted;
}

void
H4DecoderInit(H4_DECODER *Decoder)
{
    if (Decoder == NULL) {
        return;
    }
    memset(Decoder, 0, sizeof(*Decoder));
    Decoder->State = H4StateIdle;
}

void
H4DecoderDiscardPacket(H4_DECODER *Decoder)
{
    if (Decoder != NULL && Decoder->State != H4StateIdle) {
        Decoder->Discard = 1u;
    }
}

void
H4DecoderFeedEx(H4_DECODER *Decoder,
                const unsigned char *Data,
                unsigned long Length,
                H4_PACKET_CALLBACK Callback,
                H4_OOB_CALLBACK OutOfBand,
                void *Context)
{
    (void)H4DecoderFeedLimited(Decoder, Data, Length, (unsigned long)-1, Callback, OutOfBand, Context);
}

unsigned long
H4DecoderFeedLimited(H4_DECODER *Decoder,
                     const unsigned char *Data,
                     unsigned long Length,
                     unsigned long MaxPackets,
                     H4_PACKET_CALLBACK Callback,
                     H4_OOB_CALLBACK OutOfBand,
                     void *Context)
{
    unsigned long i;
    unsigned long emitted = 0u;

    if (Decoder == NULL || Data == NULL || Callback == NULL) {
        return 0u;
    }

    for (i = 0u; i < Length && emitted < MaxPackets; i++) {
        Decoder->BytesConsumed++;
        emitted += H4FeedByte(Decoder, Data[i], Callback, OutOfBand, Context);
    }
    return i;
}

void
H4DecoderFeed(H4_DECODER *Decoder,
              const unsigned char *Data,
              unsigned long Length,
              H4_PACKET_CALLBACK Callback,
              void *Context)
{
    H4DecoderFeedEx(Decoder, Data, Length, Callback, NULL, Context);
}

unsigned long
H4EncodePacket(unsigned char Type,
               const unsigned char *Packet,
               unsigned long PacketLength,
               unsigned char *OutBuffer,
               unsigned long OutCapacity)
{
    if (OutBuffer == NULL || OutCapacity < (1u + PacketLength)) {
        return 0u;
    }
    if (PacketLength > 0u && Packet == NULL) {
        return 0u;
    }
    if (Type != H4_PKT_COMMAND &&
        Type != H4_PKT_ACL &&
        Type != H4_PKT_SCO &&
        Type != H4_PKT_EVENT) {
        return 0u;
    }

    OutBuffer[0] = Type;
    if (PacketLength > 0u) {
        memcpy(OutBuffer + 1u, Packet, (size_t)PacketLength);
    }
    return 1u + PacketLength;
}
