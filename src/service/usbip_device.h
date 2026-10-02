/*
 * usbip_device.h - the DeckBtService Bluetooth radio as a USB/IP server-side device.
 *
 * The descriptors (src/common/usb_descriptors.c), EP0 behaviour, HCI transport seam
 * (src/include/hci_transport.h) and SCO pacing (src/common/sco_usb.c) of an emulated USB
 * Bluetooth radio, driven by USB/IP CMD_SUBMIT / CMD_UNLINK.
 *
 * Platform-free: no sockets, threads, clocks or allocation. The host supplies
 *   - a Send callback that writes one complete USB/IP message to the connection,
 *   - the current time in 100 ns units on every call that can complete a transfer,
 *   - mutual exclusion: every UsbipDevice* call and every HCI_TRANSPORT call the backend makes
 *     under the controller lock must be serialised by the same lock.
 *
 * Differences from UdeCx that shape this file:
 *   - usbip-win2 forwards GET_DESCRIPTOR, SET_CONFIGURATION and SET_INTERFACE to the server
 *     (its upper filter turns SELECT_CONFIGURATION / SELECT_INTERFACE URBs into control
 *     transfers), so they are answered here. The SCO alternate setting therefore comes straight
 *     from SET_INTERFACE(interface 1) instead of being inferred from endpoint lists.
 *   - A port reset arrives as SET_PORT_FEATURE(PORT_RESET) on EP0 (bmRequestType 0x23), the way
 *     the Linux usbip host driver receives it.
 */

#pragma once

#include <windows.h>

#include "../include/hci_transport.h"
#include "../include/sco_usb.h"

#define USBIP_DEV_MAX_PARKED       32u    /* parked interrupt / bulk IN reads per endpoint */
#define USBIP_DEV_MAX_ISO_URBS     16u    /* parked isochronous transfers per direction */
#define USBIP_DEV_MAX_ISO_PACKETS  256u   /* packets accepted per isochronous transfer */
#define USBIP_DEV_SCRATCH_BYTES    65536u /* largest IN payload sent in one RET_SUBMIT */
#define USBIP_DEV_SENT_HISTORY     32u    /* recent event / ACL replies kept per stream */
#define USBIP_DEV_SENT_BYTES       1028u  /* largest HCI event or ACL packet (1025) */
#define USBIP_DEV_ANSWERED_HISTORY 256u   /* answered writes, control and voice transfers */
#define USBIP_DEV_ORDER_HOLD       200000ull /* 20 ms in 100 ns: longest cross-endpoint hold */

/* Writes one complete message. Returns nonzero on success; zero marks the connection broken. */
typedef int (*USBIP_DEVICE_SEND)(void *Context, const unsigned char *Data, unsigned long Length);

/*
 * Optional diagnostic hook for EP0: the setup packet, the OUT data stage or IN reply, and the
 * USB/IP status sent back (0 or a negative Linux errno).
 */
typedef void (*USBIP_DEVICE_TRACE)(void *Context, const unsigned char Setup[8],
                                   const unsigned char *Data, unsigned long Length, long Status);

typedef struct _USBIP_PARKED {
    unsigned long Seqnum;
    unsigned long Capacity;
} USBIP_PARKED;

typedef struct _USBIP_PARK_QUEUE {
    USBIP_PARKED Items[USBIP_DEV_MAX_PARKED];
    unsigned int Count;             /* FIFO, index 0 is the oldest */
} USBIP_PARK_QUEUE;

typedef struct _USBIP_ISO_URB {
    unsigned long      Seqnum;
    unsigned long long Due;         /* completion time on the endpoint's frame clock */
    unsigned long      Packets;
    unsigned long      Bytes;       /* OUT: bytes accepted */
    unsigned long      MaxPacket;   /* wMaxPacketSize of the alternate setting at submit time */
    long               StartFrame;
    unsigned long      Offset[USBIP_DEV_MAX_ISO_PACKETS];
    unsigned long      Length[USBIP_DEV_MAX_ISO_PACKETS];
} USBIP_ISO_URB;

typedef struct _USBIP_ISO_QUEUE {
    USBIP_ISO_URB Items[USBIP_DEV_MAX_ISO_URBS];
    unsigned int  Head;             /* ring; submission order == due order */
    unsigned int  Count;
} USBIP_ISO_QUEUE;

/*
 * An event or ACL packet already sent in a RET_SUBMIT. usbip-win2 completes a transfer as
 * cancelled the moment it sends CMD_UNLINK and drops any RET_SUBMIT that arrives for it later
 * (drivers/ude/device_ioctl.cpp, send_cmd_unlink_and_complete). A reply that crossed the unlink on
 * the wire therefore never reached Windows; the packet is marked Lost and given to the next read
 * of the endpoint, ahead of newer data. UDE purges every endpoint when the device is suspended or
 * reset. With usbip-win2 0.9.8.0 BTHUSB reset the device every ~77 ms during connection setup
 * (usbip-win2 issue #190, see docs/VERIFICATION.md); with 0.9.8.1 the race is rare.
 */
typedef struct _USBIP_SENT_IN {
    unsigned long Seqnum;
    unsigned long Length;
    unsigned long Order;            /* controller arrival order, if Ordered */
    int           Ordered;
    int           Lost;
    unsigned char Data[USBIP_DEV_SENT_BYTES];
} USBIP_SENT_IN;

typedef struct _USBIP_SENT_RING {
    USBIP_SENT_IN Items[USBIP_DEV_SENT_HISTORY];
    unsigned int  Head;             /* ring in send order; Head is the oldest */
    unsigned int  Count;
    unsigned int  Lost;             /* entries marked Lost, owed to the next reads */
} USBIP_SENT_RING;

typedef struct _USBIP_ANSWERED_RING {
    unsigned long Items[USBIP_DEV_ANSWERED_HISTORY];
    unsigned int  Head;
    unsigned int  Count;
} USBIP_ANSWERED_RING;

typedef struct _USBIP_DEVICE_STATS {
    unsigned long ControlRequests;
    unsigned long HciCommands;
    unsigned long Stalls;
    unsigned long EventsIn;
    unsigned long AclOut;
    unsigned long AclIn;
    unsigned long ScoOutUrbs;
    unsigned long ScoInUrbs;
    unsigned long ScoOutBytes;
    unsigned long ScoInBytes;
    unsigned long ScoOutHci;
    unsigned long ScoOutRejected;
    unsigned long ScoRejectedUrbs;
    unsigned long ScoUnpacedUrbs;     /* completed at once: every slot for the direction was taken */
    unsigned long Unlinks;
    unsigned long UnlinksMissed;      /* unlink of a transfer already answered */
    unsigned long RepliesLost;        /* ...whose reply carried an event or ACL packet */
    unsigned long UnlinksAnswered;     /* ...whose answer was a write, control or voice transfer */
    unsigned long UnlinksIrrecoverable;/* ...whose event/ACL reply aged out of the reply history */
    unsigned long Redelivered;        /* lost packets given to a later read */
    unsigned long OrderHolds;         /* a packet waited for an earlier one on the other endpoint */
    unsigned long OrderHoldTimeouts;  /* ...and was delivered anyway after USBIP_DEV_ORDER_HOLD */
    unsigned long AltChanges;
    unsigned long PortResets;
    unsigned long MaxLateUs;
} USBIP_DEVICE_STATS;

typedef struct _USBIP_DEVICE {
    HCI_TRANSPORT     *Transport;
    USBIP_DEVICE_SEND  Send;
    void              *SendContext;
    USBIP_DEVICE_TRACE Trace;
    void              *TraceContext;
    int                Broken;         /* a Send failed; the host must drop the connection */
    int                Poisoned;       /* an unrecoverable unlink; the host must drop the connection */

    unsigned char      Configuration;
    unsigned char      ScoAlt;

    USBIP_PARK_QUEUE   EventIn;
    USBIP_PARK_QUEUE   AclIn;
    USBIP_SENT_RING    EventSent;
    USBIP_SENT_RING    AclSent;
    USBIP_ANSWERED_RING Answered;      /* answered writes, control and voice transfers */
    USBIP_ISO_QUEUE    ScoOutQ;
    USBIP_ISO_QUEUE    ScoInQ;
    USBIP_ISO_URB      IsoOverflow;    /* a transfer arriving with every slot taken */
    unsigned long long HoldSince;      /* 0, or when a packet began waiting for the other endpoint */

    SCO_USB_CLOCK      ScoOutClock;
    SCO_USB_CLOCK      ScoInClock;
    SCO_USB_OUT        ScoOut;
    SCO_USB_IN         ScoIn;

    USBIP_DEVICE_STATS Stats;

    unsigned char      Scratch[USBIP_DEV_SCRATCH_BYTES];
    unsigned char      IsoDesc[USBIP_DEV_MAX_ISO_PACKETS * 16u];
    unsigned char      Tx[48u + USBIP_DEV_SCRATCH_BYTES + USBIP_DEV_MAX_ISO_PACKETS * 16u];
} USBIP_DEVICE;

void UsbipDeviceInit(USBIP_DEVICE *Device, HCI_TRANSPORT *Transport,
                     USBIP_DEVICE_SEND Send, void *SendContext);

/*
 * Start of an imported session: unconfigured, SCO at alternate setting 0, nothing parked, the
 * transport's queues emptied. Parked transfers are forgotten without replies (the connection
 * they belonged to is gone).
 */
void UsbipDeviceBeginSession(USBIP_DEVICE *Device);

/* OP_REP_DEVLIST body for this device (usb_device + interfaces). Returns bytes written. */
unsigned long UsbipDeviceDescribe(unsigned char *Out, unsigned long Capacity, const char *BusId,
                                  unsigned long BusNum, unsigned long DevNum, int WithInterfaces);

/*
 * Bytes that follow a 48-byte URB header on the wire (OUT data + isochronous descriptors), or
 * 0xFFFFFFFF if the header is malformed.
 */
unsigned long UsbipCommandPayloadLength(const unsigned char Header[48]);

/*
 * One CMD_SUBMIT or CMD_UNLINK with its payload. Returns zero on a protocol violation, after
 * which the host must drop the connection.
 */
int UsbipDeviceHandle(USBIP_DEVICE *Device, const unsigned char Header[48],
                      const unsigned char *Payload, unsigned long PayloadLength,
                      unsigned long long Now);

/*
 * Moves waiting events and ACL packets into parked IN reads, in the order the controller sent them
 * (when the backend reports it, HciTransportPeekOrder). Events and ACL use different endpoints: a
 * packet whose endpoint has no read parked holds back later packets of the other endpoint, for at
 * most USBIP_DEV_ORDER_HOLD; UsbipDeviceTick releases an expired hold. Called on backend notify.
 */
void UsbipDeviceDrain(USBIP_DEVICE *Device, unsigned long long Now);

/*
 * SCO pacing tick: releases OUT voice whose frame has passed and completes due isochronous
 * transfers. Returns the earliest time more work falls due, or 0 when nothing is pending.
 */
unsigned long long UsbipDeviceTick(USBIP_DEVICE *Device, unsigned long long Now);
