/*
 * qca_protocol.h - Qualcomm QCA2066 UART bring-up protocol definitions.
 *
 * Protocol constants and operational opcodes are derived from the upstream Linux kernel Bluetooth
 * drivers (drivers/bluetooth/btqca.h, btqca.c, and hci_qca.c), accessible at:
 *   https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/btqca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137
 * These functional values are utilized solely for hardware protocol interoperability.
 */

#pragma once

#include <windows.h>

/* ---------------------------------------------------------------- H4 framing (UART only)
 * On UART the packet type is a single leading byte. On the USB transport it does not exist -
 * endpoints separate the streams - so this framing belongs to the UART backend alone.
 */
#define H4_PKT_COMMAND   0x01u
#define H4_PKT_ACL       0x02u
#define H4_PKT_SCO       0x03u
#define H4_PKT_EVENT     0x04u

/*
 * In-band sleep (IBS) single bytes, sent between H4 packets (upstream hci_qca.c: QCA_IBS_*).
 * SLEEP_IND: sender's transmit side goes idle. WAKE_IND: sender wants to transmit and waits for
 * WAKE_ACK before it does (hci_qca.c device_want_to_wakeup / device_woke_up).
 */
#define QCA_IBS_SLEEP_IND 0xFEu
#define QCA_IBS_WAKE_IND  0xFDu
#define QCA_IBS_WAKE_ACK  0xFCu

/* ---------------------------------------------------------------- vendor opcodes (btqca.h) */
#define EDL_PATCH_CMD_OPCODE        0xFC00u
#define EDL_NVM_ACCESS_OPCODE       0xFC0Bu
#define EDL_WRITE_BD_ADDR_OPCODE    0xFC14u
#define QCA_PRE_SHUTDOWN_CMD        0xFC08u
#define QCA_DISABLE_LOGGING         0xFC17u
#define QCA_DISABLE_LOGGING_SUB_OP  0x14u     /* btqca.h QCA_DISABLE_LOGGING_SUB_OP */
#define QCA_BAUDRATE_CMD_OPCODE     0xFC48u   /* hci_qca.c: { 0x01, 0x48, 0xFC, 0x01, <rate> } */

/* Sub-commands carried as the first parameter byte of EDL_PATCH_CMD_OPCODE. */
#define EDL_PATCH_VER_REQ_CMD       0x19u
#define EDL_PATCH_TLV_REQ_CMD       0x1Eu
#define EDL_GET_BUILD_INFO_CMD      0x20u
#define EDL_GET_BID_REQ_CMD         0x23u
#define EDL_PATCH_CONFIG_CMD        0x28u
#define EDL_PATCH_RESET_SOC_CMD     0x05u     /* hci_qca.c edl_reset_soc_cmd */

/* Vendor event types (btqca.h). HCI_EV_VENDOR is 0xFF. */
#define HCI_EV_VENDOR               0xFFu
#define EDL_CMD_REQ_RES_EVT         0x00u
#define EDL_PATCH_VER_RES_EVT       0x19u
#define EDL_TVL_DNLD_RES_EVT        0x04u
#define EDL_SET_BAUDRATE_RSP_EVT    0x92u
#define EDL_NVM_ACCESS_CODE_EVT     0x0Bu

/* Standard HCI values the QCA backend inspects in steady state (Core spec Vol 4 Part E). */
#define HCI_EV_COMMAND_COMPLETE         0x0Eu
#define HCI_OP_WRITE_LE_HOST_SUPPORTED  0x0C6Du
#define HCI_ERR_UNSUPPORTED_FEATURE     0x11u   /* Unsupported Feature or Parameter Value */
#define HCI_ERR_REMOTE_POWER_OFF        0x15u   /* Remote Device Terminated Connection due to Power Off */
#define HCI_EV_LE_META                  0x3Eu
#define HCI_LE_SUBEV_ADV_REPORT         0x02u
#define HCI_LE_SUBEV_DIRECT_ADV_REPORT  0x0Bu
#define HCI_LE_SUBEV_EXT_ADV_REPORT     0x0Du
#define HCI_EV_SYNC_CONN_COMPLETE       0x2Cu
#define HCI_EV_SYNC_CONN_CHANGED        0x2Du


/* ---------------------------------------------------------------- foundry & board constants (btqca.h) */
#define QCA_HSP_GF_SOC_ID           0x1200u
#define QCA_HSP_GF_SOC_MASK         0x0000FF00u
/* ---------------------------------------------------------------- TLV */
#define QCA_MAX_SIZE_PER_TLV_SEGMENT 243u     /* btqca.h MAX_SIZE_PER_TLV_SEGMENT */

#define QCA_TLV_TYPE_PATCH  1u
#define QCA_TLV_TYPE_NVM    2u
#define QCA_ELF_TYPE_PATCH  3u

/* btqca.h qca_tlv_dnld_mode: whether the chip acks intermediate segments. */
#define QCA_SKIP_EVT_NONE       0u
#define QCA_SKIP_EVT_VSE        1u
#define QCA_SKIP_EVT_CC         2u
#define QCA_SKIP_EVT_VSE_CC     3u

/* EDL_TAG_ID_HCI and 12-byte NVM tag header (tag_id, tag_len, reserve1, reserve2) per btqca.h */
#define EDL_TAG_ID_HCI          17u
#define QCA_NVM_TAG_HDR_SIZE    12u

/* ---------------------------------------------------------------- baud rates
 * btqca.h enum qca_baudrate. The wire value is the ENUM INDEX, not the bit rate.
 */
#define QCA_BAUDRATE_115200     0u
#define QCA_BAUDRATE_921600     10u
#define QCA_BAUDRATE_1000000    11u
#define QCA_BAUDRATE_2000000    13u
#define QCA_BAUDRATE_3000000    14u
#define QCA_BAUDRATE_3200000    17u   /* qca_set_baudrate rejects anything above this */

#define QCA_INIT_BAUD_RATE      115200u
#define QCA_OPER_BAUD_RATE      3000000u

/* ---------------------------------------------------------------- parsed TLV header */
typedef struct _QCA_TLV_INFO {
    UCHAR  Type;        /* QCA_TLV_TYPE_*                              */
    ULONG  Length;      /* declared payload length (type_len >> 8)     */
    ULONG  FileSize;    /* actual file size, for cross-checking        */
    /* Only meaningful when Type == QCA_TLV_TYPE_PATCH. */
    ULONG  TotalSize;
    ULONG  DataLength;
    UCHAR  DownloadMode;  /* QCA_SKIP_EVT_* declared by the patch file */
    USHORT ProductId;
    USHORT RomBuild;
    USHORT PatchVersion;
} QCA_TLV_INFO, *PQCA_TLV_INFO;

/* ---------------------------------------------------------------- parsed SOC version (btqca.h) */
typedef struct _QCA_SOC_VERSION {
    ULONG  ProductId;        /* le32 product_id            */
    USHORT PatchVersion;     /* le16 patch_ver             */
    USHORT RomVersionField;  /* le16 rom_ver, as received  */
    ULONG  SocId;            /* le32 soc_id                */
    ULONG  SocVersion;       /* (SocId << 16) | RomVersionField */
    UCHAR  RomVersion;       /* ((SocVersion & 0xf00) >> 4) | (SocVersion & 0xf) */
} QCA_SOC_VERSION, *PQCA_SOC_VERSION;

/*
 * Parses and validates the 4-byte tlv_type_hdr (and tlv_type_patch when present).
 * Returns FALSE if the buffer is too small or the declared length disagrees with FileSize.
 */
BOOLEAN QcaParseTlv(
    _In_reads_bytes_(Size) const UCHAR *Data,
    _In_ ULONG Size,
    _Out_ PQCA_TLV_INFO Info);

/* Number of 243-byte segments the whole file (header included) is split into. */
ULONG QcaTlvSegmentCount(_In_ ULONG FileSize);

/*
 * Builds one download command for segment `Index`:
 *     [0]      H4_PKT_COMMAND
 *     [1..2]   EDL_PATCH_CMD_OPCODE, little endian
 *     [3]      parameter length = segment size + 2
 *     [4]      EDL_PATCH_TLV_REQ_CMD
 *     [5]      segment size
 *     [6..]    segment bytes
 * Returns the number of bytes written, or 0 on error (bad index or short output buffer).
 * `*AckExpected` reports whether this segment is acknowledged: the last segment and any
 * short segment always are, regardless of the file's declared download mode
 * (btqca.c: "The last segment is always acked regardless download mode").
 */
ULONG QcaBuildTlvSegmentCommand(
    _In_reads_bytes_(FileSize) const UCHAR *Data,
    _In_ ULONG FileSize,
    _In_ ULONG Index,
    _In_ UCHAR DownloadMode,
    _Out_writes_bytes_to_(OutCapacity, return) UCHAR *Out,
    _In_ ULONG OutCapacity,
    _Out_ BOOLEAN *AckExpected);

/*
 * Locates the UART baud-index byte of an NVM image: data[1] of tag EDL_TAG_ID_HCI (btqca.c
 * qca_tlv_check_data). The shipped hpnv21* files carry index 17 (3.2 Mbaud); the initialization
 * sequence patches that byte to the operating baud index before download.
 * Walks the type-2 tag list with bounds checks; FALSE if the image is malformed or the tag is
 * missing or shorter than 3 bytes. *Offset is a byte offset into the whole file.
 */
BOOLEAN QcaFindNvmHciBaudOffset(
    _In_reads_bytes_(Size) const UCHAR *Data,
    _In_ ULONG Size,
    _Out_ ULONG *Offset);

/* { H4, 0x48, 0xFC, 0x01, BaudRateIndex }. Returns bytes written (5) or 0 if rejected. */
ULONG QcaBuildBaudRateCommand(_In_ UCHAR BaudRateIndex,
                              _Out_writes_bytes_to_(OutCapacity, return) UCHAR *Out,
                              _In_ ULONG OutCapacity);

/* { H4, 0x00, 0xFC, 0x01, sub }. Used for the version request and the SOC reset. */
ULONG QcaBuildEdlCommand(_In_ UCHAR SubCommand,
                         _Out_writes_bytes_to_(OutCapacity, return) UCHAR *Out,
                         _In_ ULONG OutCapacity);

/* Maps a bit rate to the enum index the chip expects. Returns FALSE if unsupported. */
BOOLEAN QcaBaudRateToIndex(_In_ ULONG BitsPerSecond, _Out_ UCHAR *Index);

/*
 * Validates one complete H4 EDL response: a vendor event, or a Command Complete for
 * 0xFC00 (QCA2066). Requires cresp == 0 and the expected response selector.
 * Data borrows the packet storage after cresp/rtype; outputs are unchanged on failure.
 */
BOOLEAN QcaParseEdlResponse(
    _In_reads_bytes_(Length) const UCHAR *Packet,
    _In_ ULONG Length,
    _In_ UCHAR ResponseType,
    _Out_ const UCHAR **Data,
    _Out_ ULONG *DataLength);

/*
 * Parses a version response (0xFC00 / 0x19) in either EDL envelope.
 * Upstream: drivers/bluetooth/btqca.c: qca_read_soc_version().
 * QCA2066 Command Complete carries one extra byte before the 12-byte version.
 * Rejects malformed headers, status, selector, or lengths without modifying Out.
 */
BOOLEAN
QcaParseVersionEvent(
    _In_reads_bytes_(Length) const UCHAR *Packet,
    _In_ ULONG Length,
    _Out_ PQCA_SOC_VERSION Out);

/*
 * Builds the board-ID query command (0xFC00 / 0x23).
 * Upstream: drivers/bluetooth/btqca.c: qca_read_fw_board_id().
 * Returns bytes written (5) or 0 on error / undersized buffer.
 */
ULONG
QcaBuildBoardIdCommand(
    _Out_writes_bytes_to_(Capacity, return) UCHAR *Out,
    _In_ ULONG Capacity);

/*
 * Parses the board-ID response (0xFC00 / 0x23) in either EDL envelope.
 * Upstream: drivers/bluetooth/btqca.c: qca_read_fw_board_id().
 * Rejects malformed headers, status, selector, or lengths without modifying BoardId.
 */
BOOLEAN
QcaParseBoardIdEvent(
    _In_reads_bytes_(Length) const UCHAR *Packet,
    _In_ ULONG Length,
    _Out_ USHORT *BoardId);

/*
 * Builds the board-specific NVM filename per QCA2066 rules.
 * Upstream: drivers/bluetooth/btqca.c: qca_get_nvm_name_by_board().
 * Fails closed (returns FALSE) if Capacity is insufficient instead of truncating.
 */
BOOLEAN
QcaBuildNvmFileName(
    _In_ ULONG SocId,
    _In_ UCHAR RomVersion,
    _In_ USHORT BoardId,
    _Out_writes_z_(Capacity) char *Out,
    _In_ ULONG Capacity);

/*
 * Builds the fallback .bin NVM filename for a board-specific NVM name.
 * Upstream: drivers/bluetooth/btqca.c: qca_get_alt_nvm_file().
 * Returns FALSE if Name already ends in .bin or if Capacity is insufficient.
 */
BOOLEAN
QcaBuildAltNvmFileName(
    _In_z_ const char *Name,
    _Out_writes_z_(Capacity) char *Out,
    _In_ ULONG Capacity);
