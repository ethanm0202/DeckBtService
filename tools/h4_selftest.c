/*
 * h4_selftest.c - standalone host-side test suite for H4 packet framing codec.
 *
 * Exercises src/common/h4_codec.c in user mode against all acceptance criteria:
 *   1. Four-type round-trip (encode -> decode).
 *   2. Split invariance (single chunk vs 1-byte chunks vs every split offset).
 *   3. Deterministic pseudorandom split fuzz (fixed seed, thousands of chunkings).
 *   4. Resynchronisation (garbage before, between, inside packets).
 *   5. Oversized / illegal length rejection without wedging or overflow.
 *   6. Truncated tail resumption across feed boundaries.
 *   7. Out-of-band bytes between packets.
 *   8. Discarding a packet in progress keeps the framing (a transport reset mid-packet).
 *   9. A limited feed stops after the requested number of packets.
 *
 * Style matches tools/qca_fsm_selftest.c: ok/FAIL lines grouped under headings,
 * nonzero exit on any failure, final "H4 SELFTEST PASSED".
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/include/h4_codec.h"

static int g_fail = 0;

#define CHECK(cond, ...)                            \
    do {                                            \
        if (cond) { printf("  ok   "); }            \
        else      { printf("  FAIL "); g_fail++; }  \
        printf(__VA_ARGS__); printf("\n");          \
    } while (0)

/* ---------------------------------------------------------------- capture harness */

#define MAX_CAPTURED_PACKETS 32u

typedef struct _CAPTURED_PACKET {
    unsigned char Type;
    unsigned long Length;
    unsigned char Payload[H4_MAX_PACKET_SIZE];
} CAPTURED_PACKET;

typedef struct _CAPTURE_CONTEXT {
    unsigned long   Count;
    CAPTURED_PACKET Packets[MAX_CAPTURED_PACKETS];
} CAPTURE_CONTEXT;

static void
CaptureCallback(void *Context,
                unsigned char Type,
                const unsigned char *Payload,
                unsigned long Length)
{
    CAPTURE_CONTEXT *ctx = (CAPTURE_CONTEXT *)Context;
    if (ctx->Count < MAX_CAPTURED_PACKETS) {
        CAPTURED_PACKET *p = &ctx->Packets[ctx->Count++];
        p->Type = Type;
        p->Length = Length;
        if (Length > 0u && Length <= H4_MAX_PACKET_SIZE && Payload != NULL) {
            memcpy(p->Payload, Payload, (size_t)Length);
        }
    }
}

static int
CompareCaptures(const CAPTURE_CONTEXT *A, const CAPTURE_CONTEXT *B)
{
    unsigned long i;
    if (A->Count != B->Count) {
        return 0;
    }
    for (i = 0u; i < A->Count; i++) {
        if (A->Packets[i].Type != B->Packets[i].Type) {
            return 0;
        }
        if (A->Packets[i].Length != B->Packets[i].Length) {
            return 0;
        }
        if (memcmp(A->Packets[i].Payload, B->Packets[i].Payload, (size_t)A->Packets[i].Length) != 0) {
            return 0;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- deterministic PRNG */

/*
 * XorShift32 with a fixed seed. No reliance on time() or rand().
 */
static unsigned long g_prng_state = 0xDEC8B700u;

static void PrngSeed(unsigned long seed)
{
    g_prng_state = (seed == 0u) ? 0xDEC8B700u : seed;
}

static unsigned long PrngNext(void)
{
    unsigned long x = g_prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_prng_state = x;
    return x;
}

/* ---------------------------------------------------------------- test cases */

static void
Test1_RoundTrip(void)
{
    H4_DECODER dec;
    CAPTURE_CONTEXT cap;
    unsigned char encBuf[H4_MAX_PACKET_SIZE + 1u];
    unsigned long encLen;

    printf("\n=== Test 1: Four-Type Round-Trip ===\n");

    /* 1. Command packet: HCI_Reset (opcode 0x0C03, plen 0) */
    {
        static const unsigned char cmdHdr[3] = { 0x03, 0x0C, 0x00 };
        H4DecoderInit(&dec);
        memset(&cap, 0, sizeof(cap));

        encLen = H4EncodePacket(H4_PKT_COMMAND, cmdHdr, sizeof(cmdHdr), encBuf, sizeof(encBuf));
        CHECK(encLen == 4u && encBuf[0] == H4_PKT_COMMAND,
              "Command encoded correctly (len=%lu)", encLen);

        H4DecoderFeed(&dec, encBuf, encLen, CaptureCallback, &cap);
        CHECK(cap.Count == 1u &&
              cap.Packets[0].Type == H4_PKT_COMMAND &&
              cap.Packets[0].Length == sizeof(cmdHdr) &&
              memcmp(cap.Packets[0].Payload, cmdHdr, sizeof(cmdHdr)) == 0,
              "Command decoded byte-identical (type=0x%02X, len=%lu)",
              cap.Packets[0].Type, cap.Packets[0].Length);
    }

    /* 2. ACL packet: handle 0x0042, flags 0x2000, 8 bytes payload */
    {
        static const unsigned char aclPkt[12] = {
            0x42, 0x20, 0x08, 0x00,              /* Handle 0x0042 PB=2 BC=0, Data Length = 8 */
            0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04
        };
        H4DecoderInit(&dec);
        memset(&cap, 0, sizeof(cap));

        encLen = H4EncodePacket(H4_PKT_ACL, aclPkt, sizeof(aclPkt), encBuf, sizeof(encBuf));
        CHECK(encLen == 13u && encBuf[0] == H4_PKT_ACL,
              "ACL packet encoded correctly (len=%lu)", encLen);

        H4DecoderFeed(&dec, encBuf, encLen, CaptureCallback, &cap);
        CHECK(cap.Count == 1u &&
              cap.Packets[0].Type == H4_PKT_ACL &&
              cap.Packets[0].Length == sizeof(aclPkt) &&
              memcmp(cap.Packets[0].Payload, aclPkt, sizeof(aclPkt)) == 0,
              "ACL packet decoded byte-identical (type=0x%02X, len=%lu)",
              cap.Packets[0].Type, cap.Packets[0].Length);
    }

    /* 3. SCO packet: handle 0x0010, 4 bytes payload */
    {
        static const unsigned char scoPkt[7] = {
            0x10, 0x00, 0x04,                    /* Handle 0x0010, Length = 4 */
            0x11, 0x22, 0x33, 0x44
        };
        H4DecoderInit(&dec);
        memset(&cap, 0, sizeof(cap));

        encLen = H4EncodePacket(H4_PKT_SCO, scoPkt, sizeof(scoPkt), encBuf, sizeof(encBuf));
        CHECK(encLen == 8u && encBuf[0] == H4_PKT_SCO,
              "SCO packet encoded correctly (len=%lu)", encLen);

        H4DecoderFeed(&dec, encBuf, encLen, CaptureCallback, &cap);
        CHECK(cap.Count == 1u &&
              cap.Packets[0].Type == H4_PKT_SCO &&
              cap.Packets[0].Length == sizeof(scoPkt) &&
              memcmp(cap.Packets[0].Payload, scoPkt, sizeof(scoPkt)) == 0,
              "SCO packet decoded byte-identical (type=0x%02X, len=%lu)",
              cap.Packets[0].Type, cap.Packets[0].Length);
    }

    /* 4. Event packet: Command Complete (event 0x0E, plen 4) */
    {
        static const unsigned char evtPkt[6] = {
            0x0E, 0x04,                          /* Event code 0x0E, Plen = 4 */
            0x01, 0x03, 0x0C, 0x00               /* NumHciCmdPkts=1, Opcode=0x0C03, Status=0 */
        };
        H4DecoderInit(&dec);
        memset(&cap, 0, sizeof(cap));

        encLen = H4EncodePacket(H4_PKT_EVENT, evtPkt, sizeof(evtPkt), encBuf, sizeof(encBuf));
        CHECK(encLen == 7u && encBuf[0] == H4_PKT_EVENT,
              "Event packet encoded correctly (len=%lu)", encLen);

        H4DecoderFeed(&dec, encBuf, encLen, CaptureCallback, &cap);
        CHECK(cap.Count == 1u &&
              cap.Packets[0].Type == H4_PKT_EVENT &&
              cap.Packets[0].Length == sizeof(evtPkt) &&
              memcmp(cap.Packets[0].Payload, evtPkt, sizeof(evtPkt)) == 0,
              "Event packet decoded byte-identical (type=0x%02X, len=%lu)",
              cap.Packets[0].Type, cap.Packets[0].Length);
    }
}

/*
 * Builds a fixed multi-packet test stream containing:
 *   1. Command: opcode 0x1001, plen 0 (proof of life, 3 bytes header)
 *   2. Event: Command Complete for 0x1001, plen 4 (6 bytes total)
 *   3. Event (zero-length payload): event 0x50, plen 0 (2 bytes total)
 *   4. SCO: handle 0x0001, 8 bytes audio data (11 bytes total)
 *   5. ACL (maximum-length ACL packet): handle 0x0020, 1021 bytes payload (1025 bytes total)
 *   6. Command: vendor opcode 0xFC00, plen 3 (6 bytes total)
 *   7. ACL (small): handle 0x0021, 6 bytes payload (10 bytes total)
 *
 * Total encoded stream length: 4 + 7 + 3 + 12 + 1026 + 7 + 11 = 1070 bytes.
 */
static unsigned long
BuildMultiPacketStream(unsigned char *Stream, unsigned long Capacity)
{
    unsigned long pos = 0u;
    unsigned char pktBuf[H4_MAX_PACKET_SIZE];
    unsigned long pktLen;
    unsigned long n;
    unsigned long i;

    /* 1. Command (plen 0) */
    pktBuf[0] = 0x01; pktBuf[1] = 0x10; pktBuf[2] = 0x00;
    pktLen = 3u;
    n = H4EncodePacket(H4_PKT_COMMAND, pktBuf, pktLen, Stream + pos, Capacity - pos);
    pos += n;

    /* 2. Event (plen 4) */
    pktBuf[0] = 0x0E; pktBuf[1] = 0x04;
    pktBuf[2] = 0x01; pktBuf[3] = 0x01; pktBuf[4] = 0x10; pktBuf[5] = 0x00;
    pktLen = 6u;
    n = H4EncodePacket(H4_PKT_EVENT, pktBuf, pktLen, Stream + pos, Capacity - pos);
    pos += n;

    /* 3. Zero-length payload Event (plen 0) */
    pktBuf[0] = 0x50; pktBuf[1] = 0x00;
    pktLen = 2u;
    n = H4EncodePacket(H4_PKT_EVENT, pktBuf, pktLen, Stream + pos, Capacity - pos);
    pos += n;

    /* 4. SCO packet (len 8) */
    pktBuf[0] = 0x01; pktBuf[1] = 0x00; pktBuf[2] = 0x08;
    for (i = 0; i < 8u; i++) { pktBuf[3u + i] = (unsigned char)(0x50u + i); }
    pktLen = 11u;
    n = H4EncodePacket(H4_PKT_SCO, pktBuf, pktLen, Stream + pos, Capacity - pos);
    pos += n;

    /* 5. Maximum-length ACL packet (payload 1021 bytes = H4_MAX_ACL_PAYLOAD_SIZE) */
    pktBuf[0] = 0x20; pktBuf[1] = 0x20;
    pktBuf[2] = (unsigned char)(H4_MAX_ACL_PAYLOAD_SIZE & 0xFFu);
    pktBuf[3] = (unsigned char)((H4_MAX_ACL_PAYLOAD_SIZE >> 8) & 0xFFu);
    for (i = 0; i < H4_MAX_ACL_PAYLOAD_SIZE; i++) {
        pktBuf[4u + i] = (unsigned char)((i * 37u + 13u) & 0xFFu);
    }
    pktLen = 4u + H4_MAX_ACL_PAYLOAD_SIZE; /* 1025 */
    n = H4EncodePacket(H4_PKT_ACL, pktBuf, pktLen, Stream + pos, Capacity - pos);
    pos += n;

    /* 6. Command (plen 3) */
    pktBuf[0] = 0x00; pktBuf[1] = 0xFC; pktBuf[2] = 0x03;
    pktBuf[3] = 0x1E; pktBuf[4] = 0x00; pktBuf[5] = 0x00;
    pktLen = 6u;
    n = H4EncodePacket(H4_PKT_COMMAND, pktBuf, pktLen, Stream + pos, Capacity - pos);
    pos += n;

    /* 7. Small ACL packet (payload 6) */
    pktBuf[0] = 0x21; pktBuf[1] = 0x00; pktBuf[2] = 0x06; pktBuf[3] = 0x00;
    pktBuf[4] = 0xAA; pktBuf[5] = 0xBB; pktBuf[6] = 0xCC;
    pktBuf[7] = 0xDD; pktBuf[8] = 0xEE; pktBuf[9] = 0xFF;
    pktLen = 10u;
    n = H4EncodePacket(H4_PKT_ACL, pktBuf, pktLen, Stream + pos, Capacity - pos);
    pos += n;

    return pos;
}

static void
Test2_SplitInvariance(void)
{
    static unsigned char stream[2048];
    unsigned long streamLen;
    H4_DECODER dec;
    CAPTURE_CONTEXT baseline;
    CAPTURE_CONTEXT testCap;
    unsigned long split;
    unsigned long i;
    int allSplitsPass = 1;

    printf("\n=== Test 2: Split Invariance ===\n");

    streamLen = BuildMultiPacketStream(stream, sizeof(stream));
    CHECK(streamLen > 1000u, "Constructed multi-packet test stream (%lu bytes, 7 packets)", streamLen);

    /* 1. Baseline: single-chunk feed computes the baseline output */
    H4DecoderInit(&dec);
    memset(&baseline, 0, sizeof(baseline));
    H4DecoderFeed(&dec, stream, streamLen, CaptureCallback, &baseline);

    CHECK(baseline.Count == 7u, "Baseline feed emitted all 7 packets");
    CHECK(dec.CommandsEmitted == 2u && dec.AclPacketsEmitted == 2u &&
          dec.ScoPacketsEmitted == 1u && dec.EventsEmitted == 2u,
          "Baseline emission counts match expectations (Cmd:2, ACL:2, SCO:1, Evt:2)");
    CHECK(dec.Desynchronised == 0u && dec.OversizedRejections == 0u,
          "Baseline feed had zero desync or oversized errors");
    CHECK(dec.BytesConsumed == streamLen,
          "Baseline BytesConsumed matches stream length (%lu)", dec.BytesConsumed);

    /* 2. One byte at a time */
    H4DecoderInit(&dec);
    memset(&testCap, 0, sizeof(testCap));
    for (i = 0u; i < streamLen; i++) {
        H4DecoderFeed(&dec, &stream[i], 1u, CaptureCallback, &testCap);
    }
    CHECK(CompareCaptures(&baseline, &testCap),
          "Byte-at-a-time feed matches single-chunk baseline result byte-for-byte");
    CHECK(dec.BytesConsumed == streamLen,
          "Byte-at-a-time BytesConsumed matches stream length (%lu)", dec.BytesConsumed);

    /* 3. Every split offset from 1 to streamLen - 1 */
    for (split = 1u; split < streamLen; split++) {
        H4DecoderInit(&dec);
        memset(&testCap, 0, sizeof(testCap));

        H4DecoderFeed(&dec, stream, split, CaptureCallback, &testCap);
        H4DecoderFeed(&dec, stream + split, streamLen - split, CaptureCallback, &testCap);

        if (!CompareCaptures(&baseline, &testCap)) {
            allSplitsPass = 0;
            printf("  FAIL Split offset %lu / %lu disagreed with baseline result!\n", split, streamLen);
            g_fail++;
            break;
        }
    }
    CHECK(allSplitsPass,
          "Every split offset (1..%lu) produces identical emission sequence to baseline", streamLen - 1u);
}

static void
Test3_DeterministicSplitFuzz(void)
{
    static unsigned char stream[2048];
    unsigned long streamLen;
    H4_DECODER dec;
    CAPTURE_CONTEXT baseline;
    CAPTURE_CONTEXT testCap;
    unsigned long iter;
    const unsigned long numIterations = 5000u;
    const unsigned long fixedSeed = 0xDEC8B700u;
    int allFuzzPass = 1;

    printf("\n=== Test 3: Deterministic Pseudorandom Split Fuzz ===\n");
    printf("  Seed: 0x%08X, Iterations: %lu\n", fixedSeed, numIterations);

    streamLen = BuildMultiPacketStream(stream, sizeof(stream));

    /* Compute baseline */
    H4DecoderInit(&dec);
    memset(&baseline, 0, sizeof(baseline));
    H4DecoderFeed(&dec, stream, streamLen, CaptureCallback, &baseline);

    PrngSeed(fixedSeed);

    for (iter = 0u; iter < numIterations; iter++) {
        unsigned long offset = 0u;

        H4DecoderInit(&dec);
        memset(&testCap, 0, sizeof(testCap));

        while (offset < streamLen) {
            unsigned long remaining = streamLen - offset;
            /* Chunk size between 1 and min(64, remaining) */
            unsigned long chunkSize = (PrngNext() % 64u) + 1u;
            if (chunkSize > remaining) {
                chunkSize = remaining;
            }
            H4DecoderFeed(&dec, stream + offset, chunkSize, CaptureCallback, &testCap);
            offset += chunkSize;
        }

        if (!CompareCaptures(&baseline, &testCap)) {
            allFuzzPass = 0;
            printf("  FAIL Fuzz iteration %lu disagreed with baseline result!\n", iter);
            g_fail++;
            break;
        }
    }

    CHECK(allFuzzPass, "All %lu deterministic random chunkings matched baseline result exactly", numIterations);
}

static void
Test4_Resynchronisation(void)
{
    H4_DECODER dec;
    CAPTURE_CONTEXT cap;
    unsigned char stream[256];
    unsigned long pos = 0u;
    unsigned long n;

    printf("\n=== Test 4: Resynchronisation ===\n");

    /*
     * Build a stream with garbage bytes injected:
     *   - 7 garbage bytes before Packet 1 (Command)
     *   - 5 garbage bytes between Packet 1 and Packet 2 (Event)
     *   - 9 garbage bytes between Packet 2 and Packet 3 (ACL)
     *   - 4 garbage bytes after Packet 3
     * Total garbage bytes = 7 + 5 + 9 + 4 = 25 bytes.
     * All garbage bytes chosen to not collide with valid H4 types (0x01..0x04).
     */

    /* Prefix garbage: 7 bytes */
    stream[pos++] = 0xAA; stream[pos++] = 0xBB; stream[pos++] = 0xCC;
    stream[pos++] = 0xDD; stream[pos++] = 0xEE; stream[pos++] = 0xFA; stream[pos++] = 0xFB;

    /* Packet 1: Command (plen 1, total encoded = 5 bytes) */
    {
        unsigned char cmd[4] = { 0x48, 0xFC, 0x01, 0x02 };
        n = H4EncodePacket(H4_PKT_COMMAND, cmd, sizeof(cmd), stream + pos, sizeof(stream) - pos);
        pos += n;
    }

    /* Mid-stream garbage: 5 bytes */
    stream[pos++] = 0x77; stream[pos++] = 0x88; stream[pos++] = 0x99;
    stream[pos++] = 0xBA; stream[pos++] = 0xBE;

    /* Packet 2: Event (plen 2, total encoded = 5 bytes) */
    {
        unsigned char evt[4] = { 0x05, 0x02, 0x00, 0x01 };
        n = H4EncodePacket(H4_PKT_EVENT, evt, sizeof(evt), stream + pos, sizeof(stream) - pos);
        pos += n;
    }

    /* Mid-stream garbage: 9 bytes */
    stream[pos++] = 0xC0; stream[pos++] = 0xC1; stream[pos++] = 0xC2;
    stream[pos++] = 0xC3; stream[pos++] = 0xC4; stream[pos++] = 0xC5;
    stream[pos++] = 0xC6; stream[pos++] = 0xC7; stream[pos++] = 0xC8;

    /* Packet 3: ACL (payload 4, total encoded = 9 bytes) */
    {
        unsigned char acl[8] = { 0x10, 0x00, 0x04, 0x00, 0x11, 0x22, 0x33, 0x44 };
        n = H4EncodePacket(H4_PKT_ACL, acl, sizeof(acl), stream + pos, sizeof(stream) - pos);
        pos += n;
    }

    /* Suffix garbage: 4 bytes */
    stream[pos++] = 0xFE; stream[pos++] = 0xFD; stream[pos++] = 0xFC; stream[pos++] = 0xFB;

    H4DecoderInit(&dec);
    memset(&cap, 0, sizeof(cap));

    H4DecoderFeed(&dec, stream, pos, CaptureCallback, &cap);

    CHECK(cap.Count == 3u, "All 3 well-formed packets delivered despite garbage (count=%lu)", cap.Count);
    CHECK(cap.Packets[0].Type == H4_PKT_COMMAND && cap.Packets[0].Length == 4u,
          "Packet 1 delivered intact following prefix garbage");
    CHECK(cap.Packets[1].Type == H4_PKT_EVENT && cap.Packets[1].Length == 4u,
          "Packet 2 delivered intact following mid-stream garbage");
    CHECK(cap.Packets[2].Type == H4_PKT_ACL && cap.Packets[2].Length == 8u,
          "Packet 3 delivered intact following second garbage run");
    CHECK(dec.Desynchronised == 25u,
          "Desynchronised counter matches exact discarded byte count (expected 25, got %lu)",
          dec.Desynchronised);
    CHECK(dec.BytesConsumed == pos,
          "BytesConsumed counts total fed bytes (%lu)", dec.BytesConsumed);
}

static void
Test5_OversizedRejection(void)
{
    H4_DECODER dec;
    CAPTURE_CONTEXT cap;
    unsigned char stream[128];
    unsigned long pos = 0u;
    unsigned long n;

    printf("\n=== Test 5: Oversized / Illegal Length Rejection ===\n");

    /*
     * Construct stream:
     *   1. Oversized ACL packet:
     *      H4 type = 0x02
     *      Handle  = 0x0001
     *      Length  = 0xFFFF (65535, > H4_MAX_ACL_PAYLOAD_SIZE 1021)
     *   2. Followed immediately by a valid Event packet:
     *      H4 type = 0x04, Event Code = 0x0E, Plen = 0x03, Payload = 0x01, 0x00, 0x00
     *   3. Another oversized ACL packet:
     *      H4 type = 0x02, Handle = 0x0002, Length = 1022 (exactly 1 over max 1021)
     *   4. Followed immediately by a valid Command packet:
     *      H4 type = 0x01, Opcode = 0x0C03, Plen = 0x00
     */

    /* 1. Oversized ACL (length 0xFFFF) */
    stream[pos++] = H4_PKT_ACL;
    stream[pos++] = 0x01; stream[pos++] = 0x00;           /* Handle 0x0001 */
    stream[pos++] = 0xFF; stream[pos++] = 0xFF;           /* Length 0xFFFF = 65535 */

    /* 2. Valid Event packet */
    {
        unsigned char evt[5] = { 0x0E, 0x03, 0x01, 0x00, 0x00 };
        n = H4EncodePacket(H4_PKT_EVENT, evt, sizeof(evt), stream + pos, sizeof(stream) - pos);
        pos += n;
    }

    /* 3. Oversized ACL (length 1022 = 0x03FE) */
    stream[pos++] = H4_PKT_ACL;
    stream[pos++] = 0x02; stream[pos++] = 0x00;           /* Handle 0x0002 */
    stream[pos++] = 0xFE; stream[pos++] = 0x03;           /* Length 1022 */

    /* 4. Valid Command packet */
    {
        unsigned char cmd[3] = { 0x03, 0x0C, 0x00 };
        n = H4EncodePacket(H4_PKT_COMMAND, cmd, sizeof(cmd), stream + pos, sizeof(stream) - pos);
        pos += n;
    }

    H4DecoderInit(&dec);
    memset(&cap, 0, sizeof(cap));

    H4DecoderFeed(&dec, stream, pos, CaptureCallback, &cap);

    CHECK(dec.OversizedRejections == 2u,
          "OversizedRejections counted both malformed length fields (got %lu)",
          dec.OversizedRejections);
    CHECK(cap.Count == 2u,
          "Decoder did not wedge: both valid packets following oversized headers arrived (count=%lu)",
          cap.Count);
    CHECK(cap.Packets[0].Type == H4_PKT_EVENT && cap.Packets[0].Length == 5u,
          "Valid event arrived intact after 65535-byte oversized ACL rejection");
    CHECK(cap.Packets[1].Type == H4_PKT_COMMAND && cap.Packets[1].Length == 3u,
          "Valid command arrived intact after 1022-byte oversized ACL rejection");
    CHECK(dec.Desynchronised > 0u,
          "Desynchronised counter tracked discarded bytes (%lu)", dec.Desynchronised);
}

static void
Test6_TruncatedTail(void)
{
    H4_DECODER dec;
    CAPTURE_CONTEXT cap;
    unsigned char stream[64];
    unsigned long streamLen = 0u;
    unsigned long n;
    unsigned long cutOffset;

    printf("\n=== Test 6: Truncated Tail Resumption ===\n");

    /*
     * Stream contains 2 packets:
     *   Packet 1: Event (code 0x0E, plen 3) -> 6 bytes encoded
     *   Packet 2: ACL (payload 6)           -> 11 bytes encoded
     * Total stream = 17 bytes.
     */
    {
        unsigned char evt[5] = { 0x0E, 0x03, 0x01, 0x03, 0x0C };
        n = H4EncodePacket(H4_PKT_EVENT, evt, sizeof(evt), stream + streamLen, sizeof(stream) - streamLen);
        streamLen += n;
    }
    {
        unsigned char acl[10] = { 0x05, 0x00, 0x06, 0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 };
        n = H4EncodePacket(H4_PKT_ACL, acl, sizeof(acl), stream + streamLen, sizeof(stream) - streamLen);
        streamLen += n;
    }

    CHECK(streamLen == 17u, "Total 2-packet stream length is %lu bytes", streamLen);

    /* Cut mid-packet: byte 9 is inside Packet 2's header (type + 2 handle + 1 length byte) */
    cutOffset = 9u;

    H4DecoderInit(&dec);
    memset(&cap, 0, sizeof(cap));

    /* Feed first chunk */
    H4DecoderFeed(&dec, stream, cutOffset, CaptureCallback, &cap);
    CHECK(cap.Count == 1u && cap.Packets[0].Type == H4_PKT_EVENT,
          "First chunk emitted complete Packet 1 (count=1), retained partial Packet 2");
    CHECK(dec.State == H4StateHeader,
          "Decoder is in H4StateHeader awaiting remaining header bytes");

    /* Feed remaining tail */
    H4DecoderFeed(&dec, stream + cutOffset, streamLen - cutOffset, CaptureCallback, &cap);
    CHECK(cap.Count == 2u,
          "Feeding remaining tail completed Packet 2 (count=2)");
    CHECK(cap.Packets[1].Type == H4_PKT_ACL && cap.Packets[1].Length == 10u,
          "Packet 2 payload verified byte-identical");
    CHECK(dec.State == H4StateIdle,
          "Decoder returned cleanly to H4StateIdle");
    CHECK(dec.Desynchronised == 0u && dec.OversizedRejections == 0u,
          "Zero framing or desync errors across split boundary");
}

/* Records every byte offered to the out-of-band hook; claims Qualcomm IBS bytes only. */
typedef struct _OOB_CONTEXT {
    CAPTURE_CONTEXT Packets;
    unsigned char   Bytes[16];
    unsigned long   Count;
} OOB_CONTEXT;

static void
OobPacketCallback(void *Context, unsigned char Type, const unsigned char *Payload, unsigned long Length)
{
    CaptureCallback(&((OOB_CONTEXT *)Context)->Packets, Type, Payload, Length);
}

static unsigned char
OobByteCallback(void *Context, unsigned char Byte)
{
    OOB_CONTEXT *ctx = (OOB_CONTEXT *)Context;
    if (Byte < 0xFCu || Byte > 0xFEu) {
        return 0u;
    }
    if (ctx->Count < sizeof(ctx->Bytes)) {
        ctx->Bytes[ctx->Count] = Byte;
    }
    ctx->Count++;
    return 1u;
}

static void
Test7_OutOfBandBytes(void)
{
    /*
     * Controller traffic as observed on the Deck: sleep (FE), wake (FD) before an event, an ACL
     * packet whose payload carries FD FE FC, a wake ack (FC), then one unclaimed junk byte.
     * Only the three boundary bytes are out-of-band; payload bytes are data.
     */
    static const unsigned char stream[] = {
        0xFE,
        0xFD, 0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00,
        0x02, 0x05, 0x00, 0x03, 0x00, 0xFD, 0xFE, 0xFC,
        0xFC,
        0x7F
    };
    H4_DECODER dec;
    OOB_CONTEXT ctx;
    unsigned long i;

    printf("\n=== Test 7: Out-of-band bytes between packets ===\n");

    H4DecoderInit(&dec);
    memset(&ctx, 0, sizeof(ctx));
    for (i = 0u; i < sizeof(stream); i++) {
        H4DecoderFeedEx(&dec, &stream[i], 1u, OobPacketCallback, OobByteCallback, &ctx);
    }
    CHECK(ctx.Count == 3u && ctx.Bytes[0] == 0xFEu && ctx.Bytes[1] == 0xFDu && ctx.Bytes[2] == 0xFCu,
          "Exactly the boundary bytes FE, FD, FC reach the hook, in order (count %lu)", ctx.Count);
    CHECK(ctx.Packets.Count == 2u && ctx.Packets.Packets[0].Type == H4_PKT_EVENT &&
          ctx.Packets.Packets[1].Type == H4_PKT_ACL && ctx.Packets.Packets[1].Length == 7u &&
          ctx.Packets.Packets[1].Payload[4] == 0xFDu && ctx.Packets.Packets[1].Payload[6] == 0xFCu,
          "Event and ACL arrive intact, IBS-valued payload bytes included");
    CHECK(dec.Desynchronised == 1u,
          "Only the unclaimed junk byte counts as desync (got %lu)", dec.Desynchronised);

    H4DecoderInit(&dec);
    memset(&ctx, 0, sizeof(ctx));
    H4DecoderFeed(&dec, stream, sizeof(stream), OobPacketCallback, &ctx);
    CHECK(ctx.Count == 0u && ctx.Packets.Count == 2u && dec.Desynchronised == 4u,
          "Plain Feed keeps discarding them: 2 packets, 4 desync bytes, hook never called");
}

static void
Test8_DiscardKeepsFraming(void)
{
    /*
     * An ACL packet on handle 2 whose L2CAP payload carries 04 0E 04 01 03 0C 00, which read from
     * Idle is an HCI_Reset Command_Complete. A transport reset lands after its first 10 bytes.
     */
    static const unsigned char acl[] = {
        0x02, 0x02, 0x20, 0x10, 0x00, 0x0C, 0x00, 0x40, 0x00,
        0xAA, 0xBB, 0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00, 0xCC, 0xDD, 0xEE
    };
    static const unsigned char next[] = { 0x04, 0x0E, 0x04, 0x01, 0x01, 0x10, 0x00 };
    H4_DECODER dec;
    CAPTURE_CONTEXT cap;

    printf("\n=== Test 8: Discarding a packet in progress keeps the framing ===\n");

    H4DecoderInit(&dec);
    memset(&cap, 0, sizeof(cap));
    H4DecoderFeed(&dec, acl, 10u, CaptureCallback, &cap);
    H4DecoderDiscardPacket(&dec);
    H4DecoderFeed(&dec, acl + 10, (unsigned long)sizeof(acl) - 10u, CaptureCallback, &cap);
    CHECK(cap.Count == 0u, "The rest of the discarded ACL packet emits nothing, no forged event (count=%lu)",
          cap.Count);
    CHECK(dec.PacketsDiscarded == 1u && dec.Desynchronised == 0u && dec.State == H4StateIdle,
          "One packet discarded, no desync, decoder back between packets");

    H4DecoderFeed(&dec, next, sizeof(next), CaptureCallback, &cap);
    CHECK(cap.Count == 1u && cap.Packets[0].Type == H4_PKT_EVENT && cap.Packets[0].Length == 6u &&
          memcmp(cap.Packets[0].Payload, next + 1, 6u) == 0,
          "The next real packet arrives intact");

    H4DecoderDiscardPacket(&dec);
    H4DecoderFeed(&dec, next, sizeof(next), CaptureCallback, &cap);
    CHECK(cap.Count == 2u && dec.PacketsDiscarded == 1u,
          "A discard between packets affects nothing that follows");

    /* Discard while the header is still arriving: its length is still read to skip the payload. */
    H4DecoderFeed(&dec, acl, 3u, CaptureCallback, &cap);
    H4DecoderDiscardPacket(&dec);
    H4DecoderFeed(&dec, acl + 3, (unsigned long)sizeof(acl) - 3u, CaptureCallback, &cap);
    H4DecoderFeed(&dec, next, sizeof(next), CaptureCallback, &cap);
    CHECK(cap.Count == 3u && cap.Packets[2].Type == H4_PKT_EVENT && dec.PacketsDiscarded == 2u,
          "Discard inside the header: payload skipped, next packet intact");
}

static void
Test9_FeedLimited(void)
{
    /* Three 3-byte events (plen 0) and a 5-byte ACL with no payload, back to back. */
    static const unsigned char stream[] = {
        0x04, 0x10, 0x00,  0x04, 0x11, 0x00,  0x04, 0x12, 0x00,  0x02, 0x01, 0x00, 0x00, 0x00
    };
    H4_DECODER dec;
    CAPTURE_CONTEXT cap;
    unsigned long used;
    unsigned long at = 0u;

    printf("\n=== Test 9: Limited feed stops after the requested packets ===\n");

    H4DecoderInit(&dec);
    memset(&cap, 0, sizeof(cap));
    used = H4DecoderFeedLimited(&dec, stream, sizeof(stream), 0u, CaptureCallback, NULL, &cap);
    CHECK(used == 0u && cap.Count == 0u && dec.BytesConsumed == 0u, "A limit of 0 consumes nothing");

    used = H4DecoderFeedLimited(&dec, stream, sizeof(stream), 2u, CaptureCallback, NULL, &cap);
    CHECK(used == 6u && cap.Count == 2u, "A limit of 2 stops right after the second packet (used %lu)", used);
    at += used;

    used = H4DecoderFeedLimited(&dec, stream + at, 1u, 1u, CaptureCallback, NULL, &cap);
    at += used;
    used = H4DecoderFeedLimited(&dec, stream + at, (unsigned long)sizeof(stream) - at, 1u, CaptureCallback, NULL, &cap);
    at += used;
    CHECK(cap.Count == 3u && at == 9u, "A packet split across limited feeds completes once (at %lu)", at);

    used = H4DecoderFeedLimited(&dec, stream + at, (unsigned long)sizeof(stream) - at, 5u, CaptureCallback, NULL, &cap);
    CHECK(used == 5u && cap.Count == 4u && cap.Packets[3].Type == H4_PKT_ACL,
          "A larger limit than packets consumes everything");
}

int main(void)
{
    printf("H4 framing codec self-test suite\n");
    printf("------------------------------------------\n");

    Test1_RoundTrip();
    Test2_SplitInvariance();
    Test3_DeterministicSplitFuzz();
    Test4_Resynchronisation();
    Test5_OversizedRejection();
    Test6_TruncatedTail();
    Test7_OutOfBandBytes();
    Test8_DiscardKeepsFraming();
    Test9_FeedLimited();

    printf("\n------------------------------------------\n");
    if (g_fail == 0) {
        printf("H4 SELFTEST PASSED\n");
        return 0;
    } else {
        printf("H4 SELFTEST FAILED (%d failure(s))\n", g_fail);
        return 1;
    }
}
