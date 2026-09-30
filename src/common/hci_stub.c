/*
 * hci_stub.c - synthetic HCI controller. See hci_stub.h for scope and rationale.
 *
 * Design: one table maps an opcode to its Command_Complete return parameters. Any opcode not in
 * the table still gets a well-formed Command_Complete with status 0x00, because BTHPORT probes a
 * long tail of optional commands and a missing response stalls enumeration.
 */

#include "../include/hci_stub.h"

#define HCI_STATUS_SUCCESS 0x00u

/* Return parameters, excluding the leading status byte, for commands that need them. */

/* Read_Local_Version_Information: HCI 5.2 (0x0B), rev 0, LMP 5.2, manufacturer 0x001D
 * (Qualcomm, as the Deck's controller reports: BTHENUM ...LOCALMFG&001d), LMP subversion 0. */
static const UCHAR g_LocalVersion[] = {
    0x0B, 0x00, 0x00, 0x0B, 0x1D, 0x00, 0x00, 0x00
};

/* Read_Local_Supported_Commands: 64 bytes. All ones would claim unsupported commands,
 * so advertise the mandatory core set only: this is the conservative choice and BTHPORT copes. */
static const UCHAR g_SupportedCommands[64] = {
    0xFF, 0xFF, 0xFF, 0x03, 0xCE, 0xFF, 0xEF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

/* Read_Local_Supported_Features: 8 bytes. SCO + eSCO bits matter for this project:
 * byte 0 bit 3 = SCO link, byte 3 bit 7 = eSCO (EV3), byte 4 bit 7 = LE supported. */
static const UCHAR g_LocalFeatures[8] = {
    0xBF, 0xFE, 0xCF, 0xFE, 0xDB, 0xFF, 0x7B, 0x87
};

/* Read_Buffer_Size: ACL 1021 bytes, SCO 255 bytes, 8 ACL packets, 8 SCO packets. */
static const UCHAR g_BufferSize[] = {
    0xFD, 0x03, 0xFF, 0x08, 0x00, 0x08, 0x00
};

/* LE_Read_Buffer_Size: 251-byte LE ACL, 4 packets. */
static const UCHAR g_LeBufferSize[] = { 0xFB, 0x00, 0x04 };

/* LE_Read_Local_Supported_Features: 8 bytes. */
static const UCHAR g_LeFeatures[8] = { 0x7D, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

/*
 * LE_Read_Supported_States: 8 bytes, little-endian bitmask.
 * BTHUSB requires a minimum supported state mask covering 0x2491f7fffff.
 * Claim every state so the required mask is a subset.
 */
static const UCHAR g_LeStates[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* Read_Local_Name: exactly 248 bytes, NUL padded (Core Spec 7.3.12). */
static const UCHAR g_LocalName[248] = { 'D','e','c','k','B','t','U','s','b' };

/* Read_Class_Of_Device: 3 bytes LE. 0x1C0104 = Computer / Laptop, common inbox value. */
static const UCHAR g_ClassOfDevice[3] = { 0x04, 0x01, 0x1C };

/* Read_Voice_Setting: 2 bytes LE. 0x0060 = CVSD, 16-bit linear input, 2's complement. */
static const UCHAR g_VoiceSetting[2] = { 0x60, 0x00 };

/* Read_Page_Timeout: 2 bytes LE, 0x2000 slots - the specification default. */
static const UCHAR g_PageTimeout[2] = { 0x00, 0x20 };

/* Read_Scan_Enable: 1 byte. 0x00 = no scans enabled, which is the post-reset state. */
static const UCHAR g_ScanEnable[1] = { 0x00 };

/*
 * Read_Local_Supported_Codecs [v1]:
 *   Num_Supported_Standard_Codecs, Standard_Codec_IDs[], Num_Vendor_Codecs, Vendor_IDs[]
 * Advertise CVSD (0x02) and mSBC (0x05) - the two codecs Hands-Free actually uses, and the
 * whole point of this project.
 */
static const UCHAR g_LocalCodecs[] = { 0x02, 0x02, 0x05, 0x00 };

/* LE_Read_Advertising_Physical_Channel_Tx_Power: 1 byte, dBm. */
static const UCHAR g_LeAdvTxPower[1] = { 0x09 };

/*
 * LE_Read_Filter_Accept_List_Size / LE_Read_Resolving_List_Size: 1 byte each.
 * 8 entries drew BTHUSB event 31 ("does not support the minimum buffer requirement to support
 * the hardware filtering of Bluetooth Low Energy advertisements"), so advertise 32.
 */
static const UCHAR g_LeListSize[1] = { 0x20 };

/* LE_Read_Suggested_Default_Data_Length: 2 x uint16 LE (octets, time). */
static const UCHAR g_LeSuggestedDataLength[4] = { 0xFB, 0x00, 0x48, 0x08 };

/* Read_Inquiry_Response_Transmit_Power_Level: 1 byte, signed dBm. */
static const UCHAR g_InqRspTxPower[1] = { 0x00 };

/* Read_Stored_Link_Key: Max_Num_Keys LE16, Num_Keys_Read LE16. Claim room for 8, none stored. */
static const UCHAR g_StoredLinkKeys[4] = { 0x08, 0x00, 0x00, 0x00 };

/* Write_Stored_Link_Key: Num_Keys_Written, 1 byte. */
static const UCHAR g_LinkKeysWritten[1] = { 0x01 };

/* Delete_Stored_Link_Key: Num_Keys_Deleted LE16. */
static const UCHAR g_LinkKeysDeleted[2] = { 0x00, 0x00 };

/*
 * LE_Read_Maximum_Data_Length: 4 x uint16 LE -
 * supported max TX octets/time, max RX octets/time.
 */
static const UCHAR g_LeMaxDataLength[8] = { 0xFB, 0x00, 0x48, 0x08, 0xFB, 0x00, 0x48, 0x08 };

VOID HciStubInit(_Out_ PHCI_STUB Stub)
{
    RtlZeroMemory(Stub, sizeof(*Stub));

    /* Locally administered, non-multicast address so it cannot collide with real hardware. */
    Stub->BdAddr[0] = 0x01;
    Stub->BdAddr[1] = 0x00;
    Stub->BdAddr[2] = 0x00;
    Stub->BdAddr[3] = 0xCB;
    Stub->BdAddr[4] = 0xDE;
    Stub->BdAddr[5] = 0x02;  /* 02: locally administered, unicast */
}

BOOLEAN HciStubHasEvent(_In_ const HCI_STUB *Stub)
{
    return Stub->Count != 0;
}

static HCI_EVENT_SLOT *HciStubAllocSlot(_Inout_ PHCI_STUB Stub)
{
    HCI_EVENT_SLOT *slot;

    if (Stub->Count >= HCI_EVENT_FIFO_DEPTH) {
        Stub->Dropped++;
        return NULL;
    }
    slot = &Stub->Fifo[Stub->Tail];
    Stub->Tail = (Stub->Tail + 1) % HCI_EVENT_FIFO_DEPTH;
    Stub->Count++;
    return slot;
}

/*
 * Queue a Command_Complete: event code 0x0E, plen, num_hci_command_packets, opcode LE16,
 * status, then any extra return parameters.
 */
static BOOLEAN HciStubQueueCommandComplete(
    _Inout_ PHCI_STUB Stub,
    _In_ USHORT Opcode,
    _In_reads_bytes_opt_(ExtraLength) const UCHAR *Extra,
    _In_ ULONG ExtraLength)
{
    HCI_EVENT_SLOT *slot;
    ULONG plen = 4u + ExtraLength;   /* ncmd + opcode(2) + status + extra */

    if (plen > 255u || (2u + plen) > HCI_MAX_EVENT_SIZE) {
        return FALSE;
    }

    slot = HciStubAllocSlot(Stub);
    if (slot == NULL) {
        return FALSE;
    }

    slot->Data[0] = HCI_EVT_COMMAND_COMPLETE;
    slot->Data[1] = (UCHAR)plen;
    slot->Data[2] = 1;                              /* num_hci_command_packets */
    slot->Data[3] = (UCHAR)(Opcode & 0xFFu);
    slot->Data[4] = (UCHAR)((Opcode >> 8) & 0xFFu);
    slot->Data[5] = HCI_STATUS_SUCCESS;
    if (ExtraLength != 0 && Extra != NULL) {
        RtlCopyMemory(&slot->Data[6], Extra, ExtraLength);
    }
    slot->Length = 6u + ExtraLength;
    return TRUE;
}

BOOLEAN HciStubSubmitCommand(
    _Inout_ PHCI_STUB Stub,
    _In_reads_bytes_(Length) const UCHAR *Command,
    _In_ ULONG Length)
{
    USHORT opcode;
    UCHAR  plen;

    if (Length < 3u) {
        return FALSE;
    }

    opcode = (USHORT)(Command[0] | ((USHORT)Command[1] << 8));
    plen   = Command[2];
    if (Length < 3u + (ULONG)plen) {
        return FALSE;   /* truncated command packet */
    }

    switch (opcode) {
    case HCI_OP_READ_LOCAL_VERSION:
        return HciStubQueueCommandComplete(Stub, opcode, g_LocalVersion, sizeof(g_LocalVersion));

    case HCI_OP_READ_LOCAL_SUPPORTED_CMDS:
        return HciStubQueueCommandComplete(Stub, opcode, g_SupportedCommands, sizeof(g_SupportedCommands));

    case HCI_OP_READ_LOCAL_FEATURES:
        return HciStubQueueCommandComplete(Stub, opcode, g_LocalFeatures, sizeof(g_LocalFeatures));

    case HCI_OP_READ_LOCAL_EXT_FEATURES: {
        /* page number echoed, max page 2, then the 8 feature bytes. */
        UCHAR ext[10];
        ext[0] = (plen >= 1u) ? Command[3] : 0u;
        ext[1] = 2u;
        RtlCopyMemory(&ext[2], g_LocalFeatures, 8);
        return HciStubQueueCommandComplete(Stub, opcode, ext, sizeof(ext));
    }

    case HCI_OP_READ_BUFFER_SIZE:
        return HciStubQueueCommandComplete(Stub, opcode, g_BufferSize, sizeof(g_BufferSize));

    case HCI_OP_READ_BD_ADDR:
        return HciStubQueueCommandComplete(Stub, opcode, Stub->BdAddr, sizeof(Stub->BdAddr));

    case HCI_OP_LE_READ_BUFFER_SIZE:
        return HciStubQueueCommandComplete(Stub, opcode, g_LeBufferSize, sizeof(g_LeBufferSize));

    case HCI_OP_LE_READ_LOCAL_FEATURES:
        return HciStubQueueCommandComplete(Stub, opcode, g_LeFeatures, sizeof(g_LeFeatures));

    case HCI_OP_LE_READ_SUPPORTED_STATES:
        return HciStubQueueCommandComplete(Stub, opcode, g_LeStates, sizeof(g_LeStates));

    case HCI_OP_READ_LOCAL_NAME:
        return HciStubQueueCommandComplete(Stub, opcode, g_LocalName, sizeof(g_LocalName));

    case HCI_OP_READ_CLASS_OF_DEVICE:
        return HciStubQueueCommandComplete(Stub, opcode, g_ClassOfDevice, sizeof(g_ClassOfDevice));

    case HCI_OP_READ_VOICE_SETTING:
        return HciStubQueueCommandComplete(Stub, opcode, g_VoiceSetting, sizeof(g_VoiceSetting));

    case HCI_OP_READ_PAGE_TIMEOUT:
        return HciStubQueueCommandComplete(Stub, opcode, g_PageTimeout, sizeof(g_PageTimeout));

    case HCI_OP_READ_SCAN_ENABLE:
        return HciStubQueueCommandComplete(Stub, opcode, g_ScanEnable, sizeof(g_ScanEnable));

    case HCI_OP_READ_LOCAL_SUPPORTED_CODECS:
        return HciStubQueueCommandComplete(Stub, opcode, g_LocalCodecs, sizeof(g_LocalCodecs));

    case HCI_OP_LE_READ_ADV_TX_POWER:
        return HciStubQueueCommandComplete(Stub, opcode, g_LeAdvTxPower, sizeof(g_LeAdvTxPower));

    case HCI_OP_LE_READ_ACCEPT_LIST_SIZE:
    case HCI_OP_LE_READ_RESOLVING_LIST_SIZE:
        return HciStubQueueCommandComplete(Stub, opcode, g_LeListSize, sizeof(g_LeListSize));

    case HCI_OP_LE_READ_MAX_DATA_LENGTH:
        return HciStubQueueCommandComplete(Stub, opcode, g_LeMaxDataLength,
                                           sizeof(g_LeMaxDataLength));

    case HCI_OP_LE_READ_SUGGESTED_DATA_LEN:
        return HciStubQueueCommandComplete(Stub, opcode, g_LeSuggestedDataLength,
                                           sizeof(g_LeSuggestedDataLength));

    case HCI_OP_READ_INQ_RSP_TX_POWER:
        return HciStubQueueCommandComplete(Stub, opcode, g_InqRspTxPower,
                                           sizeof(g_InqRspTxPower));

    case HCI_OP_READ_STORED_LINK_KEY:
        return HciStubQueueCommandComplete(Stub, opcode, g_StoredLinkKeys,
                                           sizeof(g_StoredLinkKeys));

    case HCI_OP_WRITE_STORED_LINK_KEY:
        return HciStubQueueCommandComplete(Stub, opcode, g_LinkKeysWritten,
                                           sizeof(g_LinkKeysWritten));

    case HCI_OP_DELETE_STORED_LINK_KEY:
        return HciStubQueueCommandComplete(Stub, opcode, g_LinkKeysDeleted,
                                           sizeof(g_LinkKeysDeleted));

    default:
        /* Status-only Command_Complete. Covers Reset, Set_Event_Mask, writes, and the long tail
         * of optional commands BTHPORT probes during bring-up. */
        return HciStubQueueCommandComplete(Stub, opcode, NULL, 0);
    }
}

BOOLEAN HciStubPopEvent(
    _Inout_ PHCI_STUB Stub,
    _Out_writes_bytes_to_(Capacity, *Written) UCHAR *Buffer,
    _In_ ULONG Capacity,
    _Out_ ULONG *Written)
{
    const HCI_EVENT_SLOT *slot;

    *Written = 0;
    if (Stub->Count == 0) {
        return FALSE;
    }

    slot = &Stub->Fifo[Stub->Head];
    if (slot->Length > Capacity) {
        /*
         * The interrupt endpoint's wMaxPacketSize is 16, so a long event legitimately needs
         * several transfers. The synthetic stub produces only short events; drop over-capacity
         * events and report failure rather than silently truncating.
         */
        Stub->Head = (Stub->Head + 1) % HCI_EVENT_FIFO_DEPTH;
        Stub->Count--;
        Stub->Dropped++;
        return FALSE;
    }

    RtlCopyMemory(Buffer, slot->Data, slot->Length);
    *Written = slot->Length;
    Stub->Head = (Stub->Head + 1) % HCI_EVENT_FIFO_DEPTH;
    Stub->Count--;
    return TRUE;
}

/*
 * -----------------------------------------------------------------------------
 * HCI_TRANSPORT binding for the synthetic stub controller
 * -----------------------------------------------------------------------------
 */

static unsigned char
HciStubTransportSubmitCommand(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length)
{
    PHCI_STUB stub = (PHCI_STUB)Transport->Context;
    if (stub == NULL) {
        return 0;
    }
    return (unsigned char)(HciStubSubmitCommand(stub, Packet, Length) ? 1 : 0);
}

static unsigned char
HciStubTransportSubmitAcl(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length)
{
    (void)Transport;
    (void)Packet;
    (void)Length;
    /* Accept and discard: no radio behind the stub */
    return 1;
}

static unsigned char
HciStubTransportSubmitSco(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length)
{
    (void)Transport;
    (void)Packet;
    (void)Length;
    /* Accept and discard: no radio behind the stub */
    return 1;
}

static unsigned char
HciStubTransportHasStream(
    const HCI_TRANSPORT *Transport,
    HCI_STREAM Stream)
{
    const HCI_STUB *stub = (const HCI_STUB *)Transport->Context;
    if (stub == NULL) {
        return 0;
    }
    if (Stream == HciStreamEvent) {
        return (unsigned char)(stub->Count != 0 ? 1 : 0);
    }
    /* HciStreamAcl and HciStreamSco are always empty (there is no radio) */
    return 0;
}

static unsigned char
HciStubTransportPopStream(
    HCI_TRANSPORT *Transport,
    HCI_STREAM Stream,
    unsigned char *Buffer,
    unsigned long Capacity,
    unsigned long *Written)
{
    PHCI_STUB stub = (PHCI_STUB)Transport->Context;

    if (Written != NULL) {
        *Written = 0;
    }
    if (stub == NULL || Stream != HciStreamEvent || Written == NULL) {
        return 0;
    }
    if (stub->Count == 0) {
        return 0;
    }
    /*
     * Contract: FALSE if packet does not fit Capacity, in which case
     * *Written is 0 and the packet is left queued. Never partially fills.
     */
    if (stub->Fifo[stub->Head].Length > Capacity) {
        return 0;
    }
    return (unsigned char)(HciStubPopEvent(stub, Buffer, Capacity, Written) ? 1 : 0);
}

static unsigned long
HciStubTransportLastEventLength(
    const HCI_TRANSPORT *Transport)
{
    const HCI_STUB *stub = (const HCI_STUB *)Transport->Context;
    ULONG last;

    if (stub == NULL || stub->Count == 0) {
        return 0;
    }
    last = (stub->Tail + HCI_EVENT_FIFO_DEPTH - 1) % HCI_EVENT_FIFO_DEPTH;
    return stub->Fifo[last].Length;
}

static void
HciStubTransportReset(
    HCI_TRANSPORT *Transport)
{
    PHCI_STUB stub = (PHCI_STUB)Transport->Context;
    if (stub != NULL) {
        HciStubInit(stub);
    }
}
static VOID
HciStubTransportFlushSco(HCI_TRANSPORT *Transport)
{
    (VOID)Transport;
}


static const HCI_TRANSPORT_OPS g_HciStubOps = {
    HciStubTransportSubmitCommand,
    HciStubTransportSubmitAcl,
    HciStubTransportSubmitSco,
    HciStubTransportHasStream,
    HciStubTransportPopStream,
    HciStubTransportLastEventLength,
    HciStubTransportReset,
    NULL,
    HciStubTransportFlushSco,
};

VOID
HciStubBindTransport(
    _Out_ HCI_TRANSPORT *Transport,
    _Inout_ HCI_STUB *Stub)
{
    Transport->Ops = &g_HciStubOps;
    Transport->Context = Stub;
    Transport->Backend = HCI_BACKEND_STUB;
}
