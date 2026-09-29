/*
 * hci_stub.h - synthetic HCI controller.
 *
 * Answers the HCI command set that BTHUSB/BTHPORT issue while bringing a radio up, from static
 * tables, so that the driver can exercise USB emulation and PnP binding without hardware.
 * The interface below is what the data plane is written against.
 *
 * Framing note: over the USB Bluetooth transport, HCI commands arrive as the data stage of an
 * EP0 class OUT transfer, and events are read from the interrupt IN endpoint. The H4 one-byte
 * packet-type prefix used on UART is not present on USB - endpoints separate the streams.
 */

#pragma once

#include <windows.h>

#include "hci_transport.h"

#define HCI_MAX_EVENT_SIZE      257u   /* 2-byte header + up to 255 payload bytes */
#define HCI_EVENT_FIFO_DEPTH    32u

/* Event codes */
#define HCI_EVT_COMMAND_COMPLETE 0x0Eu
#define HCI_EVT_COMMAND_STATUS   0x0Fu

/* OGF/OCF opcodes, little-endian on the wire */
#define HCI_OP_RESET                        0x0C03u
#define HCI_OP_SET_EVENT_MASK               0x0C01u
#define HCI_OP_SET_EVENT_MASK_PAGE_2        0x0C63u
#define HCI_OP_WRITE_LE_HOST_SUPPORT        0x0C6Du
#define HCI_OP_READ_LOCAL_VERSION           0x1001u
#define HCI_OP_READ_LOCAL_SUPPORTED_CMDS    0x1002u
#define HCI_OP_READ_LOCAL_FEATURES          0x1003u
#define HCI_OP_READ_LOCAL_EXT_FEATURES      0x1004u
#define HCI_OP_READ_BUFFER_SIZE             0x1005u
#define HCI_OP_READ_BD_ADDR                 0x1009u
#define HCI_OP_LE_READ_BUFFER_SIZE          0x2002u
#define HCI_OP_LE_READ_LOCAL_FEATURES       0x2003u
#define HCI_OP_LE_READ_SUPPORTED_STATES     0x201Cu

/*
 * Commands BTHUSB issues that require return parameters. A status-only reply to any of these
 * produces an event size mismatch (BTHUSB event 5) or a command timeout (event 3).
 */
#define HCI_OP_READ_LOCAL_NAME              0x0C14u
#define HCI_OP_READ_PAGE_TIMEOUT            0x0C17u
#define HCI_OP_READ_SCAN_ENABLE             0x0C19u
#define HCI_OP_READ_CLASS_OF_DEVICE         0x0C23u
#define HCI_OP_READ_VOICE_SETTING           0x0C25u
#define HCI_OP_READ_LOCAL_SUPPORTED_CODECS  0x100Bu
#define HCI_OP_LE_READ_ADV_TX_POWER         0x2007u
#define HCI_OP_LE_READ_ACCEPT_LIST_SIZE     0x200Fu
/*
 * 0x2023 is LE_Read_Suggested_Default_Data_Length (4 bytes) and
 * 0x202F is LE_Read_Maximum_Data_Length (8 bytes).
 */
#define HCI_OP_LE_READ_SUGGESTED_DATA_LEN   0x2023u
#define HCI_OP_LE_READ_MAX_DATA_LENGTH      0x202Fu
/* Read_Inquiry_Response_Transmit_Power_Level: 1 byte. */
#define HCI_OP_READ_INQ_RSP_TX_POWER        0x0C58u

/*
 * Stored link keys. BTHUSB event 18 ("Windows cannot store Bluetooth authentication codes
 * (link keys) on the local adapter") comes from these returning nothing useful. Harmless for a
 * stub, but link-key storage is how pairings survive, so answer them properly.
 */
#define HCI_OP_READ_STORED_LINK_KEY         0x0C0Du
#define HCI_OP_WRITE_STORED_LINK_KEY        0x0C11u
#define HCI_OP_DELETE_STORED_LINK_KEY       0x0C12u
#define HCI_OP_LE_READ_RESOLVING_LIST_SIZE  0x202Au

typedef struct _HCI_EVENT_SLOT {
    ULONG Length;
    UCHAR Data[HCI_MAX_EVENT_SIZE];
} HCI_EVENT_SLOT;

typedef struct _HCI_STUB {
    HCI_EVENT_SLOT Fifo[HCI_EVENT_FIFO_DEPTH];
    ULONG          Head;      /* next slot to read  */
    ULONG          Tail;      /* next slot to write */
    ULONG          Count;
    ULONG          Dropped;   /* events lost to a full FIFO; nonzero means a real bug */
    UCHAR          BdAddr[6];
} HCI_STUB, *PHCI_STUB;

VOID HciStubInit(_Out_ PHCI_STUB Stub);

/* Binds the stub to an HCI_TRANSPORT structure (fills Ops, Context, Backend). */
VOID HciStubBindTransport(
    _Out_ HCI_TRANSPORT *Transport,
    _Inout_ HCI_STUB *Stub);

/*
 * Consume one HCI command packet (opcode LE16, plen, params) and queue the resulting event(s).
 * Returns FALSE only if the command packet is malformed.
 * Caller must hold the controller lock.
 */
BOOLEAN HciStubSubmitCommand(
    _Inout_ PHCI_STUB Stub,
    _In_reads_bytes_(Length) const UCHAR *Command,
    _In_ ULONG Length);

/* Pops the oldest queued event. Returns FALSE if the FIFO is empty. Caller holds the lock. */
BOOLEAN HciStubPopEvent(
    _Inout_ PHCI_STUB Stub,
    _Out_writes_bytes_to_(Capacity, *Written) UCHAR *Buffer,
    _In_ ULONG Capacity,
    _Out_ ULONG *Written);

BOOLEAN HciStubHasEvent(_In_ const HCI_STUB *Stub);
