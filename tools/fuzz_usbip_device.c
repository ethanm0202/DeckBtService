/*
 * fuzz_usbip_device.c - libFuzzer target for the USB/IP command parser and device model.
 *
 * deckbt-usbip.exe runs as LocalSystem and parses every byte that arrives on its loopback socket,
 * so src/service/usbip_device.c is the privilege boundary. The input is a stream of USB/IP
 * commands (48-byte headers and their payloads), fed to UsbipDeviceHandle exactly as the session
 * thread does, with pacing ticks in between; the backend is the synthetic HCI stub.
 *
 * Every message the device sends back is checked: a RET_SUBMIT or RET_UNLINK header, a payload
 * no longer than what the client asked for, and isochronous descriptors that fit the transfer.
 *
 *   tools\fuzz.cmd [seconds]
 */

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "../src/service/usbip_device.h"
#include "../src/service/usbip_proto.h"
#include "../src/include/hci_stub.h"

static USBIP_DEVICE *g_Device;
static HCI_TRANSPORT g_Transport;
static HCI_STUB g_Stub;

/*
 * The request each outstanding seqnum made: direction, requested length, packet count. A seqnum
 * reused while its first transfer is still outstanding (usbip-win2 never does this) makes the
 * replies ambiguous; such a seqnum is only checked for framing.
 */
#define TRACK 4096u
static struct { unsigned long Seqnum, Length, Packets; unsigned char In, Live, Ambiguous; } g_Track[TRACK];

static void
Fail(void)
{
    abort();   /* libFuzzer records the input */
}

static void
Remember(const unsigned char *Header)
{
    unsigned long seqnum = UsbipGet32(Header + USBIP_HDR_SEQNUM);
    unsigned long slot = seqnum % TRACK;

    g_Track[slot].Ambiguous = (unsigned char)(g_Track[slot].Live && g_Track[slot].Seqnum == seqnum);
    g_Track[slot].Seqnum = seqnum;
    g_Track[slot].Length = UsbipGet32(Header + USBIP_HDR_LENGTH);
    g_Track[slot].Packets = UsbipGet32(Header + USBIP_HDR_PACKETS);
    g_Track[slot].In = (unsigned char)(UsbipGet32(Header + USBIP_HDR_DIRECTION) == USBIP_DIR_IN);
    g_Track[slot].Live = 1;
}

static int
CheckReply(void *Context, const unsigned char *Data, unsigned long Length)
{
    unsigned long command;
    unsigned long seqnum;

    (void)Context;
    if (Length < USBIP_URB_HEADER_SIZE) {
        Fail();
    }
    command = UsbipGet32(Data + USBIP_HDR_COMMAND);
    seqnum = UsbipGet32(Data + USBIP_HDR_SEQNUM);
    if (command == USBIP_RET_UNLINK) {
        if (Length != USBIP_URB_HEADER_SIZE) {
            Fail();
        }
        return 1;
    }
    if (command != USBIP_RET_SUBMIT) {
        Fail();
    }
    {
        unsigned long slot = seqnum % TRACK;
        unsigned long actual = UsbipGet32(Data + USBIP_HDR_ACTUAL);
        unsigned long packets = UsbipGet32(Data + USBIP_HDR_RET_PACKETS);
        unsigned long isoBytes = (packets == USBIP_NON_ISO_PACKETS) ? 0u : packets * USBIP_ISO_DESC_SIZE;
        unsigned long dataBytes = Length - USBIP_URB_HEADER_SIZE - isoBytes;

        if (isoBytes > Length - USBIP_URB_HEADER_SIZE) {
            Fail();
        }
        if (g_Track[slot].Live && g_Track[slot].Seqnum == seqnum && !g_Track[slot].Ambiguous) {
            /* Never more data than asked for; IN payload equals actual_length; OUT has none. */
            if (actual > g_Track[slot].Length) {
                Fail();
            }
            if (g_Track[slot].In ? dataBytes != actual : dataBytes != 0) {
                Fail();
            }
            if (packets != USBIP_NON_ISO_PACKETS && packets != g_Track[slot].Packets &&
                (long)UsbipGet32(Data + USBIP_HDR_STATUS) == 0) {
                Fail();
            }
            g_Track[slot].Live = 0;
        }
    }
    return 1;
}

int
LLVMFuzzerTestOneInput(const unsigned char *Data, size_t Size)
{
    static int initialized;
    unsigned long long now = 1000000000ull;
    size_t at = 0;

    if (!initialized) {
        g_Device = (USBIP_DEVICE *)calloc(1, sizeof(USBIP_DEVICE));
        initialized = 1;
    }
    memset(g_Track, 0, sizeof(g_Track));
    HciStubInit(&g_Stub);
    HciStubBindTransport(&g_Transport, &g_Stub);
    UsbipDeviceInit(g_Device, &g_Transport, CheckReply, NULL);
    UsbipDeviceBeginSession(g_Device);

    while (Size - at >= USBIP_URB_HEADER_SIZE + 1u) {
        unsigned char header[USBIP_URB_HEADER_SIZE];
        unsigned long payload;
        unsigned char step = Data[at++];

        memcpy(header, Data + at, sizeof(header));
        at += sizeof(header);
        payload = UsbipCommandPayloadLength(header);
        if (payload == 0xFFFFFFFFul || payload > Size - at) {
            break;   /* the session thread drops the connection here */
        }
        if (UsbipGet32(header + USBIP_HDR_COMMAND) == USBIP_CMD_SUBMIT) {
            Remember(header);
        }
        /* The payload is handed over exactly as received: a separate heap copy lets ASan see overreads. */
        {
            unsigned char *copy = (unsigned char *)malloc(payload ? payload : 1u);
            int ok;

            memcpy(copy, Data + at, payload);
            ok = UsbipDeviceHandle(g_Device, header, copy, payload, now);
            free(copy);
            at += payload;
            if (!ok) {
                break;
            }
        }
        now += (unsigned long long)step * 2500ull;   /* up to ~64 ms between commands */
        if (step & 1u) {
            (void)UsbipDeviceTick(g_Device, now);
        }
        if (step & 2u) {
            UsbipDeviceDrain(g_Device, now);
        }
    }
    (void)UsbipDeviceTick(g_Device, now + 10000000ull);
    return 0;
}
