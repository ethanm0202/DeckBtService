/*
 * qca_backend.h - the QCA2066 controller as an HCI_TRANSPORT backend, from user mode.
 *
 * It uses the pure-logic modules (qca_identify, qca_init_fsm, h4_codec, hci_bridge, sco_route)
 * for the identify ladder and SoC reset to reach ROM at 115200, firmware bring-up at 3,000,000
 * baud, host IBS wake, WAKE_IND acknowledgement off the read path, the Write_LE_Host_Support
 * status rewrite, voice routing onto HCI, and the hand-back to ROM on stop.
 *
 * I/O goes through the SerCx2-published COM port interface (uart_win32.h). Threads:
 *   - reader: one read always pending; feeds the H4 decoder under the controller lock and
 *     notifies the front end afterwards;
 *   - writer (steady state): sends queued H4 packets whole, and WAKE_ACKs between packets.
 * Submit* from the front end never block: packets are queued for the writer.
 */

#pragma once

#include <windows.h>

#include "uart_win32.h"
#include "../include/hci_transport.h"
#include "../include/hci_bridge.h"
#include "../include/h4_codec.h"
#include "../include/qca_identify.h"
#include "../include/qca_init_fsm.h"
#include "../include/sco_route.h"

#define QCA_BACKEND_NVM_FILES   4u
#define QCA_BACKEND_TX_SLOTS    32u
#define QCA_BACKEND_TX_SLOT_BYTES 2048u

typedef void (*QCA_BACKEND_LOG)(const char *Format, ...);
typedef void (*QCA_BACKEND_FAULT)(void *Context, const char *Why);

typedef struct _QCA_BACKEND_TX_SLOT {
    ULONG Length;
    UCHAR Data[QCA_BACKEND_TX_SLOT_BYTES];
} QCA_BACKEND_TX_SLOT;

typedef struct _QCA_BACKEND_STATS {
    ULONG  IbsWakeIndRx;
    ULONG  IbsSleepIndRx;
    ULONG  IbsWakeAckTx;
    ULONG  IbsAckCtsLow;
    ULONG  IbsWakeTries;
    ULONG  TxQueueFull;
    ULONG  WriteErrors;
    ULONG  ReadErrors;
    ULONGLONG BytesRead;
    ULONGLONG BytesWritten;
    ULONG  EntryBaud;           /* rate the controller answered on at start; 0 = silent */
    ULONG  EntryReset;
    ULONG  HandbackBaud;
    ULONG  BringUpMs;
} QCA_BACKEND_STATS;

typedef struct _QCA_BACKEND {
    /* Configuration, set before QcaBackendStart. */
    WCHAR            Controller[200];     /* UART controller instance, e.g. ACPI\AMDI0020\4 */
    WCHAR            FirmwareDir[MAX_PATH];
    QCA_BACKEND_LOG  Log;
    /* Optional, once per start in steady state, on a worker with neither lock held; signal only. */
    QCA_BACKEND_FAULT OnFault;
    void            *FaultContext;

    UART_PORT        Port;
    ULONG            CurrentBaud;

    CRITICAL_SECTION *Lock;               /* the front end's controller lock */
    HCI_TRANSPORT    *Transport;          /* the transport the front end calls */
    HCI_TRANSPORT     BridgeTransport;
    HCI_BRIDGE        Bridge;
    SCO_ROUTE         ScoRoute;
    H4_DECODER        Decoder;
    ULONG             PendingNotifyMask;

    volatile LONG     Phase;              /* 0 identify, 1 bring-up, 2 steady, 3 closing */
    QCA_IDENTIFY      Identify;
    QCA_INIT_FSM      Fsm;
    BOOLEAN           Waiting;            /* a bring-up/identify reply is awaited */
    BOOLEAN           Matched;
    HANDLE            ReplyEvent;
    UCHAR             HciReset[16];
    ULONG             HciResetLength;

    HANDLE            Reader;
    volatile LONG     ReaderPause;
    volatile LONG     ReaderPaused;
    HANDLE            ReaderIdle;
    HANDLE            ReaderResume;

    HANDLE            Writer;
    HANDLE            TxEvent;
    CRITICAL_SECTION  TxLock;
    QCA_BACKEND_TX_SLOT Tx[QCA_BACKEND_TX_SLOTS];
    ULONG             TxHead;
    ULONG             TxCount;
    volatile LONG     IbsAckPending;
    volatile LONG     IbsTxAwake;
    HANDLE            IbsAckEvent;

    volatile LONG     Stop;               /* ends the reader (and the writer) */
    volatile LONG     WriterStop;         /* ends the writer only; the reader serves the hand-back */
    volatile LONG     Faulted;            /* reader/writer share one fault notification per start */
    volatile LONG     Stuck;              /* a worker did not stop: retain state until process exit */

    UCHAR            *Patch;
    ULONG             PatchSize;
    UCHAR            *Nvm[QCA_BACKEND_NVM_FILES];
    QCA_NVM_CANDIDATE Candidates[QCA_BACKEND_NVM_FILES];
    ULONG             CandidateCount;

    QCA_BACKEND_STATS Stats;
} QCA_BACKEND;

/*
 * Binds Transport (Ops, Context, Backend; Notify is left to the front end), opens the UART and
 * brings the controller up to steady state with the bridge ready. On failure the controller is
 * handed back and everything is released unless Stuck is set. Stuck rejects a restart with
 * ERROR_BUSY. Blocking; takes several seconds.
 */
DWORD QcaBackendStart(QCA_BACKEND *Backend, CRITICAL_SECTION *Lock, HCI_TRANSPORT *Transport);

/* Closes the bridge, stops the threads, hands back to ROM and closes the UART; no-op if Stuck. */
void QcaBackendStop(QCA_BACKEND *Backend);

/*
 * Ends every open ACL link with HCI_Disconnect (remote device powered off), as Windows does when
 * Bluetooth is switched off, and waits up to TimeoutMs for the controller to close them. Peers
 * then see an orderly disconnect instead of a lost link when the controller is handed back.
 * Host commands are withheld from here on (hci_bridge.h, CLEAN SHUTDOWN): call only right before
 * the device is detached and the backend stopped. Stores the number of links found open in *Found
 * and returns the number still open at the deadline.
 */
ULONG QcaBackendDisconnectAll(QCA_BACKEND *Backend, ULONG TimeoutMs, ULONG *Found);

/* Newest %SystemRoot%\System32\DriverStore\FileRepository\qcbtuart.inf_amd64_* with the rampatch. */
DWORD QcaBackendFindVendorFirmware(WCHAR *Directory, DWORD Chars);
