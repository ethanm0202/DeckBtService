/*
 * fuzz_usbip_device.c - libFuzzer target for the USB/IP command parser and device model.
 *
 * deckbt-usbip.exe runs as LocalSystem and parses every byte that arrives on its loopback socket,
 * so src/service/usbip_device.c is the privilege boundary. The input is a stream of USB/IP
 * commands (48-byte headers and their payloads), fed to UsbipDeviceHandle exactly as the session
 * thread does, with pacing ticks in between; the backend is the synthetic HCI stub.
 *
 * Every message the device sends back is checked: a RET_SUBMIT or RET_UNLINK header, a payload
 * no longer than what the client asked for, and isochronous descriptors that fit the transfer,
 * none of an accepted transfer longer than the SCO setting's wMaxPacketSize at submit time.
 *
 *   tools\fuzz.cmd [seconds]
 */

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "../src/service/usbip_device.h"
#include "../src/service/usbip_proto.h"
#include "../src/include/hci_stub.h"
#include "../src/include/usb_descriptors.h"

static USBIP_DEVICE *g_Device;
static HCI_TRANSPORT g_Transport;
static HCI_STUB g_Stub;

/*
 * The request each outstanding seqnum made: direction, requested length, packet count, and the
 * SCO wMaxPacketSize in force when it was submitted. A seqnum reused while its first transfer is
 * still outstanding (usbip-win2 never does this) makes the replies ambiguous; such a seqnum is
 * only checked for framing.
 */
#define TRACK 1024u
static struct {
    unsigned long Seqnum, Length, Packets, MaxPacket;
    unsigned char In, Live, Ambiguous;
    unsigned int LiveCount;
} g_Track[TRACK];
static unsigned int g_TrackCount;

static void
Fail(void)
{
    abort();   /* libFuzzer records the input */
}

static void
Remember(const unsigned char *Header)
{
    unsigned long seqnum = UsbipGet32(Header + USBIP_HDR_SEQNUM);
    unsigned int i;

    for (i = 0; i < g_TrackCount; i++) {
        if (g_Track[i].Live && g_Track[i].Seqnum == seqnum) {
            g_Track[i].Ambiguous = 1;
            g_Track[i].LiveCount++;
            return;
        }
    }
    if (g_TrackCount < TRACK) {
        i = g_TrackCount++;
        g_Track[i].Seqnum = seqnum;
        g_Track[i].Length = UsbipGet32(Header + USBIP_HDR_LENGTH);
        g_Track[i].Packets = UsbipGet32(Header + USBIP_HDR_PACKETS);
        g_Track[i].In = (unsigned char)(UsbipGet32(Header + USBIP_HDR_DIRECTION) == USBIP_DIR_IN);
        g_Track[i].MaxPacket = DeckBtScoAltPacketSize[g_Device->ScoAlt];
        g_Track[i].Live = 1;
        g_Track[i].Ambiguous = 0;
        g_Track[i].LiveCount = 1;
    }
}

static void
Forget(unsigned long Seqnum)
{
    for (unsigned int i = 0; i < g_TrackCount; i++) {
        if (g_Track[i].Live && g_Track[i].Seqnum == Seqnum) {
            if (g_Track[i].LiveCount > 0) {
                g_Track[i].LiveCount--;
            }
            if (g_Track[i].LiveCount == 0) {
                g_Track[i] = g_Track[--g_TrackCount];
                memset(&g_Track[g_TrackCount], 0, sizeof(g_Track[0]));
            }
            return;
        }
    }
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
        unsigned long actual = UsbipGet32(Data + USBIP_HDR_ACTUAL);
        unsigned long packets = UsbipGet32(Data + USBIP_HDR_RET_PACKETS);
        unsigned long isoBytes = (packets == USBIP_NON_ISO_PACKETS) ? 0u : packets * USBIP_ISO_DESC_SIZE;
        unsigned long dataBytes = Length - USBIP_URB_HEADER_SIZE - isoBytes;
        unsigned int i;

        if (isoBytes > Length - USBIP_URB_HEADER_SIZE) {
            Fail();
        }
        for (i = 0; i < g_TrackCount; i++) {
            if (g_Track[i].Live && g_Track[i].Seqnum == seqnum) {
                break;
            }
        }
        if (i < g_TrackCount) {
            if (!g_Track[i].Ambiguous) {
                /* Never more data than asked for; IN payload equals actual_length; OUT has none. */
                if (actual > g_Track[i].Length) {
                    Fail();
                }
                if (g_Track[i].In ? dataBytes != actual : dataBytes != 0) {
                    Fail();
                }
                if (packets != USBIP_NON_ISO_PACKETS && packets != g_Track[i].Packets &&
                    (long)UsbipGet32(Data + USBIP_HDR_STATUS) == 0) {
                    Fail();
                }
                /* An accepted isochronous transfer: every packet fits one frame of its setting. */
                if (packets != USBIP_NON_ISO_PACKETS && (long)UsbipGet32(Data + USBIP_HDR_STATUS) == 0) {
                    const unsigned char *d = Data + Length - isoBytes;

                    for (unsigned long p = 0; p < packets; p++, d += USBIP_ISO_DESC_SIZE) {
                        if (UsbipGet32(d + 4) > g_Track[i].MaxPacket || UsbipGet32(d + 8) > UsbipGet32(d + 4)) {
                            Fail();
                        }
                    }
                }
            }
            if (g_Track[i].LiveCount > 0) {
                g_Track[i].LiveCount--;
            }
            if (g_Track[i].LiveCount == 0) {
                g_Track[i] = g_Track[--g_TrackCount];
                memset(&g_Track[g_TrackCount], 0, sizeof(g_Track[0]));
            }
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
    g_TrackCount = 0;
    memset(g_Track, 0, sizeof(g_Track));
    HciStubInit(&g_Stub);
    HciStubBindTransport(&g_Transport, &g_Stub);
    UsbipDeviceInit(g_Device, &g_Transport, CheckReply, NULL);
    UsbipDeviceBeginSession(g_Device);

    while (Size - at >= USBIP_URB_HEADER_SIZE + 1u) {
        unsigned char header[USBIP_URB_HEADER_SIZE];
        unsigned long payload;
        unsigned long cmd;
        unsigned char step = Data[at++];

        memcpy(header, Data + at, sizeof(header));
        at += sizeof(header);
        payload = UsbipCommandPayloadLength(header);
        if (payload == 0xFFFFFFFFul || payload > Size - at) {
            break;   /* the session thread drops the connection here */
        }
        cmd = UsbipGet32(header + USBIP_HDR_COMMAND);
        if (cmd == USBIP_CMD_SUBMIT) {
            Remember(header);
        } else if (cmd == USBIP_CMD_UNLINK) {
            Forget(UsbipGet32(header + USBIP_HDR_UNLINK_SEQNUM));
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
