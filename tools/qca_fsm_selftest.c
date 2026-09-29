/*
 * qca_fsm_selftest.c - drives src/common/qca_init_fsm.c through a complete QCA2066
 * bring-up against a mock chip, using firmware images from the DriverStore.
 *
 * Exercises the initialization finite state machine in user mode against mock chip
 * responses, validating patch download, NVM download, baud switching, and error handling.
 *
 * Build: tools\selftest.cmd
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

static int g_fail = 0;

#define CHECK(cond, ...)                           \
    do {                                           \
        if (cond) { printf("  ok   "); }           \
        else      { printf("  FAIL "); g_fail++; } \
        printf(__VA_ARGS__); printf("\n");         \
    } while (0)

static UCHAR *LoadFile(const char *name, ULONG *size)
{
    char path[MAX_PATH];
    FILE *f = NULL;
    UCHAR *buf;
    long len;

    *size = 0;
    sprintf_s(path, sizeof(path), "%s\\%s", FwDir(), name);
    if (fopen_s(&f, path, "rb") != 0 || f == NULL) { return NULL; }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    buf = (UCHAR *)malloc((size_t)len);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = (ULONG)len;
    return buf;
}

/* ---------------------------------------------------------------- mock chip */

typedef struct {
    ULONG BytesReceived;
    ULONG CommandsReceived;
    ULONG TlvSegmentsReceived;
    ULONG HostBaudSwitches;
    ULONG BaudAtFirstPatchSegment;   /* records the ordering bug if the switch comes too late */
    ULONG CurrentHostBaud;
    ULONG StateAfterBaudSwitch;      /* FSM state when the host switches; must already be patch */
    BOOLEAN UseCommandComplete;     /* QCA2066 EDL Command Complete envelope */
    UCHAR *Tlv;                     /* every TLV payload byte received, in order (patch, NVM) */
    ULONG TlvLength;
    ULONG TlvCapacity;
    UCHAR Reply[64];
    ULONG ReplyLength;
} MOCK_CHIP;

static void MockVendorEvent(MOCK_CHIP *c, UCHAR rtype, UCHAR payload)
{
    c->Reply[0] = H4_PKT_EVENT;
    c->Reply[1] = HCI_EV_VENDOR;
    c->Reply[2] = 3;            /* plen: cresp, rtype, payload */
    c->Reply[3] = EDL_CMD_REQ_RES_EVT;
    c->Reply[4] = rtype;
    c->Reply[5] = payload;
    c->ReplyLength = 6;
}
static void MockVersionEvent(MOCK_CHIP *c, ULONG productId, USHORT patchVer,
                             USHORT romVerField, ULONG socId)
{
    c->Reply[0] = H4_PKT_EVENT;
    c->Reply[1] = HCI_EV_VENDOR;
    c->Reply[2] = 14;           /* plen: cresp (1) + rtype (1) + payload (12) */
    c->Reply[3] = EDL_CMD_REQ_RES_EVT;
    c->Reply[4] = EDL_PATCH_VER_RES_EVT;
    c->Reply[5] = (UCHAR)(productId & 0xFF);
    c->Reply[6] = (UCHAR)((productId >> 8) & 0xFF);
    c->Reply[7] = (UCHAR)((productId >> 16) & 0xFF);
    c->Reply[8] = (UCHAR)((productId >> 24) & 0xFF);
    c->Reply[9] = (UCHAR)(patchVer & 0xFF);
    c->Reply[10] = (UCHAR)((patchVer >> 8) & 0xFF);
    c->Reply[11] = (UCHAR)(romVerField & 0xFF);
    c->Reply[12] = (UCHAR)((romVerField >> 8) & 0xFF);
    c->Reply[13] = (UCHAR)(socId & 0xFF);
    c->Reply[14] = (UCHAR)((socId >> 8) & 0xFF);
    c->Reply[15] = (UCHAR)((socId >> 16) & 0xFF);
    c->Reply[16] = (UCHAR)((socId >> 24) & 0xFF);
    c->ReplyLength = 17;
}

static void MockBoardIdEvent(MOCK_CHIP *c, USHORT bid)
{
    c->Reply[0] = H4_PKT_EVENT;
    c->Reply[1] = HCI_EV_VENDOR;
    c->Reply[2] = 5;            /* plen: cresp (1) + rtype (1) + payload (3) */
    c->Reply[3] = EDL_CMD_REQ_RES_EVT;
    c->Reply[4] = EDL_GET_BID_REQ_CMD;
    c->Reply[5] = 0x00;
    c->Reply[6] = (UCHAR)((bid >> 8) & 0xFF);
    c->Reply[7] = (UCHAR)(bid & 0xFF);
    c->ReplyLength = 8;
}
/* QCA2066 returns the EDL header after ncmd/opcode in Command Complete. */
static void MockEdlCommandComplete(MOCK_CHIP *c)
{
    if (c->Reply[4] == EDL_PATCH_VER_RES_EVT) {
        memmove(c->Reply + 6, c->Reply + 5, 12);
        c->Reply[5] = 12;
        c->ReplyLength++;
        c->Reply[2]++;
    }
    memmove(c->Reply + 6, c->Reply + 3, c->ReplyLength - 3);
    c->Reply[1] = 0x0E;
    c->Reply[2] += 3;
    c->Reply[3] = 1;
    c->Reply[4] = 0x00;
    c->Reply[5] = 0xFC;
    c->ReplyLength += 3;
}



static void MockCommandComplete(MOCK_CHIP *c, USHORT opcode, UCHAR status)
{
    c->Reply[0] = H4_PKT_EVENT;
    c->Reply[1] = 0x0E;
    c->Reply[2] = 4;
    c->Reply[3] = 1;
    c->Reply[4] = (UCHAR)(opcode & 0xFF);
    c->Reply[5] = (UCHAR)(opcode >> 8);
    c->Reply[6] = status;
    c->ReplyLength = 7;
}

/* Consumes one command and, when the chip would answer, stages the reply. */
static void MockConsume(MOCK_CHIP *c, const UCHAR *cmd, ULONG len)
{
    USHORT opcode;

    c->ReplyLength = 0;
    c->BytesReceived += len;
    if (len < 4 || cmd[0] != H4_PKT_COMMAND) { return; }
    c->CommandsReceived++;

    opcode = (USHORT)(cmd[1] | ((USHORT)cmd[2] << 8));

    if (opcode == EDL_PATCH_CMD_OPCODE) {
        UCHAR sub = cmd[4];
        if (sub == EDL_PATCH_VER_REQ_CMD) {
            MockVersionEvent(c, 0x00000008ul, 0x0111u, 0x0201u, 0x00000100ul);
        } else if (sub == EDL_PATCH_TLV_REQ_CMD) {
            c->TlvSegmentsReceived++;
            if (c->Tlv != NULL && len >= 6u && c->TlvLength + cmd[5] <= c->TlvCapacity) {
                memcpy(c->Tlv + c->TlvLength, cmd + 6, cmd[5]);
                c->TlvLength += cmd[5];
            }
            if (c->TlvSegmentsReceived == 1) {
                c->BaudAtFirstPatchSegment = c->CurrentHostBaud;
            }
            MockVendorEvent(c, EDL_TVL_DNLD_RES_EVT, 0);
            if (c->UseCommandComplete) {
                c->Reply[4] = EDL_PATCH_TLV_REQ_CMD;
                c->Reply[2] = 2;
                c->ReplyLength = 5;  /* modern EDL ack has no result byte */
            }
        } else if (sub == EDL_GET_BID_REQ_CMD) {
            MockBoardIdEvent(c, 0x0000);
        } else if (sub == EDL_GET_BUILD_INFO_CMD) {
            MockVendorEvent(c, EDL_GET_BUILD_INFO_CMD, 3);
            memcpy(c->Reply + 6, "QCA", 3);
            c->Reply[2] += 3;
            c->ReplyLength += 3;
        }
        if (c->UseCommandComplete && c->ReplyLength != 0) {
            MockEdlCommandComplete(c);
        }
        return;
    }
    if (opcode == QCA_BAUDRATE_CMD_OPCODE) {
        /* The vendor driver's trace shows no event for 48 FC 01 0E; upstream reads none. */
        return;
    }
    if (opcode == QCA_DISABLE_LOGGING) {
        CHECK(len == 6 && cmd[3] == 2 &&
              cmd[4] == QCA_DISABLE_LOGGING_SUB_OP && cmd[5] == 0,
              "disable logging emits 01 17 FC 02 14 00");
        MockCommandComplete(c, opcode, 0);
        if (c->UseCommandComplete) {
            /* QCA2066 echoes sub-op: 04 0E 05 01 17 FC 00 14. */
            c->Reply[2] = 5;
            c->Reply[7] = QCA_DISABLE_LOGGING_SUB_OP;
            c->ReplyLength = 8;
        }
        return;
    }
    MockCommandComplete(c, opcode, 0);   /* HCI_Reset, Read_Local_Version */
    if (opcode == 0x1001) {
        /* status + HCI version/revision + LMP version/manufacturer/subversion */
        static const UCHAR version[] = { 9, 0x11, 0x01, 9, 0x1D, 0, 0x21, 0 };
        memcpy(c->Reply + 7, version, sizeof(version));
        c->Reply[2] += sizeof(version);
        c->ReplyLength += sizeof(version);
    }
}

/*
 * Rejection must preserve the entire pending operation, not just its state enum.
 * The only permitted mutation is one UnexpectedEvents increment.
 */
static void CheckRejected(const QCA_INIT_FSM *pending, const UCHAR *packet,
                          ULONG length, const char *reason)
{
    QCA_INIT_FSM actual = *pending;
    QCA_INIT_FSM expected = *pending;
    BOOLEAN handled = QcaFsmOnPacket(&actual, packet, length);
    expected.UnexpectedEvents++;
    CHECK(!handled && memcmp(&actual, &expected, sizeof(actual)) == 0,
          "%s rejects %s without changing pending transfer/selection",
          QcaFsmStateName(pending->State), reason);
}

static void CheckAdversarialResponses(const QCA_INIT_FSM *pending, const MOCK_CHIP *chip,
                                     USHORT opcode, const UCHAR *prior, ULONG priorLength)
{
    static const UCHAR asyncEvent[] = { 4, 0x13, 1, 0 }; /* completed packets, zero handles */
    MOCK_CHIP bad = { 0 };
    ULONG edlOffset = chip->Reply[1] == HCI_EV_VENDOR ? 3u : 6u;

    MockCommandComplete(&bad, 0x9999, 0);
    CheckRejected(pending, bad.Reply, bad.ReplyLength, "unrelated Command Complete");
    MockCommandComplete(&bad, opcode, 0x0C);
    CheckRejected(pending, bad.Reply, bad.ReplyLength, "failed outstanding opcode");
    MockVendorEvent(&bad, 0x7F, 0);
    CheckRejected(pending, bad.Reply, bad.ReplyLength, "unrelated vendor event");
    CheckRejected(pending, asyncEvent, sizeof(asyncEvent), "asynchronous HCI event");
    if (priorLength != 0) {
        CheckRejected(pending, prior, priorLength, "previous state's duplicate response");
    }
    CheckRejected(pending, NULL, 0, "absent packet");
    CheckRejected(pending, chip->Reply, 2, "truncated HCI header");
    CheckRejected(pending, chip->Reply, chip->ReplyLength - 1, "truncated expected reply");
    bad = *chip;
    bad.Reply[2]--;
    CheckRejected(pending, bad.Reply, bad.ReplyLength, "underdeclared HCI plen");
    bad = *chip;
    bad.Reply[2]++;
    CheckRejected(pending, bad.Reply, bad.ReplyLength, "overdeclared HCI plen");
    bad = *chip;
    bad.Reply[bad.ReplyLength] = 0;
    CheckRejected(pending, bad.Reply, bad.ReplyLength + 1, "bytes beyond declared event");
    bad = *chip;
    bad.Reply[0] = H4_PKT_ACL;
    CheckRejected(pending, bad.Reply, bad.ReplyLength, "wrong H4 packet type");

    if (opcode == EDL_PATCH_CMD_OPCODE) {
        bad = *chip;
        bad.Reply[edlOffset] = 1;
        CheckRejected(pending, bad.Reply, bad.ReplyLength, "nonzero EDL response status");
        bad = *chip;
        bad.Reply[edlOffset + 1] = 0x7F;
        CheckRejected(pending, bad.Reply, bad.ReplyLength, "wrong EDL subcommand");
        MockCommandComplete(&bad, EDL_PATCH_CMD_OPCODE, 0);
        CheckRejected(pending, bad.Reply, bad.ReplyLength, "bare FC00 completion without selector");
        if (chip->UseCommandComplete) {
            bad = *chip;
            bad.Reply[4] = 0x17;
            CheckRejected(pending, bad.Reply, bad.ReplyLength, "EDL payload under wrong opcode");
        }
    }
    if (pending->State == QcaFsmStatePatchDownload) {
        MockBoardIdEvent(&bad, 0x0309);
        CheckRejected(pending, bad.Reply, bad.ReplyLength, "premature board ID vendor reply");
        MockEdlCommandComplete(&bad);
        CheckRejected(pending, bad.Reply, bad.ReplyLength, "premature board ID Command Complete");
    }
    if (pending->State == QcaFsmStatePatchDownload ||
        pending->State == QcaFsmStateNvmDownload) {
        MockVendorEvent(&bad, EDL_TVL_DNLD_RES_EVT, 1);
        CheckRejected(pending, bad.Reply, bad.ReplyLength, "failed TLV result");
        bad.Reply[2] = 2;
        CheckRejected(pending, bad.Reply, 5, "TLV reply missing result");
    }
    if (pending->State == QcaFsmStateBuildInfo) {
        bad = *chip;
        bad.Reply[edlOffset + 2] = 0xFF;
        CheckRejected(pending, bad.Reply, bad.ReplyLength, "truncated build label");
        bad.Reply[2] = (UCHAR)(edlOffset - 1);
        CheckRejected(pending, bad.Reply, edlOffset + 2, "build label length absent");
    }
    if (pending->State == QcaFsmStateReadLocalVersion) {
        MockCommandComplete(&bad, 0x1001, 0);
        CheckRejected(pending, bad.Reply, bad.ReplyLength, "version proof missing version fields");
    }
}

/*
 * Pumps the FSM to completion, injecting adversarial traffic at every waiting state before
 * delivering the correct response. Exercises production sequencing with real firmware bytes.
 */
static QCA_FSM_ACTION RunToCompletion(QCA_INIT_FSM *fsm, MOCK_CHIP *chip, ULONG maxSteps,
                                      ULONG *steps)
{
    UCHAR cmd[QCA_FSM_MAX_COMMAND];
    ULONG len = 0, baud = 0;
    QCA_FSM_ACTION action = QcaFsmActionFailed;
    ULONG checkedStates = 0;
    UCHAR prior[64] = { 0 };
    ULONG priorLength = 0;

    *steps = 0;
    while ((*steps)++ < maxSteps) {
        action = QcaFsmNext(fsm, cmd, &len, &baud);
        if (action == QcaFsmActionDone || action == QcaFsmActionFailed) { break; }

        MockConsume(chip, cmd, len);
        if (action == QcaFsmActionSendBaudAndSwitch) {
            /* Nothing is read for 0xFC48; the machine must already have moved on. */
            chip->HostBaudSwitches++;
            chip->CurrentHostBaud = baud;
            chip->StateAfterBaudSwitch = (ULONG)fsm->State;
        }
        if (action == QcaFsmActionSend) {
            if (chip->ReplyLength == 0) { return QcaFsmActionFailed; }
            if (!(checkedStates & (1u << fsm->State))) {
                CheckAdversarialResponses(fsm, chip,
                    (USHORT)(cmd[1] | ((USHORT)cmd[2] << 8)), prior, priorLength);
                checkedStates |= 1u << fsm->State;
            }
            if (!QcaFsmOnPacket(fsm, chip->Reply, chip->ReplyLength)) {
                return QcaFsmActionFailed;
            }
            memcpy(prior, chip->Reply, chip->ReplyLength);
            priorLength = chip->ReplyLength;
        }
    }
    CHECK(checkedStates == 0x3FAu, "adversarial replies exercised all eight waiting states");
    return action;
}

int main(void)
{
    ULONG patchSize = 0, nvmSize = 0;
    UCHAR *patch = LoadFile("hpbtfw21.tlv", &patchSize);
    UCHAR *nvm   = LoadFile("hpnv21.bin",   &nvmSize);
    QCA_INIT_FSM fsm;
    MOCK_CHIP chip;
    ULONG steps = 0;
    QCA_FSM_ACTION action;
    ULONG expectedSegments;
    QCA_NVM_CANDIDATE candidates[1];
    QCA_NVM_CANDIDATE badCandidates[1];
    QCA_NVM_CANDIDATE noHciTag[1];
    UCHAR *nvmNoHci;
    UCHAR *wire;
    ULONG wireCapacity;
    ULONG i, nvmDiffs = 0, diffAt = 0;

    if (!patch || !nvm) {
        printf("FAIL cannot read the firmware images from the DriverStore\n");
        return 1;
    }
    candidates[0].Name = "hpnv21.bin";
    candidates[0].Data = nvm;
    candidates[0].Size = nvmSize;

    badCandidates[0].Name = "bad.bin";
    badCandidates[0].Data = patch;
    badCandidates[0].Size = patchSize;

    /* Same NVM with the HCI tag renamed: tag header at 0x87, data[1] at 0x94 in every hpnv21*. */
    nvmNoHci = (UCHAR *)malloc(nvmSize);
    wireCapacity = patchSize + nvmSize;
    wire = (UCHAR *)malloc(wireCapacity);
    if (!nvmNoHci || !wire) {
        printf("FAIL out of memory\n");
        return 1;
    }
    memcpy(nvmNoHci, nvm, nvmSize);
    CHECK(nvm[0x87] == EDL_TAG_ID_HCI && nvm[0x88] == 0 && nvm[0x94] == QCA_BAUDRATE_3200000,
          "hpnv21.bin HCI tag sits at 0x87 and names index 17 (3.2 Mbaud)");
    nvmNoHci[0x87] = 0x7F;
    noHciTag[0].Name = "hpnv21.bin";
    noHciTag[0].Data = nvmNoHci;
    noHciTag[0].Size = nvmSize;

    printf("initialisation guards\n");
    CHECK(QcaFsmInit(&fsm, patch, patchSize, candidates, 1, 3000000),
          "accepts the real images at 3,000,000 baud");
    CHECK(fsm.PatchSegments == 639,
          "patch segment count 639 (got %lu)", fsm.PatchSegments);
    CHECK(fsm.PatchDownloadMode == QCA_SKIP_EVT_VSE_CC,
          "patch declares download mode 3, so intermediate segments go unacked");
    CHECK(!QcaFsmInit(&fsm, patch, patchSize, candidates, 1, 4000000),
          "rejects an unsupported baud rate before touching the chip");
    CHECK(!QcaFsmInit(&fsm, nvm, nvmSize, candidates, 1, 3000000),
          "rejects an NVM image supplied as the rampatch");
    CHECK(!QcaFsmInit(&fsm, patch, patchSize, badCandidates, 1, 3000000),
          "rejects a rampatch supplied as the NVM image");
    CHECK(!QcaFsmInit(&fsm, patch, 3, candidates, 1, 3000000),
          "rejects a truncated rampatch");
    CHECK(!QcaFsmInit(&fsm, patch, patchSize, noHciTag, 1, 3000000),
          "rejects an NVM with no HCI tag: its baud byte cannot be patched");

    printf("\nfull bring-up against the mock chip\n");
    memset(&chip, 0, sizeof(chip));
    chip.CurrentHostBaud = 115200;
    chip.Tlv = wire;
    chip.TlvCapacity = wireCapacity;
    if (!QcaFsmInit(&fsm, patch, patchSize, candidates, 1, 3000000)) {
        printf("  FAIL re-init failed\n");
        return 1;
    }

    action = RunToCompletion(&fsm, &chip, 2000, &steps);

    CHECK(action == QcaFsmActionDone, "reaches Done (action=%d, state=%s)",
          (int)action, QcaFsmStateName(fsm.State));
    CHECK(fsm.State == QcaFsmStateReady, "final state Ready (got %s)", QcaFsmStateName(fsm.State));
    CHECK(fsm.NvmSegments == 28, "NVM segment count 28 (got %lu)", fsm.NvmSegments);
    CHECK(fsm.UnexpectedEvents == 0, "no unexpected events (got %lu)", fsm.UnexpectedEvents);

    expectedSegments = 639 + 28;
    CHECK(chip.TlvSegmentsReceived == expectedSegments,
          "chip received %lu TLV segments (want %lu)", chip.TlvSegmentsReceived, expectedSegments);

    /* version + baud + patch segments + board-id + NVM segments + disable-logging + build-info + reset + read-version */
    CHECK(chip.CommandsReceived == expectedSegments + 7,
          "chip received %lu commands (want %lu)", chip.CommandsReceived, expectedSegments + 7);
    CHECK(fsm.CommandsSent == chip.CommandsReceived,
          "FSM command counter agrees with the chip (%lu vs %lu)",
          fsm.CommandsSent, chip.CommandsReceived);

    printf("\nfirmware bytes on the wire\n");
    CHECK(chip.TlvLength == patchSize + nvmSize,
          "chip received every firmware byte (%lu of %lu)", chip.TlvLength, patchSize + nvmSize);
    CHECK(memcmp(wire, patch, patchSize) == 0, "rampatch reaches the chip unmodified");
    for (i = 0; i < nvmSize && patchSize + i < chip.TlvLength; i++) {
        if (wire[patchSize + i] != nvm[i]) { nvmDiffs++; diffAt = i; }
    }
    CHECK(nvmDiffs == 1 && diffAt == 0x94 && wire[patchSize + 0x94] == QCA_BAUDRATE_3000000,
          "NVM differs from the file only in HCI tag data[1], set to index 14 (%lu diffs, last 0x%lx)",
          nvmDiffs, diffAt);
    CHECK(nvm[0x94] == QCA_BAUDRATE_3200000, "the NVM buffer itself is never written");

    printf("\nbaud rate switch ordering\n");
    CHECK(chip.HostBaudSwitches == 1, "host UART reprogrammed exactly once (got %lu)",
          chip.HostBaudSwitches);
    CHECK(chip.BaudAtFirstPatchSegment == 3000000,
          "first patch segment is sent at 3,000,000 baud, not %lu",
          chip.BaudAtFirstPatchSegment);
    CHECK(chip.StateAfterBaudSwitch == QcaFsmStatePatchDownload,
          "0xFC48 awaits no reply: the FSM is already in patch download at the switch (state %lu)",
          chip.StateAfterBaudSwitch);
    {
        /* A late 0xFC48 reply that escaped the purge must not be taken as a patch ack. */
        MOCK_CHIP late;
        UCHAR cmd[QCA_FSM_MAX_COMMAND];
        ULONG len = 0, baud = 0;
        memset(&late, 0, sizeof(late));
        QcaFsmInit(&fsm, patch, patchSize, candidates, 1, 3000000);
        (void)QcaFsmNext(&fsm, cmd, &len, &baud);
        MockConsume(&late, cmd, len);
        (void)QcaFsmOnPacket(&fsm, late.Reply, late.ReplyLength);
        CHECK(QcaFsmNext(&fsm, cmd, &len, &baud) == QcaFsmActionSendBaudAndSwitch && baud == 3000000,
              "version reply leads to the baud switch at 3,000,000");
        MockCommandComplete(&late, QCA_BAUDRATE_CMD_OPCODE, 0);
        CHECK(!QcaFsmOnPacket(&fsm, late.Reply, late.ReplyLength) &&
              fsm.State == QcaFsmStatePatchDownload && fsm.PatchIndex == 0 && fsm.UnexpectedEvents == 1,
              "a stray 0xFC48 Command Complete is rejected without advancing the patch");
    }

    printf("\nQCA2066 Command Complete bring-up\n");
    memset(&chip, 0, sizeof(chip));
    chip.UseCommandComplete = TRUE;
    chip.CurrentHostBaud = 115200;
    QcaFsmInit(&fsm, patch, patchSize, candidates, 1, 3000000);
    action = RunToCompletion(&fsm, &chip, 2000, &steps);
    CHECK(action == QcaFsmActionDone && fsm.State == QcaFsmStateReady,
          "QCA2066 responses reach Ready without accepting unrelated traffic");
    CHECK(fsm.UnexpectedEvents == 0 && chip.TlvSegmentsReceived == expectedSegments,
          "QCA2066 response envelopes preserve the entire firmware transfer");

    printf("\nsilent chip is reported, not spun on\n");
    {
        MOCK_CHIP dead;
        memset(&dead, 0, sizeof(dead));
        dead.CurrentHostBaud = 115200;
        QcaFsmInit(&fsm, patch, patchSize, candidates, 1, 3000000);
        /* RunToCompletion bails out as soon as a Send gets no staged reply. */
        {
            UCHAR cmd[QCA_FSM_MAX_COMMAND];
            ULONG len = 0, baud = 0;
            QCA_FSM_ACTION a = QcaFsmNext(&fsm, cmd, &len, &baud);
            CHECK(a == QcaFsmActionSend, "first action is a send that expects a reply");
            a = QcaFsmNext(&fsm, cmd, &len, &baud);
            CHECK(a == QcaFsmActionFailed && fsm.State == QcaFsmStateFailed,
                  "pumping again without a reply fails loudly instead of resending blindly");
        }
    }
    free(wire);
    free(nvmNoHci);

    free(patch);
    free(nvm);
    printf("\n%s\n", g_fail ? "QCA FSM SELFTEST FAILED" : "QCA FSM SELFTEST PASSED");
    return g_fail ? 1 : 0;
}
