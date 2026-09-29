/*
 * hci_selftest.c - exercises the real src/common/hci_stub.c translation unit in user mode.
 *
 * Validates synthetic controller responses against BTHUSB requirements.
 *
 * Build: tools\selftest.cmd (compiles this plus hci_stub.c and runs it)
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "../src/include/hci_stub.h"
#include "../src/include/hci_transport.h"

static int g_fail = 0;

#define CHECK(cond, ...)                     \
    do {                                     \
        if (cond) { printf("  ok   "); }     \
        else      { printf("  FAIL "); g_fail++; } \
        printf(__VA_ARGS__);                 \
        printf("\n");                        \
    } while (0)

/* Build an HCI command packet: opcode LE16, plen, params. */
static ULONG MakeCmd(UCHAR *out, USHORT opcode, const UCHAR *params, UCHAR plen)
{
    out[0] = (UCHAR)(opcode & 0xFF);
    out[1] = (UCHAR)(opcode >> 8);
    out[2] = plen;
    if (plen && params) { memcpy(&out[3], params, plen); }
    return 3u + plen;
}

/*
 * Submit one command and pop the single event it must produce. Returns event length, 0 on
 * failure. Validates the Command_Complete envelope common to every response.
 */
static ULONG RoundTrip(HCI_STUB *stub, USHORT opcode, const UCHAR *params, UCHAR plen,
                       UCHAR *evt, ULONG evtCapacity)
{
    UCHAR cmd[260];
    ULONG cmdLen = MakeCmd(cmd, opcode, params, plen);
    ULONG written = 0;

    if (!HciStubSubmitCommand(stub, cmd, cmdLen)) { return 0; }
    if (!HciStubPopEvent(stub, evt, evtCapacity, &written)) { return 0; }
    return written;
}

static void TestEnvelope(void)
{
    HCI_STUB stub;
    UCHAR evt[HCI_MAX_EVENT_SIZE];
    ULONG len;

    HciStubInit(&stub);
    printf("Command_Complete envelope (HCI_Reset)\n");

    CHECK(!HciStubHasEvent(&stub), "FIFO starts empty");
    len = RoundTrip(&stub, HCI_OP_RESET, NULL, 0, evt, sizeof(evt));
    CHECK(len == 6, "event length 6 (got %lu)", len);
    if (len < 6) { return; }
    CHECK(evt[0] == HCI_EVT_COMMAND_COMPLETE, "event code 0x0E");
    CHECK(evt[1] == len - 2, "plen %u == length-2", evt[1]);
    CHECK(evt[2] == 1, "num_hci_command_packets 1 (credit returned, else the host stalls)");
    CHECK((USHORT)(evt[3] | (evt[4] << 8)) == HCI_OP_RESET, "opcode echoed as 0x0C03");
    CHECK(evt[5] == 0x00, "status 0x00 success");
    CHECK(!HciStubHasEvent(&stub), "FIFO drained after pop");
}

static void TestReturnParameters(void)
{
    struct { USHORT opcode; ULONG expectLen; const char *name; } cases[] = {
        { HCI_OP_READ_LOCAL_VERSION,      6 + 8,  "Read_Local_Version_Information" },
        { HCI_OP_READ_LOCAL_SUPPORTED_CMDS, 6 + 64, "Read_Local_Supported_Commands" },
        { HCI_OP_READ_LOCAL_FEATURES,     6 + 8,  "Read_Local_Supported_Features" },
        { HCI_OP_READ_BUFFER_SIZE,        6 + 7,  "Read_Buffer_Size" },
        { HCI_OP_READ_BD_ADDR,            6 + 6,  "Read_BD_ADDR" },
        { HCI_OP_LE_READ_BUFFER_SIZE,     6 + 3,  "LE_Read_Buffer_Size" },
        { HCI_OP_LE_READ_LOCAL_FEATURES,  6 + 8,  "LE_Read_Local_Supported_Features" },
        { HCI_OP_LE_READ_SUPPORTED_STATES,6 + 8,  "LE_Read_Supported_States" },
        { HCI_OP_SET_EVENT_MASK,          6,      "Set_Event_Mask (status only)" },
        { 0x0C13,                         6,      "Write_Local_Name (unknown -> status only)" },
        /* A status-only reply to these commands produces BTHUSB event 5
         * ("expected an HCI event with a certain size") or a command timeout. */
        { HCI_OP_READ_LOCAL_NAME,         6 + 248, "Read_Local_Name" },
        { HCI_OP_READ_CLASS_OF_DEVICE,    6 + 3,   "Read_Class_Of_Device" },
        { HCI_OP_READ_VOICE_SETTING,      6 + 2,   "Read_Voice_Setting" },
        { HCI_OP_READ_PAGE_TIMEOUT,       6 + 2,   "Read_Page_Timeout" },
        { HCI_OP_READ_SCAN_ENABLE,        6 + 1,   "Read_Scan_Enable" },
        { HCI_OP_READ_LOCAL_SUPPORTED_CODECS, 6 + 4, "Read_Local_Supported_Codecs" },
        { HCI_OP_LE_READ_ADV_TX_POWER,    6 + 1,   "LE_Read_Advertising_Channel_Tx_Power" },
        { HCI_OP_LE_READ_ACCEPT_LIST_SIZE,6 + 1,   "LE_Read_Filter_Accept_List_Size" },
        { HCI_OP_LE_READ_RESOLVING_LIST_SIZE, 6 + 1, "LE_Read_Resolving_List_Size" },
        { HCI_OP_LE_READ_MAX_DATA_LENGTH, 6 + 8,   "LE_Read_Maximum_Data_Length (0x202F)" },
        /* Regression: 0x202F and 0x2023 were swapped, so 0x202F got a status-only reply. */
        { HCI_OP_LE_READ_SUGGESTED_DATA_LEN, 6 + 4, "LE_Read_Suggested_Default_Data_Length (0x2023)" },
        { HCI_OP_READ_INQ_RSP_TX_POWER,   6 + 1,   "Read_Inquiry_Response_Transmit_Power_Level" },
        { HCI_OP_READ_STORED_LINK_KEY,    6 + 4,   "Read_Stored_Link_Key" },
        { HCI_OP_WRITE_STORED_LINK_KEY,   6 + 1,   "Write_Stored_Link_Key" },
        { HCI_OP_DELETE_STORED_LINK_KEY,  6 + 2,   "Delete_Stored_Link_Key" },
    };
    HCI_STUB stub;
    UCHAR evt[HCI_MAX_EVENT_SIZE];

    printf("return parameter sizes\n");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ULONG len;
        HciStubInit(&stub);
        len = RoundTrip(&stub, cases[i].opcode, NULL, 0, evt, sizeof(evt));
        CHECK(len == cases[i].expectLen, "%s: %lu bytes (want %lu)",
              cases[i].name, len, cases[i].expectLen);
        if (len >= 6) {
            CHECK(evt[1] == (UCHAR)(len - 2), "%s: plen consistent", cases[i].name);
        }
    }
}

static void TestBdAddr(void)
{
    HCI_STUB stub;
    UCHAR evt[HCI_MAX_EVENT_SIZE];
    ULONG len;

    HciStubInit(&stub);
    printf("Read_BD_ADDR contents\n");
    len = RoundTrip(&stub, HCI_OP_READ_BD_ADDR, NULL, 0, evt, sizeof(evt));
    CHECK(len == 12, "length 12");
    if (len != 12) { return; }
    /* Address is little-endian on the wire, so the OUI byte is the last one. */
    CHECK((evt[11] & 0x02) != 0, "locally administered bit set (cannot clash with real hardware)");
    CHECK((evt[11] & 0x01) == 0, "unicast bit clear");
}

static void TestExtendedFeatures(void)
{
    HCI_STUB stub;
    UCHAR evt[HCI_MAX_EVENT_SIZE];
    UCHAR page = 1;
    ULONG len;

    HciStubInit(&stub);
    printf("Read_Local_Extended_Features echoes the requested page\n");
    len = RoundTrip(&stub, HCI_OP_READ_LOCAL_EXT_FEATURES, &page, 1, evt, sizeof(evt));
    CHECK(len == 6 + 10, "length 16 (got %lu)", len);
    if (len != 16) { return; }
    CHECK(evt[6] == 1, "page number echoed");
    CHECK(evt[7] == 2, "max page 2");
}

static void TestScoFeatureBits(void)
{
    HCI_STUB stub;
    UCHAR evt[HCI_MAX_EVENT_SIZE];
    ULONG len;

    HciStubInit(&stub);
    printf("advertised features must claim SCO, else the host never opens a voice channel\n");
    len = RoundTrip(&stub, HCI_OP_READ_LOCAL_FEATURES, NULL, 0, evt, sizeof(evt));
    CHECK(len == 14, "length 14");
    if (len != 14) { return; }
    /* LMP feature bit numbering: byte 0 bit 3 = SCO link; byte 3 bit 7 = EV3 (eSCO). */
    CHECK((evt[6] & 0x08) != 0, "byte0 bit3 SCO link supported");
    CHECK((evt[9] & 0x80) != 0, "byte3 bit7 eSCO (EV3) supported");
}

/*
 * BTHUSB event 34: "minimum required supported state mask is
 * 0x2491f7fffff, got 0x1fffffffff". Assert the advertised mask covers the requirement.
 */
static void TestLeStateMask(void)
{
    HCI_STUB stub;
    UCHAR evt[HCI_MAX_EVENT_SIZE];
    ULONG len;

    HciStubInit(&stub);
    printf("LE supported-state mask covers what BTHUSB demands\n");
    len = RoundTrip(&stub, HCI_OP_LE_READ_SUPPORTED_STATES, NULL, 0, evt, sizeof(evt));
    CHECK(len == 14, "length 14");
    if (len != 14) { return; }

    unsigned long long mask = 0;
    for (int i = 7; i >= 0; i--) { mask = (mask << 8) | evt[6 + i]; }

    const unsigned long long required = 0x2491f7fffffULL;
    printf("       advertised 0x%llX, required 0x%llX\n", mask, required);
    CHECK((mask & required) == required, "advertised mask is a superset of the required mask");
    CHECK(mask != 0x1fffffffffULL, "advertised mask meets BTHUSB requirement");
}

/* CVSD and mSBC must both be advertised - they are the Hands-Free voice codecs. */
static void TestCodecs(void)
{
    HCI_STUB stub;
    UCHAR evt[HCI_MAX_EVENT_SIZE];
    ULONG len;

    HciStubInit(&stub);
    printf("local supported codecs include the Hands-Free ones\n");
    len = RoundTrip(&stub, HCI_OP_READ_LOCAL_SUPPORTED_CODECS, NULL, 0, evt, sizeof(evt));
    CHECK(len == 10, "length 10 (got %lu)", len);
    if (len != 10) { return; }
    CHECK(evt[6] == 2, "advertises 2 standard codecs");
    CHECK(evt[7] == 0x02, "codec 0x02 = CVSD (narrowband Hands-Free)");
    CHECK(evt[8] == 0x05, "codec 0x05 = mSBC (wideband Hands-Free)");
    CHECK(evt[9] == 0, "no vendor-specific codecs");
}

/* Read_Local_Name must be exactly 248 bytes or BTHUSB reports a size mismatch. */
static void TestLocalName(void)
{
    HCI_STUB stub;
    UCHAR evt[HCI_MAX_EVENT_SIZE];
    ULONG len;

    HciStubInit(&stub);
    printf("Read_Local_Name is the spec-mandated fixed 248 bytes\n");
    len = RoundTrip(&stub, HCI_OP_READ_LOCAL_NAME, NULL, 0, evt, sizeof(evt));
    CHECK(len == 254, "event length 254 (6 + 248), got %lu", len);
    if (len != 254) { return; }
    CHECK(evt[1] == 252, "plen 252 fits in one byte");
    CHECK(evt[6] == 'D' && evt[7] == 'e' && evt[8] == 'c' && evt[9] == 'k', "name starts 'Deck'");
    CHECK(evt[253] == 0, "trailing bytes are NUL padded");
}

static void TestMalformed(void)
{
    HCI_STUB stub;
    UCHAR cmd[8] = { 0x03, 0x0C, 0x04, 0x00 };  /* claims 4 params, supplies 1 */

    HciStubInit(&stub);
    printf("malformed input\n");
    CHECK(!HciStubSubmitCommand(&stub, cmd, 2), "2-byte packet rejected (no opcode+plen)");
    CHECK(!HciStubSubmitCommand(&stub, cmd, 4), "truncated parameters rejected");
    CHECK(!HciStubHasEvent(&stub), "no event queued for rejected commands");
    CHECK(HciStubSubmitCommand(&stub, cmd, 7), "complete packet accepted");
}

static void TestFifoBounds(void)
{
    HCI_STUB stub;
    UCHAR evt[HCI_MAX_EVENT_SIZE];
    ULONG accepted = 0;
    ULONG drained = 0;
    ULONG written = 0;

    HciStubInit(&stub);
    printf("FIFO depth and ordering\n");

    for (ULONG i = 0; i < HCI_EVENT_FIFO_DEPTH + 4; i++) {
        UCHAR cmd[8];
        /* Distinct opcodes so ordering is observable in the popped events. */
        ULONG n = MakeCmd(cmd, (USHORT)(0x0C00u + i), NULL, 0);
        if (HciStubSubmitCommand(&stub, cmd, n)) { accepted++; }
    }
    CHECK(accepted == HCI_EVENT_FIFO_DEPTH, "accepts exactly %u events then reports failure (got %lu)",
          HCI_EVENT_FIFO_DEPTH, accepted);
    CHECK(stub.Dropped == 4, "overflow counted: Dropped == 4 (got %lu)", stub.Dropped);

    while (HciStubPopEvent(&stub, evt, sizeof(evt), &written)) {
        USHORT opcode = (USHORT)(evt[3] | (evt[4] << 8));
        if (opcode != (USHORT)(0x0C00u + drained)) {
            printf("  FAIL ordering broken at %lu: opcode 0x%04X\n", drained, opcode);
            g_fail++;
            break;
        }
        drained++;
    }
    CHECK(drained == HCI_EVENT_FIFO_DEPTH, "FIFO order preserved across %lu events", drained);
    CHECK(!HciStubHasEvent(&stub), "empty after full drain");

    /* Wrap-around: after a full drain the ring must be reusable. */
    {
        UCHAR cmd[8];
        ULONG n = MakeCmd(cmd, HCI_OP_RESET, NULL, 0);
        CHECK(HciStubSubmitCommand(&stub, cmd, n), "ring reusable after wrap");
        CHECK(HciStubPopEvent(&stub, evt, sizeof(evt), &written) && written == 6,
              "event retrievable after wrap");
    }
}

static void TestUndersizedBuffer(void)
{
    HCI_STUB stub;
    UCHAR tiny[8];   /* NB: `small` is a typedef in rpcndr.h */
    ULONG written = 123;

    HciStubInit(&stub);
    printf("undersized reader buffer\n");
    {
        UCHAR cmd[8];
        ULONG n = MakeCmd(cmd, HCI_OP_READ_LOCAL_SUPPORTED_CMDS, NULL, 0);  /* 70-byte event */
        CHECK(HciStubSubmitCommand(&stub, cmd, n), "70-byte event queued");
    }
    CHECK(!HciStubPopEvent(&stub, tiny, sizeof(tiny), &written),
          "pop into an 8-byte buffer fails rather than truncating framing");
    CHECK(written == 0, "reports zero bytes written");
    CHECK(stub.Dropped == 1, "counts the dropped event so the bug is visible");
    CHECK(!HciStubHasEvent(&stub), "oversized event is not left blocking the FIFO");
}

static void TestTransportSeam(void)
{
    HCI_STUB stub;
    HCI_TRANSPORT transport;
    UCHAR evt[HCI_MAX_EVENT_SIZE];
    ULONG written = 0;
    UCHAR dummy[64] = { 0 };

    memset(&transport, 0xCC, sizeof(transport));
    HciStubInit(&stub);
    HciStubBindTransport(&transport, &stub);

    printf("HCI_TRANSPORT seam: vtable binding and stream contracts\n");


    /* Initial stream emptiness */
    CHECK(!HciTransportHasStream(&transport, HciStreamEvent), "event stream starts empty");
    CHECK(!HciTransportHasStream(&transport, HciStreamAcl), "ACL stream starts empty");
    CHECK(!HciTransportHasStream(&transport, HciStreamSco), "SCO stream starts empty");

    /* Non-event streams are always empty and cannot be popped */
    written = 999;
    CHECK(!HciTransportPopStream(&transport, HciStreamAcl, evt, sizeof(evt), &written),
          "PopStream on HciStreamAcl returns 0");
    CHECK(written == 0, "PopStream on HciStreamAcl writes 0 bytes");

    written = 999;
    CHECK(!HciTransportPopStream(&transport, HciStreamSco, evt, sizeof(evt), &written),
          "PopStream on HciStreamSco returns 0");
    CHECK(written == 0, "PopStream on HciStreamSco writes 0 bytes");

    /* LastEventLength starts at 0 */
    CHECK(HciTransportLastEventLength(&transport) == 0, "LastEventLength is 0 before any command");

    /* SubmitAcl and SubmitSco accept-and-discard */
    CHECK(HciTransportSubmitAcl(&transport, dummy, sizeof(dummy)) == 1,
          "SubmitAcl accepts and discards (returns 1)");
    CHECK(!HciTransportHasStream(&transport, HciStreamAcl), "ACL stream remains empty after SubmitAcl");

    CHECK(HciTransportSubmitSco(&transport, dummy, sizeof(dummy)) == 1,
          "SubmitSco accepts and discards (returns 1)");
    CHECK(!HciTransportHasStream(&transport, HciStreamSco), "SCO stream remains empty after SubmitSco");

    /* Submit command through vtable: HCI_Reset */
    {
        UCHAR cmd[16];
        ULONG cmdLen = MakeCmd(cmd, HCI_OP_RESET, NULL, 0);

        CHECK(HciTransportSubmitCommand(&transport, cmd, cmdLen) == 1,
              "SubmitCommand(HCI_Reset) through vtable succeeds");
        CHECK(HciTransportLastEventLength(&transport) == 6,
              "LastEventLength matches queued event (6 bytes for HCI_Reset)");
        CHECK(HciTransportHasStream(&transport, HciStreamEvent) == 1,
              "HasStream(HciStreamEvent) reports complete packet available");
        CHECK(!HciTransportHasStream(&transport, HciStreamAcl),
              "HasStream(HciStreamAcl) remains 0");
        CHECK(!HciTransportHasStream(&transport, HciStreamSco),
              "HasStream(HciStreamSco) remains 0");

        /* Pop through vtable */
        written = 0;
        CHECK(HciTransportPopStream(&transport, HciStreamEvent, evt, sizeof(evt), &written) == 1,
              "PopStream(HciStreamEvent) succeeds");
        CHECK(written == 6, "popped 6 bytes");
        CHECK(evt[0] == HCI_EVT_COMMAND_COMPLETE, "event code 0x0E");
        CHECK((USHORT)(evt[3] | (evt[4] << 8)) == HCI_OP_RESET, "opcode echoed");
        CHECK(!HciTransportHasStream(&transport, HciStreamEvent),
              "event stream empty after pop");
    }

    /* Return parameters through vtable: Read_Local_Version, Read_BD_ADDR, Read_Local_Name */
    {
        struct { USHORT opcode; ULONG expectLen; const char *name; } cases[] = {
            { HCI_OP_READ_LOCAL_VERSION, 6 + 8, "Read_Local_Version_Information" },
            { HCI_OP_READ_BD_ADDR, 6 + 6, "Read_BD_ADDR" },
            { HCI_OP_READ_LOCAL_NAME, 6 + 248, "Read_Local_Name" },
            { HCI_OP_READ_BUFFER_SIZE, 6 + 7, "Read_Buffer_Size" },
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            UCHAR cmd[16];
            ULONG cmdLen = MakeCmd(cmd, cases[i].opcode, NULL, 0);

            CHECK(HciTransportSubmitCommand(&transport, cmd, cmdLen) == 1,
                  "%s: SubmitCommand through vtable succeeds", cases[i].name);
            CHECK(HciTransportLastEventLength(&transport) == cases[i].expectLen,
                  "%s: LastEventLength matches expected", cases[i].name);

            written = 0;
            CHECK(HciTransportPopStream(&transport, HciStreamEvent, evt, sizeof(evt), &written) == 1,
                  "%s: PopStream succeeds", cases[i].name);
            CHECK(written == cases[i].expectLen,
                  "%s: popped length matches expected (%lu)", cases[i].name, written);
        }
    }

    /* PopStream contract: undersized buffer leaves packet queued, never partially fills */
    {
        UCHAR cmd[16];
        ULONG cmdLen = MakeCmd(cmd, HCI_OP_READ_LOCAL_SUPPORTED_CMDS, NULL, 0); /* 70-byte event */
        UCHAR tiny[8];

        CHECK(HciTransportSubmitCommand(&transport, cmd, cmdLen) == 1,
              "SubmitCommand(Read_Local_Supported_Commands) queued 70-byte event");
        CHECK(HciTransportLastEventLength(&transport) == 70, "LastEventLength is 70");

        written = 999;
        CHECK(HciTransportPopStream(&transport, HciStreamEvent, tiny, sizeof(tiny), &written) == 0,
              "PopStream into undersized buffer returns 0 per contract");
        CHECK(written == 0, "*Written is 0 on undersized PopStream");
        CHECK(HciTransportHasStream(&transport, HciStreamEvent) == 1,
              "packet was left queued after undersized PopStream attempt");

        /* Now pop with full capacity: packet is still there! */
        written = 0;
        CHECK(HciTransportPopStream(&transport, HciStreamEvent, evt, sizeof(evt), &written) == 1,
              "PopStream with sufficient buffer retrieves the queued packet");
        CHECK(written == 70, "retrieved full 70-byte event");
        CHECK(!HciTransportHasStream(&transport, HciStreamEvent), "stream now empty");
    }

    /* Reset clears queued events and resets LastEventLength */
    {
        UCHAR cmd[16];
        ULONG cmdLen = MakeCmd(cmd, HCI_OP_RESET, NULL, 0);

        CHECK(HciTransportSubmitCommand(&transport, cmd, cmdLen) == 1, "command queued");
        CHECK(HciTransportHasStream(&transport, HciStreamEvent) == 1, "event pending");

        HciTransportReset(&transport);

        CHECK(!HciTransportHasStream(&transport, HciStreamEvent),
              "Reset empties the event FIFO");
        CHECK(HciTransportLastEventLength(&transport) == 0,
              "Reset clears LastEventLength to 0");
        written = 999;
        CHECK(!HciTransportPopStream(&transport, HciStreamEvent, evt, sizeof(evt), &written),
              "PopStream fails on reset FIFO");
        CHECK(written == 0, "written is 0");
    }
}

int main(void)
{
    TestEnvelope();
    TestReturnParameters();
    TestBdAddr();
    TestExtendedFeatures();
    TestScoFeatureBits();
    TestLeStateMask();
    TestCodecs();
    TestLocalName();
    TestMalformed();
    TestFifoBounds();
    TestUndersizedBuffer();
    TestTransportSeam();
    printf("\n%s\n", g_fail ? "HCI SELFTEST FAILED" : "HCI SELFTEST PASSED");
    return g_fail ? 1 : 0;
}
