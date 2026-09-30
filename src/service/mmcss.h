/*
 * mmcss.h - the Multimedia Class Scheduler (MMCSS) for the threads that keep audio on time.
 *
 * The pacing thread, the USB/IP session thread and the UART reader and writer carry every voice and
 * music packet. At normal priority an application starting could delay them by tens of
 * milliseconds (74 ms of pacing lateness was measured), heard as crackle in calls and stutter in
 * music. They join the "Pro Audio" task, as the Windows audio engine's own threads do. MMCSS
 * reserves a share of each second for other threads, so a busy registered thread cannot starve
 * the system.
 */

#pragma once

#include <windows.h>
#include <avrt.h>

#pragma comment(lib, "avrt.lib")

/* Registers the calling thread. NULL, with GetLastError, if MMCSS refused: it keeps normal priority. */
static __inline HANDLE
MmcssEnter(void)
{
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    if (task != NULL) {
        (void)AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);
    }
    return task;
}

static __inline void
MmcssLeave(HANDLE Task)
{
    if (Task != NULL) {
        (void)AvRevertMmThreadCharacteristics(Task);
    }
}
