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

// The Flashback recorder's internals: what its source files share. (What it
// does: flashback.h.)
//
//   flashback.c              the recorder's thread, its settings and recording
//                            format, its stats, and the save hotkey
//   flashback_framebuffer.c  capturing frames from the screen (with flashback_capture.s)
//   flashback_compress.c     compressing frames (with flashback_pack.s)
//   flashback_audio.c        recording the game's audio
//   flashback_input.c        recording the inputs (buttons, Circle Pad, touch screen)
//   flashback_record.c       the capture queue, staging, the writer, and starting
//                            and stopping recording
//   flashback_clip.c         saving clips
//   flashback_menu.c         the Flashback screen and the settings file
//
// Framebuffer layout reminder: screen buffers are stored rotated 90deg. Each
// screen column x is one contiguous run of 240 pixels at pa + x * stride, and
// within a column, offset y * bytesPerPixel is screen row (239 - y), i.e. y = 0
// is the BOTTOM row.

#pragma once

#include <3ds.h>
#include "flashback.h"
#include "ifile.h"

// User settings.
typedef enum FlashbackResolution {
    FLASHBACK_RES_HALF = 0,     // 200 x 120
    FLASHBACK_RES_FULL,         // 400 x 240
    FLASHBACK_RES_COUNT,
} FlashbackResolution;

static const u32 g_fpsChoices[] = { 10, 20, 30 };
#define FLASHBACK_FPS_CHOICES   (sizeof(g_fpsChoices) / sizeof(g_fpsChoices[0]))

// Clip length in seconds (how much the ring buffer holds), set digit by digit
// in the menu. At the longest, full resolution and 30 fps, the ring file (and
// a saved clip) is about 3.6 GB: under the 4 GB a file can be on the SD card
// (FAT32), with clip offsets still fitting in 32 bits.
#define FLASHBACK_MIN_SECONDS       1
#define FLASHBACK_MAX_SECONDS       600     // 10 minutes
#define FLASHBACK_DEFAULT_SECONDS   30

// The recorder's threads (flashback.c).
extern bool g_threadRunning;
extern bool g_audioRunning;
extern bool g_packRunning;
extern bool g_writerRunning;

// Settings, and saves (flashback.c).
extern volatile bool g_enabled;
extern volatile u32  g_wantRes;
extern volatile u32  g_wantFpsIdx;
extern volatile u32  g_wantSeconds;
extern volatile u32  g_saveCombo;
extern volatile bool g_saveRequested;
extern volatile bool g_saveFromHotkey;
extern volatile bool g_saving;
extern volatile s32  g_lastSaveResult;
extern char g_lastClip[96];

// The recording format (flashback.c).
extern volatile u32 g_res;
extern volatile u32 g_fpsIdx;
extern volatile u32 g_seconds;
extern u32 g_w, g_h, g_fps, g_frames;
extern u32 g_slotBytes, g_frameOffset, g_memSize;
extern u32 g_slotStride;

// The ring (flashback_record.c). What's known about each of its slots is in
// g_mem, g_frames of each (flashbackLayout, flashbackUseMem).
extern bool  g_active;
extern u8   *g_mem;
extern IFile g_ring;
extern u32   g_head;
extern volatile u32 g_count;
extern u16  *g_repeat;
extern u32  *g_slotLen;

// Audio (flashback_audio.c).
#define FLASHBACK_AUDIO_FRAME_SAMPLES   160
#define FLASHBACK_AUDIO_FRAME_BYTES     (FLASHBACK_AUDIO_FRAME_SAMPLES * 4)     // stereo PCM16
#define FLASHBACK_AUDIO_FRAME_TICKS     (FLASHBACK_AUDIO_FRAME_SAMPLES * 8192)  // ARM11 ticks per audio frame
#define FLASHBACK_AUDIO_RATE_MHZ        32728498    // SYSCLOCK_ARM11 / 8192, in millihertz
// Audio is written in chunks of this many frames (40960 bytes, a whole number
// of SD sectors; each write request costs the card ~9 ms however small, so
// fewer, bigger ones leave it more time for video). The ring and the staging
// memory hold whole chunks, so a chunk never wraps around in either.
#define FLASHBACK_AUDIO_CHUNK_FRAMES    64
// Audio staging: enough for the writer to fall behind for a while. In Ocarina
// of Time 3D, writes regularly stall for 0.4-0.8 s while the game reads the SD
// card (video frames get dropped then); audio staging covers that. If there
// isn't memory for it, recording starts with the smaller amount instead.
#define FLASHBACK_AUDIO_STAGE_FRAMES    512         // ~2.5 s
#define FLASHBACK_AUDIO_STAGE_FRAMES_MIN 128        // ~0.6 s
// The audio ring holds this much more than the clip length, so a clip's audio
// is all still there when it's saved.
#define FLASHBACK_AUDIO_EXTRA_SECONDS   2

extern volatile bool g_wantAudio;
extern volatile bool g_audio;
extern u32 g_audioCapFrames;
extern u32 g_audioBytes;
extern u32 g_videoOffset;
extern u32 g_audioStageOffset;
extern u32 g_audioStageFrames;
extern volatile u32 g_audioProduced, g_audioConsumed, g_audioFlushed;
extern volatile u32 g_audioMissed;
extern volatile u32 g_audioFull;
extern volatile bool g_audioFlushRequest;
extern u32  *g_slotAudioFrame;

// Staging and the capture queue (flashback_record.c).
//
// Frames waiting to be written to the ring file. The packer thread compresses
// captured frames into staging memory, and the writer thread writes them out,
// so compressing the next frame overlaps with writing the previous ones.
// (Compression is what makes full resolution at 30 fps fit the SD card's
// speed.)
//
// The card also pauses now and then, and some cards slow down for seconds at
// a time; every frame captured meanwhile waits in staging. So there's as much
// staging as there's memory for: FLASHBACK_STAGE_SLOTS_BASE uncompressed
// frames' worth (~3 MB at full resolution), halving down to
// FLASHBACK_STAGE_SLOTS_MIN if memory is short, and growing up to
// FLASHBACK_STAGE_SLOTS_MAX (~25 MB) while that leaves FLASHBACK_MEM_MARGIN of
// the system's memory free (the HOME Menu, applets, and Rosalina's menu and
// screenshots need some). A New 3DS takes the most; an Old 3DS stays at the
// base. (How much cards need, and how these were chosen: see
// docs/flashback-design.md, "Staging".)
#define FLASHBACK_STAGE_SLOTS_BASE  16
#define FLASHBACK_STAGE_SLOTS_MAX   128
#define FLASHBACK_STAGE_SLOTS_MIN   4
#define FLASHBACK_MEM_MARGIN        (24 * 1024 * 1024)

extern u32 g_stageSlots;
extern u32 g_stageBytes;
extern volatile bool g_writeFailed;
extern bool g_ringLost;
extern LightEvent g_writeEvent;

// Frames captured and waiting to be compressed. The capture thread only
// captures, on the frame grid, into one of g_capBufs capture buffers; the
// packer thread compresses them, in order, into staging. So a frame that's
// slow to compress (the processor held up for a while) no longer makes the
// next capture late: the packer catches up within a few frames (see
// docs/flashback-design.md, "The capture queue"). As many buffers as there's
// memory for (see flashbackStart), up to FLASHBACK_CAPTURE_BUFS_MAX; with 1
// the capture waits for each compression again. Frame number p goes to buffer
// p % g_capBufs (a power of two, as with the staging entries).
#define FLASHBACK_CAPTURE_BUFS_MAX  4

extern u32 g_capBufs;
extern u32 g_capStride;
extern u32 g_pendingMissed;
extern LightEvent g_packEvent;

// Inputs (flashback_input.c): a record of them for every grid slot, in a ring
// of g_frames records in g_mem from g_inputOffset (flashbackLayout).
typedef struct FlashbackInput {
    u32 buttons;            // KEY_* held at any time during the frame (KEY_TOUCH: the touch screen)
    s16 circleX, circleY;   // the Circle Pad, as HID reports it (about -156 to 156)
    u16 touchX, touchY;     // where the touch screen was touched (with KEY_TOUCH)
    s16 cstickX, cstickY;   // the C-Stick (New 3DS): not recorded yet, 0
} FlashbackInput;
_Static_assert(sizeof(FlashbackInput) == 16, "an input record is 16 bytes");

extern u32 g_inputOffset;

// Stats (flashback.c).
extern volatile u32 g_statFpsX10;
extern volatile u32 g_statCaptureUs;
extern volatile u32 g_statCompressUs;
extern volatile u32 g_statSizePct;
extern volatile u32 g_statRawPct;
extern volatile u32 g_statWriteUs;
extern volatile u32 g_lastSaveMs;
extern const char *volatile g_recordError;
extern volatile u32 g_writeTotalUs, g_writeTotalFrames;
extern u64 g_winCaptureTicks, g_winCompressTicks, g_winPackedBytes;
extern u32 g_winFrames, g_winRawFrames;

// flashback.c
u32 flashbackAudioFramesFor(u32 seconds);
void flashbackLayout(void);
void flashbackUseMem(u8 *mem);
u32 flashbackSlotStride(u32 slotBytes);
u32 flashbackVideoOffset(u32 audioBytes);
void flashbackResetStats(void);
bool flashbackIsSdMode(void);

// flashback_compress.c
u32 flashbackCompress(const u16 *cols, u32 width, u32 height, u8 *dst, u32 cap);
void flashbackColumnsToRows(const u16 *cols, u32 width, u32 height, u16 *dst);

// flashback_framebuffer.c
bool flashbackCapture(u16 *dst);

// flashback_input.c
void flashbackInputReset(void);
void flashbackInputResync(void);
void flashbackInputPoll(void);
void flashbackInputSample(u32 slots);
void flashbackInputLatest(u32 count, FlashbackInput *dst);
void flashbackInputSleepUntil(u64 tick);

// flashback_audio.c
void flashbackAudioSetCapture(bool on);
void flashbackAudioPause(void);
void flashbackAudioReset(void);
void flashbackAudioThreadMain(void);

// Audio samples in 'frames' video frames (at the recording's frame rate).
static inline u32 flashbackAudioSamplesFor(u32 frames)
{
    return (u32)((u64)frames * SYSCLOCK_ARM11 / ((u64)g_fps * 8192));
}

// Audio is recorded only if the audio thread could be started.
static inline bool flashbackAudioWanted(void)
{
    return g_wantAudio && g_audioRunning;
}

// flashback_record.c
Result flashbackCreateDirectory(FS_Archive archive, const char *path);
void flashbackWriterThreadMain(void);
void flashbackDrainWrites(void);
Result flashbackOpenRing(FS_Archive archive);
Result flashbackStart(void);
void flashbackDrainCaptures(void);
void flashbackStop(void);
bool flashbackCaptureFrame(void);
void flashbackPackThreadMain(void);

// flashback_clip.c
// Which ring slots make up a clip of the chosen length, oldest first,
// with each slot repeated 1 + g_repeat[] times to fill missed frames.
typedef struct FlashbackClipPlan {
    u32 slots;          // number of ring slots used
    u32 trimFirst;      // repeats to drop from the oldest slot (clip would be too long)
    u32 frames;         // frames in the clip, duplicates included
} FlashbackClipPlan;
FlashbackClipPlan flashbackPlanClip(void);
u32 flashbackClipTailMax(u32 frames);
s32 flashbackSave(void);

// flashback_menu.c
void flashbackLoadSettings(void);
