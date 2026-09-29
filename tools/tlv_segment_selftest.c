/*
 * tlv_segment_selftest.c - host-side regression suite for Qualcomm
 * TLV firmware segmentation and command framing arithmetic.
 *
 * Validates src/common/qca_tlv.c in user mode without requiring hardware or
 * DriverStore filesystem presence.
 *
 * Covers synthetic boundaries for firmware sizes (155044 B patch, 6610 B NVM),
 * parameter-length rules, exact capacity bounds, and intermediate ACK suppression.
 *
 * Build: tools\selftest.cmd
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/include/qca_protocol.h"

static int g_fail = 0;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (cond) { printf("  ok   "); }                    \
        else      { printf("  FAIL "); g_fail++; }          \
        printf(__VA_ARGS__); printf("\n");                  \
    } while (0)

static void VerifyFirmwareBoundary(
    const char *name,
    ULONG fileSize,
    ULONG expectSegments,
    ULONG expectLastSegSize,
    UCHAR downloadMode)
{
    UCHAR *src = NULL;
    UCHAR *rebuilt = NULL;
    ULONG segCount;
    ULONG rebuiltLen = 0;
    ULONG lastSegSize = 0;
    ULONG lastParamLen = 0;
    BOOLEAN allFramingOk = TRUE;
    BOOLEAN allParamLensOk = TRUE;
    BOOLEAN allSegSizesOk = TRUE;
    UCHAR cmd[QCA_MAX_SIZE_PER_TLV_SEGMENT + 16];

    printf("%s (payload: %lu bytes)\n", name, fileSize);

    segCount = QcaTlvSegmentCount(fileSize);
    CHECK(segCount == expectSegments,
          "segment count is %lu (expected %lu)",
          segCount, expectSegments);

    if (fileSize == 0 || segCount != expectSegments) {
        return;
    }

    src = (UCHAR *)malloc((size_t)fileSize);
    rebuilt = (UCHAR *)malloc((size_t)fileSize);
    if (src == NULL || rebuilt == NULL) {
        printf("  FAIL out of memory allocating %lu bytes\n", fileSize);
        g_fail++;
        free(src);
        free(rebuilt);
        return;
    }

    /* Fill source with deterministic pseudo-random sequence */
    for (ULONG i = 0; i < fileSize; i++) {
        src[i] = (UCHAR)((i * 37u + 13u) & 0xFFu);
    }

    for (ULONG i = 0; i < segCount; i++) {
        BOOLEAN ack = FALSE;
        ULONG n = QcaBuildTlvSegmentCommand(src, fileSize, i, downloadMode,
                                            cmd, sizeof(cmd), &ack);
        ULONG segSize;
        ULONG paramLen;

        if (n < 6u) {
            printf("  FAIL segment %lu build returned %lu\n", i, n);
            g_fail++;
            allFramingOk = FALSE;
            break;
        }

        segSize = n - 6u;
        paramLen = (ULONG)cmd[3];

        if (cmd[0] != H4_PKT_COMMAND ||
            cmd[1] != (UCHAR)(EDL_PATCH_CMD_OPCODE & 0xFFu) ||
            cmd[2] != (UCHAR)((EDL_PATCH_CMD_OPCODE >> 8) & 0xFFu) ||
            cmd[4] != EDL_PATCH_TLV_REQ_CMD ||
            cmd[5] != (UCHAR)segSize) {
            allFramingOk = FALSE;
            break;
        }

        if (paramLen != segSize + 2u) {
            allParamLensOk = FALSE;
        }

        if (i + 1u < segCount) {
            if (segSize != QCA_MAX_SIZE_PER_TLV_SEGMENT) {
                allSegSizesOk = FALSE;
            }
        }

        memcpy(rebuilt + rebuiltLen, &cmd[6], segSize);
        rebuiltLen += segSize;
        lastSegSize = segSize;
        lastParamLen = paramLen;
    }

    CHECK(allFramingOk, "every segment carries H4(0x01) + 0xFC00 + sub-cmd 0x1E framing");
    CHECK(allParamLensOk, "per-segment parameter length == seglen + 2 for all %lu segment(s)", segCount);
    CHECK(allSegSizesOk, "all intermediate segments are exactly 243 bytes");
    CHECK(lastSegSize == expectLastSegSize,
          "final segment length is %lu bytes (expected %lu)",
          lastSegSize, expectLastSegSize);
    CHECK(lastParamLen == expectLastSegSize + 2u,
          "final segment parameter length is %lu (seglen + 2)",
          lastParamLen);
    CHECK(rebuiltLen == fileSize && memcmp(rebuilt, src, (size_t)fileSize) == 0,
          "reassembled %lu bytes match synthetic payload byte-for-byte",
          rebuiltLen);

    free(src);
    free(rebuilt);
}

static void TestRealFirmwareBoundaries(void)
{
    printf("--- Real firmware payload boundaries (synthetic host-side) ---\n");

    /*
     * Patch: hpbtfw21.tlv is 155,044 bytes.
     * Arithmetic: 155044 = 638 * 243 + 10 bytes remainder.
     * Expected: exactly 639 segments, final segment 10 bytes, final param len 12.
     * Tested in-memory so test runs without DriverStore filesystem access.
     */
    VerifyFirmwareBoundary("real patch boundary: 155044 bytes (hpbtfw21.tlv)",
                           155044, 639, 10, QCA_SKIP_EVT_VSE_CC);

    /*
     * NVM: hpnv21.bin is 6,610 bytes.
     * Arithmetic: 6610 = 27 * 243 + 49 bytes remainder.
     * Expected: exactly 28 segments, final segment 49 bytes, final param len 51.
     */
    VerifyFirmwareBoundary("real NVM boundary: 6610 bytes (hpnv21.bin)",
                           6610, 28, 49, QCA_SKIP_EVT_NONE);
}

static void TestParameterLengthRule(void)
{
    static const ULONG testLengths[] = {
        1u, 2u, 5u, 10u, 14u, 49u, 57u, 100u, 132u, 200u, 242u, 243u
    };

    printf("\n--- Parameter-length rule (plen == seglen + 2) across diverse sizes ---\n");

    for (size_t idx = 0; idx < sizeof(testLengths) / sizeof(testLengths[0]); idx++) {
        ULONG len = testLengths[idx];
        UCHAR src[243];
        UCHAR cmd[QCA_MAX_SIZE_PER_TLV_SEGMENT + 16];
        BOOLEAN ack = FALSE;
        ULONG written;
        ULONG segLen;
        ULONG paramLen;

        memset(src, (int)(len & 0xFFu), len);

        written = QcaBuildTlvSegmentCommand(src, len, 0, QCA_SKIP_EVT_NONE,
                                            cmd, sizeof(cmd), &ack);
        CHECK(written == 6u + len,
              "payload size %lu: total command size %lu == 6 + %lu",
              len, written, len);

        segLen = (ULONG)cmd[5];
        paramLen = (ULONG)cmd[3];

        CHECK(segLen == len,
              "payload size %lu: segment length field (cmd[5]) is %lu",
              len, segLen);
        CHECK(paramLen == len + 2u,
              "payload size %lu: parameter length (cmd[3]) is %lu (seglen + 2)",
              len, paramLen);
    }
}

static void TestCapacityExactBoundary(void)
{
    UCHAR dummy[243];
    UCHAR cmd[QCA_MAX_SIZE_PER_TLV_SEGMENT + 16];
    BOOLEAN ack = FALSE;

    printf("\n--- Exact capacity boundary (248 rejected vs 249 accepted) ---\n");
    memset(dummy, 0xEE, sizeof(dummy));

    /*
     * For a full 243-byte segment, total command length is 6 + 243 = 249 bytes:
     * H4(1) + Opcode(2) + ParamLen(1) + SubOp(1) + SegLen(1) + Payload(243).
     * qca_selftest.c tests only a generic short buffer (capacity 8).
     * Pins the exact off-by-one boundary: capacity 248 is rejected, 249 succeeds.
     */
    CHECK(QcaBuildTlvSegmentCommand(dummy, sizeof(dummy), 0, 0, cmd, 248, &ack) == 0,
          "capacity 248 (1 byte short of 6 + 243) is rejected");

    CHECK(QcaBuildTlvSegmentCommand(dummy, sizeof(dummy), 0, 0, cmd, 249, &ack) == 249,
          "capacity 249 (exact required size) succeeds");
}

static void TestDownloadModeIntermediateAckSuppression(void)
{
    UCHAR buf[486];
    UCHAR cmd[QCA_MAX_SIZE_PER_TLV_SEGMENT + 16];
    BOOLEAN ackIntermediate = TRUE;
    BOOLEAN ackNone = FALSE;

    printf("\n--- Download-mode intermediate ACK suppression ---\n");
    memset(buf, 0xAB, sizeof(buf));

    /*
     * Intermediate full segments skip ACK when download mode is
     * QCA_SKIP_EVT_VSE_CC or QCA_SKIP_EVT_CC.
     * qca_selftest.c asserts that the final segment has ack == TRUE, and tallies
     * intermediate acks in a printf, but does not assert that intermediate
     * segments suppress acks (ack == FALSE).
     */
    (void)QcaBuildTlvSegmentCommand(buf, sizeof(buf), 0, QCA_SKIP_EVT_VSE_CC,
                                    cmd, sizeof(cmd), &ackIntermediate);
    CHECK(ackIntermediate == FALSE,
          "mode QCA_SKIP_EVT_VSE_CC: intermediate full segment 0 skips ACK");

    (void)QcaBuildTlvSegmentCommand(buf, sizeof(buf), 0, QCA_SKIP_EVT_NONE,
                                    cmd, sizeof(cmd), &ackNone);
    CHECK(ackNone == TRUE,
          "mode QCA_SKIP_EVT_NONE: intermediate full segment 0 requires ACK");
}

int main(void)
{
    printf("=== TLV Segmentation Regression Suite ===\n\n");

    TestRealFirmwareBoundaries();
    TestParameterLengthRule();
    TestCapacityExactBoundary();
    TestDownloadModeIntermediateAckSuppression();

    printf("\n%s\n", g_fail ? "TLV SEGMENT SELFTEST FAILED" : "TLV SEGMENT SELFTEST PASSED");
    return g_fail ? 1 : 0;
}
