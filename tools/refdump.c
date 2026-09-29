/*
 * refdump.c - Emits baseline USB descriptor bytes and expected synthetic HCI responses.
 *
 * Output is recorded in reference/VIRTUAL-HCI-REFERENCE.txt for regression testing.
 *
 * Build/run: tools\refdump.cmd
 */

#include <windows.h>
#include <stdio.h>

#include "../src/include/usb_descriptors.h"
#include "../src/include/hci_stub.h"

static void Hex(const char *label, const UCHAR *p, ULONG len)
{
    printf("%s (%lu bytes)\n", label, len);
    for (ULONG i = 0; i < len; i++) {
        if (i % 16 == 0) { printf("  %04lX: ", i); }
        printf("%02X ", p[i]);
        if (i % 16 == 15 || i + 1 == len) { printf("\n"); }
    }
    printf("\n");
}

static const char *DescTypeName(UCHAR t)
{
    switch (t) {
    case 0x01: return "DEVICE";
    case 0x02: return "CONFIGURATION";
    case 0x03: return "STRING";
    case 0x04: return "INTERFACE";
    case 0x05: return "ENDPOINT";
    default:   return "?";
    }
}

static const char *XferName(UCHAR attr)
{
    switch (attr & 3) {
    case 0:  return "control";
    case 1:  return "isochronous";
    case 2:  return "bulk";
    default: return "interrupt";
    }
}

static void WalkConfig(void)
{
    const UCHAR *p = DeckBtConfigDescriptor;
    ULONG len = DeckBtConfigDescriptorSize;
    ULONG off = 0;

    printf("Configuration descriptor set, parsed\n");
    printf("  offset  len  type              detail\n");
    while (off < len) {
        UCHAR bLength = p[off];
        UCHAR bType   = p[off + 1];

        printf("  %5lu   %3u  %-16s  ", off, bLength, DescTypeName(bType));
        if (bType == 0x02) {
            printf("wTotalLength=%u bNumInterfaces=%u bConfigurationValue=%u "
                   "bmAttributes=0x%02X bMaxPower=%u (%u mA)\n",
                   (USHORT)(p[off + 2] | (p[off + 3] << 8)), p[off + 4], p[off + 5],
                   p[off + 7], p[off + 8], p[off + 8] * 2);
        } else if (bType == 0x04) {
            printf("bInterfaceNumber=%u bAlternateSetting=%u bNumEndpoints=%u "
                   "class=%02X/%02X/%02X\n",
                   p[off + 2], p[off + 3], p[off + 4], p[off + 5], p[off + 6], p[off + 7]);
        } else if (bType == 0x05) {
            printf("bEndpointAddress=0x%02X %-11s wMaxPacketSize=%u bInterval=%u\n",
                   p[off + 2], XferName(p[off + 3]),
                   (USHORT)(p[off + 4] | (p[off + 5] << 8)), p[off + 6]);
        } else {
            printf("\n");
        }
        if (bLength == 0) { break; }
        off += bLength;
    }
    printf("\n");
}

/* Opcodes worth freezing: everything the stub answers, plus a deliberate unknown. */
static const struct { USHORT op; const char *name; } g_Ops[] = {
    { HCI_OP_RESET,                       "HCI_Reset" },
    { HCI_OP_SET_EVENT_MASK,              "Set_Event_Mask" },
    { HCI_OP_SET_EVENT_MASK_PAGE_2,       "Set_Event_Mask_Page_2" },
    { HCI_OP_WRITE_LE_HOST_SUPPORT,       "Write_LE_Host_Support" },
    { HCI_OP_READ_LOCAL_VERSION,          "Read_Local_Version_Information" },
    { HCI_OP_READ_LOCAL_SUPPORTED_CMDS,   "Read_Local_Supported_Commands" },
    { HCI_OP_READ_LOCAL_FEATURES,         "Read_Local_Supported_Features" },
    { HCI_OP_READ_LOCAL_EXT_FEATURES,     "Read_Local_Extended_Features(page 0)" },
    { HCI_OP_READ_BUFFER_SIZE,            "Read_Buffer_Size" },
    { HCI_OP_READ_BD_ADDR,                "Read_BD_ADDR" },
    { HCI_OP_READ_LOCAL_NAME,             "Read_Local_Name" },
    { HCI_OP_READ_PAGE_TIMEOUT,           "Read_Page_Timeout" },
    { HCI_OP_READ_SCAN_ENABLE,            "Read_Scan_Enable" },
    { HCI_OP_READ_CLASS_OF_DEVICE,        "Read_Class_Of_Device" },
    { HCI_OP_READ_VOICE_SETTING,          "Read_Voice_Setting" },
    { HCI_OP_READ_INQ_RSP_TX_POWER,       "Read_Inquiry_Response_Transmit_Power_Level" },
    { HCI_OP_READ_LOCAL_SUPPORTED_CODECS, "Read_Local_Supported_Codecs" },
    { HCI_OP_READ_STORED_LINK_KEY,        "Read_Stored_Link_Key" },
    { HCI_OP_WRITE_STORED_LINK_KEY,       "Write_Stored_Link_Key" },
    { HCI_OP_DELETE_STORED_LINK_KEY,      "Delete_Stored_Link_Key" },
    { HCI_OP_LE_READ_BUFFER_SIZE,         "LE_Read_Buffer_Size" },
    { HCI_OP_LE_READ_LOCAL_FEATURES,      "LE_Read_Local_Supported_Features" },
    { HCI_OP_LE_READ_SUPPORTED_STATES,    "LE_Read_Supported_States" },
    { HCI_OP_LE_READ_ADV_TX_POWER,        "LE_Read_Advertising_Physical_Channel_Tx_Power" },
    { HCI_OP_LE_READ_ACCEPT_LIST_SIZE,    "LE_Read_Filter_Accept_List_Size" },
    { HCI_OP_LE_READ_MAX_DATA_LENGTH,     "LE_Read_Maximum_Data_Length" },
    { HCI_OP_LE_READ_SUGGESTED_DATA_LEN,  "LE_Read_Suggested_Default_Data_Length" },
    { HCI_OP_LE_READ_RESOLVING_LIST_SIZE, "LE_Read_Resolving_List_Size" },
    { 0x0C24,                             "Write_Class_Of_Device (status-only)" },
    { 0x0C52,                             "Write_Extended_Inquiry_Response (status-only)" },
    { 0x0FFF,                             "unknown opcode (default path)" },
};

int main(void)
{
    printf("=================================================================\n");
    printf(" DeckBtService - Virtual HCI Reference\n");
    printf(" Generated from the production sources: src/common/usb_descriptors.c\n");
    printf(" and src/common/hci_stub.c. Do not hand-edit.\n");
    printf("=================================================================\n\n");

    printf("--- IDENTITY ---\n");
    printf("  idVendor            0x%04X\n", DECKBT_VENDOR_ID);
    printf("  idProduct           0x%04X\n", DECKBT_PRODUCT_ID);
    printf("  class/subclass/prot %02X/%02X/%02X (wireless / RF / Bluetooth)\n",
           DECKBT_CLASS_WIRELESS, DECKBT_SUBCLASS_RF, DECKBT_PROTOCOL_BLUETOOTH);
    printf("  speed               high (required; see src/include/usb_descriptors.h)\n");
    printf("  endpoints           EP 0x%02X int IN (events), 0x%02X bulk OUT / 0x%02X bulk IN (ACL),\n",
           DECKBT_EP_EVENT_IN, DECKBT_EP_ACL_OUT, DECKBT_EP_ACL_IN);
    printf("                      0x%02X isoch OUT / 0x%02X isoch IN (SCO)\n\n",
           DECKBT_EP_SCO_OUT, DECKBT_EP_SCO_IN);

    printf("--- USB DESCRIPTORS ---\n\n");
    Hex("Device descriptor", DeckBtDeviceDescriptor, DeckBtDeviceDescriptorSize);
    Hex("Configuration descriptor set", DeckBtConfigDescriptor, DeckBtConfigDescriptorSize);
    WalkConfig();

    printf("SCO alternate-setting table (Bluetooth SIG USB transport layout)\n");
    for (UCHAR a = 0; a <= DECKBT_SCO_ALT_MAX; a++) {
        printf("  alt %u: %3u bytes/frame, descriptor offset %lu%s\n",
               a, DeckBtScoAltPacketSize[a], DeckBtScoAltOffset(a),
               a == 0 ? "  (zero isochronous bandwidth)" : "");
    }
    printf("\n");

    for (UCHAR i = 0; i < DECKBT_ISTRING_COUNT; i++) {
        ULONG slen = 0;
        const UCHAR *s = DeckBtGetStringDescriptor(i, &slen);
        char label[64];
        sprintf_s(label, sizeof(label), "String descriptor index %u", i);
        if (s) { Hex(label, s, slen); }
    }

    printf("--- HCI COMMAND / EVENT PAIRS ---\n");
    printf("Command packets are what BTHUSB writes to EP0 (opcode LE16, plen, params).\n");
    printf("Event packets are what the stub returns on EP 0x81 (evt code, plen, payload).\n\n");

    for (size_t i = 0; i < sizeof(g_Ops) / sizeof(g_Ops[0]); i++) {
        HCI_STUB stub;
        UCHAR cmd[8];
        UCHAR evt[HCI_MAX_EVENT_SIZE];
        ULONG written = 0;
        ULONG cmdLen;

        HciStubInit(&stub);
        cmd[0] = (UCHAR)(g_Ops[i].op & 0xFF);
        cmd[1] = (UCHAR)(g_Ops[i].op >> 8);
        cmd[2] = 0;
        cmdLen = 3;

        printf("%-46s opcode 0x%04X\n", g_Ops[i].name, g_Ops[i].op);
        printf("  CMD  ");
        for (ULONG k = 0; k < cmdLen; k++) { printf("%02X ", cmd[k]); }
        printf("\n");

        if (!HciStubSubmitCommand(&stub, cmd, cmdLen)) {
            printf("  EVT  <rejected>\n\n");
            continue;
        }
        if (!HciStubPopEvent(&stub, evt, sizeof(evt), &written)) {
            printf("  EVT  <none>\n\n");
            continue;
        }
        printf("  EVT  ");
        for (ULONG k = 0; k < written; k++) {
            printf("%02X ", evt[k]);
            if (k % 16 == 15 && k + 1 < written) { printf("\n       "); }
        }
        printf("\n       (%lu bytes)\n\n", written);
    }

    printf("--- FIFO / LIMITS ---\n");
    printf("  event FIFO depth        %u slots\n", HCI_EVENT_FIFO_DEPTH);
    printf("  max event size          %u bytes\n", HCI_MAX_EVENT_SIZE);
    printf("\nEnd of Virtual HCI Reference.\n");
    return 0;
}
