/*
*   This file is part of Luma3DS
*   Copyright (C) 2026 LiamTrader
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*   Additional Terms 7.b and 7.c of GPLv3 apply to this file:
*       * Requiring preservation of specified reasonable legal notices or
*         author attributions in that material or in the Appropriate Legal
*         Notices displayed by works containing it.
*       * Prohibiting misrepresentation of the origin of that material,
*         or requiring that modified versions of such material be marked in
*         reasonable ways as different from the original version.
*/

// Flashback: rolling "last N seconds" recorder for the top screen.
//
// Runs as a Rosalina thread, so it costs the running game no memory: frames
// are read straight from the display hardware's current framebuffer (like the
// screenshot feature), converted to RGB565 at half (200x120) or full (400x240)
// resolution, and written to a ring file on the SD card at 10, 20 or 30 fps.
// Saving turns the ring into a numbered BMP sequence in
// /luma/flashback/clip_<date>/.
//
// Per 10 s clip:      ring file    BMPs written on save
//   half, 10 fps         4.8 MB       7 MB
//   full, 30 fps         58 MB      86 MB
// If the SD card can't keep up, frames are skipped (see the stats in the
// Flashback menu) and clips play back too fast.

#pragma once

#include <3ds/types.h>
#include "MyThread.h"

#define FLASHBACK_SECONDS   10

#define FLASHBACK_DIR       "/luma/flashback"
#define FLASHBACK_RING_FILE FLASHBACK_DIR "/ring.bin"

// Default save hotkey (changeable in the Flashback menu), checked by the menu
// thread while no menu is open.
#define FLASHBACK_DEFAULT_SAVE_COMBO (KEY_L | KEY_R | KEY_DDOWN)

MyThread *flashbackCreateThread(void);

// Called by the menu thread with the currently held keys.
void Flashback_HandleKeys(u32 heldKeys);

// Rosalina menu entry.
void FlashbackMenu_Show(void);
