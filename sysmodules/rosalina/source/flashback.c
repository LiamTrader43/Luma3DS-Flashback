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

// See flashback.h, and flashback_internal.h for the recorder's other files.

#include <3ds.h>
#include <string.h>
#include "flashback_internal.h"
#include "draw.h"
#include "menu.h"

// Semi-transparent bottom screen tints used as feedback for the hotkey
// (0x00BBGGRR), blended over the screen through the display's color lookup table.
#define FLASHBACK_TINT_SAVING   0x00FFFFFF  // white
#define FLASHBACK_TINT_OK       0x0000C000  // green
#define FLASHBACK_TINT_FAILED   0x000000E0  // red
#define FLASHBACK_TINT_ALPHA    128         // tint strength, out of 256 (50%)

// The recorder's threads (see flashbackCreateThread).
static MyThread flashbackThread;
static u8 CTR_ALIGN(8) flashbackThreadStack[0x2000];
bool g_threadRunning;

static MyThread flashbackAudioThread;
static u8 CTR_ALIGN(8) flashbackAudioStack[0x1000];
bool g_audioRunning;                        // the audio thread exists

static MyThread flashbackPackThread;
static u8 CTR_ALIGN(8) flashbackPackStack[0x1000];
bool g_packRunning;                     // false: frames are packed inline by the capture thread

static MyThread flashbackWriterThread;
static u8 CTR_ALIGN(8) flashbackWriterStack[0x1000];
bool g_writerRunning;                   // false: frames are written inline as they're packed

volatile bool g_enabled;                // user toggle
// Defaults when no settings are saved, on every model: full resolution, 30 fps,
// 30 seconds. (An Old 3DS keeps up at full resolution too: Mario Kart 7 drops
// only a few frames a race.)
volatile u32  g_wantRes = FLASHBACK_RES_FULL;          // FlashbackResolution chosen in the menu
volatile u32  g_wantFpsIdx = 2;                        // index into g_fpsChoices chosen in the menu (30 fps)
volatile u32  g_wantLenIdx = FLASHBACK_DEFAULT_LENGTH_IDX; // index into g_lengthChoices chosen in the menu
volatile u32  g_saveCombo = FLASHBACK_DEFAULT_SAVE_COMBO;
volatile bool g_saveRequested;
volatile bool g_saveFromHotkey;         // show bottom-screen feedback
volatile bool g_saving;
volatile s32  g_lastSaveResult;         // frames written, or < 0 on error (0 = never saved)
char g_lastClip[96];

// Recording format currently in effect. Written only by the flashback thread
// (when nothing is recording); read by the menu for display.
volatile u32 g_res = FLASHBACK_RES_FULL;
volatile u32 g_fpsIdx = 2;
volatile u32 g_lenIdx = FLASHBACK_DEFAULT_LENGTH_IDX;
u32 g_w, g_h, g_fps, g_frames;          // derived from g_res / g_fpsIdx / g_lenIdx
u32 g_slotBytes, g_frameOffset, g_memSize;
u32 g_slotStride;                       // ring slot size: g_slotBytes rounded up (flashbackSlotStride)

// Performance stats shown in the menu, updated about once a second while recording.
volatile u32 g_statFpsX10;              // frames actually recorded per second, x10
volatile u32 g_statCaptureUs;           // average time to read + convert one frame
volatile u32 g_statCompressUs;          // average time to compress one frame (or turn it upright, if stored as it is)
volatile u32 g_statSizePct;             // average compressed size, % of the uncompressed frame
volatile u32 g_statRawPct;              // frames stored as they are (not compressed), %
volatile u32 g_statWriteUs;             // average SD write time per frame
volatile u32 g_lastSaveMs;              // duration of the last save
const char *volatile g_recordError; // why recording switched itself off, or NULL

// Written by the writer thread only, never reset (wrap-around safe deltas).
volatile u32 g_writeTotalUs, g_writeTotalFrames;

static u64 g_winStart;
u64 g_winCaptureTicks, g_winCompressTicks, g_winPackedBytes;
u32 g_winFrames, g_winRawFrames;
static u32 g_winWriteUs0, g_winWriteFrames0;

u32 flashbackAudioFramesFor(u32 seconds)
{
    // (64-bit: SYSCLOCK_ARM11 is a 32-bit constant, and 16 s of ticks don't fit in 32 bits.)
    u32 frames = (u32)((u64)(seconds + FLASHBACK_AUDIO_EXTRA_SECONDS) * SYSCLOCK_ARM11 / FLASHBACK_AUDIO_FRAME_TICKS) + 1;
    return (frames + FLASHBACK_AUDIO_CHUNK_FRAMES - 1) / FLASHBACK_AUDIO_CHUNK_FRAMES * FLASHBACK_AUDIO_CHUNK_FRAMES;
}

// Where things are in the recorder's memory, for g_stageSlots frames' worth of
// staging, g_capBufs capture buffers and g_audioStageFrames of audio staging:
// the staging first (the clip's inputs and index while saving, when it's
// empty: see flashbackSaveClipFile), then the capture buffers, which hold
// frames as captured (before compression), each with a column's worth after it
// that compressing reads past the frame (flashbackCompress), then the audio
// staging, then the inputs' ring (flashback_input.c).
void flashbackLayout(void)
{
    g_stageBytes = g_stageSlots * g_slotStride;
    g_frameOffset = (g_stageBytes + 0xFFF) & ~0xFFF;
    g_capStride = (g_slotBytes + g_h * 2 + 0xFFF) & ~0xFFF;
    g_memSize = g_frameOffset + g_capBufs * g_capStride;
    g_audioStageOffset = g_memSize;
    if (g_audio)
        g_memSize += (g_audioStageFrames * FLASHBACK_AUDIO_FRAME_BYTES + 0xFFF) & ~0xFFF;
    g_inputOffset = g_memSize;
    g_memSize += (g_frames * sizeof(FlashbackInput) + 0xFFF) & ~0xFFF;
}

// Every frame is written starting on a boundary of the card's internal blocks:
// a write that starts partway into one costs the card extra work, and the
// processor too (see docs/flashback-design.md, "SD card writes"). So a ring
// slot is the frame rounded up to the largest power of two up to 64 KB that
// adds at most 1/32 (64 KB at full resolution, 16 KB at half), and the video
// slots start on a 64 KB boundary after the audio ring.
u32 flashbackSlotStride(u32 slotBytes)
{
    u32 align = 0x10000;
    while (align > 512 && ((slotBytes + align - 1) & ~(align - 1)) - slotBytes > slotBytes / 32)
        align /= 2;
    return (slotBytes + align - 1) & ~(align - 1);
}

u32 flashbackVideoOffset(u32 audioBytes)
{
    return (audioBytes + 0xFFFF) & ~0xFFFF;
}

static void flashbackApplyFormat(u32 res, u32 fpsIdx, u32 lenIdx, bool audio)
{
    g_res = res;
    g_fpsIdx = fpsIdx;
    g_lenIdx = lenIdx;

    g_w = res == FLASHBACK_RES_FULL ? 400 : 200;
    g_h = res == FLASHBACK_RES_FULL ? 240 : 120;
    g_fps = g_fpsChoices[fpsIdx];
    g_frames = g_lengthChoices[lenIdx] * g_fps;

    g_slotBytes = g_w * g_h * 2;
    g_slotStride = flashbackSlotStride(g_slotBytes);

    // Audio: a ring at the start of the ring file (a whole number of chunks of
    // whole SD sectors), the video slots after it, and staging memory.
    g_audio = audio;
    g_audioCapFrames = audio ? flashbackAudioFramesFor(g_lengthChoices[lenIdx]) : 0;
    g_audioBytes = g_audioCapFrames * FLASHBACK_AUDIO_FRAME_BYTES;
    g_videoOffset = flashbackVideoOffset(g_audioBytes);

    // (flashbackStart settles how much memory there is for staging.)
    g_stageSlots = FLASHBACK_STAGE_SLOTS_MAX;
    g_capBufs = FLASHBACK_CAPTURE_BUFS_MAX;
    g_audioStageFrames = FLASHBACK_AUDIO_STAGE_FRAMES;
    flashbackLayout();
}

void flashbackResetStats(void)
{
    g_statFpsX10 = g_statCaptureUs = g_statCompressUs = g_statSizePct = g_statRawPct = g_statWriteUs = 0;
    g_winStart = svcGetSystemTick();
    g_winCaptureTicks = g_winCompressTicks = g_winPackedBytes = 0;
    g_winFrames = g_winRawFrames = 0;
    g_winWriteUs0 = g_writeTotalUs;
    g_winWriteFrames0 = g_writeTotalFrames;
}

static void flashbackUpdateStats(void)
{
    u64 now = svcGetSystemTick();
    u64 elapsed = now - g_winStart;
    if (elapsed < SYSCLOCK_ARM11)
        return;

    g_statFpsX10 = (u32)((u64)g_winFrames * 10 * SYSCLOCK_ARM11 / elapsed);
    if (g_winFrames != 0)
    {
        g_statCaptureUs = (u32)(g_winCaptureTicks * 1000000 / SYSCLOCK_ARM11 / g_winFrames);
        g_statCompressUs = (u32)(g_winCompressTicks * 1000000 / SYSCLOCK_ARM11 / g_winFrames);
        g_statSizePct = (u32)(g_winPackedBytes * 100 / ((u64)g_winFrames * g_slotBytes));
        g_statRawPct = g_winRawFrames * 100 / g_winFrames;
    }

    u32 writeUs = g_writeTotalUs, writeFrames = g_writeTotalFrames;
    if (writeFrames != g_winWriteFrames0)
        g_statWriteUs = (writeUs - g_winWriteUs0) / (writeFrames - g_winWriteFrames0);

    g_winStart = now;
    g_winCaptureTicks = g_winCompressTicks = g_winPackedBytes = 0;
    g_winFrames = g_winRawFrames = 0;
    g_winWriteUs0 = writeUs;
    g_winWriteFrames0 = writeFrames;
}

bool flashbackIsSdMode(void)
{
    s64 out = 0;
    // Never write to CTRNAND: only run when Luma boots from the SD card.
    return R_SUCCEEDED(svcGetSystemInfo(&out, 0x10000, 0x203)) && out != 0;
}

// ---------------------------------------------------------------------------
// Thread, hotkey, menu
// ---------------------------------------------------------------------------

// Hotkey feedback: a semi-transparent tint over the bottom screen. The
// display's color lookup table maps every output level; writing a copy blended
// toward the tint color tints the whole screen while the game stays visible.
// The table in effect before (screen filter or the system's calibration) is
// read back first and restored afterwards.

typedef union FlashbackLutEntry {
    struct { u8 r, g, b, z; };
    u32 raw;
} FlashbackLutEntry;

static u32  g_savedLut[256];
static bool g_lutTinted;

// Like ScreenFiltersMenu_RestoreSettings: GSP must be paused while the table
// changes, or colors can end up glitched. Under a Rosalina menu it already is
// (menuEnter pauses it), so only pause it ourselves otherwise. Holding the draw
// lock keeps menuEnter/menuLeave from changing that state meanwhile.
static bool flashbackPauseGspForLut(void)
{
    if (menuIsOpen())
        return false;
    svcKernelSetState(0x10000, 2);
    svcSleepThread(5 * 1000 * 100LL);
    return true;
}

static void flashbackResumeGspForLut(bool paused)
{
    if (!paused)
        return;
    svcKernelSetState(0x10000, 2);
    svcSleepThread(5 * 1000 * 100LL);
}

static void flashbackWriteTintedLut(u32 color)
{
    FlashbackLutEntry tint = { .raw = color };
    GPU_FB_BOTTOM_COL_LUT_INDEX = 0;
    for (u32 i = 0; i < 256; i++)
    {
        FlashbackLutEntry px = { .raw = g_savedLut[i] };
        px.r += ((s32)tint.r - px.r) * FLASHBACK_TINT_ALPHA / 256;
        px.g += ((s32)tint.g - px.g) * FLASHBACK_TINT_ALPHA / 256;
        px.b += ((s32)tint.b - px.b) * FLASHBACK_TINT_ALPHA / 256;
        GPU_FB_BOTTOM_COL_LUT_ELEM = px.raw;
    }
}

static void flashbackSetTint(u32 color)
{
    Draw_Lock();
    bool paused = flashbackPauseGspForLut();

    if (!g_lutTinted)
    {
        GPU_FB_BOTTOM_COL_LUT_INDEX = 0;
        for (u32 i = 0; i < 256; i++)
            g_savedLut[i] = GPU_FB_BOTTOM_COL_LUT_ELEM;

        // A real table gets brighter from entry 0 to 255. If reading it back
        // gave nothing usable, restore to a neutral table instead.
        FlashbackLutEntry lo = { .raw = g_savedLut[0] }, hi = { .raw = g_savedLut[255] };
        if (hi.r + hi.g + hi.b <= lo.r + lo.g + lo.b + 96)
            for (u32 i = 0; i < 256; i++)
                g_savedLut[i] = i | (i << 8) | (i << 16);

        g_lutTinted = true;
    }

    flashbackWriteTintedLut(color);

    flashbackResumeGspForLut(paused);
    Draw_Unlock();
}

static void flashbackClearTint(void)
{
    Draw_Lock();
    if (g_lutTinted)
    {
        bool paused = flashbackPauseGspForLut();
        GPU_FB_BOTTOM_COL_LUT_INDEX = 0;
        for (u32 i = 0; i < 256; i++)
            GPU_FB_BOTTOM_COL_LUT_ELEM = g_savedLut[i];
        g_lutTinted = false;
        flashbackResumeGspForLut(paused);
    }
    Draw_Unlock();
}

static void flashbackThreadMain(void)
{
    bool sdMode = flashbackIsSdMode();
    if (sdMode)
        flashbackLoadSettings();
    flashbackApplyFormat(g_wantRes, g_wantFpsIdx, g_wantLenIdx, flashbackAudioWanted());

    u64 period = SYSCLOCK_ARM11 / g_fps;
    u64 nextTick = svcGetSystemTick();
    bool wasRecording = false;

    while (!preTerminationRequested)
    {
        if (g_saveRequested)
        {
            bool feedback = g_saveFromHotkey;

            g_saving = true;
            if (feedback)
                flashbackSetTint(FLASHBACK_TINT_SAVING);

            u64 t0 = svcGetSystemTick();
            s32 res = flashbackSave();
            g_lastSaveMs = (u32)((svcGetSystemTick() - t0) * 1000 / SYSCLOCK_ARM11);
            g_lastSaveResult = res;

            if (feedback)
            {
                // Skip the result flash if a menu was opened mid-save: the
                // tint would color the menu instead of the game.
                if (!menuIsOpen())
                {
                    flashbackSetTint(res > 0 ? FLASHBACK_TINT_OK : FLASHBACK_TINT_FAILED);
                    svcSleepThread(300 * 1000 * 1000LL);
                }
                flashbackClearTint();
            }

            g_saveFromHotkey = false;
            g_saving = false;
            g_saveRequested = false;
            nextTick = svcGetSystemTick(); // don't try to catch up on missed frames
            wasRecording = false;          // ...or count them as dropped
        }

        // Settings changed in the menu: the ring layout depends on them, so
        // drop the buffer and restart with the new format.
        if (g_wantRes != g_res || g_wantFpsIdx != g_fpsIdx || g_wantLenIdx != g_lenIdx ||
            flashbackAudioWanted() != g_audio)
        {
            flashbackStop();
            flashbackApplyFormat(g_wantRes, g_wantFpsIdx, g_wantLenIdx, flashbackAudioWanted());
            period = SYSCLOCK_ARM11 / g_fps;
            nextTick = svcGetSystemTick();
            wasRecording = false;
        }

        // Turning recording off clears the buffer (and frees the recorder's
        // memory); turning it back on starts a fresh one.
        if (!g_enabled && g_active)
        {
            flashbackStop();
            wasRecording = false;
        }

        if (g_enabled && !g_active && sdMode && R_FAILED(flashbackStart()))
        {
            g_enabled = false;
            g_recordError = "Couldn't start: not enough memory or SD space.";
        }

        // menuShouldExit: going to sleep / shell closed / terminating.
        // menuIsOpen: the game is paused under a Rosalina menu, so recording
        // would only add frozen frames. Pausing here (rather than counting
        // missed frames) leaves no gap or duplicates in the clip.
        bool recording = g_enabled && g_active && !menuShouldExit && !menuIsOpen();
        flashbackAudioSetCapture(recording && g_audio);
        if (recording && !wasRecording)
        {
            flashbackResetStats();
            flashbackInputResync();     // (leaving out what was pressed in a menu)
        }
        if (recording)
        {
            // Only the capture here: the packer thread compresses. A grid
            // slot without a capture becomes a copy of the frame before it,
            // once that's packed (flashbackPackFrame). Either way, it gets a
            // record of the inputs.
            if (!flashbackCaptureFrame())
                g_pendingMissed++;
            flashbackInputSample(1);
            flashbackUpdateStats();
        }
        wasRecording = recording;

        // A ring write failed (card removed or broken): frames already marked
        // as recorded may be missing from the file, so drop the buffer rather
        // than risk saving stale frames, and stop.
        if (g_writeFailed && g_active)
        {
            flashbackStop();
            g_enabled = false;
            g_recordError = "SD card write failed: recording stopped.";
            wasRecording = false;
        }

        // A save turned the ring into a clip but there wasn't room for a new
        // ring (each one is as big as a full clip): stop recording.
        if (g_ringLost && g_active)
        {
            flashbackStop();
            g_enabled = false;
            g_recordError = "SD card full: recording stopped.";
            wasRecording = false;
        }

        // Frames sit on a fixed time grid (one slot every 'period'). Every
        // grid slot is either recorded or filled with a duplicate, so clips
        // keep real-time speed even when each frame takes longer than one
        // period to capture and write.
        nextTick += period;
        u64 now = svcGetSystemTick();
        if (nextTick > now && recording)
            flashbackInputSleepUntil(nextTick);     // (watching the inputs meanwhile)
        else if (nextTick > now)
            svcSleepThread((s64)((nextTick - now) * 1000000000ULL / SYSCLOCK_ARM11));
        else if (recording)
        {
            // Grid slots that ended before 'now' are lost; record the current
            // slot (the one containing 'now') immediately, slightly late.
            u32 missed = (u32)((now - nextTick) / period);
            g_pendingMissed += missed;      // (copies, as above)
            flashbackInputSample(missed);   // (the inputs as they are now)
            nextTick += (u64)missed * period;
        }
        else
            nextTick = now;
    }

    flashbackStop();    // waits for the writer to finish first

    if (g_packRunning)
    {
        LightEvent_Signal(&g_packEvent);
        MyThread_Join(&flashbackPackThread, -1LL);
    }
    if (g_writerRunning)
    {
        LightEvent_Signal(&g_writeEvent);
        MyThread_Join(&flashbackWriterThread, -1LL);
    }
    if (g_audioRunning)
        MyThread_Join(&flashbackAudioThread, -1LL);
}

MyThread *flashbackCreateThread(void)
{
    LightEvent_Init(&g_writeEvent, RESET_ONESHOT);

    // The capture, writer and packer threads have the lowest priorities among
    // Rosalina's threads and run on the system core. The packer, which does
    // nearly all the work (compressing), is at the bottom, 60; the capture
    // thread is one step above it, so it captures on time, interrupting the
    // packer for the few ms that takes. The writer is level with the capture
    // thread, above the packer: it hardly uses the CPU (it mostly waits on the
    // SD card), and running first means it sends the next write the moment
    // the previous one finishes. (What was tried before: see
    // docs/flashback-design.md, "Threads and priorities".) None of the three
    // is essential to Rosalina: without the writer, frames are written inline,
    // without the packer they're compressed inline, and without the capture
    // thread the recorder stays off. The audio thread, on the other hand, runs
    // well above them: it has to catch each audio frame within the ~8 ms it
    // stays put, so it wakes every millisecond or few, but it uses very little
    // CPU. Without it, there's no audio.
    g_audioRunning = R_SUCCEEDED(MyThread_Create(&flashbackAudioThread, flashbackAudioThreadMain, flashbackAudioStack,
                                                 sizeof(flashbackAudioStack), 25, CORE_SYSTEM));
    g_writerRunning = R_SUCCEEDED(MyThread_Create(&flashbackWriterThread, flashbackWriterThreadMain, flashbackWriterStack,
                                                  sizeof(flashbackWriterStack), 59, CORE_SYSTEM));
    LightEvent_Init(&g_packEvent, RESET_ONESHOT);
    g_packRunning = R_SUCCEEDED(MyThread_Create(&flashbackPackThread, flashbackPackThreadMain, flashbackPackStack,
                                                sizeof(flashbackPackStack), 60, CORE_SYSTEM));
    g_threadRunning = R_SUCCEEDED(MyThread_Create(&flashbackThread, flashbackThreadMain, flashbackThreadStack,
                                                  sizeof(flashbackThreadStack), 59, CORE_SYSTEM));
    return g_threadRunning ? &flashbackThread : NULL;
}

void Flashback_HandleKeys(u32 heldKeys)
{
    static bool wasHeld = false;
    u32 combo = g_saveCombo;
    bool isHeld = (heldKeys & combo) == combo;

    if (isHeld && !wasHeld && g_enabled && !g_saveRequested)
    {
        g_saveFromHotkey = true;
        g_saveRequested = true;
    }
    wasHeld = isHeld;
}
