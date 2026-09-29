/*
 * sco_route.h - put BTHPORT's synchronous (voice) links on the HCI data path.
 *
 * BTHPORT sets up voice using legacy Setup/Accept_Synchronous_Connection commands (0x0428/0x0429),
 * which carry no data-path field. On QCA2066, the controller defaults to offloaded audio
 * rather than HCI. The Enhanced synchronous commands (0x043D/0x043E) specify Input/Output_Data_Path;
 * data path 0x00 selects HCI (compare upstream Linux hci_conn.c hci_enhanced_setup_sync:
 * https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/net/bluetooth/hci_conn.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137).
 *
 * This module rewrites legacy setup/accept commands to Enhanced equivalents specifying data path
 * 0x00 (HCI), preserving BTHPORT's bandwidths, latency, packet types, and retransmission effort,
 * and deriving codec fields from Voice_Setting (transparent = mSBC frames, CVSD = 16-bit linear PCM
 * on the host side). When the controller replies with Command Status or Command Complete for the
 * Enhanced opcode, the legacy opcode is restored for BTHPORT. Air codings other than CVSD and
 * transparent (u-law, A-law) pass through unchanged.
 */

#pragma once

#include <windows.h>

#define SCO_ROUTE_OP_SETUP             0x0428u
#define SCO_ROUTE_OP_ACCEPT            0x0429u
#define SCO_ROUTE_OP_ENH_SETUP         0x043Du
#define SCO_ROUTE_OP_ENH_ACCEPT        0x043Eu
#define SCO_ROUTE_ENH_SETUP_PARAMS     59u
#define SCO_ROUTE_ENH_ACCEPT_PARAMS    63u
#define SCO_ROUTE_MAX_COMMAND          (3u + SCO_ROUTE_ENH_ACCEPT_PARAMS)
#define SCO_ROUTE_DATA_PATH_HCI        0x00u

typedef struct _SCO_ROUTE {
    unsigned char SetupPending;     /* rewritten setups awaiting their Command Status */
    unsigned char AcceptPending;    /* rewritten accepts awaiting their Command Status */
    unsigned long Rewritten;        /* commands rewritten to the Enhanced form */
    unsigned long Restored;         /* controller answers given back the legacy opcode */
} SCO_ROUTE;

void ScoRouteReset(_Out_ SCO_ROUTE *Route);

/*
 * If Command (opcode LE16, length, parameters) is a legacy Setup/Accept_Synchronous_Connection with a
 * CVSD or transparent air coding, writes the Enhanced equivalent with HCI data paths into Out and
 * returns its length; otherwise returns 0 and the command goes out unchanged.
 */
unsigned long ScoRouteRewriteCommand(_Inout_ SCO_ROUTE *Route,
                                     _In_reads_bytes_(Length) const unsigned char *Command,
                                     _In_ unsigned long Length,
                                     _Out_writes_bytes_(Capacity) unsigned char *Out,
                                     _In_ unsigned long Capacity);

/*
 * If Event (code, length, parameters) is the Command Status or Command Complete for a command this
 * module rewrote, puts BTHPORT's legacy opcode back in place and returns 1.
 */
unsigned char ScoRouteRestoreEvent(_Inout_ SCO_ROUTE *Route,
                                   _Inout_updates_bytes_(Length) unsigned char *Event,
                                   _In_ unsigned long Length);
