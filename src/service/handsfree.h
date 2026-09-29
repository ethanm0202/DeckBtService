/*
 * handsfree.h - restart a headset's Hands-Free profile device so Windows opens the profile.
 *
 * After a resume, Windows creates the profile devices of the re-arrived adapter, but for a headset
 * that connects in the first seconds it never opens the hands-free profile (no RFCOMM connection):
 * the microphone stays unavailable and the stereo output appears only after about 20 s, without it.
 * Restarting the headset's Hands-Free AG device (BTHENUM\{0000111E-...}, which Windows names
 * "<headset> Hands-Free AG") makes Windows open the
 * profile at once. See docs/VERIFICATION.md.
 */

#pragma once

#include <windows.h>

/*
 * Restarts every present Hands-Free AG device of the paired device with BD_ADDR Address (least
 * significant byte first, as HCI carries it). Returns the number restarted. Blocks about a second.
 */
ULONG HandsFreeRestart(const unsigned char Address[6]);
