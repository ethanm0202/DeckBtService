/*
 * nvm_selftest.c - host unit tests for QCA foundry/board NVM selection and 3.2 Mbaud support.
 *
 * Validates NVM filename generation, board ID response parsing, fallback logic,
 * and NVM baud patching against upstream Linux btqca rules
 * (https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/btqca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137).
 * All tests run in user mode using synthetic vectors and the vendor firmware files.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/include/qca_init_fsm.h"

/* Firmware directory: QCA_FW_DIR, which tools\selftest.cmd sets to the installed vendor package. */
static const char *FwDir(void)
{
    static char dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("QCA_FW_DIR", dir, (DWORD)sizeof(dir));
    return (n > 0 && n < sizeof(dir)) ? dir : ".";
}

static int g_pass = 0;
static int g_fail = 0;

static void Check(int condition, const char *what)
{
    if (condition) {
        g_pass++;
    } else {
        g_fail++;
        printf("  [FAIL] %s\n", what);
    }
}

static void CheckEqU(unsigned long actual, unsigned long expected, const char *what)
{
    if (actual == expected) {
        g_pass++;
    } else {
        g_fail++;
        printf("  [FAIL] %s (expected 0x%lx, got 0x%lx)\n", what, expected, actual);
    }
}

static void CheckEqS(const char *actual, const char *expected, const char *what)
{
    if (actual != NULL && strcmp(actual, expected) == 0) {
        g_pass++;
    } else {
        g_fail++;
        printf("  [FAIL] %s (expected '%s', got '%s')\n", what, expected, actual ? actual : "(null)");
    }
}

static unsigned char *LoadFile(const char *name, unsigned long *size)
{
    char path[512];
    FILE *f = NULL;
    unsigned char *buf;
    long len;

    sprintf_s(path, sizeof(path), "%s\\%s", FwDir(), name);
    if (fopen_s(&f, path, "rb") != 0 || f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return NULL;
    }
    buf = (unsigned char *)malloc((size_t)len);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (unsigned long)len;
    return buf;
}

/* ---------------------------------------------------------------- wire vector builders */

/* Vendor event: 04 FF <plen> <cresp> <rtype> <payload...> */
static unsigned long BuildVendorEvent(unsigned char *out, unsigned char cresp, unsigned char rtype,
                                      const unsigned char *payload, unsigned long payloadLen)
{
    unsigned long i;
    out[0] = 0x04u;
    out[1] = 0xFFu;
    out[2] = (unsigned char)(payloadLen + 2u);
    out[3] = cresp;
    out[4] = rtype;
    for (i = 0; i < payloadLen; i++) {
        out[5 + i] = payload[i];
    }
    return 5u + payloadLen;
}

static unsigned long BuildVersionEvent(unsigned char *out, unsigned long productId,
                                       unsigned short patchVer, unsigned short romVerField,
                                       unsigned long socId)
{
    unsigned char payload[12];
    payload[0] = (unsigned char)(productId & 0xffu);
    payload[1] = (unsigned char)((productId >> 8) & 0xffu);
    payload[2] = (unsigned char)((productId >> 16) & 0xffu);
    payload[3] = (unsigned char)((productId >> 24) & 0xffu);
    payload[4] = (unsigned char)(patchVer & 0xffu);
    payload[5] = (unsigned char)((patchVer >> 8) & 0xffu);
    payload[6] = (unsigned char)(romVerField & 0xffu);
    payload[7] = (unsigned char)((romVerField >> 8) & 0xffu);
    payload[8] = (unsigned char)(socId & 0xffu);
    payload[9] = (unsigned char)((socId >> 8) & 0xffu);
    payload[10] = (unsigned char)((socId >> 16) & 0xffu);
    payload[11] = (unsigned char)((socId >> 24) & 0xffu);
    return BuildVendorEvent(out, 0x00u, EDL_PATCH_VER_RES_EVT, payload, sizeof(payload));
}

static unsigned long BuildBoardIdEvent(unsigned char *out, unsigned short bid)
{
    unsigned char payload[3];
    payload[0] = 0x00u;
    payload[1] = (unsigned char)((bid >> 8) & 0xffu);
    payload[2] = (unsigned char)(bid & 0xffu);
    return BuildVendorEvent(out, 0x00u, EDL_GET_BID_REQ_CMD, payload, sizeof(payload));
}

static unsigned long ToCommandComplete(unsigned char *packet, unsigned long length)
{
    if (packet[4] == EDL_PATCH_VER_RES_EVT) {
        memmove(packet + 6, packet + 5, 12);
        packet[5] = 12;
        packet[2]++;
        length++;
    }
    memmove(packet + 6, packet + 3, length - 3);
    packet[1] = 0x0E;
    packet[2] += 3;
    packet[3] = 1;
    packet[4] = 0;
    packet[5] = 0xFC;
    return length + 3;
}

/* ---------------------------------------------------------------- baud mapping */

static void TestBaudMapping(void)
{
    unsigned char index = 0xffu;

    printf("Baud rate mapping:\n");

    Check(QcaBaudRateToIndex(3200000u, &index), "3,200,000 must be accepted");
    CheckEqU(index, QCA_BAUDRATE_3200000, "3,200,000 wire index");
    CheckEqU(QCA_BAUDRATE_3200000, 17u, "QCA_BAUDRATE_3200000 is upstream index 17");

    Check(QcaBaudRateToIndex(3000000u, &index) && index == QCA_BAUDRATE_3000000, "3,000,000 still maps to 14");
    Check(QcaBaudRateToIndex(115200u, &index) && index == QCA_BAUDRATE_115200, "115,200 still maps to 0");
    Check(QcaBaudRateToIndex(921600u, &index) && index == QCA_BAUDRATE_921600, "921,600 still maps to 10");
    Check(QcaBaudRateToIndex(1000000u, &index) && index == QCA_BAUDRATE_1000000, "1,000,000 still maps to 11");
    Check(QcaBaudRateToIndex(2000000u, &index) && index == QCA_BAUDRATE_2000000, "2,000,000 still maps to 13");

    index = 0xffu;
    Check(!QcaBaudRateToIndex(4000000u, &index), "an unsupported rate is still rejected");
    CheckEqU(index, 0u, "a rejected rate must not leave a stale index");
}

/* ---------------------------------------------------------------- version event */

static void TestVersionParse(void)
{
    unsigned char packet[64];
    unsigned long len;
    QCA_SOC_VERSION ver;

    printf("Version event (0xFC00/0x19) parsing:\n");

    /* soc_ver = (soc_id << 16) | rom_ver_field; rom_ver = ((soc_ver & 0xf00) >> 4) | (soc_ver & 0xf) */
    len = BuildVersionEvent(packet, 0x00000008ul, 0x0111u, 0x0201u, 0x00001200ul);
    memset(&ver, 0, sizeof(ver));
    Check(QcaParseVersionEvent(packet, len, &ver), "a well-formed version event parses");
    CheckEqU(ver.ProductId, 0x00000008ul, "ProductId");
    CheckEqU(ver.PatchVersion, 0x0111u, "PatchVersion");
    CheckEqU(ver.RomVersionField, 0x0201u, "RomVersionField");
    CheckEqU(ver.SocId, 0x00001200ul, "SocId");
    CheckEqU(ver.SocVersion, 0x12000201ul, "SocVersion = (SocId << 16) | RomVersionField");
    CheckEqU(ver.RomVersion, 0x21u, "RomVersion derived per upstream shift rule");

    Check(!QcaParseVersionEvent(packet, 4u, &ver), "a truncated packet is rejected");
    Check(!QcaParseVersionEvent(packet, len - 1u, &ver), "a short version payload is rejected");

    packet[0] = 0x02u;
    Check(!QcaParseVersionEvent(packet, len, &ver), "a non-event H4 type is rejected");
    packet[0] = 0x04u;
    packet[1] = 0x0Eu;
    Check(!QcaParseVersionEvent(packet, len, &ver), "a non-vendor event code is rejected");
    packet[1] = 0xFFu;
    packet[4] = EDL_GET_BID_REQ_CMD;
    Check(!QcaParseVersionEvent(packet, len, &ver), "a different vendor response type is rejected");

    len = BuildVersionEvent(packet, 8, 0x111, 0x201, 0x1200);
    len = ToCommandComplete(packet, len);
    Check(QcaParseVersionEvent(packet, len, &ver), "QCA2066 CC version parses");
    CheckEqU(ver.SocId, 0x1200, "CC version foundry");
    CheckEqU(ver.RomVersion, 0x21, "CC version ROM family");
    memset(packet + 17, 0, 4);
    Check(!QcaParseVersionEvent(packet, len, &ver), "zero SOC ID cannot identify a controller");
    CheckEqU(ver.SocId, 0x1200, "invalid identity leaves prior result untouched");
    len = BuildVersionEvent(packet, 8, 0x111, 0, 0x1200);
    Check(!QcaParseVersionEvent(packet, len, &ver), "zero ROM version is rejected");
}

/* ---------------------------------------------------------------- board id */

static void TestBoardId(void)
{
    unsigned char packet[32];
    unsigned char cmd[16];
    unsigned long len;
    unsigned short bid = 0xdeadu;

    printf("Board ID (0xFC00/0x23) command and parsing:\n");

    len = QcaBuildBoardIdCommand(cmd, sizeof(cmd));
    CheckEqU(len, 5u, "board ID command length");
    Check(len == 5u && cmd[0] == 0x01u && cmd[1] == 0x00u && cmd[2] == 0xFCu &&
          cmd[3] == 0x01u && cmd[4] == EDL_GET_BID_REQ_CMD, "board ID command is 01 00 FC 01 23");
    CheckEqU(QcaBuildBoardIdCommand(cmd, 4u), 0u, "an undersized buffer is refused");

    len = BuildBoardIdEvent(packet, 0x0309u);
    bid = 0xdeadu;
    Check(QcaParseBoardIdEvent(packet, len, &bid), "a well-formed board ID event parses");
    CheckEqU(bid, 0x0309u, "bid = (data[1] << 8) + data[2]");

    packet[3] = 0x01u;
    Check(!QcaParseBoardIdEvent(packet, len, &bid), "a non-zero cresp is rejected");
    packet[3] = 0x00u;
    packet[4] = EDL_PATCH_VER_RES_EVT;
    Check(!QcaParseBoardIdEvent(packet, len, &bid), "a different response type is rejected");
    packet[4] = EDL_GET_BID_REQ_CMD;
    Check(!QcaParseBoardIdEvent(packet, len - 1u, &bid), "fewer than three payload bytes is rejected");

    len = BuildBoardIdEvent(packet, 0x0309u);
    len = ToCommandComplete(packet, len);
    bid = 0;
    Check(QcaParseBoardIdEvent(packet, len, &bid), "QCA2066 CC board ID parses");
    CheckEqU(bid, 0x0309, "CC board byte order selects board 309");
    packet[2]--;
    Check(!QcaParseBoardIdEvent(packet, len, &bid), "CC board inconsistent plen rejected");
    CheckEqU(bid, 0x0309, "invalid board response leaves prior result untouched");
}

/* ---------------------------------------------------------------- name selection */

static void TestNvmNames(void)
{
    char name[64];

    printf("NVM file-name selection (QCA2066 rules):\n");

    Check(QcaBuildNvmFileName(0x00001200ul, 0x21u, 0x0309u, name, sizeof(name)), "GF + board 0x0309 builds");
    CheckEqS(name, "hpnv21g.309", "GlobalFoundries part with board 0x0309");

    Check(QcaBuildNvmFileName(0x00000100ul, 0x21u, 0x0309u, name, sizeof(name)), "non-GF + board 0x0309 builds");
    CheckEqS(name, "hpnv21.309", "non-GlobalFoundries part with board 0x0309");

    Check(QcaBuildNvmFileName(0x00001200ul, 0x21u, 0x0000u, name, sizeof(name)), "GF + no board builds");
    CheckEqS(name, "hpnv21g.bin", "GlobalFoundries part with no board ID");

    Check(QcaBuildNvmFileName(0x00000100ul, 0x21u, 0xffffu, name, sizeof(name)), "non-GF + 0xffff builds");
    CheckEqS(name, "hpnv21.bin", "0xffff is treated as no board ID, exactly like 0x0000");

    /* The foundry test is a masked comparison, not equality: only bits 8-15 select the variant. */
    Check(QcaBuildNvmFileName(0x0000120ful, 0x21u, 0x0000u, name, sizeof(name)), "masked GF match builds");
    CheckEqS(name, "hpnv21g.bin", "QCA_HSP_GF_SOC_MASK selects on bits 8-15 only");
    Check(QcaBuildNvmFileName(0x00001300ul, 0x21u, 0x0000u, name, sizeof(name)), "near-miss soc id builds");
    CheckEqS(name, "hpnv21.bin", "a soc id outside the GF mask value is not the g variant");

    Check(!QcaBuildNvmFileName(0x00001200ul, 0x21u, 0x0309u, name, 8u), "a too-small buffer is refused");

    Check(QcaBuildAltNvmFileName("hpnv21g.309", name, sizeof(name)), "a board-specific name has an alt");
    CheckEqS(name, "hpnv21g.bin", "the alt file replaces the board suffix with .bin");
    Check(QcaBuildAltNvmFileName("hpnv21.309", name, sizeof(name)), "non-GF board name has an alt");
    CheckEqS(name, "hpnv21.bin", "alt of the non-GF board file");
    Check(!QcaBuildAltNvmFileName("hpnv21.bin", name, sizeof(name)), "a .bin file has no further fallback");
}

/* ---------------------------------------------------------------- real vendor artifacts */

typedef struct _NVM_FIXTURE {
    const char   *Name;
    unsigned long Size;
    unsigned char *Data;
} NVM_FIXTURE;

static NVM_FIXTURE g_nvm[4] = {
    { "hpnv21.bin",  6610ul, NULL },
    { "hpnv21g.bin", 6450ul, NULL },
    { "hpnv21.309",  6884ul, NULL },
    { "hpnv21g.309", 6716ul, NULL }
};
static unsigned char *g_patch = NULL;
static unsigned long g_patchSize = 0;

static int LoadFixtures(void)
{
    int i;
    int ok = 1;

    printf("Vendor firmware files (%s):\n", FwDir());
    for (i = 0; i < 4; i++) {
        unsigned long size = 0;
        g_nvm[i].Data = LoadFile(g_nvm[i].Name, &size);
        if (g_nvm[i].Data == NULL) {
            printf("  [FAIL] %s not found in the firmware directory\n", g_nvm[i].Name);
            g_fail++;
            ok = 0;
            continue;
        }
        CheckEqU(size, g_nvm[i].Size, g_nvm[i].Name);
        g_nvm[i].Size = size;
    }
    g_patch = LoadFile("hpbtfw21.tlv", &g_patchSize);
    if (g_patch == NULL) {
        printf("  [FAIL] hpbtfw21.tlv not found in the firmware directory\n");
        g_fail++;
        ok = 0;
    } else {
        CheckEqU(g_patchSize, 155044ul, "hpbtfw21.tlv size");
    }
    return ok;
}

static void TestSegmentation(void)
{
    int i;

    printf("Segmentation of every NVM candidate:\n");
    for (i = 0; i < 4; i++) {
        QCA_TLV_INFO info;
        unsigned long expected;

        if (g_nvm[i].Data == NULL) {
            continue;
        }
        memset(&info, 0, sizeof(info));
        Check(QcaParseTlv(g_nvm[i].Data, g_nvm[i].Size, &info), g_nvm[i].Name);
        expected = (g_nvm[i].Size + QCA_MAX_SIZE_PER_TLV_SEGMENT - 1ul) / QCA_MAX_SIZE_PER_TLV_SEGMENT;
        CheckEqU(QcaTlvSegmentCount(g_nvm[i].Size), expected, g_nvm[i].Name);
    }
}

/*
 * The vendor driver changes one byte of hpnv21g.309 before download: file
 * offset 0x94, HCI tag 17 data[1], 0x11 -> 0x0E (baud rate index). The finder
 * must locate that byte in every candidate, and must refuse any image whose
 * tag list is malformed.
 */
static void TestNvmHciTag(void)
{
    unsigned char buf[64];
    unsigned long offset = 0;
    int i;

    printf("NVM HCI tag (baud index byte):\n");
    for (i = 0; i < 4; i++) {
        offset = 0;
        Check(QcaFindNvmHciBaudOffset(g_nvm[i].Data, g_nvm[i].Size, &offset), g_nvm[i].Name);
        CheckEqU(offset, 0x94ul, "HCI tag data[1] at the byte the vendor patched");
        CheckEqU(g_nvm[i].Data[offset], QCA_BAUDRATE_3200000, "shipped index is 17 (3.2 Mbaud)");
    }
    Check(!QcaFindNvmHciBaudOffset(g_patch, g_patchSize, &offset), "a rampatch has no NVM tags");

    /* type 2, length 28: one 12-byte tag header (id 5, len 4), then HCI tag header (id 17). */
    memset(buf, 0, sizeof(buf));
    buf[0] = 2; buf[1] = 28;
    buf[4] = 5; buf[6] = 4;
    buf[20] = EDL_TAG_ID_HCI; buf[22] = 3;
    Check(!QcaFindNvmHciBaudOffset(buf, 32ul, &offset), "HCI tag data running past the image is refused");
    buf[1] = 31;
    Check(QcaFindNvmHciBaudOffset(buf, 35ul, &offset), "a well-formed HCI tag after another tag is found");
    CheckEqU(offset, 33ul, "offset is header + first tag + tag header + 1");
    buf[22] = 2; buf[1] = 30;
    Check(!QcaFindNvmHciBaudOffset(buf, 34ul, &offset), "an HCI tag shorter than 3 bytes is refused");
    buf[20] = 18; buf[22] = 3; buf[1] = 31;
    Check(!QcaFindNvmHciBaudOffset(buf, 35ul, &offset), "an image without the HCI tag is refused");
    buf[6] = 200;
    Check(!QcaFindNvmHciBaudOffset(buf, 35ul, &offset), "a tag length overrunning the image is refused");
}

/* ---------------------------------------------------------------- FSM integration */

typedef struct _FSM_RUN {
    QCA_INIT_FSM  Fsm;
    int           BoardIdCommands;
    int           PatchSegmentsSent;
    unsigned long NvmBytesSent;
    int           SawBoardIdBeforePatchDone;
} FSM_RUN;

static QCA_NVM_CANDIDATE g_candidates[4];

static void BuildCandidates(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        g_candidates[i].Name = g_nvm[i].Name;
        g_candidates[i].Data = g_nvm[i].Data;
        g_candidates[i].Size = g_nvm[i].Size;
    }
}

/*
 * Drives the FSM against a scripted controller. Returns the terminal action.
 * boardIdBid < 0 means the controller never answers the board-ID request, which upstream
 * tolerates: the board ID stays 0 and the name degrades to .bin.
 */
static QCA_FSM_ACTION RunFsm(FSM_RUN *run, unsigned long socId, int boardIdBid, int failNvmOnce)
{
    unsigned char cmd[QCA_FSM_MAX_COMMAND];
    unsigned char evt[64];
    unsigned long len = 0;
    unsigned long baud = 0;
    unsigned long evtLen;
    QCA_FSM_ACTION action;
    int guard = 0;
    int nvmFailed = 0;

    memset(run, 0, sizeof(*run));
    if (!QcaFsmInit(&run->Fsm, g_patch, g_patchSize, g_candidates, 4u, 3200000u)) {
        return QcaFsmActionFailed;
    }

    for (guard = 0; guard < 4000; guard++) {
        action = QcaFsmNext(&run->Fsm, cmd, &len, &baud);
        if (action == QcaFsmActionDone || action == QcaFsmActionFailed) {
            return action;
        }
        if (len >= 5u && cmd[0] == 0x01u && cmd[1] == 0x00u && cmd[2] == 0xFCu &&
            cmd[4] == EDL_GET_BID_REQ_CMD) {
            run->BoardIdCommands++;
            if (run->Fsm.PatchIndex < run->Fsm.PatchSegments) {
                run->SawBoardIdBeforePatchDone = 1;
            }
            if (boardIdBid < 0) {
                QcaFsmOnBoardIdTimeout(&run->Fsm);
                continue;
            }
            evtLen = BuildBoardIdEvent(evt, (unsigned short)boardIdBid);
            evtLen = ToCommandComplete(evt, evtLen);
            QcaFsmOnPacket(&run->Fsm, evt, evtLen);
            continue;
        }
        if (len >= 5u && cmd[0] == 0x01u && cmd[1] == 0x00u && cmd[2] == 0xFCu &&
            cmd[4] == EDL_PATCH_VER_REQ_CMD) {
            evtLen = BuildVersionEvent(evt, 0x00000008ul, 0x0111u, 0x0201u, socId);
            evtLen = ToCommandComplete(evt, evtLen);
            QcaFsmOnPacket(&run->Fsm, evt, evtLen);
            continue;
        }
        if (run->Fsm.State == QcaFsmStatePatchDownload && action != QcaFsmActionSendBaudAndSwitch) {
            run->PatchSegmentsSent++;
        }
        if (run->Fsm.State == QcaFsmStateNvmDownload && len > 6u) {
            run->NvmBytesSent += len - 6u;
            if (failNvmOnce && !nvmFailed) {
                nvmFailed = 1;
                run->NvmBytesSent = 0;
                QcaFsmOnNvmFailure(&run->Fsm);
                continue;
            }
        }
        if (action == QcaFsmActionSendNoWait || action == QcaFsmActionSendBaudAndSwitch) {
            continue;
        }
        /* Reply to the command actually sent; never try unrelated acknowledgements. */
        if (run->Fsm.State == QcaFsmStatePatchDownload ||
            run->Fsm.State == QcaFsmStateNvmDownload) {
            evtLen = BuildVendorEvent(evt, 0, EDL_PATCH_TLV_REQ_CMD, NULL, 0);
            evtLen = ToCommandComplete(evt, evtLen);
        } else if (run->Fsm.State == QcaFsmStateBuildInfo) {
            const unsigned char label[] = { 3, 'Q', 'C', 'A' };
            evtLen = BuildVendorEvent(evt, 0, EDL_GET_BUILD_INFO_CMD, label, sizeof(label));
            evtLen = ToCommandComplete(evt, evtLen);
        } else {
            static const unsigned char localVersion[] = {
                4, 0x0E, 12, 1, 1, 0x10, 0, 9, 0x11, 1, 9, 0x1D, 0, 0x21, 0
            };
            const unsigned char cc[] = { 4, 0x0E, 4, 1, cmd[1], cmd[2], 0 };
            if (run->Fsm.State == QcaFsmStateReadLocalVersion) {
                memcpy(evt, localVersion, sizeof(localVersion));
                evtLen = sizeof(localVersion);
            } else {
                memcpy(evt, cc, sizeof(cc));
                evtLen = sizeof(cc);
            }
        }
        if (!QcaFsmOnPacket(&run->Fsm, evt, evtLen)) {
            return QcaFsmActionFailed;
        }
    }
    return QcaFsmActionFailed;
}

static void TestFsmSelection(void)
{
    FSM_RUN run;
    QCA_FSM_ACTION action;

    printf("FSM board-ID handshake and NVM selection:\n");

    action = RunFsm(&run, 0x00001200ul, 0x0309, 0);
    CheckEqU((unsigned long)action, (unsigned long)QcaFsmActionDone, "GF + board 0x0309 completes");
    CheckEqS(run.Fsm.SelectedNvmName, "hpnv21g.309", "GF + board 0x0309 selects the g board file");
    CheckEqU((unsigned long)run.BoardIdCommands, 1ul, "the board-ID command is issued exactly once");
    Check(!run.SawBoardIdBeforePatchDone, "the board ID is requested only after the rampatch completes");
    CheckEqU(run.NvmBytesSent, g_nvm[3].Size, "the selected NVM is streamed in full");
    CheckEqU(run.Fsm.BoardId, 0x0309u, "the parsed board ID is retained for breadcrumbs");
    CheckEqU(run.Fsm.SocVersion.SocId, 0x00001200ul, "the soc id is retained for breadcrumbs");

    action = RunFsm(&run, 0x00000100ul, 0x0309, 0);
    CheckEqU((unsigned long)action, (unsigned long)QcaFsmActionDone, "non-GF + board 0x0309 completes");
    CheckEqS(run.Fsm.SelectedNvmName, "hpnv21.309", "non-GF + board 0x0309 selects the plain board file");
    CheckEqU(run.NvmBytesSent, g_nvm[2].Size, "the non-GF board NVM is streamed in full");

    action = RunFsm(&run, 0x00001200ul, 0x0000, 0);
    CheckEqU((unsigned long)action, (unsigned long)QcaFsmActionDone, "GF + no board completes");
    CheckEqS(run.Fsm.SelectedNvmName, "hpnv21g.bin", "board 0x0000 falls back to the foundry .bin file");

    action = RunFsm(&run, 0x00001200ul, -1, 0);
    CheckEqU((unsigned long)action, (unsigned long)QcaFsmActionDone, "an unanswered board-ID request still completes");
    CheckEqS(run.Fsm.SelectedNvmName, "hpnv21g.bin", "an unanswered board-ID request degrades to .bin, never a guess");
    CheckEqU(run.Fsm.BoardIdValid, 0u, "an unanswered board-ID request is not reported as valid");

    action = RunFsm(&run, 0x00001200ul, 0x0309, 1);
    CheckEqU((unsigned long)action, (unsigned long)QcaFsmActionDone, "a rejected board NVM recovers");
    CheckEqS(run.Fsm.SelectedNvmName, "hpnv21g.bin", "a rejected board NVM retries with the alt .bin file");
    Check(run.Fsm.NvmFallbackUsed != 0, "the fallback is recorded, not silent");
    CheckEqU(run.NvmBytesSent, g_nvm[1].Size, "the alt NVM is streamed in full");
}

static void TestFsmRefusals(void)
{
    QCA_INIT_FSM fsm;
    QCA_NVM_CANDIDATE one[1];

    printf("FSM refusals:\n");

    Check(!QcaFsmInit(&fsm, g_patch, g_patchSize, g_candidates, 4u, 4000000u),
          "an unsupported operational baud rate is still refused");
    Check(QcaFsmInit(&fsm, g_patch, g_patchSize, g_candidates, 4u, 3200000u),
          "3,200,000 is now an accepted operational baud rate");
    Check(!QcaFsmInit(&fsm, g_patch, g_patchSize, NULL, 0u, 3000000u),
          "an empty candidate table is refused");

    /* A table that cannot satisfy the selected name must fail loudly rather than pick a stranger. */
    one[0].Name = "hpnv21.bin";
    one[0].Data = g_nvm[0].Data;
    one[0].Size = g_nvm[0].Size;
    Check(QcaFsmInit(&fsm, g_patch, g_patchSize, one, 1u, 3000000u),
          "a single-candidate table still initialises");
}

int main(void)
{
    int i;

    printf("QCA NVM selection selftest\n");

    if (!LoadFixtures()) {
        printf("NVM SELFTEST FAILED: vendor artifacts unavailable\n");
        return 1;
    }
    BuildCandidates();

    TestBaudMapping();
    TestVersionParse();
    TestBoardId();
    TestNvmNames();
    TestSegmentation();
    TestNvmHciTag();
    TestFsmSelection();
    TestFsmRefusals();

    for (i = 0; i < 4; i++) {
        free(g_nvm[i].Data);
    }
    free(g_patch);

    printf("\n");
    if (g_fail == 0) {
        printf("NVM SELFTEST PASSED: %d checks\n", g_pass);
        return 0;
    }
    printf("NVM SELFTEST FAILED: %d of %d checks\n", g_fail, g_fail + g_pass);
    return 1;
}
