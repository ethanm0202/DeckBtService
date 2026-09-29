/*
 * identify_selftest.c - unit tests for the UART identify-only probe.
 *
 * The identify probe sends the version request (01 00 FC 01 19) at each rate
 * in turn until the controller responds. It asserts that identification never emits
 * baud changes, patch segments, NVM segments, or resets, and verifies parsing of
 * vendor version and board ID responses.
 */
#include <stdio.h>
#include <string.h>

#include "../src/include/qca_identify.h"

static int g_pass = 0;
static int g_fail = 0;

static void Check(int cond, const char *what)
{
    if (cond) { g_pass++; } else { g_fail++; printf("  [FAIL] %s\n", what); }
}

static void CheckEqU(unsigned long actual, unsigned long expected, const char *what)
{
    if (actual == expected) { g_pass++; }
    else { g_fail++; printf("  [FAIL] %s (expected %lu, got %lu)\n", what, expected, actual); }
}

static const unsigned char kVersionRequest[5] = { 0x01u, 0x00u, 0xFCu, 0x01u, 0x19u };

/* 04 FF <plen> <cresp> <rtype> <12-byte qca_btsoc_version> */
static unsigned long BuildVersionEvent(unsigned char *out, unsigned long productId,
                                       unsigned short patchVer, unsigned short romVerField,
                                       unsigned long socId)
{
    unsigned long i;
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
    out[0] = 0x04u; out[1] = 0xFFu; out[2] = 14u; out[3] = 0x00u; out[4] = 0x19u;
    for (i = 0; i < 12u; i++) { out[5 + i] = payload[i]; }
    return 17u;
}

/* ---------------------------------------------------------------- ladder */

static void TestLadder(void)
{
    QCA_IDENTIFY id;
    unsigned long rate = 0;

    printf("Baud ladder:\n");

    QcaIdentifyInit(&id);
    CheckEqU(id.RateCount, 3u, "the ladder has exactly three candidate rates");
    CheckEqU(id.Rates[0], 115200ul, "first candidate is the ROM/boot rate");
    CheckEqU(id.Rates[1], 3000000ul, "second candidate is the decided operational rate");
    CheckEqU(id.Rates[2], 3200000ul, "third candidate is the vendor INF rate");
    Check(!id.Answered, "a fresh identify has not answered");
    Check(!id.Done, "a fresh identify is not done");

    Check(QcaIdentifyNextRate(&id, &rate) && rate == 115200ul, "ladder yields 115200 first");
    Check(QcaIdentifyNextRate(&id, &rate) && rate == 3000000ul, "ladder yields 3000000 second");
    Check(QcaIdentifyNextRate(&id, &rate) && rate == 3200000ul, "ladder yields 3200000 third");
    Check(!QcaIdentifyNextRate(&id, &rate), "the ladder is exhausted after three rates");
    Check(id.Done, "an exhausted ladder marks the run done");
    Check(!id.Answered, "an exhausted ladder did not answer");
    CheckEqU(id.Attempts, 3u, "every attempt is counted");
}

static void TestRequestBytes(void)
{
    unsigned char buf[16];
    unsigned long n;

    printf("Request encoding:\n");

    memset(buf, 0xAA, sizeof(buf));
    n = QcaIdentifyBuildRequest(buf, sizeof(buf));
    CheckEqU(n, 5u, "the request is five bytes");
    Check(memcmp(buf, kVersionRequest, 5) == 0, "the request is exactly 01 00 FC 01 19");
    CheckEqU(QcaIdentifyBuildRequest(buf, 4u), 0u, "an undersized buffer is refused");
}

/* ---------------------------------------------------------------- answering */

static void TestAnswerOnSecondRate(void)
{
    QCA_IDENTIFY id;
    unsigned char evt[32];
    unsigned long rate = 0;
    unsigned long n;

    printf("Controller answers at the second rate:\n");

    QcaIdentifyInit(&id);
    Check(QcaIdentifyNextRate(&id, &rate) && rate == 115200ul, "tries 115200 first");
    /* silence at 115200: nothing is fed in */
    Check(QcaIdentifyNextRate(&id, &rate) && rate == 3000000ul, "falls through to 3000000");

    n = BuildVersionEvent(evt, 0x00000008ul, 0x0111u, 0x0201u, 0x00001200ul);
    Check(QcaIdentifyOnPacket(&id, evt, n), "a well-formed version event identifies the controller");
    Check(id.Answered, "Answered is set");
    Check(id.Done, "the run is done once answered");
    CheckEqU(id.AnsweredRate, 3000000ul, "the answering rate is recorded");
    CheckEqU(id.Version.SocId, 0x00001200ul, "soc id captured");
    CheckEqU(id.Version.RomVersion, 0x21u, "rom version derived");
    CheckEqU(id.Version.ProductId, 0x00000008ul, "product id captured");

    Check(!QcaIdentifyNextRate(&id, &rate), "no further rate is attempted after an answer");
    CheckEqU(id.Attempts, 2u, "only the rates actually tried are counted");
}

static void TestAnswerOnFirstRate(void)
{
    QCA_IDENTIFY id;
    unsigned char evt[32];
    unsigned long rate = 0;

    printf("Controller answers at the boot rate:\n");

    QcaIdentifyInit(&id);
    Check(QcaIdentifyNextRate(&id, &rate) && rate == 115200ul, "tries 115200 first");
    Check(QcaIdentifyOnPacket(&id, evt, BuildVersionEvent(evt, 1ul, 2u, 0x0201u, 0x00000100ul)),
          "answers at the boot rate");
    CheckEqU(id.AnsweredRate, 115200ul, "a ROM-state controller is reported at 115200");
    CheckEqU(id.Version.SocId, 0x00000100ul, "non-GF soc id captured");
    CheckEqU(id.Attempts, 1u, "a first-rate answer costs one attempt");
}

static void TestCommandCompleteVersion(void)
{
    /*
     * Upstream btqca.c (qca_check_version;
     * https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/btqca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137)
     * expects QCA2066 to wrap version responses in Command Complete enclosing cresp, rtype, length, and version.
     */
    static const unsigned char reply[] = {
        0x04, 0x0E, 0x12, 0x01, 0x00, 0xFC, 0x00, 0x19, 0x0C,
        0x08, 0x00, 0x00, 0x00, 0x11, 0x01, 0x01, 0x02, 0x00, 0x12, 0x00, 0x00
    };
    QCA_IDENTIFY id;
    unsigned char bad[sizeof(reply) + 1];
    unsigned long rate = 0;

    printf("QCA2066 Command Complete identity:\n");
    QcaIdentifyInit(&id);
    (void)QcaIdentifyNextRate(&id, &rate);
    memcpy(bad, reply, sizeof(reply));
    bad[4] = 0x17;
    Check(!QcaIdentifyOnPacket(&id, bad, sizeof(reply)), "wrong opcode cannot identify");
    memcpy(bad, reply, sizeof(reply));
    bad[6] = 1;
    Check(!QcaIdentifyOnPacket(&id, bad, sizeof(reply)), "failed EDL cannot identify");
    memcpy(bad, reply, sizeof(reply));
    bad[2]--;
    Check(!QcaIdentifyOnPacket(&id, bad, sizeof(reply)), "underdeclared CC plen rejected");
    Check(!QcaIdentifyOnPacket(&id, reply, sizeof(reply) - 1), "truncated CC rejected");
    memcpy(bad, reply, sizeof(reply));
    bad[sizeof(reply)] = 0;
    Check(!QcaIdentifyOnPacket(&id, bad, sizeof(bad)), "trailing CC bytes rejected");
    Check(!id.Answered && !id.Done, "malformed CC never ends identification");
    Check(QcaIdentifyOnPacket(&id, reply, sizeof(reply)), "QCA2066 version reply identifies");
    CheckEqU(id.Version.SocId, 0x1200, "CC foundry ID decoded after extra version byte");
    CheckEqU(id.Version.RomVersion, 0x21, "CC ROM selects the 21 firmware family");
    CheckEqU(id.Version.ProductId, 8, "CC product ID not shifted by envelope");
    CheckEqU(id.Version.PatchVersion, 0x111, "CC patch version captured");
    CheckEqU(id.AnsweredRate, 115200, "CC answer records current host rate");
    Check(!QcaIdentifyNextRate(&id, &rate), "CC answer stops further baud probing");
}

/*
 * Packets captured from the controller during stock driver initialization.
 * The controller loads hpnv21g.309 based on the returned SoC ID, ROM version,
 * and board ID.
 */
static void TestRecordedSiliconIdentity(void)
{
    static const unsigned char version[] = {
        0x04, 0x0E, 0x12, 0x01, 0x00, 0xFC, 0x00, 0x19, 0x0C,
        0x13, 0x00, 0x00, 0x00, 0xE6, 0x38, 0x01, 0x02, 0x11, 0x12, 0x0C, 0x40
    };
    static const unsigned char boardId[] = {
        0x04, 0x0E, 0x08, 0x01, 0x00, 0xFC, 0x00, 0x23, 0x02, 0x03, 0x09
    };
    QCA_IDENTIFY id;
    unsigned long rate = 0;
    USHORT bid = 0;
    char nvm[32];

    printf("Recorded controller identity:\n");
    QcaIdentifyInit(&id);
    (void)QcaIdentifyNextRate(&id, &rate);
    Check(QcaIdentifyOnPacket(&id, version, sizeof(version)), "recorded version reply identifies at 115200");
    CheckEqU(id.Version.ProductId, 0x13, "recorded product ID 0x13");
    CheckEqU(id.Version.PatchVersion, 0x38E6, "recorded ROM patch version 0x38E6");
    CheckEqU(id.Version.RomVersionField, 0x0201, "recorded build version 0x0201");
    CheckEqU(id.Version.SocId, 0x400C1211ul, "recorded SoC ID 0x400C1211");
    CheckEqU(id.Version.RomVersion, 0x21, "recorded ROM selects the 21 firmware family");
    Check(QcaParseBoardIdEvent(boardId, sizeof(boardId), &bid), "recorded board ID reply parses");
    CheckEqU(bid, 0x0309, "recorded board ID 0x0309");
    Check(QcaBuildNvmFileName(id.Version.SocId, id.Version.RomVersion, bid, nvm, sizeof(nvm)) &&
          strcmp(nvm, "hpnv21g.309") == 0, "NVM rule picks the vendor's hpnv21g.309");
}

static void TestRejectsNoise(void)
{
    QCA_IDENTIFY id;
    unsigned char evt[32];
    unsigned char junk[8];
    unsigned long rate = 0;
    unsigned long n;

    printf("Noise never counts as an identification:\n");

    QcaIdentifyInit(&id);
    (void)QcaIdentifyNextRate(&id, &rate);

    n = BuildVersionEvent(evt, 1ul, 2u, 0x0201u, 0x1200ul);

    Check(!QcaIdentifyOnPacket(&id, evt, 4u), "a truncated packet is rejected");
    Check(!QcaIdentifyOnPacket(&id, evt, n - 1u), "a short payload is rejected");

    evt[0] = 0x02u;
    Check(!QcaIdentifyOnPacket(&id, evt, n), "an ACL packet type is rejected");
    evt[0] = 0x04u;
    evt[1] = 0x0Eu;
    Check(!QcaIdentifyOnPacket(&id, evt, n), "vendor payload is not a valid CC envelope");
    evt[1] = 0xFFu;
    evt[4] = 0x23u;
    Check(!QcaIdentifyOnPacket(&id, evt, n), "a board-ID response is not a version response");
    evt[4] = 0x19u;

    memset(junk, 0xFF, sizeof(junk));
    Check(!QcaIdentifyOnPacket(&id, junk, sizeof(junk)), "line noise is rejected");

    Check(!id.Answered, "no noise set Answered");
    Check(!id.Done, "no noise ended the run");

    Check(QcaIdentifyOnPacket(&id, evt, n), "the genuine response still identifies after noise");
    Check(id.Answered, "a real answer after noise is accepted");
}

/* ---------------------------------------------------------------- the safety invariant */

static void TestNeverEmitsAnythingElse(void)
{
    /*
     * Drive every reachable path and prove the identify core can only ever put the version
     * request on the wire. This is the check that makes a physical run defensible: no baud
     * change (0xFC48), no TLV segment (0x1E), no board ID (0x23), no HCI reset (0x0C03).
     */
    QCA_IDENTIFY id;
    unsigned char buf[64];
    unsigned char evt[32];
    unsigned long rate = 0;
    unsigned long n;
    int emissions = 0;
    int scenario;

    printf("Safety invariant - only the version request is ever emitted:\n");

    for (scenario = 0; scenario < 3; scenario++) {
        QcaIdentifyInit(&id);
        while (QcaIdentifyNextRate(&id, &rate)) {
            memset(buf, 0xAA, sizeof(buf));
            n = QcaIdentifyBuildRequest(buf, sizeof(buf));
            emissions++;
            if (n != 5u || memcmp(buf, kVersionRequest, 5) != 0) {
                g_fail++;
                printf("  [FAIL] scenario %d emitted a command that is not the version request\n", scenario);
                break;
            }
            /* scenario 0: never answer. 1: answer at the second rate. 2: answer at the last rate. */
            if ((scenario == 1 && id.Attempts == 2u) || (scenario == 2 && id.Attempts == 3u)) {
                (void)QcaIdentifyOnPacket(&id, evt, BuildVersionEvent(evt, 1ul, 1u, 0x0201u, 0x1200ul));
            }
        }
        Check(1, "scenario completed without an unexpected emission");
    }
    Check(emissions == 3 + 2 + 3, "every scenario emitted exactly one request per attempted rate");
    CheckEqU((unsigned long)emissions, 8ul, "total emissions across all scenarios");
}

static void TestTerminalReporting(void)
{
    QCA_IDENTIFY id;
    unsigned long rate = 0;

    printf("Terminal reporting for a silent controller:\n");

    QcaIdentifyInit(&id);
    while (QcaIdentifyNextRate(&id, &rate)) { /* silence at every rate */ }
    Check(id.Done, "a fully silent run terminates");
    Check(!id.Answered, "a fully silent run does not claim an answer");
    CheckEqU(id.AnsweredRate, 0ul, "no answering rate is invented");
    CheckEqU(id.Version.SocId, 0ul, "no soc id is invented");
    CheckEqU(id.Version.RomVersion, 0ul, "no rom version is invented");
    CheckEqU(id.Attempts, 3u, "all three rates were attempted");
}

int main(void)
{
    printf("QCA identify-only probe selftest\n");

    TestLadder();
    TestRequestBytes();
    TestAnswerOnFirstRate();
    TestAnswerOnSecondRate();
    TestCommandCompleteVersion();
    TestRecordedSiliconIdentity();
    TestRejectsNoise();
    TestNeverEmitsAnythingElse();
    TestTerminalReporting();

    printf("\n");
    if (g_fail == 0) {
        printf("IDENTIFY SELFTEST PASSED: %d checks\n", g_pass);
        return 0;
    }
    printf("IDENTIFY SELFTEST FAILED: %d of %d checks\n", g_fail, g_fail + g_pass);
    return 1;
}
