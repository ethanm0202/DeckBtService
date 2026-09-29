/*
 * usb_descriptors.c - descriptor byte tables for the DeckBtService virtual Bluetooth radio.
 * See usb_descriptors.h for the rationale behind every non-obvious value.
 *
 * Layout is written out literally rather than built from structs so that the exact wire bytes
 * are reviewable, and so tools/check_descriptors.py can validate them without a compiler.
 */

#include "../include/usb_descriptors.h"

#define LO(x) ((UCHAR)((x) & 0xFFu))
#define HI(x) ((UCHAR)(((x) >> 8) & 0xFFu))

const USHORT DeckBtScoAltPacketSize[DECKBT_SCO_ALT_COUNT] = { 0, 9, 17, 25, 33, 49, 63 };

/* ---------------------------------------------------------------- device */

const UCHAR DeckBtDeviceDescriptor[] = {
    18,                             /* bLength                                    */
    0x01,                           /* bDescriptorType = DEVICE                   */
    0x00, 0x02,                     /* bcdUSB = 2.00 (high speed emulation)       */
    DECKBT_CLASS_WIRELESS,          /* bDeviceClass                               */
    DECKBT_SUBCLASS_RF,             /* bDeviceSubClass                            */
    DECKBT_PROTOCOL_BLUETOOTH,      /* bDeviceProtocol                            */
    64,                             /* bMaxPacketSize0                            */
    LO(DECKBT_VENDOR_ID),  HI(DECKBT_VENDOR_ID),
    LO(DECKBT_PRODUCT_ID), HI(DECKBT_PRODUCT_ID),
    LO(DECKBT_DEVICE_BCD), HI(DECKBT_DEVICE_BCD),
    DECKBT_ISTRING_MANUFACTURER,
    DECKBT_ISTRING_PRODUCT,
    DECKBT_ISTRING_SERIAL,
    1                               /* bNumConfigurations                         */
};
const ULONG DeckBtDeviceDescriptorSize = sizeof(DeckBtDeviceDescriptor);

/* ---------------------------------------------------------------- configuration */

/*
 * wTotalLength arithmetic, kept explicit so a reviewer can check it by hand:
 *      configuration                          9
 *      interface 0                            9
 *        interrupt IN  EP 0x81                7
 *        bulk OUT      EP 0x02                7
 *        bulk IN       EP 0x82                7
 *      interface 1 alt 0..6   7 * (9 + 7 + 7) = 161
 *                                          -----
 *                                            200
 */
#define DECKBT_CONFIG_TOTAL_LENGTH 200u

/* One SCO alternate setting: interface descriptor + isoch OUT + isoch IN. */
#define DECKBT_SCO_ALT(alt, size)                                                \
    9, 0x04, DECKBT_IFACE_SCO, (alt), 2,                                         \
        DECKBT_CLASS_WIRELESS, DECKBT_SUBCLASS_RF, DECKBT_PROTOCOL_BLUETOOTH, 0, \
    7, 0x05, DECKBT_EP_SCO_OUT, 0x01, LO(size), HI(size), DECKBT_EP_SCO_INTERVAL,\
    7, 0x05, DECKBT_EP_SCO_IN,  0x01, LO(size), HI(size), DECKBT_EP_SCO_INTERVAL

const UCHAR DeckBtConfigDescriptor[] = {
    /* --- configuration ------------------------------------------------------- */
    9,                              /* bLength                                    */
    0x02,                           /* bDescriptorType = CONFIGURATION            */
    LO(DECKBT_CONFIG_TOTAL_LENGTH), HI(DECKBT_CONFIG_TOTAL_LENGTH),
    2,                              /* bNumInterfaces                             */
    1,                              /* bConfigurationValue                        */
    0,                              /* iConfiguration                             */
    0x80,                           /* bmAttributes: bus powered, no remote wakeup */
                                    /* (the service never signals resume; bus
                                       powered matches bMaxPower and the
                                       GET_STATUS reply in usbip_device.c)        */
    50,                             /* bMaxPower = 100 mA                         */

    /* --- interface 0: HCI command/event/ACL ---------------------------------- */
    9, 0x04, DECKBT_IFACE_HCI, 0, 3,
        DECKBT_CLASS_WIRELESS, DECKBT_SUBCLASS_RF, DECKBT_PROTOCOL_BLUETOOTH, 0,

    /* interrupt IN - HCI events */
    7, 0x05, DECKBT_EP_EVENT_IN, 0x03,
        LO(DECKBT_EP_EVENT_MAXPACKET), HI(DECKBT_EP_EVENT_MAXPACKET),
        DECKBT_EP_EVENT_INTERVAL,
    /* bulk OUT - ACL data */
    7, 0x05, DECKBT_EP_ACL_OUT, 0x02,
        LO(DECKBT_EP_ACL_MAXPACKET), HI(DECKBT_EP_ACL_MAXPACKET), 0,
    /* bulk IN - ACL data */
    7, 0x05, DECKBT_EP_ACL_IN, 0x02,
        LO(DECKBT_EP_ACL_MAXPACKET), HI(DECKBT_EP_ACL_MAXPACKET), 0,

    /* --- interface 1: SCO voice, alternate settings 0..6 --------------------- */
    DECKBT_SCO_ALT(0, 0),
    DECKBT_SCO_ALT(1, 9),
    DECKBT_SCO_ALT(2, 17),
    DECKBT_SCO_ALT(3, 25),
    DECKBT_SCO_ALT(4, 33),
    DECKBT_SCO_ALT(5, 49),
    DECKBT_SCO_ALT(6, 63)
};
const ULONG DeckBtConfigDescriptorSize = sizeof(DeckBtConfigDescriptor);

/* ---------------------------------------------------------------- strings */

const UCHAR DeckBtStringLangIds[] = {
    4, 0x03, LO(DECKBT_LANGID_EN_US), HI(DECKBT_LANGID_EN_US)
};

/* "DeckBtService" */
const UCHAR DeckBtStringManufacturer[] = {
    28, 0x03,
    'D',0, 'e',0, 'c',0, 'k',0, 'B',0, 't',0, 'S',0, 'e',0, 'r',0, 'v',0, 'i',0, 'c',0, 'e',0
};

/* "Bluetooth Radio" */
const UCHAR DeckBtStringProduct[] = {
    32, 0x03,
    'B',0, 'l',0, 'u',0, 'e',0, 't',0, 'o',0, 'o',0, 't',0, 'h',0, ' ',0,
    'R',0, 'a',0, 'd',0, 'i',0, 'o',0
};

/* "DECKBT0001" */
const UCHAR DeckBtStringSerial[] = {
    22, 0x03,
    'D',0, 'E',0, 'C',0, 'K',0, 'B',0, 'T',0, '0',0, '0',0, '0',0, '1',0
};

_Success_(return != NULL)
const UCHAR *DeckBtGetStringDescriptor(_In_ UCHAR Index, _Out_ ULONG *Length)
{
    switch (Index) {
    case DECKBT_ISTRING_LANGIDS:
        *Length = sizeof(DeckBtStringLangIds);
        return DeckBtStringLangIds;
    case DECKBT_ISTRING_MANUFACTURER:
        *Length = sizeof(DeckBtStringManufacturer);
        return DeckBtStringManufacturer;
    case DECKBT_ISTRING_PRODUCT:
        *Length = sizeof(DeckBtStringProduct);
        return DeckBtStringProduct;
    case DECKBT_ISTRING_SERIAL:
        *Length = sizeof(DeckBtStringSerial);
        return DeckBtStringSerial;
    default:
        *Length = 0;
        return NULL;
    }
}

ULONG DeckBtScoAltOffset(_In_ UCHAR Alt)
{
    /* 9 config + 9 iface0 + 3*7 endpoints = 39 bytes precede interface 1 alt 0. */
    const ULONG headerBytes = 9u + 9u + (3u * 7u);
    const ULONG altBytes    = 9u + 7u + 7u;

    if (Alt > DECKBT_SCO_ALT_MAX) {
        return 0;
    }
    return headerBytes + (altBytes * (ULONG)Alt);
}
