/*
 * uart_probe.c - checks user-mode access to the QCA2066 UART.
 *
 * Finds the SerCx2-published interface of the UART controller, opens it, applies the bring-up
 * serial settings of qca_backend.c, performs the CTS wake handshake and sends the
 * read-only EDL version request (01 00 FC 01 19) up the same rate ladder as
 * src/common/qca_identify.c. Nothing is written to the controller beyond that request, so its
 * firmware and rate stay as they were; only the host UART's rate changes as the probe climbs
 * the ladder.
 *
 *   deckbt-uartprobe.exe [--controller ACPI\AMDI0020\4]
 * Exit: 0 controller answered, 1 no answer, 2 interface absent or unusable.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "uart_win32.h"
#include "../include/h4_codec.h"
#include "../include/qca_identify.h"

#define BRINGUP_READ_INTERVAL_MS  20u     /* as qca_backend.c */
#define BRINGUP_WRITE_TIMEOUT_MS  1500u
#define ANSWER_TIMEOUT_MS         1500u   /* identify per-rung response budget */

static QCA_IDENTIFY g_Identify;
static unsigned char g_Packet[300];
static unsigned long g_PacketLength;
static unsigned long g_Packets;

static void
OnPacket(void *Context, unsigned char Type, const unsigned char *Payload, unsigned long Length)
{
    (void)Context;
    g_Packets++;
    if (Length + 1u > sizeof(g_Packet)) {
        return;
    }
    g_Packet[0] = Type;
    memcpy(g_Packet + 1, Payload, Length);
    if (QcaIdentifyOnPacket(&g_Identify, g_Packet, Length + 1u)) {
        g_PacketLength = Length + 1u;
    }
}

static void
PrintHex(const char *Label, const unsigned char *Data, unsigned long Length)
{
    printf("%s", Label);
    for (unsigned long i = 0; i < Length; i++) {
        printf(" %02X", Data[i]);
    }
    printf("\n");
}

int
wmain(int argc, wchar_t **argv)
{
    WCHAR controller[200] = L"ACPI\\AMDI0020\\4";
    WCHAR path[512];
    WCHAR portName[64];
    UART_PORT port;
    DWORD error;
    ULONG rate;
    ULONG status = 0;
    int result = 1;

    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--controller") == 0 && i + 1 < argc &&
            wcscpy_s(controller, ARRAYSIZE(controller), argv[i + 1]) == 0) {
            i++;
        } else {
            fprintf(stderr, "usage: deckbt-uartprobe [--controller <instance id>]\n");
            return 2;
        }
    }

    error = UartFindInterface(controller, path, ARRAYSIZE(path), portName, ARRAYSIZE(portName));
    if (error != ERROR_SUCCESS) {
        printf("interface: none published for %ls (Win32 %lu)\n", controller, error);
        return 2;
    }
    printf("interface: %ls\nport name: %ls\n", path, portName);

    error = UartOpen(&port, path);
    if (error != ERROR_SUCCESS) {
        printf("open: failed, Win32 %lu\n", error);
        return 2;
    }
    printf("open: ok (exclusive)\n");

    QcaIdentifyInit(&g_Identify);
    while (QcaIdentifyNextRate(&g_Identify, &rate)) {
        unsigned char request[8];
        unsigned long requestLength = QcaIdentifyBuildRequest(request, sizeof(request));
        H4_DECODER decoder;
        BOOL ctsBefore = FALSE;
        ULONG pulses = 0;
        LARGE_INTEGER frequency, start, now;
        ULONG received = 0;

        error = UartConfigure(&port, rate);
        if (error != ERROR_SUCCESS) {
            printf("rate %lu: host UART refused (Win32 %lu); skipped\n", rate, error);
            continue;
        }
        (void)UartSetTimeouts(&port, BRINGUP_READ_INTERVAL_MS, 0, 0, BRINGUP_WRITE_TIMEOUT_MS);
        (void)UartPurge(&port);

        error = UartWakeController(&port, &ctsBefore, &pulses);
        (void)UartGetModemStatus(&port, &status);
        printf("rate %lu: CTS %s before wake, %lu wake pulse(s), modem status 0x%02lX -> %s\n",
               rate, ctsBefore ? "asserted" : "low", pulses, status,
               error == ERROR_SUCCESS ? "CTS asserted" : "CTS never asserted");
        if (error != ERROR_SUCCESS) {
            continue;   /* no byte is sent without observed CTS */
        }

        H4DecoderInit(&decoder);
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        error = UartWrite(&port, request, requestLength, BRINGUP_WRITE_TIMEOUT_MS);
        if (error != ERROR_SUCCESS) {
            printf("rate %lu: write failed, Win32 %lu\n", rate, error);
            continue;
        }
        for (;;) {
            unsigned char buffer[512];
            ULONG got = 0;
            double elapsedMs;

            QueryPerformanceCounter(&now);
            elapsedMs = (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)frequency.QuadPart;
            if (g_Identify.Answered || elapsedMs >= ANSWER_TIMEOUT_MS) {
                if (g_Identify.Answered) {
                    printf("rate %lu: answered in %.0f ms\n", rate, elapsedMs);
                } else {
                    printf("rate %lu: no answer in %u ms (%lu byte(s), %lu packet(s))\n",
                           rate, ANSWER_TIMEOUT_MS, received, g_Packets);
                }
                break;
            }
            error = UartRead(&port, buffer, sizeof(buffer), &got, 250);
            if (error != ERROR_SUCCESS) {
                printf("rate %lu: read failed, Win32 %lu\n", rate, error);
                break;
            }
            if (got != 0) {
                received += got;
                PrintHex("  rx:", buffer, got);
                H4DecoderFeed(&decoder, buffer, got, OnPacket, NULL);
            }
        }
    }

    if (g_Identify.Answered) {
        const QCA_SOC_VERSION *v = &g_Identify.Version;

        PrintHex("reply:", g_Packet, g_PacketLength);
        printf("controller: answered at %lu baud (%s); SoC 0x%08lX, product 0x%08lX, patch 0x%04X, "
               "ROM 0x%04X\n",
               g_Identify.AnsweredRate, g_Identify.AnsweredRate == 115200ul ? "ROM" : "running firmware",
               v->SocId, v->ProductId, v->PatchVersion, v->RomVersionField);
        result = 0;
    } else {
        printf("controller: no answer on any rate\n");
    }

    /* Leave the line at the ROM rate under the handshake, as qca_backend.c's hand-back does. */
    (void)UartConfigure(&port, 115200ul);
    UartClose(&port);
    return result;
}
