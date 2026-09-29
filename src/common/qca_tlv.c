/*
 * qca_tlv.c - Qualcomm QCA2066 TLV firmware framing and segmentation.
 *
 * Free of OS or transport dependencies: implements firmware TLV parsing,
 * segmentation, and NVM file selection.
 */

#include "../include/qca_protocol.h"

static ULONG Rd32(const UCHAR *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

static USHORT Rd16(const UCHAR *p)
{
    return (USHORT)((USHORT)p[0] | ((USHORT)p[1] << 8));
}

BOOLEAN
QcaParseTlv(
    _In_reads_bytes_(Size) const UCHAR *Data,
    _In_ ULONG Size,
    _Out_ PQCA_TLV_INFO Info)
{
    ULONG typeLen;

    RtlZeroMemory(Info, sizeof(*Info));

    /* The 4-byte TLV header encodes payload length in the upper 24 bits and type in the low byte. */
    if (Size < 4) {
        return FALSE;
    }

    typeLen        = Rd32(Data);
    Info->Type     = (UCHAR)(typeLen & 0xFFu);
    Info->Length   = typeLen >> 8;
    Info->FileSize = Size;

    /* The declared length covers everything after the 4-byte header. */
    if (Info->Length != Size - 4u) {
        return FALSE;
    }

    if (Info->Type == QCA_TLV_TYPE_PATCH) {
        /*
         * Patch TLV metadata layout per upstream btqca.h:
         * https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/btqca.h?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137
         * The 24 bytes following the 4-byte TLV header contain:
         * total_size (4B), data_length (4B), format_version (1B), signature (1B),
         * download_mode (1B), reserved1 (1B), product_id (2B), rom_build (2B),
         * patch_version (2B), reserved2 (2B), and entry (4B).
         */
        if (Size < 4u + 24u) {
            return FALSE;
        }
        Info->TotalSize    = Rd32(Data + 4);
        Info->DataLength   = Rd32(Data + 8);
        Info->DownloadMode = Data[14];
        Info->ProductId    = Rd16(Data + 16);
        Info->RomBuild     = Rd16(Data + 18);
        Info->PatchVersion = Rd16(Data + 20);

        if (Info->DownloadMode > QCA_SKIP_EVT_VSE_CC) {
            return FALSE;
        }
    } else if (Info->Type != QCA_TLV_TYPE_NVM && Info->Type != QCA_ELF_TYPE_PATCH) {
        return FALSE;
    }

    return TRUE;
}

ULONG
QcaTlvSegmentCount(_In_ ULONG FileSize)
{
    if (FileSize == 0) {
        return 0;
    }
    /* The whole file, header included, is streamed per upstream btqca.c. */
    return (FileSize + QCA_MAX_SIZE_PER_TLV_SEGMENT - 1u) / QCA_MAX_SIZE_PER_TLV_SEGMENT;
}

BOOLEAN
QcaFindNvmHciBaudOffset(
    _In_reads_bytes_(Size) const UCHAR *Data,
    _In_ ULONG Size,
    _Out_ ULONG *Offset)
{
    QCA_TLV_INFO info;
    ULONG idx = 0;

    *Offset = 0;
    if (Data == NULL || !QcaParseTlv(Data, Size, &info) || info.Type != QCA_TLV_TYPE_NVM) {
        return FALSE;
    }
    /* Tags are packed back to back after the 4-byte TLV header (btqca.c qca_tlv_check_data). */
    while (idx + QCA_NVM_TAG_HDR_SIZE <= info.Length) {
        const UCHAR *tag = Data + 4u + idx;
        USHORT tagId = Rd16(tag);
        ULONG tagLen = Rd16(tag + 2);

        if (info.Length - idx - QCA_NVM_TAG_HDR_SIZE < tagLen) {
            return FALSE;
        }
        if (tagId == EDL_TAG_ID_HCI) {
            if (tagLen < 3u) {
                return FALSE;
            }
            *Offset = 4u + idx + QCA_NVM_TAG_HDR_SIZE + 1u;
            return TRUE;
        }
        idx += QCA_NVM_TAG_HDR_SIZE + tagLen;
    }
    return FALSE;
}

ULONG
QcaBuildTlvSegmentCommand(
    _In_reads_bytes_(FileSize) const UCHAR *Data,
    _In_ ULONG FileSize,
    _In_ ULONG Index,
    _In_ UCHAR DownloadMode,
    _Out_writes_bytes_to_(OutCapacity, return) UCHAR *Out,
    _In_ ULONG OutCapacity,
    _Out_ BOOLEAN *AckExpected)
{
    ULONG offset;
    ULONG segSize;
    ULONG total;

    *AckExpected = TRUE;

    if (Data == NULL || FileSize == 0 || Index >= QcaTlvSegmentCount(FileSize)) {
        return 0;
    }

    offset  = Index * QCA_MAX_SIZE_PER_TLV_SEGMENT;
    segSize = FileSize - offset;
    if (segSize > QCA_MAX_SIZE_PER_TLV_SEGMENT) {
        segSize = QCA_MAX_SIZE_PER_TLV_SEGMENT;
    }

    /* H4 + opcode(2) + plen(1) + sub(1) + segsize(1) + payload */
    total = 6u + segSize;
    if (OutCapacity < total) {
        return 0;
    }

    Out[0] = H4_PKT_COMMAND;
    Out[1] = (UCHAR)(EDL_PATCH_CMD_OPCODE & 0xFFu);
    Out[2] = (UCHAR)((EDL_PATCH_CMD_OPCODE >> 8) & 0xFFu);
    Out[3] = (UCHAR)(segSize + 2u);          /* HCI parameter length */
    Out[4] = EDL_PATCH_TLV_REQ_CMD;
    Out[5] = (UCHAR)segSize;
    RtlCopyMemory(&Out[6], Data + offset, segSize);

    /*
     * Per upstream btqca.c (qca_tlv_send_segment):
     * The last segment and any short segment always require an acknowledgement,
     * regardless of the declared download mode.
     */
    if (segSize < QCA_MAX_SIZE_PER_TLV_SEGMENT || offset + segSize >= FileSize) {
        *AckExpected = TRUE;
    }
    /*
     * Per upstream btqca.c:
     * Only full-size intermediate segments skip the acknowledgement, and only
     * when the download mode is QCA_SKIP_EVT_VSE_CC or QCA_SKIP_EVT_VSE.
     */
    else if (DownloadMode == QCA_SKIP_EVT_VSE_CC || DownloadMode == QCA_SKIP_EVT_VSE) {
        *AckExpected = FALSE;
    } else {
        *AckExpected = TRUE;
    }

    return total;
}

ULONG
QcaBuildBaudRateCommand(
    _In_ UCHAR BaudRateIndex,
    _Out_writes_bytes_to_(OutCapacity, return) UCHAR *Out,
    _In_ ULONG OutCapacity)
{
    if (OutCapacity < 5u || BaudRateIndex > QCA_BAUDRATE_3200000) {
        return 0;   /* Unsupported baud rate index; max index is QCA_BAUDRATE_3200000. */
    }

    Out[0] = H4_PKT_COMMAND;
    Out[1] = (UCHAR)(QCA_BAUDRATE_CMD_OPCODE & 0xFFu);
    Out[2] = (UCHAR)((QCA_BAUDRATE_CMD_OPCODE >> 8) & 0xFFu);
    Out[3] = 1u;
    Out[4] = BaudRateIndex;
    return 5u;
}

ULONG
QcaBuildEdlCommand(
    _In_ UCHAR SubCommand,
    _Out_writes_bytes_to_(OutCapacity, return) UCHAR *Out,
    _In_ ULONG OutCapacity)
{
    if (OutCapacity < 5u) {
        return 0;
    }

    Out[0] = H4_PKT_COMMAND;
    Out[1] = (UCHAR)(EDL_PATCH_CMD_OPCODE & 0xFFu);
    Out[2] = (UCHAR)((EDL_PATCH_CMD_OPCODE >> 8) & 0xFFu);
    Out[3] = 1u;
    Out[4] = SubCommand;
    return 5u;
}

BOOLEAN
QcaBaudRateToIndex(_In_ ULONG BitsPerSecond, _Out_ UCHAR *Index)
{
    switch (BitsPerSecond) {
    case 115200u:  *Index = QCA_BAUDRATE_115200;  return TRUE;
    case 921600u:  *Index = QCA_BAUDRATE_921600;  return TRUE;
    case 1000000u: *Index = QCA_BAUDRATE_1000000; return TRUE;
    case 2000000u: *Index = QCA_BAUDRATE_2000000; return TRUE;
    case 3000000u: *Index = QCA_BAUDRATE_3000000; return TRUE;
    case 3200000u: *Index = QCA_BAUDRATE_3200000; return TRUE;
    default:       *Index = 0;                    return FALSE;
    }
}

static const char g_hexDigitsLower[] = "0123456789abcdef";

/*
 * Formats `val` as lowercase hex with minimum 2 digits (equivalent to %02x).
 * Does not truncate: a 3-digit or 4-digit value emits 3 or 4 hex characters.
 * Returns number of characters written to buf (not null-terminated).
 */
static ULONG
FormatHexMin2(
    _In_ USHORT Val,
    _Out_writes_(Capacity) char *Buf,
    _In_ ULONG Capacity)
{
    ULONG digits;

    if (Val >= 0x1000u) {
        digits = 4;
    } else if (Val >= 0x100u) {
        digits = 3;
    } else {
        digits = 2;
    }

    if (Capacity < digits) {
        return 0;
    }

    if (digits == 4) {
        Buf[0] = g_hexDigitsLower[(Val >> 12) & 0x0Fu];
        Buf[1] = g_hexDigitsLower[(Val >> 8) & 0x0Fu];
        Buf[2] = g_hexDigitsLower[(Val >> 4) & 0x0Fu];
        Buf[3] = g_hexDigitsLower[Val & 0x0Fu];
    } else if (digits == 3) {
        Buf[0] = g_hexDigitsLower[(Val >> 8) & 0x0Fu];
        Buf[1] = g_hexDigitsLower[(Val >> 4) & 0x0Fu];
        Buf[2] = g_hexDigitsLower[Val & 0x0Fu];
    } else {
        Buf[0] = g_hexDigitsLower[(Val >> 4) & 0x0Fu];
        Buf[1] = g_hexDigitsLower[Val & 0x0Fu];
    }

    return digits;
}

BOOLEAN
QcaParseEdlResponse(
    _In_reads_bytes_(Length) const UCHAR *Packet,
    _In_ ULONG Length,
    _In_ UCHAR ResponseType,
    _Out_ const UCHAR **Data,
    _Out_ ULONG *DataLength)
{
    ULONG offset;

    if (Packet == NULL || Data == NULL || DataLength == NULL ||
        Length < 5u || Packet[0] != H4_PKT_EVENT ||
        Length != 3u + (ULONG)Packet[2]) {
        return FALSE;
    }
    if (Packet[1] == HCI_EV_VENDOR) {
        offset = 3u;
    } else if (Packet[1] == 0x0Eu && Length >= 8u &&
               Rd16(Packet + 4) == EDL_PATCH_CMD_OPCODE) {
        /* Skip ncmd/opcode; the return parameters start with cresp/rtype. */
        offset = 6u;
    } else {
        return FALSE;
    }
    if (Packet[offset] != EDL_CMD_REQ_RES_EVT || Packet[offset + 1] != ResponseType) {
        return FALSE;
    }
    *Data = Packet + offset + 2u;
    *DataLength = Length - offset - 2u;
    return TRUE;
}

/*
 * QcaParseVersionEvent
 *
 * Validates the event envelope before decoding the fixed-size version structure.
 * Upstream reference: qca_read_soc_version() in drivers/bluetooth/btqca.c:
 * https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/btqca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137
 * For QCA2066 Command Complete events, skips one extra byte before the version payload.
 * Supports both the vendor event envelope and the Command Complete envelope.
 */
BOOLEAN
QcaParseVersionEvent(
    _In_reads_bytes_(Length) const UCHAR *Packet,
    _In_ ULONG Length,
    _Out_ PQCA_SOC_VERSION Out)
{
    ULONG productId;
    USHORT patchVersion;
    USHORT romVersionField;
    ULONG socId;
    ULONG socVersion;
    UCHAR romVersion;
    const UCHAR *data;
    ULONG dataLength;

    if (Out == NULL ||
        !QcaParseEdlResponse(Packet, Length, EDL_PATCH_VER_RES_EVT, &data, &dataLength)) {
        return FALSE;
    }
    if (Packet[1] == 0x0Eu) {
        if (dataLength != 13u) {
            return FALSE;
        }
        data++;
    } else if (dataLength != 12u) {
        return FALSE;
    }

    productId       = Rd32(data);
    patchVersion    = Rd16(data + 4);
    romVersionField = Rd16(data + 6);
    socId           = Rd32(data + 8);
    if (socId == 0 || romVersionField == 0) {
        return FALSE;   /* Reject uninitialised identity fields. */
    }

    /* Pack soc_ver per btqca.h get_soc_ver(). */
    socVersion = (socId << 16) | (ULONG)romVersionField;

    /* Extract rom_ver for QCA2066 per btqca.c qca_uart_setup(). */
    romVersion = (UCHAR)(((socVersion & 0x00000F00u) >> 4) | (socVersion & 0x0000000Fu));

    Out->ProductId       = productId;
    Out->PatchVersion    = patchVersion;
    Out->RomVersionField = romVersionField;
    Out->SocId           = socId;
    Out->SocVersion      = socVersion;
    Out->RomVersion      = romVersion;

    return TRUE;
}

/*
 * QcaBuildBoardIdCommand
 *
 * Builds EDL_GET_BID_REQ_CMD (0xFC00 / 0x23).
 * Upstream reference: qca_read_fw_board_id() in drivers/bluetooth/btqca.c.
 * Returns bytes written (5) or 0 on error / undersized buffer.
 */
ULONG
QcaBuildBoardIdCommand(
    _Out_writes_bytes_to_(Capacity, return) UCHAR *Out,
    _In_ ULONG Capacity)
{
    if (Out == NULL || Capacity < 5u) {
        return 0;
    }

    return QcaBuildEdlCommand(EDL_GET_BID_REQ_CMD, Out, Capacity);
}

/*
 * QcaParseBoardIdEvent
 *
 * Parses the board-ID response per upstream qca_read_fw_board_id().
 * Both EDL envelopes carry three data bytes: an ignored byte followed by the
 * big-endian board ID. Validates before modifying BoardId.
 */
BOOLEAN
QcaParseBoardIdEvent(
    _In_reads_bytes_(Length) const UCHAR *Packet,
    _In_ ULONG Length,
    _Out_ USHORT *BoardId)
{
    const UCHAR *data;
    ULONG dataLength;

    if (BoardId == NULL ||
        !QcaParseEdlResponse(Packet, Length, EDL_GET_BID_REQ_CMD, &data, &dataLength) ||
        dataLength < 3u) {
        return FALSE;
    }

    *BoardId = (USHORT)(((USHORT)data[1] << 8) | (USHORT)data[2]);
    return TRUE;
}

/*
 * QcaBuildNvmFileName
 *
 * Builds the board-specific NVM filename per QCA2066 rules.
 * Upstream reference: qca_get_nvm_name_by_board() in drivers/bluetooth/btqca.c:
 * https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/btqca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137
 * Fails closed: validates that Out is non-NULL and Capacity is sufficient to hold
 * the complete null-terminated filename. Returns FALSE without writing if Capacity
 * is insufficient.
 */
BOOLEAN
QcaBuildNvmFileName(
    _In_ ULONG SocId,
    _In_ UCHAR RomVersion,
    _In_ USHORT BoardId,
    _Out_writes_z_(Capacity) char *Out,
    _In_ ULONG Capacity)
{
    char romVerStr[2];
    char bidStr[4];
    ULONG romVerDigits;
    ULONG bidDigits = 0;
    BOOLEAN isGf;
    BOOLEAN hasBoard;
    ULONG needed;
    ULONG pos = 0;

    if (Out == NULL || Capacity == 0) {
        return FALSE;
    }

    /* Check GlobalFoundries foundry variant per upstream btqca.c. */
    isGf = ((SocId & QCA_HSP_GF_SOC_MASK) == QCA_HSP_GF_SOC_ID);

    /* Board ID 0x0000 or 0xFFFF indicates no specific board; use generic .bin */
    hasBoard = (BoardId != 0x0000u && BoardId != 0xFFFFu);

    romVerDigits = FormatHexMin2((USHORT)RomVersion, romVerStr, sizeof(romVerStr));
    if (romVerDigits != 2u) {
        return FALSE;
    }

    if (hasBoard) {
        bidDigits = FormatHexMin2(BoardId, bidStr, sizeof(bidStr));
        if (bidDigits < 2u) {
            return FALSE;
        }
    }

    /*
     * Length calculation:
     *   stem "hpnv":       4 chars
     *   romVerStr:         2 chars
     *   variant "g":       1 char if isGf, else 0
     *   suffix:
     *     if !hasBoard:    ".bin" (4 chars)
     *     if hasBoard:     "." (1 char) + bidDigits
     *   null terminator:   1 byte
     */
    needed = 4u + 2u + (isGf ? 1u : 0u) + (hasBoard ? (1u + bidDigits) : 4u) + 1u;
    if (Capacity < needed) {
        return FALSE;
    }

    /* Build string safely without CRT */
    Out[pos++] = 'h';
    Out[pos++] = 'p';
    Out[pos++] = 'n';
    Out[pos++] = 'v';

    Out[pos++] = romVerStr[0];
    Out[pos++] = romVerStr[1];

    if (isGf) {
        Out[pos++] = 'g';
    }

    if (!hasBoard) {
        Out[pos++] = '.';
        Out[pos++] = 'b';
        Out[pos++] = 'i';
        Out[pos++] = 'n';
    } else {
        ULONG i;
        Out[pos++] = '.';
        for (i = 0; i < bidDigits; i++) {
            Out[pos++] = bidStr[i];
        }
    }

    Out[pos] = '\0';
    return TRUE;
}

/*
 * QcaBuildAltNvmFileName
 *
 * Builds the fallback .bin NVM filename for a board-specific NVM name.
 * Upstream reference: qca_get_alt_nvm_file() in drivers/bluetooth/btqca.c.
 * Fails closed: returns FALSE if Name is NULL or Out is NULL, if Name lacks an extension,
 * if Name already ends in ".bin", or if Capacity is insufficient to store the stem + ".bin"
 * + null terminator. Does not write partial output.
 */
BOOLEAN
QcaBuildAltNvmFileName(
    _In_z_ const char *Name,
    _Out_writes_z_(Capacity) char *Out,
    _In_ ULONG Capacity)
{
    ULONG len = 0;
    ULONG dotIndex = 0;
    ULONG i;
    ULONG needed;

    if (Name == NULL || Out == NULL || Capacity == 0) {
        return FALSE;
    }

    while (Name[len] != '\0') {
        len++;
    }

    /* Find the last '.' */
    dotIndex = len;
    for (i = len; i > 0; i--) {
        if (Name[i - 1] == '.') {
            dotIndex = i - 1;
            break;
        }
    }

    /* Must have a non-empty stem and a non-empty extension */
    if (dotIndex == 0 || dotIndex >= len - 1) {
        return FALSE;
    }

    /* If the extension is already ".bin" (case-insensitive), return FALSE to skip retry */
    if ((len - dotIndex == 4u) &&
        (Name[dotIndex + 1] == 'b' || Name[dotIndex + 1] == 'B') &&
        (Name[dotIndex + 2] == 'i' || Name[dotIndex + 2] == 'I') &&
        (Name[dotIndex + 3] == 'n' || Name[dotIndex + 3] == 'N')) {
        return FALSE;
    }

    /* Stem is Name[0..dotIndex-1]. New name is stem + ".bin" + '\0' */
    needed = dotIndex + 4u + 1u;
    if (Capacity < needed) {
        return FALSE;
    }

    for (i = 0; i < dotIndex; i++) {
        Out[i] = Name[i];
    }
    Out[dotIndex]     = '.';
    Out[dotIndex + 1] = 'b';
    Out[dotIndex + 2] = 'i';
    Out[dotIndex + 3] = 'n';
    Out[dotIndex + 4] = '\0';

    return TRUE;
}
