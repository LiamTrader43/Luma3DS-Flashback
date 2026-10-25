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

// Flashback: recording the inputs (see flashback_internal.h).

#include <3ds.h>
#include <string.h>
#include "flashback_internal.h"
#include "draw.h"
#include "menu.h"

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------
//
// Every frame of the clip, copies included (one per grid slot), gets a record
// of the inputs during it, so a player can show them over the video: the
// buttons held at any time during it (so a tap shorter than a frame still
// shows), and where the Circle Pad and the touch screen were. They're HID's
// readings, the ones the game gets, from its shared memory: HID keeps the
// pad's 8 newest readings and the touch screen's 8 newest, and the recorder
// thread goes through the ones it hasn't seen every grid slot, and every
// FLASHBACK_INPUT_POLL_MS while it waits for the next one.
//
// The records go round a ring of g_frames of them, one per grid slot since the
// ring started. Every grid slot after the ring's first frame is a frame of the
// clip or a copy, so the newest records line up with the clip's frames: a save
// keeps the last ones (flashbackInputLatest).

// How often the recorder thread looks at HID's readings while it waits for
// the next grid slot: a tap lasts tens of milliseconds, so none goes by unseen.
#define FLASHBACK_INPUT_POLL_MS         15

// HID's shared memory, in words (as libctru's hidScanInput reads it). Each
// section starts with when its newest reading was taken and when the one
// before it was (u64 ticks), then, 4 words in, which of its 8 readings is the
// newest.
#define FLASHBACK_HID_PAD               0
#define FLASHBACK_HID_PAD_READINGS      10  // 4 words each: buttons held, pressed, released, Circle Pad (x | y << 16)
#define FLASHBACK_HID_TOUCH             42
#define FLASHBACK_HID_TOUCH_READINGS    50  // 2 words each: x | y << 16, touched

u32 g_inputOffset;                          // the ring, in g_mem (flashbackLayout)
static u32 g_inputCount;                    // records since the ring started
static FlashbackInput g_inputNow;           // the inputs since the last record (buttons held in any reading)
static FlashbackInput g_inputNewest;        // HID's newest readings (where the next record starts from)
static u64 g_hidPadTick, g_hidTouchTick;    // the newest readings seen...
static u32 g_hidPadIndex, g_hidTouchIndex;  // ...and which they are

// How many of a section's readings are new since the last look, and which is
// the newest. All 8 if it went all the way round, or if it's been so long
// since that it may have: the index alone can't tell 2 new readings from 10.
static u32 flashbackHidNewReadings(const vu32 *section, u64 *seenTick, u32 *seenIndex)
{
    u64 tick = section[0] | (u64)section[1] << 32, before = section[2] | (u64)section[3] << 32;
    u32 index = section[4] % 8, n;
    if (tick == *seenTick)
        n = 0;
    else if (index == *seenIndex || (tick > before && tick - *seenTick > (tick - before) * 7))
        n = 8;
    else
        n = (index - *seenIndex) % 8;
    *seenTick = tick;
    *seenIndex = index;
    return n;
}

// Goes through HID's readings since the last look: adds the buttons held in
// them to g_inputNow, and takes the newest Circle Pad and touch point. With
// 'discard', starts g_inputNow afresh from the newest readings instead.
static void flashbackInputRead(bool discard)
{
    Draw_Lock();    // (Rosalina unmaps HID's memory under this lock as it terminates)
    const vu32 *hid = hidSharedMem;
    if (isHidInitialized && hid != NULL)
    {
        u32 held = 0, touchPos = 0;
        bool touched = false;

        u32 n = flashbackHidNewReadings(hid + FLASHBACK_HID_PAD, &g_hidPadTick, &g_hidPadIndex);
        for (u32 k = 0; k < n; k++)
            held |= hid[FLASHBACK_HID_PAD_READINGS + (g_hidPadIndex - k) % 8 * 4];
        const vu32 *pad = hid + FLASHBACK_HID_PAD_READINGS + g_hidPadIndex * 4;

        n = flashbackHidNewReadings(hid + FLASHBACK_HID_TOUCH, &g_hidTouchTick, &g_hidTouchIndex);
        for (u32 k = 0; k < n && !touched; k++)     // (the newest touched one)
        {
            const vu32 *t = hid + FLASHBACK_HID_TOUCH_READINGS + (g_hidTouchIndex - k) % 8 * 2;
            touched = t[1] != 0;
            touchPos = t[0];
        }
        const vu32 *touch = hid + FLASHBACK_HID_TOUCH_READINGS + g_hidTouchIndex * 2;

        u32 circle = pad[3], pos = touch[0];
        bool touchedNow = touch[1] != 0;
        g_inputNewest.buttons = pad[0] | (touchedNow ? KEY_TOUCH : 0);
        g_inputNewest.circleX = (s16)circle;
        g_inputNewest.circleY = (s16)(circle >> 16);
        g_inputNewest.touchX = touchedNow ? (u16)pos : 0;
        g_inputNewest.touchY = touchedNow ? (u16)(pos >> 16) : 0;

        if (discard)
            g_inputNow = g_inputNewest;
        else
        {
            g_inputNow.buttons |= held | (touched ? KEY_TOUCH : 0);
            g_inputNow.circleX = g_inputNewest.circleX;
            g_inputNow.circleY = g_inputNewest.circleY;
            if (touched)
            {
                g_inputNow.touchX = (u16)touchPos;
                g_inputNow.touchY = (u16)(touchPos >> 16);
            }
        }
    }
    Draw_Unlock();
}

// Starts afresh from HID's newest readings, leaving out the ones since the
// last record: when recording resumes, they're from while a menu was open.
void flashbackInputResync(void)
{
    flashbackInputRead(true);
}

// An empty ring (recording starting, or a save).
void flashbackInputReset(void)
{
    g_inputCount = 0;
    flashbackInputResync();
}

void flashbackInputPoll(void)
{
    flashbackInputRead(false);
}

// Records the inputs of the grid slot just captured (or missed), and of
// 'slots' - 1 more missed right after it (the inputs as they are now).
void flashbackInputSample(u32 slots)
{
    if (slots == 0 || !g_active)    // (not after a failed write stopped recording: the ring is gone)
        return;
    flashbackInputRead(false);
    FlashbackInput *ring = (FlashbackInput *)(g_mem + g_inputOffset);
    for (u32 i = 0; i < slots; i++, g_inputCount++)
    {
        ring[g_inputCount % g_frames] = g_inputNow;
        g_inputNow = g_inputNewest;
    }
}

// The last 'count' records into 'dst', oldest first (zeros for any there
// weren't: for a clip's frames, there always are).
void flashbackInputLatest(u32 count, FlashbackInput *dst)
{
    const FlashbackInput *ring = (const FlashbackInput *)(g_mem + g_inputOffset);
    u32 have = g_inputCount < g_frames ? g_inputCount : g_frames;
    u32 none = count > have ? count - have : 0;
    memset(dst, 0, none * sizeof(*dst));
    for (u32 i = none; i < count; i++)
        dst[i] = ring[(g_inputCount - count + i) % g_frames];
}

// Sleeps until 'tick', looking at HID's readings every FLASHBACK_INPUT_POLL_MS
// meanwhile.
void flashbackInputSleepUntil(u64 tick)
{
    const u64 pollTicks = (u64)SYSCLOCK_ARM11 * FLASHBACK_INPUT_POLL_MS / 1000;
    u64 now = svcGetSystemTick();
    while (tick > now + pollTicks)
    {
        svcSleepThread(FLASHBACK_INPUT_POLL_MS * 1000 * 1000LL);
        flashbackInputPoll();
        now = svcGetSystemTick();
    }
    if (tick > now)
        svcSleepThread((s64)((tick - now) * 1000000000ULL / SYSCLOCK_ARM11));
}
