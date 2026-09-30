/*
 * hci_transport.h - transport seam between the Windows-facing USB front end and the HCI backend.
 *
 * DeckBtService presents a virtual USB Bluetooth radio to Windows. The USB front end
 * (src/service/usbip_device.c) moves packets between USB/IP transfers and this interface without
 * backend-specific knowledge. Two backends implement it: the synthetic stub (hci_stub.c) and the
 * QCA2066 over its UART (src/service/qca_backend.c).
 *
 * Layering rules:
 *   - Submit* / Has / Pop / LastEventLength are called with the controller lock held. They must
 *     not block or allocate.
 *   - Notify is called with no lock held that any Submit/Has/Pop/LastEventLength/Reset path
 *     acquires. It allows an asynchronous backend to wake the front end when a stream becomes
 *     readable. The stub completes synchronously inside SubmitCommand; the UART backend calls
 *     Notify from its reader after releasing the controller lock. Calling Notify with that lock
 *     held would deadlock against Pop.
 *   - Reset is called with the controller lock held. It leaves every stream empty and discards
 *     any partially received packet. On a byte stream such as the UART that means skipping the
 *     rest of that packet as it arrives, not forgetting where packets start (h4_codec.h).
 *
 * Framing:
 * Buffers crossing this interface carry no H4 packet-type prefix. USB separates streams with
 * endpoints, so an event buffer starts at the event code and an ACL buffer starts at the handle.
 * The H4 prefix is a UART-transport detail handled within the UART backend.
 *
 * No dynamic memory allocation.
 */

#pragma once

#include <windows.h>

/* Backend identifiers, for diagnostics. */
#define HCI_BACKEND_STUB 0u
#define HCI_BACKEND_UART 1u

/*
 * The three inbound streams a Bluetooth USB transport exposes, in the order of their endpoints.
 * Collapsing them into one indexed pair of Has/Pop calls keeps the front end's drain loop generic
 * instead of triplicating it.
 */
typedef enum _HCI_STREAM {
    HciStreamEvent = 0,   /* -> EP 0x81 interrupt IN */
    HciStreamAcl   = 1,   /* -> EP 0x82 bulk IN      */
    HciStreamSco   = 2,   /* -> EP 0x83 isoch IN     */
    HciStreamMax   = 3
} HCI_STREAM;

typedef struct _HCI_TRANSPORT HCI_TRANSPORT;

/*
 * Wake-up callback into the front end. `Stream` says which stream became readable. The front end
 * responds by draining that stream into whatever requests are parked on the matching endpoint
 * queue. May be called spuriously; the front end must tolerate a notify with nothing to pop.
 */
typedef void (*HCI_TRANSPORT_NOTIFY)(void *NotifyContext, HCI_STREAM Stream);

typedef struct _HCI_TRANSPORT_OPS {
    /*
     * One HCI command packet from the EP0 class OUT data stage: opcode LE16, plen, params.
     * FALSE means the packet is malformed or unanswerable and the front end should stall EP0.
     * A backend that forwards asynchronously returns TRUE as soon as the packet is accepted for
     * transmission - TRUE is not a claim that the controller answered.
     */
    unsigned char (*SubmitCommand)(HCI_TRANSPORT *Transport,
                                   const unsigned char *Packet, unsigned long Length);

    /* One HCI ACL data packet from bulk OUT. FALSE means dropped (no credit, or malformed). */
    unsigned char (*SubmitAcl)(HCI_TRANSPORT *Transport,
                               const unsigned char *Packet, unsigned long Length);

    /*
     * One HCI synchronous (SCO/eSCO) data packet from isoch OUT. Per the USB Bluetooth transport
     * specification this path is timing-sensitive: when the outbound synchronous FIFO is full,
     * the backend overwrites the oldest queued data rather than increasing queue depth.
     */
    unsigned char (*SubmitSco)(HCI_TRANSPORT *Transport,
                               const unsigned char *Packet, unsigned long Length);

    /*
     * Is there at least one complete packet waiting on `Stream`? A backend's INBOUND synchronous
     * FIFO is bounded and overwrites oldest, exactly like the outbound one: Windows does not
     * buffer incoming synchronous data for clients, so gaps in pending reads are simply lost and
     * a growing inbound queue would convert latency into permanent lag.
     */
    unsigned char (*HasStream)(const HCI_TRANSPORT *Transport, HCI_STREAM Stream);

    /*
     * Move the oldest complete packet on `Stream` into `Buffer`. FALSE if the stream is empty or
     * the packet does not fit `Capacity`, in which case *Written is 0 and the packet is left
     * queued. Never partially fills.
     *
     * Exception for HciStreamSco: a synchronous packet that does not fit `Capacity` is
     * discarded, not left queued, and PopStream returns FALSE. Isoch IN capacity is fixed by
     * whichever SCO alternate setting Windows selected (alt 1 = 9 bytes/frame ... alt 6 = 63),
     * so an oversize packet left at the head would prevent the synchronous stream from draining.
     */
    unsigned char (*PopStream)(HCI_TRANSPORT *Transport, HCI_STREAM Stream,
                               unsigned char *Buffer, unsigned long Capacity,
                               unsigned long *Written);

    /*
     * Length of the most recently queued event, or 0. Exists for the EP0 registry
     * breadcrumb trace. Without it the front end would have to reach into backend state,
     * which violates the layering boundary.
     *
     * Defined only for HCI_BACKEND_STUB, where events materialise synchronously inside
     * SubmitCommand. An asynchronous backend returns 0, and the front end uses the
     * popped length returned by PopStream.
     */
    unsigned long (*LastEventLength)(const HCI_TRANSPORT *Transport);

    /* Post-enumeration USB reset: drop all queued packets and any partial receive state. */
    void (*Reset)(HCI_TRANSPORT *Transport);

    /*
     * Arrival order of the oldest packet queued on Stream (event or ACL): one counter across both
     * streams, so the front end can hand them to the host in the order the controller sent them.
     * Events and ACL reach the host on different USB endpoints; without this an event such as
     * Disconnection_Complete can overtake the ACL data that preceded it. FALSE if the stream is
     * empty or the backend does not track order. May be NULL (no ordering).
     */
    unsigned char (*PeekOrder)(const HCI_TRANSPORT *Transport, HCI_STREAM Stream, unsigned long *Order);
    /*
     * Flush all queued synchronous (SCO) voice packets, both inbound and outbound.
     * Called when the voice stream ends (e.g. SCO alternate setting change to 0) so
     * stranded packets cannot leak into a subsequent call.
     */
    void (*FlushSco)(HCI_TRANSPORT *Transport);
} HCI_TRANSPORT_OPS;

struct _HCI_TRANSPORT {
    const HCI_TRANSPORT_OPS *Ops;   /* set by the backend's Bind function */
    void                    *Context;/* backend-private state; opaque to the front end */
    HCI_TRANSPORT_NOTIFY     Notify; /* set by the front end before the backend is started */
    void                    *NotifyContext;
    unsigned long            Backend;/* HCI_BACKEND_*; for breadcrumbs and diagnostics only */
};

/*
 * Thin inline forwarders. They exist so call sites read as verbs rather than as pointer
 * dereferences, and so a null Ops (backend never bound) fails closed instead of bugchecking.
 */

static __inline unsigned char
HciTransportSubmitCommand(HCI_TRANSPORT *Transport,
                          const unsigned char *Packet, unsigned long Length)
{
    return (Transport->Ops != NULL)
         ? Transport->Ops->SubmitCommand(Transport, Packet, Length)
         : (unsigned char)0;
}

static __inline unsigned char
HciTransportSubmitAcl(HCI_TRANSPORT *Transport,
                      const unsigned char *Packet, unsigned long Length)
{
    return (Transport->Ops != NULL)
         ? Transport->Ops->SubmitAcl(Transport, Packet, Length)
         : (unsigned char)0;
}

static __inline unsigned char
HciTransportSubmitSco(HCI_TRANSPORT *Transport,
                      const unsigned char *Packet, unsigned long Length)
{
    return (Transport->Ops != NULL)
         ? Transport->Ops->SubmitSco(Transport, Packet, Length)
         : (unsigned char)0;
}

static __inline unsigned char
HciTransportHasStream(const HCI_TRANSPORT *Transport, HCI_STREAM Stream)
{
    return (Transport->Ops != NULL)
         ? Transport->Ops->HasStream(Transport, Stream)
         : (unsigned char)0;
}

static __inline unsigned char
HciTransportPopStream(HCI_TRANSPORT *Transport, HCI_STREAM Stream,
                      unsigned char *Buffer, unsigned long Capacity, unsigned long *Written)
{
    if (Transport->Ops == NULL) {
        *Written = 0;
        return (unsigned char)0;
    }
    return Transport->Ops->PopStream(Transport, Stream, Buffer, Capacity, Written);
}

static __inline unsigned long
HciTransportLastEventLength(const HCI_TRANSPORT *Transport)
{
    return (Transport->Ops != NULL) ? Transport->Ops->LastEventLength(Transport) : 0u;
}

static __inline void
HciTransportReset(HCI_TRANSPORT *Transport)
{
    if (Transport->Ops != NULL) {
        Transport->Ops->Reset(Transport);
    }
}
static __inline void
HciTransportFlushSco(HCI_TRANSPORT *Transport)
{
    if (Transport != NULL && Transport->Ops != NULL && Transport->Ops->FlushSco != NULL) {
        Transport->Ops->FlushSco(Transport);
    }
}


static __inline unsigned char
HciTransportPeekOrder(const HCI_TRANSPORT *Transport, HCI_STREAM Stream, unsigned long *Order)
{
    return (Transport->Ops != NULL && Transport->Ops->PeekOrder != NULL)
         ? Transport->Ops->PeekOrder(Transport, Stream, Order)
         : 0u;
}

/* Called by an asynchronous backend when `Stream` becomes readable. Lock must NOT be held. */
static __inline void
HciTransportNotify(HCI_TRANSPORT *Transport, HCI_STREAM Stream)
{
    if (Transport->Notify != NULL) {
        Transport->Notify(Transport->NotifyContext, Stream);
    }
}
