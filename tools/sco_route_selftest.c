/*
 * sco_route_selftest.c - host tests for src/common/sco_route.c (voice links onto the HCI data path).
 *
 *   1. Setup_Synchronous_Connection for transparent voice (mSBC to a headset) becomes the exact Enhanced Setup
 *      structure with HCI data paths and BTHPORT's link parameters.
 *   2. CVSD with 16-bit linear host data maps to CVSD air / linear PCM host coding.
 *   3. Accept keeps the BD_ADDR and becomes Enhanced Accept.
 *   4. Everything else passes through untouched.
 *   5. The controller's Command Status/Complete gets BTHPORT's legacy opcode back, only when owed.
 *
 * Nonzero exit on any failure; final "SCO ROUTE SELFTEST PASSED".
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "../src/include/sco_route.h"

static int g_fail = 0;

#define CHECK(cond, ...)                            \
    do {                                            \
        if (cond) { printf("  ok   "); }            \
        else      { printf("  FAIL "); g_fail++; }  \
        printf(__VA_ARGS__); printf("\n");          \
    } while (0)

static void
TestSetupTransparent(void)
{
    /* ScoSetupCommand baseline: Setup_Synchronous_Connection, voice 0x0003. */
    static const unsigned char legacy[] = {
        0x28, 0x04, 0x11,
        0x09, 0x00, 0x40, 0x1F, 0x00, 0x00, 0x40, 0x1F, 0x00, 0x00, 0x0D, 0x00, 0x03, 0x00, 0x02, 0x80, 0x03 };
    static const unsigned char expected[] = {
        0x3D, 0x04, 59,
        0x09, 0x00,                                     /* handle */
        0x40, 0x1F, 0x00, 0x00, 0x40, 0x1F, 0x00, 0x00, /* Tx/Rx bandwidth 8000 (from BTHPORT) */
        0x03, 0x00, 0x00, 0x00, 0x00,                   /* Tx coding: transparent */
        0x03, 0x00, 0x00, 0x00, 0x00,                   /* Rx coding: transparent */
        0x3C, 0x00, 0x3C, 0x00,                         /* codec frame sizes 60 */
        0x40, 0x1F, 0x00, 0x00, 0x40, 0x1F, 0x00, 0x00, /* input/output bandwidth 8000 */
        0x03, 0x00, 0x00, 0x00, 0x00,                   /* input coding: transparent */
        0x03, 0x00, 0x00, 0x00, 0x00,                   /* output coding: transparent */
        0x10, 0x00, 0x10, 0x00,                         /* coded data size 16 bits */
        0x02, 0x02, 0x00, 0x00,                         /* PCM format, MSB position */
        0x00, 0x00,                                     /* data path: HCI */
        0x01, 0x01,                                     /* transport unit size */
        0x0D, 0x00,                                     /* max latency (from BTHPORT) */
        0x80, 0x03,                                     /* packet type (from BTHPORT) */
        0x02 };                                         /* retransmission effort (from BTHPORT) */
    SCO_ROUTE route;
    unsigned char out[SCO_ROUTE_MAX_COMMAND];
    unsigned long n;

    printf("Setup, transparent (mSBC voice call)\n");
    ScoRouteReset(&route);
    n = ScoRouteRewriteCommand(&route, legacy, sizeof(legacy), out, sizeof(out));
    CHECK(n == sizeof(expected), "rewritten to %lu bytes (Enhanced Setup: 3 + 59)", n);
    CHECK(n == sizeof(expected) && memcmp(out, expected, n) == 0,
          "byte-exact Enhanced_Setup_Synchronous_Connection with HCI data paths");
    CHECK(route.SetupPending == 1 && route.Rewritten == 1, "one setup now owes BTHPORT its opcode back");
}

static void
TestSetupCvsd(void)
{
    static const unsigned char legacy[] = {
        0x28, 0x04, 0x11,
        0x06, 0x00, 0x40, 0x1F, 0x00, 0x00, 0x40, 0x1F, 0x00, 0x00, 0x0A, 0x00, 0x60, 0x00, 0x01, 0x3F, 0x03 };
    SCO_ROUTE route;
    unsigned char out[SCO_ROUTE_MAX_COMMAND];
    unsigned long n;

    printf("Setup, CVSD with 16-bit linear host data (voice 0x0060)\n");
    ScoRouteReset(&route);
    n = ScoRouteRewriteCommand(&route, legacy, sizeof(legacy), out, sizeof(out));
    CHECK(n == 62, "rewritten");
    CHECK(out[3 + 10] == 0x02 && out[3 + 15] == 0x02, "air coding CVSD both ways");
    CHECK(out[3 + 24] == 0x80 && out[3 + 25] == 0x3E && out[3 + 28] == 0x80 && out[3 + 29] == 0x3E,
          "host bandwidth 16000 bytes/s both ways (16-bit samples at 8 kHz)");
    CHECK(out[3 + 32] == 0x04 && out[3 + 37] == 0x04, "host coding linear PCM");
    CHECK(out[3 + 50] == 0x00 && out[3 + 51] == 0x00, "data paths HCI");
    CHECK(out[3 + 52] == 16 && out[3 + 53] == 16, "transport unit 16 bits");
    CHECK(out[3 + 54] == 0x0A && out[3 + 56] == 0x3F && out[3 + 57] == 0x03 && out[3 + 58] == 0x01,
          "latency 10 ms, packet types 0x033F, retransmission 1 kept from BTHPORT");
}

static void
TestAccept(void)
{
    static const unsigned char legacy[] = {
        0x29, 0x04, 0x15,
        0x6B, 0x2E, 0x6C, 0xF2, 0x8C, 0x70,
        0x40, 0x1F, 0x00, 0x00, 0x40, 0x1F, 0x00, 0x00, 0x0D, 0x00, 0x03, 0x00, 0x02, 0x80, 0x03 };
    SCO_ROUTE route;
    unsigned char out[SCO_ROUTE_MAX_COMMAND];
    unsigned long n;

    printf("Accept (headset-initiated link)\n");
    ScoRouteReset(&route);
    n = ScoRouteRewriteCommand(&route, legacy, sizeof(legacy), out, sizeof(out));
    CHECK(n == 3 + 63 && out[0] == 0x3E && out[1] == 0x04 && out[2] == 63, "Enhanced_Accept_Synchronous_Connection, 63 parameters");
    CHECK(memcmp(out + 3, legacy + 3, 6) == 0, "BD_ADDR kept");
    CHECK(out[3 + 54] == 0x00 && out[3 + 55] == 0x00, "data paths HCI");
    CHECK(out[3 + 58] == 0x0D && out[3 + 60] == 0x80 && out[3 + 61] == 0x03 && out[3 + 62] == 0x02,
          "latency, packet types, retransmission kept");
    CHECK(route.AcceptPending == 1 && route.SetupPending == 0, "the accept is what is owed");
}

static void
TestPassThrough(void)
{
    static const unsigned char ulaw[] = {
        0x28, 0x04, 0x11,
        0x06, 0x00, 0x40, 0x1F, 0x00, 0x00, 0x40, 0x1F, 0x00, 0x00, 0x0A, 0x00, 0x61, 0x00, 0x01, 0x3F, 0x03 };
    static const unsigned char reset[] = { 0x03, 0x0C, 0x00 };
    static const unsigned char shortSetup[] = { 0x28, 0x04, 0x10, 0x06, 0x00 };
    SCO_ROUTE route;
    unsigned char out[SCO_ROUTE_MAX_COMMAND];

    printf("Pass-through\n");
    ScoRouteReset(&route);
    CHECK(ScoRouteRewriteCommand(&route, ulaw, sizeof(ulaw), out, sizeof(out)) == 0, "u-law air coding is left alone");
    CHECK(ScoRouteRewriteCommand(&route, reset, sizeof(reset), out, sizeof(out)) == 0, "other commands are left alone");
    CHECK(ScoRouteRewriteCommand(&route, shortSetup, sizeof(shortSetup), out, sizeof(out)) == 0, "a malformed setup is left alone");
    CHECK(ScoRouteRewriteCommand(&route, ulaw, sizeof(ulaw), out, 40) == 0, "no rewrite into a buffer too small");
    CHECK(route.SetupPending == 0 && route.Rewritten == 0, "nothing owed");
}

static void
TestRestore(void)
{
    static const unsigned char legacy[] = {
        0x28, 0x04, 0x11,
        0x09, 0x00, 0x40, 0x1F, 0x00, 0x00, 0x40, 0x1F, 0x00, 0x00, 0x0D, 0x00, 0x03, 0x00, 0x02, 0x80, 0x03 };
    SCO_ROUTE route;
    unsigned char out[SCO_ROUTE_MAX_COMMAND];
    unsigned char status[] = { 0x0F, 0x04, 0x00, 0x01, 0x3D, 0x04 };
    unsigned char again[] = { 0x0F, 0x04, 0x00, 0x01, 0x3D, 0x04 };
    unsigned char complete[] = { 0x0E, 0x04, 0x01, 0x3D, 0x04, 0x12 };
    unsigned char other[] = { 0x0F, 0x04, 0x00, 0x01, 0x05, 0x04 };

    printf("Restoring BTHPORT's opcode\n");
    ScoRouteReset(&route);
    CHECK(!ScoRouteRestoreEvent(&route, status, sizeof(status)) && status[4] == 0x3D,
          "completion for an unmodified Enhanced command remains unchanged");
    (void)ScoRouteRewriteCommand(&route, legacy, sizeof(legacy), out, sizeof(out));
    CHECK(ScoRouteRestoreEvent(&route, status, sizeof(status)) && status[4] == 0x28 && status[5] == 0x04 &&
          status[2] == 0x00, "Command Status for 0x043D now reads 0x0428, status kept");
    CHECK(!ScoRouteRestoreEvent(&route, again, sizeof(again)) && again[4] == 0x3D, "owed once, restored once");
    (void)ScoRouteRewriteCommand(&route, legacy, sizeof(legacy), out, sizeof(out));
    CHECK(ScoRouteRestoreEvent(&route, complete, sizeof(complete)) && complete[3] == 0x28 && complete[5] == 0x12,
          "a Command Complete (error path) is restored too, status kept");
    CHECK(!ScoRouteRestoreEvent(&route, other, sizeof(other)) && other[4] == 0x05, "unrelated statuses untouched");
    CHECK(route.Restored == 2, "two restorations counted");
}

int
main(void)
{
    TestSetupTransparent();
    TestSetupCvsd();
    TestAccept();
    TestPassThrough();
    TestRestore();
    if (g_fail != 0) {
        printf("\nSCO ROUTE SELFTEST FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    printf("\nSCO ROUTE SELFTEST PASSED\n");
    return 0;
}
