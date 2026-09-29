/*
 * qca_selftest.c - validates src/common/qca_tlv.c against Qualcomm firmware files
 * shipped in the DriverStore.
 *
 * Exercises TLV parsing, header validation, command framing, and segmentation
 * arithmetic in user mode against actual patch and NVM files.
 *
 * Build: tools\selftest.cmd
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/include/qca_protocol.h"

/* Firmware directory: QCA_FW_DIR, which tools\selftest.cmd sets to the installed vendor package. */
static const char *FwDir(void)
{
    static char dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("QCA_FW_DIR", dir, (DWORD)sizeof(dir));
    return (n > 0 && n < sizeof(dir)) ? dir : ".";
}

static int g_fail = 0;

#define CHECK(cond, ...)                                   \
    do {                                                   \
        if (cond) { printf("  ok   "); }                   \
        else      { printf("  FAIL "); g_fail++; }         \
        printf(__VA_ARGS__); printf("\n");                 \
    } while (0)

static UCHAR *LoadFile(const char *path, ULONG *size)
{
    FILE *f = NULL;
    UCHAR *buf;
    long len;

    *size = 0;
    if (fopen_s(&f, path, "rb") != 0 || f == NULL) { return NULL; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }

    buf = (UCHAR *)malloc((size_t)len);
    if (buf == NULL) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = (ULONG)len;
    return buf;
}

static void TestCommandFraming(void)
{
    UCHAR out[16];
    ULONG n;

    printf("command framing against upstream command formats\n");

    /*
     * Upstream hci_qca.c (qca_set_baudrate;
     * https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/hci_qca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137)
     * issues opcode 0xFC48 with 1 parameter byte containing the baud rate index.
     */
    n = QcaBuildBaudRateCommand(QCA_BAUDRATE_115200, out, sizeof(out));
    CHECK(n == 5, "baud command is 5 bytes (got %lu)", n);
    CHECK(out[0] == 0x01 && out[1] == 0x48 && out[2] == 0xFC && out[3] == 0x01 && out[4] == 0x00,
          "matches { 01 48 FC 01 00 } exactly: { %02X %02X %02X %02X %02X }",
          out[0], out[1], out[2], out[3], out[4]);

    n = QcaBuildBaudRateCommand(QCA_BAUDRATE_3000000, out, sizeof(out));
    CHECK(n == 5 && out[4] == 14, "3,000,000 baud encodes as enum index 14 (got %u)", out[4]);

    CHECK(QcaBuildBaudRateCommand(18, out, sizeof(out)) == 0,
          "index above QCA_BAUDRATE_3200000 (17) is rejected");
    CHECK(QcaBuildBaudRateCommand(QCA_BAUDRATE_115200, out, 4) == 0,
          "short output buffer is rejected");

    /*
     * Upstream hci_qca.c (qca_send_reset;
     * https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/hci_qca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137)
     * formats the EDL reset command as opcode 0xFC00 with sub-command 0x05.
     */
    n = QcaBuildEdlCommand(EDL_PATCH_RESET_SOC_CMD, out, sizeof(out));
    CHECK(n == 5 && out[0] == 0x01 && out[1] == 0x00 && out[2] == 0xFC &&
          out[3] == 0x01 && out[4] == 0x05,
          "SOC reset matches { 01 00 FC 01 05 }");

    n = QcaBuildEdlCommand(EDL_PATCH_VER_REQ_CMD, out, sizeof(out));
    CHECK(n == 5 && out[4] == 0x19, "version request carries sub-command 0x19");

    printf("baud rate mapping\n");
    {
        UCHAR idx = 0xFF;
        CHECK(QcaBaudRateToIndex(3000000, &idx) && idx == 14, "3000000 -> 14");
        CHECK(QcaBaudRateToIndex(115200, &idx) && idx == 0, "115200 -> 0");
        CHECK(QcaBaudRateToIndex(921600, &idx) && idx == 10, "921600 -> 10 (documented fallback)");
        CHECK(!QcaBaudRateToIndex(4000000, &idx), "4000000 rejected (above the 3,200,000 cap)");
    }
}

/*
 * The real test: stream a whole firmware file through the segment builder, reassemble the
 * payloads, and require a byte-exact match with the file.
 */
static void TestRealFirmware(const char *name, ULONG expectSize, UCHAR expectType,
                             ULONG expectSegments, ULONG expectLastSegment)
{
    char path[MAX_PATH];
    ULONG size = 0;
    UCHAR *fw;
    QCA_TLV_INFO info;
    ULONG segments;
    UCHAR *rebuilt;
    ULONG rebuiltLen = 0;
    ULONG acked = 0, unacked = 0;
    ULONG lastSegSize = 0;
    int framingOk = 1;

    sprintf_s(path, sizeof(path), "%s\\%s", FwDir(), name);
    printf("%s\n", path);

    fw = LoadFile(path, &size);
    if (fw == NULL) {
        printf("  FAIL cannot open (set QCA_FW_DIR, or run tools\\build.cmd to stage the firmware)\n");
        g_fail++;
        return;
    }

    CHECK(size == expectSize, "size %lu bytes (want %lu)", size, expectSize);

    CHECK(QcaParseTlv(fw, size, &info), "TLV header parses and self-validates");
    CHECK(info.Type == expectType, "type %u (want %u)", info.Type, expectType);
    CHECK(info.Length == size - 4u, "declared length %lu == size-4", info.Length);

    if (info.Type == QCA_TLV_TYPE_PATCH) {
        CHECK(info.DownloadMode <= QCA_SKIP_EVT_VSE_CC,
              "download mode %u is a valid qca_tlv_dnld_mode", info.DownloadMode);
        CHECK(info.TotalSize > 0 && info.TotalSize <= size,
              "patch total_size %lu fits in the file", info.TotalSize);
        CHECK(info.DataLength < info.TotalSize,
              "data_length %lu < total_size %lu", info.DataLength, info.TotalSize);
        printf("       product=0x%04X rom_build=0x%04X patch_ver=0x%04X dnld_mode=%u\n",
               info.ProductId, info.RomBuild, info.PatchVersion, info.DownloadMode);
    }

    segments = QcaTlvSegmentCount(size);
    CHECK(segments == expectSegments, "%lu segments at 243 bytes (want %lu)", segments, expectSegments);

    rebuilt = (UCHAR *)malloc(size);
    if (rebuilt == NULL) { free(fw); printf("  FAIL out of memory\n"); g_fail++; return; }

    for (ULONG i = 0; i < segments; i++) {
        UCHAR cmd[QCA_MAX_SIZE_PER_TLV_SEGMENT + 16];
        BOOLEAN ack = FALSE;
        ULONG n = QcaBuildTlvSegmentCommand(fw, size, i, info.DownloadMode,
                                            cmd, sizeof(cmd), &ack);
        ULONG segSize;

        if (n == 0) { printf("  FAIL segment %lu build failed\n", i); g_fail++; framingOk = 0; break; }

        segSize = n - 6u;
        if (cmd[0] != H4_PKT_COMMAND ||
            cmd[1] != (UCHAR)(EDL_PATCH_CMD_OPCODE & 0xFF) ||
            cmd[2] != (UCHAR)(EDL_PATCH_CMD_OPCODE >> 8) ||
            cmd[3] != (UCHAR)(segSize + 2u) ||
            cmd[4] != EDL_PATCH_TLV_REQ_CMD ||
            cmd[5] != (UCHAR)segSize) {
            printf("  FAIL segment %lu header wrong: %02X %02X %02X %02X %02X %02X\n",
                   i, cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], cmd[5]);
            g_fail++; framingOk = 0; break;
        }

        if (segSize > QCA_MAX_SIZE_PER_TLV_SEGMENT) {
            printf("  FAIL segment %lu oversized: %lu\n", i, segSize); g_fail++; framingOk = 0; break;
        }

        memcpy(rebuilt + rebuiltLen, &cmd[6], segSize);
        rebuiltLen += segSize;
        lastSegSize = segSize;
        if (ack) { acked++; } else { unacked++; }
    }

    CHECK(framingOk, "every segment carries a well-formed 0xFC00/0x1E command header");
    CHECK(rebuiltLen == size, "reassembled %lu bytes == file size %lu", rebuiltLen, size);
    CHECK(rebuiltLen == size && memcmp(rebuilt, fw, size) == 0,
          "reassembled payload is byte-identical to the file");
    CHECK(lastSegSize == expectLastSegment, "final segment %lu bytes (want %lu)",
          lastSegSize, expectLastSegment);

    /* The final segment always requires an ACK regardless of the download mode. */
    {
        UCHAR cmd[QCA_MAX_SIZE_PER_TLV_SEGMENT + 16];
        BOOLEAN ack = FALSE;
        (void)QcaBuildTlvSegmentCommand(fw, size, segments - 1, QCA_SKIP_EVT_VSE_CC,
                                        cmd, sizeof(cmd), &ack);
        CHECK(ack, "final segment is acked even with download mode VSE_CC");

        ack = FALSE;
        (void)QcaBuildTlvSegmentCommand(fw, size, segments - 1, QCA_SKIP_EVT_VSE,
                                        cmd, sizeof(cmd), &ack);
        CHECK(ack, "final segment is acked even with download mode VSE");
    }

    if (info.DownloadMode == QCA_SKIP_EVT_VSE_CC) {
        CHECK(acked == 1 && unacked == segments - 1,
              "mode VSE_CC: intermediate segments unacked (%lu), only final acked (%lu)",
              unacked, acked);
    } else if (info.DownloadMode == QCA_SKIP_EVT_NONE) {
        CHECK(acked == segments && unacked == 0,
              "mode NONE: every segment acked (%lu acked, 0 unacked)", acked);
    }
    printf("       acked=%lu unacked=%lu (mode %u)\n", acked, unacked, info.DownloadMode);

    free(rebuilt);
    free(fw);
}

static void TestRejections(void)
{
    QCA_TLV_INFO info;
    UCHAR buf[64];
    UCHAR cmd[16];
    BOOLEAN ack;

    printf("malformed input is rejected rather than half-flashed\n");
    memset(buf, 0, sizeof(buf));

    CHECK(!QcaParseTlv(buf, 3, &info), "3-byte buffer rejected (header is 4 bytes)");

    /* type 1 (patch) with a length that disagrees with the buffer size */
    buf[0] = 0x01; buf[1] = 0xFF; buf[2] = 0xFF; buf[3] = 0x00;
    CHECK(!QcaParseTlv(buf, sizeof(buf), &info), "length/size mismatch rejected");

    /* well-formed length but an unknown type */
    {
        ULONG len = sizeof(buf) - 4u;
        buf[0] = 0x77;
        buf[1] = (UCHAR)(len & 0xFF);
        buf[2] = (UCHAR)((len >> 8) & 0xFF);
        buf[3] = (UCHAR)((len >> 16) & 0xFF);
        CHECK(!QcaParseTlv(buf, sizeof(buf), &info), "unknown TLV type rejected");
    }

    /*
     * Upstream btqca.h (struct tlv_type_patch;
     * https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/btqca.h?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137)
     * defines struct tlv_type_patch as 24 bytes. With the 4-byte tlv_type_hdr,
     * the exact minimum size is 28 bytes.
     * 27 bytes (4 + 23) must be rejected; 28 bytes (4 + 24) must be accepted.
     */
    {
        UCHAR patchHdr[28];
        memset(patchHdr, 0, sizeof(patchHdr));
        patchHdr[0] = QCA_TLV_TYPE_PATCH;

        /* Declared payload length 23 (total file size 27): rejected because 27 < 4 + 24 */
        patchHdr[1] = 23;
        CHECK(!QcaParseTlv(patchHdr, 27, &info),
              "patch TLV shorter than 4 + 24 bytes (27 B) rejected");

        /* Declared payload length 24 (total file size 28): accepted (exact 24-byte struct) */
        patchHdr[1] = 24;
        patchHdr[14] = QCA_SKIP_EVT_VSE;
        CHECK(QcaParseTlv(patchHdr, 28, &info) && info.DownloadMode == QCA_SKIP_EVT_VSE,
              "patch TLV of exactly 4 + 24 bytes (28 B) accepted");
    }
    CHECK(QcaTlvSegmentCount(0) == 0, "zero-length file yields zero segments");
    CHECK(QcaTlvSegmentCount(243) == 1, "exactly 243 bytes is one segment");
    CHECK(QcaTlvSegmentCount(244) == 2, "244 bytes is two segments");

    CHECK(QcaBuildTlvSegmentCommand(buf, sizeof(buf), 99, 0, cmd, sizeof(cmd), &ack) == 0,
          "out-of-range segment index rejected");
    CHECK(QcaBuildTlvSegmentCommand(buf, sizeof(buf), 0, 0, cmd, 8, &ack) == 0,
          "insufficient output capacity rejected");
}
static void TestDownloadModeAckRules(void)
{
    UCHAR dummy[QCA_MAX_SIZE_PER_TLV_SEGMENT + 10];
    UCHAR cmd[QCA_MAX_SIZE_PER_TLV_SEGMENT + 16];
    BOOLEAN ack = FALSE;
    ULONG n;

    printf("download mode ack rules\n");
    memset(dummy, 0xA5, sizeof(dummy));

    /*
     * Upstream btqca.c (qca_tlv_send_segment;
     * https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/btqca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137)
     * suppresses intermediate ACKs when mode is QCA_SKIP_EVT_VSE_CC or QCA_SKIP_EVT_VSE.
     *
     * Intermediate full-size segment (segment 0 of 2):
     * - QCA_SKIP_EVT_VSE: ack is skipped (FALSE)
     * - QCA_SKIP_EVT_CC: ack is required (TRUE)
     * - QCA_SKIP_EVT_VSE_CC: ack is skipped (FALSE)
     * - QCA_SKIP_EVT_NONE: ack is required (TRUE)
     */
    n = QcaBuildTlvSegmentCommand(dummy, sizeof(dummy), 0, QCA_SKIP_EVT_VSE,
                                  cmd, sizeof(cmd), &ack);
    CHECK(n > 0 && ack == FALSE,
          "mode QCA_SKIP_EVT_VSE: intermediate full segment skips ACK (ack == FALSE)");

    n = QcaBuildTlvSegmentCommand(dummy, sizeof(dummy), 0, QCA_SKIP_EVT_CC,
                                  cmd, sizeof(cmd), &ack);
    CHECK(n > 0 && ack == TRUE,
          "mode QCA_SKIP_EVT_CC: intermediate full segment requires ACK (ack == TRUE)");

    n = QcaBuildTlvSegmentCommand(dummy, sizeof(dummy), 0, QCA_SKIP_EVT_VSE_CC,
                                  cmd, sizeof(cmd), &ack);
    CHECK(n > 0 && ack == FALSE,
          "mode QCA_SKIP_EVT_VSE_CC: intermediate full segment skips ACK (ack == FALSE)");

    n = QcaBuildTlvSegmentCommand(dummy, sizeof(dummy), 0, QCA_SKIP_EVT_NONE,
                                  cmd, sizeof(cmd), &ack);
    CHECK(n > 0 && ack == TRUE,
          "mode QCA_SKIP_EVT_NONE: intermediate full segment requires ACK (ack == TRUE)");

    /*
     * Final segment (segment 1 of 2): ack must be TRUE for all 4 download modes.
     */
    n = QcaBuildTlvSegmentCommand(dummy, sizeof(dummy), 1, QCA_SKIP_EVT_VSE,
                                  cmd, sizeof(cmd), &ack);
    CHECK(n > 0 && ack == TRUE,
          "mode QCA_SKIP_EVT_VSE: final segment requires ACK (ack == TRUE)");

    n = QcaBuildTlvSegmentCommand(dummy, sizeof(dummy), 1, QCA_SKIP_EVT_CC,
                                  cmd, sizeof(cmd), &ack);
    CHECK(n > 0 && ack == TRUE,
          "mode QCA_SKIP_EVT_CC: final segment requires ACK (ack == TRUE)");

    n = QcaBuildTlvSegmentCommand(dummy, sizeof(dummy), 1, QCA_SKIP_EVT_VSE_CC,
                                  cmd, sizeof(cmd), &ack);
    CHECK(n > 0 && ack == TRUE,
          "mode QCA_SKIP_EVT_VSE_CC: final segment requires ACK (ack == TRUE)");

    n = QcaBuildTlvSegmentCommand(dummy, sizeof(dummy), 1, QCA_SKIP_EVT_NONE,
                                  cmd, sizeof(cmd), &ack);
    CHECK(n > 0 && ack == TRUE,
          "mode QCA_SKIP_EVT_NONE: final segment requires ACK (ack == TRUE)");

    /*
     * Short single segment (e.g. 100 bytes total):
     * Short/last segment must be acked regardless of download mode.
     */
    n = QcaBuildTlvSegmentCommand(dummy, 100, 0, QCA_SKIP_EVT_VSE,
                                  cmd, sizeof(cmd), &ack);
    CHECK(n > 0 && ack == TRUE,
          "mode QCA_SKIP_EVT_VSE: short single segment requires ACK (ack == TRUE)");

    n = QcaBuildTlvSegmentCommand(dummy, 100, 0, QCA_SKIP_EVT_VSE_CC,
                                  cmd, sizeof(cmd), &ack);
    CHECK(n > 0 && ack == TRUE,
          "mode QCA_SKIP_EVT_VSE_CC: short single segment requires ACK (ack == TRUE)");
}

int main(void)
{
    TestCommandFraming();
    printf("\n");
    TestRejections();
    printf("\n");
    TestDownloadModeAckRules();
    printf("\n");
    TestRealFirmware("hpbtfw21.tlv", 155044, QCA_TLV_TYPE_PATCH, 639, 10);
    printf("\n");
    TestRealFirmware("hpnv21.bin",     6610, QCA_TLV_TYPE_NVM,    28, 49);
    printf("\n");
    TestRealFirmware("hpnv21g.bin",    6450, QCA_TLV_TYPE_NVM,    27, 132);

    printf("\n%s\n", g_fail ? "QCA SELFTEST FAILED" : "QCA SELFTEST PASSED");
    return g_fail ? 1 : 0;
}
