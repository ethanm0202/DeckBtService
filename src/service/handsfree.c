/*
 * handsfree.c - restart a headset's Hands-Free profile device (see handsfree.h).
 *
 * The restart is the class installer's property change (DIF_PROPERTYCHANGE, DICS_PROPCHANGE), the
 * same as Device Manager or `pnputil /restart-device`. Removing and re-readying the devnode through
 * cfgmgr32 leaves it in CM_PROB_WILL_BE_REMOVED instead.
 */

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <wchar.h>

#include "handsfree.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

/* Hands-Free (0x111E) is the headset's service; Windows is the audio gateway. */
#define HANDSFREE_PREFIX L"BTHENUM\\{0000111E-0000-1000-8000-00805F9B34FB}"

ULONG
HandsFreeRestart(const unsigned char Address[6])
{
    WCHAR needle[16];
    HDEVINFO set;
    SP_DEVINFO_DATA device;
    ULONG restarted = 0;

    /* Instance IDs spell the address most significant byte first: ...&001A7DDA7113_C00000000 */
    (void)swprintf_s(needle, ARRAYSIZE(needle), L"&%02X%02X%02X%02X%02X%02X_",
                     Address[5], Address[4], Address[3], Address[2], Address[1], Address[0]);
    set = SetupDiGetClassDevsW(NULL, L"BTHENUM", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        return 0;
    }
    device.cbSize = sizeof(device);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &device); i++) {
        WCHAR id[MAX_DEVICE_ID_LEN];
        SP_PROPCHANGE_PARAMS change;
        ULONG status = 0;
        ULONG problem = 0;

        if (!SetupDiGetDeviceInstanceIdW(set, &device, id, ARRAYSIZE(id), NULL)) {
            continue;
        }
        (void)_wcsupr_s(id, ARRAYSIZE(id));
        if (wcsncmp(id, HANDSFREE_PREFIX, ARRAYSIZE(HANDSFREE_PREFIX) - 1) != 0 || wcsstr(id, needle) == NULL) {
            continue;
        }
        /* Only a running device: one still being installed (first pairing) or failing is left alone. */
        if (CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0) != CR_SUCCESS ||
            !(status & DN_STARTED) || (status & DN_HAS_PROBLEM)) {
            continue;
        }
        ZeroMemory(&change, sizeof(change));
        change.ClassInstallHeader.cbSize = sizeof(change.ClassInstallHeader);
        change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        change.StateChange = DICS_PROPCHANGE;
        change.Scope = DICS_FLAG_CONFIGSPECIFIC;
        if (SetupDiSetClassInstallParamsW(set, &device, &change.ClassInstallHeader, sizeof(change)) &&
            SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &device)) {
            restarted++;
        }
    }
    (void)SetupDiDestroyDeviceInfoList(set);
    return restarted;
}
