/*
 * usb_descriptors.h - USB descriptor contract for the DeckBtService virtual Bluetooth radio.
 *
 * The USB/IP front end (src/service/usbip_device.c) depends on the endpoint addresses and the
 * alternate-setting table below; changes require matching updates there.
 *
 * Binding rationale
 * -----------------
 * C:\Windows\INF\bth.inf [GenericAdapter.NTamd64] matches the compatible ID
 *     USB\Class_E0&SubClass_01&Prot_01
 * and its ExcludeID list holds VID_0CF3 PIDs 0036 / E003 / E004 / E005 but not 6390. So a device
 * reporting VID_0CF3 & PID_6390 with class E0/01/01 is claimed by inbox bth.inf, which installs
 * BTHUSB + BTHPORT. PID_6390 is what Qualcomm's own qcbtuart.sys uses for its (non-functional)
 * VUSB_HCI personality, so it is the natural choice and is known not to collide with a real
 * excluded dongle.
 *
 * Deliberately not a composite device: no IAD, bDeviceClass = 0xE0 at device level. A composite
 * device would get usbccgp inserted, which would split HCI and SCO into separate interface
 * collections; BTHUSB expects to own the whole device.
 *
 * USB device emulation (UDE) constraints
 * -------------------------------------------------------------------
 * usbip-win2's client is a UDE driver, so Windows validates this device the way it validates any
 * UDE device (the same rules applied to the VirtBthUsb kernel driver, which used UdeCx directly):
 *   1. The device must be presented as high speed (USB/IP speed 3) even though real Bluetooth
 *      dongles are full speed. UDE/USBHUB3 treats every port as high speed and validates the configuration
 *      descriptor against HS rules; a full-speed-shaped descriptor yields
 *      CONFIGURATION_DESCRIPTOR_VALIDATION_FAILURE.
 *   2. Consequently bulk endpoints declare wMaxPacketSize 512 (HS minimum for bulk),
 *      not the 64 that a real dongle reports.
 *   3. Isochronous endpoints declare bInterval >= 4, because ucx01000 interprets bInterval
 *      as 125 us units regardless of the declared speed and rejects the URB with
 *      USBD_STATUS_INVALID_PARAMETER otherwise. bInterval 4 => 2^(4-1) = 8 microframes = 1 ms,
 *      which is exactly the SCO frame period, so the byte rates in the alternate-setting table
 *      below remain the Bluetooth-spec ones.
 *
 * Alternate-setting table is the standard Bluetooth USB transport one (Bluetooth Core Spec,
 * USB Transport Layer): 0, 9, 17, 25, 33, 49, 63 bytes per frame for alt 0..6.
 */

#pragma once

#include <windows.h>

/* ---------------------------------------------------------------- identity */

#define DECKBT_VENDOR_ID          0x0CF3u
#define DECKBT_PRODUCT_ID         0x6390u
#define DECKBT_DEVICE_BCD         0x0001u

#define DECKBT_CLASS_WIRELESS     0xE0u  /* Wireless Controller                  */
#define DECKBT_SUBCLASS_RF        0x01u  /* Radio Frequency                      */
#define DECKBT_PROTOCOL_BLUETOOTH 0x01u  /* Bluetooth programming interface      */

/* ---------------------------------------------------------------- endpoints */

#define DECKBT_EP_EVENT_IN        0x81u  /* interrupt IN  - HCI events           */
#define DECKBT_EP_ACL_OUT         0x02u  /* bulk OUT      - HCI ACL data         */
#define DECKBT_EP_ACL_IN          0x82u  /* bulk IN       - HCI ACL data         */
#define DECKBT_EP_SCO_OUT         0x03u  /* isoch OUT     - SCO / voice to air   */
#define DECKBT_EP_SCO_IN          0x83u  /* isoch IN      - SCO / microphone     */

#define DECKBT_EP_EVENT_MAXPACKET 16u
#define DECKBT_EP_ACL_MAXPACKET   512u   /* HS minimum for bulk; constraint 2    */
#define DECKBT_EP_EVENT_INTERVAL  4u     /* 2^(4-1) microframes = 1 ms           */
#define DECKBT_EP_SCO_INTERVAL    4u     /* constraint 3: at least 4             */

#define DECKBT_IFACE_HCI          0u     /* control/event/ACL, single setting    */
#define DECKBT_IFACE_SCO          1u     /* voice, alternate settings 0..6       */

#define DECKBT_SCO_ALT_COUNT      7u
#define DECKBT_SCO_ALT_MAX        6u

/* Bytes per 1 ms frame for SCO alternate settings 0..6. Index == bAlternateSetting. */
extern const USHORT DeckBtScoAltPacketSize[DECKBT_SCO_ALT_COUNT];

/* ---------------------------------------------------------------- string ids */

#define DECKBT_ISTRING_LANGIDS      0u
#define DECKBT_ISTRING_MANUFACTURER 1u
#define DECKBT_ISTRING_PRODUCT      2u
#define DECKBT_ISTRING_SERIAL       3u
#define DECKBT_ISTRING_COUNT        4u

#define DECKBT_LANGID_EN_US         0x0409u

/* ---------------------------------------------------------------- tables */

/* Standard device descriptor, 18 bytes. */
extern const UCHAR DeckBtDeviceDescriptor[];
extern const ULONG DeckBtDeviceDescriptorSize;

/*
 * Full configuration descriptor set: configuration, interface 0 (+3 endpoints),
 * interface 1 alt 0..6 (+2 isochronous endpoints each). wTotalLength covers the whole blob.
 */
extern const UCHAR DeckBtConfigDescriptor[];
extern const ULONG DeckBtConfigDescriptorSize;

extern const UCHAR DeckBtStringLangIds[];
extern const UCHAR DeckBtStringManufacturer[];
extern const UCHAR DeckBtStringProduct[];
extern const UCHAR DeckBtStringSerial[];

/* Returns NULL for an unknown index. Sets *Length on success. */
_Success_(return != NULL)
const UCHAR *DeckBtGetStringDescriptor(_In_ UCHAR Index, _Out_ ULONG *Length);

/* Byte offset of interface 1's alternate setting `Alt` inside DeckBtConfigDescriptor. */
ULONG DeckBtScoAltOffset(_In_ UCHAR Alt);
