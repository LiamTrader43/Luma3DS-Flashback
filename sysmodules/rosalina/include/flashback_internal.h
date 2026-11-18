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

// Which screens are recorded.
typedef enum FlashbackScreens {
    FLASHBACK_SCREENS_TOP = 0,  // the top screen
    FLASHBACK_SCREENS_BOTH,     // and the bottom screen (320 x 240, or 160 x 120 at half resolution)
    FLASHBACK_SCREENS_COUNT,
} FlashbackScreens;

// With both recorded, which screen comes first. The first screen is recorded
// as when it's recorded alone: captured with every frame, and its frames always
// stored. The other one, the second screen, gets the time and the room that
// are left: it isn't captured with every frame, and a frame of its that doesn't
// fit isn't stored (see flashbackCapture, flashbackPackFrame). So when an Old
// 3DS or a slow SD card can't keep up with both, it's the second screen that
// goes without. With one screen recorded, that's the first.
typedef enum FlashbackPriority {
    FLASHBACK_PRIORITY_TOP = 0,
    FLASHBACK_PRIORITY_BOTTOM,
    FLASHBACK_PRIORITY_COUNT,
} FlashbackPriority;

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
extern bool g_packOnGameCore;
extern bool g_writerRunning;

// Settings, and saves (flashback.c).
extern volatile bool g_enabled;
extern volatile u32  g_wantRes;
extern volatile u32  g_wantScreens;
extern volatile u32  g_wantPriority;
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
extern volatile u32 g_screens;
extern volatile u32 g_priority;            // FlashbackPriority in effect (the top screen's, with one screen)
extern u32 g_w, g_h, g_fps, g_frames;
extern u32 g_slotBytes, g_frameOffset, g_memSize;
extern u32 g_slotStride;
extern u32 g_botW, g_botH, g_botBytes;     // the bottom screen's frames (0 when it isn't recorded)
// The same sizes by which screen comes first (flashbackLayout).
extern bool g_priBottom;                   // the first screen is the bottom one
extern u32 g_priW, g_priH, g_priBytes;     // the first screen's frames
extern u32 g_secW, g_secH, g_secBytes;     // the second screen's (0 when only one is recorded)
extern u32 g_secOffset, g_secStride;       // the second screen's capture buffers in g_mem: the first, and bytes between them
extern u32 g_ringSlots;                    // the ring's slots (flashbackRingSlots)

// A ring slot holds what's new at one moment (a grid slot of the recording):
// the top screen's frame, the bottom screen's, or both. Nothing is stored for a
// screen that's the same as in the frame before, and no slot at all is used
// when both are: the slot before it is shown for one more grid slot instead.
// So a still picture costs the SD card nothing.
//
// A frame is stored whole, or as what changed since the frame before it on the
// same screen (see the top of flashback_compress.c), which takes far less
// while much of the picture stays put. Reading such a frame takes the frames
// before it, back to a whole one. So every FLASHBACK_KEY_SLOTS slots, each
// screen is stored whole whatever it shows (the first screen's frame in every
// FLASHBACK_KEY_SLOTS-th slot, the second screen's with its next frame from
// then on), and a clip starts with the slots from the ones with whole frames
// before its first frame (see flashbackPlanClip): at most FLASHBACK_LEAD_MAX
// back, which allows for a second screen that's captured a few frames late.
//
// Both screens' frames go in the same slot: the top screen's, then the bottom
// screen's, whichever comes first. The second screen's whole frame, when it's
// due and there isn't room for it there, gets a slot of its own, the one
// before (FLASHBACK_FRAME_UNSHOWN): a slot that's only read, for what it
// leaves on that screen, and shown for no time. So the ring has more slots
// than the longest clip has frames (flashbackRingSlots): the ones a clip
// starts with, and with both screens recorded, one in every
// FLASHBACK_KEY_SLOTS for those.
#define FLASHBACK_KEY_SLOTS         30
#define FLASHBACK_LEAD_MAX          (FLASHBACK_KEY_SLOTS + 8)

// How a slot's frames are stored (also in a clip's index: flashback_clip.c).
#define FLASHBACK_FRAME_TOP_DELTA       0x01    // the top screen's: what changed since the frame before
#define FLASHBACK_FRAME_TOP_CODES       0x02    // ...packed with Huffman codes
#define FLASHBACK_FRAME_BOTTOM_DELTA    0x04    // the same, for the bottom screen's
#define FLASHBACK_FRAME_BOTTOM_CODES    0x08
#define FLASHBACK_FRAME_STORED          0x0F    // (the ones a clip's index has)
#define FLASHBACK_FRAME_UNSHOWN         0x10    // in the ring only: the slot is shown for no time (0 copies in a clip's index)

u32 flashbackRingSlots(u32 frames, bool bottom);

// The ring (flashback_record.c). What's known about each of its slots is in
// g_mem, g_ringSlots of each (flashbackLayout, flashbackUseMem).
extern bool  g_active;
extern u8   *g_mem;
extern IFile g_ring;
extern u32   g_head;
extern volatile u32 g_count;
extern u16  *g_repeat;          // grid slots a slot is shown for after its own...
extern u16  *g_missed;          // ...of which because their capture was missed (the others: nothing had changed)
extern u32  *g_slotLen;         // the top screen's frame's size (0: the same as before)
extern u32  *g_slotBotLen;      // the bottom screen's, after it (0: the same as before, or not recorded)
extern u8   *g_slotFlags;       // FLASHBACK_FRAME_*

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
//
// With more than one buffer, the one before the frame being packed is never
// captured into: it holds the frame before, which this one is compared with
// (see flashbackPackFrame). So one fewer frame can wait than there are buffers.
//
// The second screen has as many buffers of its own (g_secOffset), filled only
// when it's captured, which isn't with every frame (see flashbackCapture). One
// of them holds the last of its frames that was stored, which the next ones
// are compared with, however many frames go by (and however many of its frames
// aren't stored: see flashbackPackFrame); a capture goes into one that neither
// holds that nor a frame waiting to be packed. There's always one: at most
// g_capBufs - 1 frames wait.
#define FLASHBACK_CAPTURE_BUFS_MAX  4
#define FLASHBACK_NO_BUFFER         0xFF

// How much of a frame is as it was in the one before (flashbackSameShare).
#define FLASHBACK_SAME_ALL          256     // all of it
#define FLASHBACK_SAME_STEP         1024    // bytes between the words compared first
// A frame is stored as what changed only if at least this much of it (of 255)
// hasn't: half. With less, that takes an Old 3DS longer than storing it whole
// does (in assembly), and saves little or nothing: on clips of Smash Bros., a
// quarter instead made frames 14% bigger. Mario Kart 7 and Smash Bros. are
// mostly under it; 2D games and still cameras are far over.
#define FLASHBACK_DELTA_MIN_SAME    128

// The second screen is captured after the first when its read fits before the
// game's next buffer swap, and otherwise at least every this many frames, after
// waiting the swap out; on an Old 3DS, only every this many frames (see
// flashbackCapture). Except while its whole frame is due (g_secKeyDue, the
// packer's: see flashbackPackFrame), when it's captured with the next frame.
#define FLASHBACK_BOTTOM_MAX_GAP    2

extern volatile bool g_secKeyDue;
extern u32 g_capBufs;
extern u32 g_capStride;
extern u32 g_pendingMissed;
extern u64 g_capWaited;
extern LightEvent g_packEvent;

// Huffman codes over compressed frames (flashbackHuffman), on a New 3DS: a
// frame is compressed into g_packedOffset in g_mem first, then packed with
// codes into staging. What packing keeps from one frame to the next, and builds
// the codes with, is in g_mem too (flashbackLayout, flashbackUseMem). Packer
// thread.
#define FLASHBACK_HUFF_MAX_BITS     12      // the longest code
typedef struct FlashbackHuffman {
    u32 counts[2][256];     // the last frame's bytes (op bytes, then the bytes after ops), then this one's
    u32 codes[2][256];      // this frame's codes: the code (first bit lowest) | its length << 16
    u32 order[256];         // building: the byte values by count (count << 8 | value)
    u32 weight[511];        // ...the leaves in that order, then the nodes as they're made
    u16 parent[511];        // ...each one's parent
    u8  depth[511];         // ...and how deep it is
} FlashbackHuffman;

extern u32 g_packedOffset;
extern FlashbackHuffman *g_huff;    // two: the top screen's, then the bottom screen's

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
u32 flashbackSameShare(const u16 *frame, const u16 *ref, u32 bytes, u32 *sameUpTo);
u32 flashbackCompressDelta(const u16 *cols, const u16 *ref, u32 pixels, u8 *dst, u32 cap, u32 start);
u32 flashbackHuffman(FlashbackHuffman *h, const u8 *src, u32 len, u8 *dst);
void flashbackColumnsToRows(const u16 *cols, u32 width, u32 height, u16 *dst);

// flashback_framebuffer.c
void flashbackCaptureWait(void);
bool flashbackCapture(u16 *first, u16 *second, bool *secondOk);

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

// The screen that comes first, as chosen in the menu: the top one, when only
// it is recorded.
static inline u32 flashbackPriorityWanted(void)
{
    return g_wantScreens == FLASHBACK_SCREENS_BOTH ? g_wantPriority : (u32)FLASHBACK_PRIORITY_TOP;
}

// Audio is always recorded, if the audio thread could be started.
static inline bool flashbackAudioWanted(void)
{
    return g_audioRunning;
}

// FS priority for the recorder's files: the ring, and a clip as it's saved.
// Rosalina's FS session runs at real-time priority, ahead of the game's own SD
// reads (streamed music...); the ring's constant writes must not get ahead of
// them, and the lowest priority costs them nothing (see
// docs/flashback-design.md, "SD card writes"). Nor must the big jobs: sizing a
// new ring (up to 3.6 GB to claim on the card), or writing a clip's index.
#define FLASHBACK_FS_PRIORITY   8

// flashback_record.c
Result flashbackCreateDirectory(FS_Archive archive, const char *path);
void flashbackWriterThreadMain(void);
void flashbackDrainWrites(void);
Result flashbackOpenRing(FS_Archive archive);
void flashbackForgetFrames(void);
void flashbackNewRing(void);
Result flashbackStart(void);
void flashbackDrainCaptures(void);
void flashbackStop(void);
bool flashbackCaptureFrame(void);
void flashbackPackThreadMain(void);

// flashback_clip.c
// Which ring slots make up a clip of the chosen length, oldest first, with
// each slot shown 1 + g_repeat[] times (while nothing changed, or captures
// were missed). Before them come the 'lead' slots a reader needs to make out
// the first one: from the last slot with whole frames on (read, not shown).
typedef struct FlashbackClipPlan {
    u32 slots;          // number of ring slots shown
    u32 trimFirst;      // repeats to drop from the oldest slot (clip would be too long)
    u32 frames;         // frames in the clip, duplicates included
    u32 lead;           // slots before them, only read
    u32 missed;         // frames whose capture was missed
} FlashbackClipPlan;
FlashbackClipPlan flashbackPlanClip(void);
u32 flashbackClipTailMax(u32 frames);
s32 flashbackSave(void);

// flashback_menu.c
void flashbackLoadSettings(void);
