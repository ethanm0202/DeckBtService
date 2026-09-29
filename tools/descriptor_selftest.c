/*
 * descriptor_selftest.c - validates the real descriptor tables in src/common/usb_descriptors.c.
 *
 * Compiles the production translation unit in user mode and walks the actual bytes, so this
 * checks the shipped artifact rather than a reimplementation of it.
 *
 * Build (from a LaunchBuildEnv shell):
 *     cl /nologo /W4 /WX /Fe:descriptor_selftest.exe descriptor_selftest.c ..\src\common\usb_descriptors.c
 */

#include <windows.h>
#include <stdio.h>

#include "../src/include/usb_descriptors.h"

static int g_fail = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (cond) {                                          \
            printf("  ok   ");                               \
        } else {                                             \
            printf("  FAIL ");                               \
            g_fail++;                                        \
        }                                                    \
        printf(__VA_ARGS__);                                 \
        printf("\n");                                        \
    } while (0)

#define USB_DT_DEVICE           0x01
#define USB_DT_CONFIG           0x02
#define USB_DT_STRING           0x03
#define USB_DT_INTERFACE        0x04
#define USB_DT_ENDPOINT         0x05

#define XFER_CONTROL 0
#define XFER_ISOCH   1
#define XFER_BULK    2
#define XFER_INTR    3

static USHORT rd16(const UCHAR *p) { return (USHORT)(p[0] | (p[1] << 8)); }

int main(void)
{
    printf("device descriptor\n");
    CHECK(DeckBtDeviceDescriptorSize == 18, "size 18 (got %lu)", DeckBtDeviceDescriptorSize);
    CHECK(DeckBtDeviceDescriptor[0] == 18, "bLength 18");
    CHECK(DeckBtDeviceDescriptor[1] == USB_DT_DEVICE, "bDescriptorType DEVICE");
    CHECK(DeckBtDeviceDescriptor[4] == DECKBT_CLASS_WIRELESS, "bDeviceClass 0xE0");
    CHECK(DeckBtDeviceDescriptor[5] == DECKBT_SUBCLASS_RF, "bDeviceSubClass 0x01");
    CHECK(DeckBtDeviceDescriptor[6] == DECKBT_PROTOCOL_BLUETOOTH, "bDeviceProtocol 0x01");
    CHECK(rd16(&DeckBtDeviceDescriptor[8]) == DECKBT_VENDOR_ID, "idVendor 0x0CF3");
    CHECK(rd16(&DeckBtDeviceDescriptor[10]) == DECKBT_PRODUCT_ID, "idProduct 0x6390");
    CHECK(DeckBtDeviceDescriptor[17] == 1, "bNumConfigurations 1");

    printf("configuration blob\n");
    const UCHAR *cfg = DeckBtConfigDescriptor;
    const ULONG  len = DeckBtConfigDescriptorSize;
    CHECK(cfg[1] == USB_DT_CONFIG, "bDescriptorType CONFIGURATION");
    CHECK(rd16(&cfg[2]) == len, "wTotalLength %u == sizeof %lu", rd16(&cfg[2]), len);
    CHECK(cfg[4] == 2, "bNumInterfaces 2");
    CHECK(cfg[5] == 1, "bConfigurationValue 1");

    /* Walk it: every descriptor must have a sane bLength and the chain must land exactly. */
    ULONG off = 0, nIface = 0, nEp = 0;
    int walkOk = 1;
    while (off < len) {
        UCHAR bLength = cfg[off];
        if (bLength == 0 || off + bLength > len) { walkOk = 0; break; }
        if (cfg[off + 1] == USB_DT_INTERFACE) nIface++;
        if (cfg[off + 1] == USB_DT_ENDPOINT)  nEp++;
        off += bLength;
    }
    CHECK(walkOk && off == len, "descriptor chain walks to exactly %lu bytes", len);
    CHECK(nIface == 1 + DECKBT_SCO_ALT_COUNT, "8 interface descriptors (1 HCI + 7 SCO alts), got %lu", nIface);
    CHECK(nEp == 3 + (2 * DECKBT_SCO_ALT_COUNT), "17 endpoint descriptors, got %lu", nEp);

    printf("interface 0 - HCI\n");
    const UCHAR *if0 = cfg + 9;
    CHECK(if0[1] == USB_DT_INTERFACE, "is an interface descriptor");
    CHECK(if0[2] == DECKBT_IFACE_HCI, "bInterfaceNumber 0");
    CHECK(if0[3] == 0, "bAlternateSetting 0");
    CHECK(if0[4] == 3, "bNumEndpoints 3");
    CHECK(if0[5] == DECKBT_CLASS_WIRELESS && if0[6] == DECKBT_SUBCLASS_RF &&
          if0[7] == DECKBT_PROTOCOL_BLUETOOTH, "class/subclass/protocol E0/01/01");

    const UCHAR *ep = if0 + 9;
    CHECK(ep[2] == DECKBT_EP_EVENT_IN && (ep[3] & 3) == XFER_INTR,
          "EP 0x81 interrupt IN");
    CHECK(rd16(&ep[4]) == DECKBT_EP_EVENT_MAXPACKET, "event wMaxPacketSize %u", rd16(&ep[4]));
    ep += 7;
    CHECK(ep[2] == DECKBT_EP_ACL_OUT && (ep[3] & 3) == XFER_BULK, "EP 0x02 bulk OUT");
    CHECK(rd16(&ep[4]) == 512, "ACL OUT wMaxPacketSize 512, got %u", rd16(&ep[4]));
    ep += 7;
    CHECK(ep[2] == DECKBT_EP_ACL_IN && (ep[3] & 3) == XFER_BULK, "EP 0x82 bulk IN");
    CHECK(rd16(&ep[4]) == 512, "ACL IN wMaxPacketSize 512, got %u", rd16(&ep[4]));

    printf("interface 1 - SCO alternate settings\n");
    for (UCHAR alt = 0; alt <= DECKBT_SCO_ALT_MAX; alt++) {
        ULONG o = DeckBtScoAltOffset(alt);
        const UCHAR *ifd = cfg + o;
        USHORT want = DeckBtScoAltPacketSize[alt];

        CHECK(o + 23 <= len, "alt %u offset %lu inside blob", alt, o);
        CHECK(ifd[1] == USB_DT_INTERFACE && ifd[2] == DECKBT_IFACE_SCO && ifd[3] == alt,
              "alt %u: interface 1 setting %u at offset %lu", alt, alt, o);
        CHECK(ifd[4] == 2, "alt %u: bNumEndpoints 2", alt);

        const UCHAR *o_ep = ifd + 9;
        const UCHAR *i_ep = ifd + 16;
        CHECK(o_ep[2] == DECKBT_EP_SCO_OUT && (o_ep[3] & 3) == XFER_ISOCH,
              "alt %u: EP 0x03 isochronous OUT", alt);
        CHECK(i_ep[2] == DECKBT_EP_SCO_IN && (i_ep[3] & 3) == XFER_ISOCH,
              "alt %u: EP 0x83 isochronous IN", alt);
        CHECK(rd16(&o_ep[4]) == want && rd16(&i_ep[4]) == want,
              "alt %u: wMaxPacketSize %u per Bluetooth spec table", alt, want);
        CHECK(o_ep[6] >= 4 && i_ep[6] >= 4,
              "alt %u: bInterval %u >= 4", alt, o_ep[6]);
    }

    printf("string descriptors\n");
    for (UCHAR i = 0; i < DECKBT_ISTRING_COUNT; i++) {
        ULONG slen = 0;
        const UCHAR *s = DeckBtGetStringDescriptor(i, &slen);
        CHECK(s != NULL && slen > 0, "index %u present", i);
        if (!s) continue;
        CHECK(s[0] == (UCHAR)slen, "index %u bLength %u == array size %lu", i, s[0], slen);
        CHECK(s[1] == USB_DT_STRING, "index %u bDescriptorType STRING", i);
        CHECK((slen % 2) == 0, "index %u even length", i);
    }
    {
        ULONG slen = 0;
        CHECK(DeckBtGetStringDescriptor(200, &slen) == NULL && slen == 0,
              "unknown string index returns NULL");
    }
    CHECK(DeckBtScoAltOffset(7) == 0, "out-of-range alt setting returns 0");

    printf("\n%s\n", g_fail ? "SELFTEST FAILED" : "SELFTEST PASSED");
    return g_fail ? 1 : 0;
}
