/*
 * usbip_proto.h - USB/IP wire format (protocol 1.1.1) as spoken by the usbip-win2 client.
 *
 * Reference: https://www.kernel.org/doc/html/latest/usb/usbip_protocol.html
 * Every multi-byte field is big-endian, except the 8-byte USB setup packet, which travels in
 * USB (little-endian) order. The URB headers are 48 bytes; the op headers are 8 bytes.
 *
 * usbip-win2 specifics (include/usbip/proto.h in that project):
 *   - number_of_packets is -1 (0xFFFFFFFF) for non-isochronous transfers.
 *   - isochronous IN data in RET_SUBMIT is compacted: packets follow each other without the
 *     gaps their offsets imply, and SUM(actual_length) == actual_length.
 */

#pragma once

#include <windows.h>

#define USBIP_VERSION             0x0111u

#define USBIP_OP_REQ_IMPORT       0x8003u
#define USBIP_OP_REP_IMPORT       0x0003u
#define USBIP_OP_REQ_DEVLIST      0x8005u
#define USBIP_OP_REP_DEVLIST      0x0005u

#define USBIP_ST_OK               0u
#define USBIP_ST_NA               1u
#define USBIP_ST_DEV_BUSY         2u
#define USBIP_ST_NODEV            4u

#define USBIP_CMD_SUBMIT          1u
#define USBIP_CMD_UNLINK          2u
#define USBIP_RET_SUBMIT          3u
#define USBIP_RET_UNLINK          4u

#define USBIP_DIR_OUT             0u
#define USBIP_DIR_IN              1u

#define USBIP_SPEED_HIGH          3u

#define USBIP_OP_HEADER_SIZE      8u
#define USBIP_BUSID_SIZE          32u
#define USBIP_PATH_SIZE           256u
/* struct usbip_usb_device: path, busid, 3 x u32, 3 x u16, 6 x u8. */
#define USBIP_USB_DEVICE_SIZE     312u
#define USBIP_USB_INTERFACE_SIZE  4u
#define USBIP_URB_HEADER_SIZE     48u
#define USBIP_ISO_DESC_SIZE       16u

#define USBIP_NON_ISO_PACKETS     0xFFFFFFFFu
#define USBIP_MAX_ISO_PACKETS     1024u

/* Linux errno values carried in RET_SUBMIT.status / RET_UNLINK.status. */
#define USBIP_EPIPE               (-32)
#define USBIP_EINVAL              (-22)
#define USBIP_ECONNRESET          (-104)

/* URB header field offsets (bytes from the start of the 48-byte header). */
#define USBIP_HDR_COMMAND         0u
#define USBIP_HDR_SEQNUM          4u
#define USBIP_HDR_DEVID           8u
#define USBIP_HDR_DIRECTION       12u
#define USBIP_HDR_EP              16u
/* CMD_SUBMIT */
#define USBIP_HDR_FLAGS           20u
#define USBIP_HDR_LENGTH          24u
#define USBIP_HDR_START_FRAME     28u
#define USBIP_HDR_PACKETS         32u
#define USBIP_HDR_INTERVAL        36u
#define USBIP_HDR_SETUP           40u
/* RET_SUBMIT */
#define USBIP_HDR_STATUS          20u
#define USBIP_HDR_ACTUAL          24u
#define USBIP_HDR_RET_START_FRAME 28u
#define USBIP_HDR_RET_PACKETS     32u
#define USBIP_HDR_ERROR_COUNT     36u
/* CMD_UNLINK / RET_UNLINK */
#define USBIP_HDR_UNLINK_SEQNUM   20u
#define USBIP_HDR_UNLINK_STATUS   20u

static __inline unsigned long
UsbipGet32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

static __inline unsigned short
UsbipGet16(const unsigned char *p)
{
    return (unsigned short)(((unsigned)p[0] << 8) | (unsigned)p[1]);
}

static __inline void
UsbipPut32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static __inline void
UsbipPut16(unsigned char *p, unsigned short v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}
