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

// See flashback.h.
//
// Framebuffer layout reminder: screen buffers are stored rotated 90deg. Each
// screen column x is one contiguous run of 240 pixels at pa + x * stride, and
// within a column, offset y * bytesPerPixel is screen row (239 - y), i.e. y = 0
// is the BOTTOM row.

#include <3ds.h>
#include <string.h>
#include "flashback.h"
#include "csvc.h"
#include "draw.h"
#include "fmt.h"
#include "ifile.h"
#include "luma_config.h"
#include "menu.h"
#include "utils.h"

// Work memory (staging for frames and audio, and a capture buffer), allocated
// from the SYSTEM region when recording starts, never from the running game,
// and sized for the current resolution. The address is clear of the
// screenshot/menu framebuffer cache at 0x0D000000.
#define FLASHBACK_MEM_ADDR      0x0D800000

// FS priority for the ring file. Rosalina's FS session runs at real-time
// priority; the ring file's constant writes must not get ahead of the game.
// Measured on a New 3DS XL at full resolution / 30 fps: write time per frame
// was the same at low and normal priority (~36 ms) and slightly worse at
// real-time (~41 ms), so the lowest priority costs nothing.
#define FLASHBACK_RING_FS_PRIORITY  8

// Semi-transparent bottom screen tints used as feedback for the hotkey
// (0x00BBGGRR), blended over the screen through the display's color lookup table.
#define FLASHBACK_TINT_SAVING   0x00FFFFFF  // white
#define FLASHBACK_TINT_OK       0x0000C000  // green
#define FLASHBACK_TINT_FAILED   0x000000E0  // red
#define FLASHBACK_TINT_ALPHA    128         // tint strength, out of 256 (50%)

#define KERNPA2VA_OFFSET()      (GET_VERSION_MINOR(osGetKernelVersion()) < 44 ? 0xD0000000 : 0xC0000000)

// User settings.
typedef enum FlashbackResolution {
    FLASHBACK_RES_HALF = 0,     // 200 x 120
    FLASHBACK_RES_FULL,         // 400 x 240
    FLASHBACK_RES_COUNT,
} FlashbackResolution;

static const u32 g_fpsChoices[] = { 10, 20, 30 };
#define FLASHBACK_FPS_CHOICES   (sizeof(g_fpsChoices) / sizeof(g_fpsChoices[0]))

// Clip length in seconds (how much the ring buffer holds).
static const u32 g_lengthChoices[] = { 10, 20, 30, 40, 50, 60 };
#define FLASHBACK_LENGTH_CHOICES (sizeof(g_lengthChoices) / sizeof(g_lengthChoices[0]))
#define FLASHBACK_DEFAULT_LENGTH_IDX 0      // 10 seconds
#define FLASHBACK_MAX_FRAMES    (60 * 30)   // longest length at the highest frame rate

static MyThread flashbackThread;
static u8 CTR_ALIGN(8) flashbackThreadStack[0x2000];
static bool g_threadRunning;

static volatile bool g_enabled;         // user toggle
// Defaults when no settings are saved: full resolution, 30 fps on New 3DS;
// half resolution, 30 fps on Old 3DS (see flashbackApplyDefaults).
static volatile u32  g_wantRes = FLASHBACK_RES_FULL;   // FlashbackResolution chosen in the menu
static volatile u32  g_wantFpsIdx = 2;                 // index into g_fpsChoices chosen in the menu (30 fps)
static volatile u32  g_wantLenIdx = FLASHBACK_DEFAULT_LENGTH_IDX; // index into g_lengthChoices chosen in the menu
static volatile u32  g_saveCombo = FLASHBACK_DEFAULT_SAVE_COMBO;

// Buttons allowed in the save hotkey: face buttons, D-Pad, L/R, ZL/ZR,
// Start/Select (not the touchscreen, Circle Pad or C-Stick).
#define FLASHBACK_COMBO_KEYS    (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R | KEY_ZL | KEY_ZR | \
                                 KEY_START | KEY_SELECT | KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)
static volatile bool g_saveRequested;
static volatile bool g_saveFromHotkey;  // show bottom-screen feedback
static volatile bool g_saving;
static volatile s32  g_lastSaveResult;  // frames written, or < 0 on error (0 = never saved)
static char g_lastClip[96];

// Recording format currently in effect. Written only by the flashback thread
// (when nothing is recording); read by the menu for display.
static volatile u32 g_res = FLASHBACK_RES_FULL;
static volatile u32 g_fpsIdx = 2;
static volatile u32 g_lenIdx = FLASHBACK_DEFAULT_LENGTH_IDX;
static u32 g_w, g_h, g_fps, g_frames;   // derived from g_res / g_fpsIdx / g_lenIdx
static u32 g_slotBytes, g_frameOffset, g_memSize;
static u32 g_slotStride;                // ring and staging slot size: g_slotBytes rounded up (flashbackSlotStride)

// Rows of a frame stored as it is turned upright at a time (see
// flashbackUprightBand): a cache line holds 16 pixels of a column.
#define FLASHBACK_BAND_ROWS     16

// Owned by the flashback thread.
static bool  g_active;                  // memory allocated + ring file open
static u8   *g_mem;
static IFile g_ring;
static u32   g_head;                    // next ring slot to write
static volatile u32 g_count;            // valid ring slots

// Frames that should have been recorded after each slot but weren't (the
// thread fell behind, or a capture/write failed). Saving repeats the slot's
// frame that many extra times so clips keep real-time speed. Kept in RAM
// rather than writing duplicates, since a slow SD write is the usual cause.
static u16   g_repeat[FLASHBACK_MAX_FRAMES];

// Size of each ring slot's contents: the compressed frame, or g_slotBytes if
// the frame was stored uncompressed (it wouldn't have gotten smaller).
static u32   g_slotLen[FLASHBACK_MAX_FRAMES];

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
//
// The DSP mixes everything the game plays 160 samples at a time (an "audio
// frame", ~4.9 ms at its 32728 Hz rate, SYSCLOCK_ARM11 / 8192) and leaves each
// mix ("final mix samples": 160 stereo PCM16 samples, what the speakers get)
// in one of the two shared memory regions it uses with the game, at physical
// 0x1FF50000 and 0x1FF70000. The two take turns: once the DSP has written a
// region's frame, the game advances that region's frame counter (its last
// u16), so the region with the newer counter holds the newest complete frame,
// and the DSP doesn't write that region again for two frames (~9.8 ms).
// (Layout from Citra/Azahar's notes on the DSP firmware. Checked on a New 3DS
// with a homebrew app and in Ocarina of Time 3D: polling about once a
// millisecond read every frame back complete and in order.)
//
// The audio thread copies each new frame into staging memory, and the writer
// thread writes staged frames to an audio ring at the start of the ring file,
// so a save is still just a rename. Each video frame records how many audio
// samples had been captured when it was, which places it in the audio. Frames
// the audio thread misses, and stretches where the DSP produces none (no game
// audio running), are filled with silence, so the audio keeps real time like
// the video.

#define FLASHBACK_DSP_REGION0           0x1FF50000
#define FLASHBACK_DSP_REGION1           0x1FF70000
#define FLASHBACK_DSP_FINAL_OFFSET      0xA80       // DSP address 0x8540 (word) in region 0
#define FLASHBACK_DSP_COUNTER_OFFSET    0x7FFE      // DSP address 0xBFFF: the frame counter
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

static volatile bool g_wantAudio = true;    // chosen in the menu
static volatile bool g_audio;               // in effect (part of the recording format)
static u32 g_audioCapFrames;                // audio ring capacity in frames (0 without audio)
static u32 g_audioBytes;                    // the audio ring's size
static u32 g_videoOffset;                   // where the video slots start in the ring file (flashbackVideoOffset)
static u32 g_audioStageOffset;              // audio staging memory, in g_mem...
static u32 g_audioStageFrames;              // ...and its size in frames (a whole number of chunks)

// Audio frames since the ring started: captured (audio thread), written in
// whole chunks (writer; frees their staging), and on the SD card (writer; ahead
// of g_audioConsumed after an unfinished chunk was written for a save).
static volatile u32 g_audioProduced, g_audioConsumed, g_audioFlushed;
static volatile u32 g_audioMissed;          // frames filled with silence because the thread missed them
static volatile u32 g_audioFull;            // ...of which because the staging memory was full (the SD card was busy)
static volatile bool g_audioCapture;        // capture thread -> audio thread: capture now...
static volatile u64 g_audioCaptureSince;    // ...since this tick...
static volatile u64 g_audioCapturedTicks;   // ...after this much time captured before (since the ring started)
static volatile bool g_audioIdle = true;    // audio thread -> capture thread: not capturing
static volatile bool g_audioFlushRequest;   // writer: write unfinished chunks too (for a save)

// Audio position (audio frames since the ring started) of each ring slot's frame.
static u32   g_slotAudioFrame[FLASHBACK_MAX_FRAMES];

static MyThread flashbackAudioThread;
static u8 CTR_ALIGN(8) flashbackAudioStack[0x1000];
static bool g_audioRunning;                 // the audio thread exists

// Frames waiting to be written to the ring file. The packer thread compresses
// captured frames into staging slots, and the writer thread writes them out,
// so compressing the next frame overlaps with writing the previous ones.
// On a New 3DS XL the SD path tops out around 5 MB/s, which is less than full
// resolution at 30 fps needs uncompressed (5.8 MB/s); compression is what
// closes that gap. (Batching several frames per SD request was tried and
// didn't help: the time is in moving the bytes, not in each request.)
//
// The card also pauses now and then (single writes of 100-200 ms on an Old 3DS,
// where the average write is barely under a frame's time), and every frame
// captured meanwhile waits in a slot. So there are as many slots as there's
// memory for, up to FLASHBACK_STAGE_SLOTS_MAX (~3 MB at full resolution): an
// Old 3DS in Mario Kart 7 dropped ~50 frames in 30 s with 4 slots, waiting on
// the card, and none with 16. Always a power of two (see below).
#define FLASHBACK_STAGE_SLOTS_MAX   16
#define FLASHBACK_STAGE_SLOTS_MIN   4

// Staging slots are used in order: frame number p (counting every frame ever
// staged) goes to staging slot p % g_stageSlots, independent of its ring slot
// (stored next to it), so any ring size works. g_stageProduced -
// g_stageConsumed frames are waiting, oldest first, starting at staging slot
// g_stageConsumed % g_stageSlots. Both counters only ever increase (and stay
// in step with the slots when they wrap, g_stageSlots being a power of two).
static u32 g_stageSlots;                // staging slots in use (flashbackStart)
static u32 g_stageRingIdx[FLASHBACK_STAGE_SLOTS_MAX];  // ring slot of each staged frame
static u32 g_stageLen[FLASHBACK_STAGE_SLOTS_MAX];      // bytes to write for each staged frame
static volatile u32 g_stageProduced;    // frames staged (packer thread)
static volatile u32 g_stageConsumed;    // frames written or given up on (writer thread)
static volatile bool g_writeFailed;     // a ring write failed (writer -> capture thread)
static bool g_ringLost;                 // a save couldn't reopen/recreate the ring file
static LightEvent g_writeEvent;         // signaled when a frame is staged

// Frames captured and waiting to be compressed. The capture thread only
// captures, on the frame grid, into one of g_capBufs capture buffers; the
// packer thread compresses them, in order, into staging slots. So a frame
// that's slow to compress (the processor held up for a while) no longer makes
// the next capture late: the packer catches up within a few frames. (An Old
// 3DS in Mario Kart 7 at full resolution: a few frames dropped in 30 s with
// the capture waiting for each compression, none with the buffers.) As many
// buffers as there's memory for (see flashbackStart), up to
// FLASHBACK_CAPTURE_BUFS_MAX; with 1 the capture waits for each compression
// again. Frame number p goes to buffer p % g_capBufs (a power of two, as with
// the staging slots).
#define FLASHBACK_CAPTURE_BUFS_MAX  4
static u32 g_capBufs;                   // capture buffers in use (flashbackStart)
static u32 g_capStride;                 // bytes between them (the first at g_frameOffset)
static volatile u32 g_capProduced;      // frames captured (capture thread)
static volatile u32 g_capConsumed;      // frames packed (packer thread): their buffers are free again
static u32 g_capAudioFrame[FLASHBACK_CAPTURE_BUFS_MAX];    // where the audio was at each capture
static u32 g_capMissedBefore[FLASHBACK_CAPTURE_BUFS_MAX];  // grid slots missed just before it (copies of the frame before)
static u64 g_capTicks[FLASHBACK_CAPTURE_BUFS_MAX];         // how long each capture took (stats)
static volatile u32 g_capTicksTotal;    // all captures' time, wrapping (stats: compressing leaves it out)
static u32 g_pendingMissed;             // capture thread: grid slots missed since the last capture
static LightEvent g_packEvent;          // signaled when a frame is captured
static MyThread flashbackPackThread;
static u8 CTR_ALIGN(8) flashbackPackStack[0x1000];
static bool g_packRunning;              // false: frames are packed inline by the capture thread

static MyThread flashbackWriterThread;
static u8 CTR_ALIGN(8) flashbackWriterStack[0x1000];
static bool g_writerRunning;            // false: frames are written inline as they're packed

// Performance stats shown in the menu, updated about once a second while recording.
static volatile u32 g_statFpsX10;       // frames actually recorded per second, x10
static volatile u32 g_statCaptureUs;    // average time to read + convert one frame
static volatile u32 g_statCompressUs;   // average time to compress one frame (or turn it upright, if stored as it is)
static volatile u32 g_statSizePct;      // average compressed size, % of the uncompressed frame
static volatile u32 g_statRawPct;       // frames stored as they are (not compressed), %
static volatile u32 g_statWriteUs;      // average SD write time per frame
static volatile u32 g_lastSaveMs;       // duration of the last save
static const char *volatile g_recordError; // why recording switched itself off, or NULL

// Written by the writer thread only, never reset (wrap-around safe deltas).
static volatile u32 g_writeTotalUs, g_writeTotalFrames;

static u64 g_winStart, g_winCaptureTicks, g_winCompressTicks, g_winPackedBytes;
static u32 g_winFrames, g_winRawFrames, g_winWriteUs0, g_winWriteFrames0;

static u32 flashbackAudioFramesFor(u32 seconds)
{
    // (64-bit: SYSCLOCK_ARM11 is a 32-bit constant, and 16 s of ticks don't fit in 32 bits.)
    u32 frames = (u32)((u64)(seconds + FLASHBACK_AUDIO_EXTRA_SECONDS) * SYSCLOCK_ARM11 / FLASHBACK_AUDIO_FRAME_TICKS) + 1;
    return (frames + FLASHBACK_AUDIO_CHUNK_FRAMES - 1) / FLASHBACK_AUDIO_CHUNK_FRAMES * FLASHBACK_AUDIO_CHUNK_FRAMES;
}

// Where things are in the recorder's memory, for g_stageSlots staging slots,
// g_capBufs capture buffers and g_audioStageFrames of audio staging: the
// staging slots first, then the capture buffers, which hold frames as captured
// (before compression) while recording, the first of them the clip's index
// while saving (the two never overlap in time: see flashbackDrainCaptures),
// each with a column's worth after it that compressing reads past the frame
// (flashbackCompress), then the audio staging.
static void flashbackLayout(void)
{
    g_frameOffset = (g_stageSlots * g_slotStride + 0xFFF) & ~0xFFF;
    g_capStride = (g_slotBytes + g_h * 2 + 0xFFF) & ~0xFFF;
    g_memSize = g_frameOffset + g_capBufs * g_capStride;
    g_audioStageOffset = g_memSize;
    if (g_audio)
        g_memSize += (g_audioStageFrames * FLASHBACK_AUDIO_FRAME_BYTES + 0xFFF) & ~0xFFF;
}

// Every frame is written starting on a boundary of the card's internal blocks:
// a write that starts partway into one costs the card extra work, and the
// processor too (an Old 3DS at full resolution, with frames every 64 KB
// instead of every 192,512 bytes: ~4 ms less per frame to write, and ~6 ms
// less of its time compressing, which writing had been slowing). So a ring
// slot is the frame rounded up to the largest power of two up to 64 KB that
// adds at most 1/32 (64 KB at full resolution, 16 KB at half), and the video
// slots start on a 64 KB boundary after the audio ring.
static u32 flashbackSlotStride(u32 slotBytes)
{
    u32 align = 0x10000;
    while (align > 512 && ((slotBytes + align - 1) & ~(align - 1)) - slotBytes > slotBytes / 32)
        align /= 2;
    return (slotBytes + align - 1) & ~(align - 1);
}

static u32 flashbackVideoOffset(u32 audioBytes)
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

static void flashbackResetStats(void)
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

// ---------------------------------------------------------------------------
// Lossless frame compression
// ---------------------------------------------------------------------------
//
// A QOI-style ("Quite OK Image") byte format for RGB565 pixels, chosen for
// speed: one pass, no tables beyond 64 recent colors, and frames from games
// (flat areas, HUDs, smooth shading) shrink a lot. Each op, by its first byte:
//   00iiiiii           INDEX  the color in slot i of the recently-seen table
//   01rrggbb           DIFF   r/g/b each differ from the previous pixel by -2..1
//   10gggggg rrrrbbbb  LUMA   g differs by -32..31, r and b by g/2 plus -8..7
//   11rrrrrr           RUN    the previous pixel repeated 1..62 times (r = n - 1)
//   11111110 lo hi     RAW    a full 16-bit pixel
// Differences wrap around within each channel (5 bits for r and b, 6 for g).
// The previous pixel starts as 0 and the table as all 0, for each frame. A
// pixel from a DIFF, LUMA or RAW op goes into the table at slot
// flashbackPackHash(pixel); INDEX and RUN leave the table alone. The readers
// (Flashback-Studio's converter and 3DS app) decode it.

// The C side of compressing (and of turning frames stored as they are
// upright) runs for every pixel too, so it's built with -O2 rather than
// Rosalina's -Os (like draw.c's -O3): on an Old 3DS, ~11% faster.
#pragma GCC push_options
#pragma GCC optimize ("O2")

#define FLASHBACK_PACK_RUN_MAX  62
#define FLASHBACK_PACK_RAW      0xFE

static inline u32 flashbackPackHash(u32 px)
{
    return (px * 0x9E3779B1u) >> 26;    // 6 bits
}

// Rows y0 .. y0 + rows - 1 (rows <= FLASHBACK_BAND_ROWS) of a frame in the
// screen's order (see flashbackCompress), upright, into 'band' (rows 'width'
// apart): for frames stored as they are (flashbackColumnsToRows). A column's
// pixels for these rows are next to each other (bottom row first): 16 columns
// at a time go through a small tile, and out a row at a time, 16 pixels
// together. So the frame is read in whole cache lines, each once, and the band
// written in runs: the Old 3DS has no L2 cache, and writing one pixel per row
// (one cache line each) is very slow there.
static void flashbackUprightBand(const u16 *cols, u32 width, u32 height, u32 y0, u32 rows, u16 *band)
{
    u16 tile[FLASHBACK_BAND_ROWS][16];

    for (u32 x0 = 0; x0 < width; x0 += 16)
    {
        u32 n = width - x0 < 16 ? width - x0 : 16;
        for (u32 i = 0; i < n; i++)
        {
            const u16 *c = cols + (x0 + i) * height + (height - y0 - rows);
            __builtin_prefetch(c + 4 * height);     // a few columns ahead
            for (u32 j = 0; j < rows; j++)
                tile[rows - 1 - j][i] = c[j];
        }
        for (u32 r = 0; r < rows; r++)
            memcpy(band + r * width + x0, tile[r], n * 2);
    }
}

// What packing carries from one pixel, and row, to the next.
typedef struct FlashbackPackState {
    u32 *index;     // the table of recent colors: 64 words (the assembly indexes it by word)
    u8 *out;        // where the next byte goes
    u32 prev;       // the previous pixel
    u32 run;        // how many times it has repeated so far (0 .. FLASHBACK_PACK_RUN_MAX - 1)
} FlashbackPackState;

// flashback_pack.s: packs the pixels at src, src + stepBytes, ... up to (not
// including) 'end', the same as flashbackPackPixels but without its room check
// (the caller has made sure there's room for 4 bytes per pixel). It reads one
// pixel past the last, at 'end'. The Pf version also prefetches the cache line
// 8 pixels on.
void flashbackPackRowAsm(const u16 *src, const u16 *end, u32 stepBytes, FlashbackPackState *st);
void flashbackPackRowAsmPf(const u16 *src, const u16 *end, u32 stepBytes, FlashbackPackState *st);

// Packs 'n' pixels from 'src', 'step' apart, as flashback_pack.s does, but
// returns false as soon as the output could pass 'limit': for the rows of a
// frame that might not fit (the assembly packs the others), and the reference
// the assembly is checked against.
//
// The format's arithmetic, done cheaply (as the assembly does):
// - a channel's difference wraps by shifting it to the top of a word and back
//   (the channels above it shift out);
// - DIFF's three range checks are one: dr+2, dg+2 and db+2 are all in 0..3
//   exactly when their OR is below 4 (a negative one has its high bits set),
//   and LUMA's two the same way;
// - op bytes are built with adds (the fields can't carry into each other).
static bool flashbackPackPixels(const u16 *src, u32 n, u32 step, FlashbackPackState *st, const u8 *limit)
{
    u32 *index = st->index;
    u32 prev = st->prev;
    u32 run = st->run;
    u8 *out = st->out;
    const u16 *end = src + n * step;

    for (; src != end; src += step)
    {
        u32 px = *src;
        if (px == prev)
        {
            if (++run == FLASHBACK_PACK_RUN_MAX)
            {
                if (out > limit)
                    return false;
                *out++ = 0xC0 | (FLASHBACK_PACK_RUN_MAX - 1);
                run = 0;
            }
            continue;
        }

        if (out > limit)
            return false;
        if (run != 0)
        {
            *out++ = (u8)(0xBF + run);      // 0xC0 | (run - 1)
            run = 0;
        }

        u32 h = flashbackPackHash(px);
        if (index[h] == px)
        {
            *out++ = (u8)h;
            prev = px;
            continue;
        }
        index[h] = px;

        s32 dr = (s32)(((px >> 11) - (prev >> 11)) << 27) >> 27;
        s32 dg = (s32)(((px >> 5) - (prev >> 5)) << 26) >> 26;
        s32 db = (s32)((px - prev) << 27) >> 27;

        if ((u32)((dr + 2) | (dg + 2) | (db + 2)) < 4)
            *out++ = (u8)(0x40 + 0x2A + (dr << 4) + (dg << 2) + db);    // 0x40 | (dr+2)<<4 | (dg+2)<<2 | (db+2)
        else
        {
            s32 tr = dr - (dg >> 1), tb = db - (dg >> 1);
            if ((u32)((tr + 8) | (tb + 8)) < 16)
            {
                out[0] = (u8)(0x80 + 32 + dg);                      // 0x80 | (dg + 32)
                out[1] = (u8)(0x88 + (tr << 4) + tb);               // (tr+8)<<4 | (tb+8)
                out += 2;
            }
            else
            {
                out[0] = FLASHBACK_PACK_RAW;
                out[1] = (u8)px;
                out[2] = (u8)(px >> 8);
                out += 3;
            }
        }
        prev = px;
    }

    st->prev = prev;
    st->run = run;
    st->out = out;
    return true;
}

// Compresses a width x height RGB565 frame into 'dst', top row first (the frame
// is upright in the clip). The frame comes in the screen's order, as captured
// (flashbackConvertFrame): column by column, each from the bottom row up, so a
// row's pixels are 'height' apart, and each row is read straight from the
// columns. At full resolution a column's 16 rows share a cache line, which the
// first of them fetches; the 400 lines a row touches (12.5 KB) stay cached for
// the other 15. An Old 3DS takes ~10 ms for a full resolution frame (~14 in C),
// ~8.5 of which reading the pixels. Reads up to 'height' pixels past the end
// of the frame (flashback_pack.s reads a pixel past each row). Returns the
// compressed size, or 0 if it wouldn't be smaller than 'cap' bytes (store the
// frame as is, upright: flashbackColumnsToRows).
static u32 flashbackCompress(const u16 *cols, u32 width, u32 height, u8 *dst, u32 cap)
{
    u32 index[64];
    memset(index, 0, sizeof(index));
    FlashbackPackState st = { index, dst, 0, 0 };
    const u8 *limit = dst + cap - 4;    // room for a run flush plus the largest op

    for (u32 y = 0; y < height; y++)
    {
        const u16 *src = cols + (height - 1 - y);   // row y of column 0

        // A pixel adds at most 4 bytes (a run flush plus the largest op), so a
        // row with room for 4 per pixel can't run out: the assembly packs it,
        // without checking. Only a frame that barely fits, or doesn't, ever
        // needs the check.
        if (st.out <= limit && (u32)(limit - st.out) >= 4 * width)
        {
            // The first row of each cache line prefetches (every row if columns
            // don't start on a cache line, as at half resolution).
            if ((height & 15) != 0 || (height - 1 - y) % 16 == 15)
                flashbackPackRowAsmPf(src, src + width * height, height * 2, &st);
            else
                flashbackPackRowAsm(src, src + width * height, height * 2, &st);
        }
        else if (!flashbackPackPixels(src, width, height, &st, limit))
            return 0;
    }

    if (st.run != 0)
    {
        if (st.out > limit)
            return 0;
        *st.out++ = 0xC0 | (st.run - 1);
    }
    return (u32)(st.out - dst);
}

// The upright frame (top row first), from a frame in the screen's order (see
// flashbackCompress): for frames stored as they are. Turned upright a band at a
// time (flashbackUprightBand), straight into 'dst'.
static void flashbackColumnsToRows(const u16 *cols, u32 width, u32 height, u16 *dst)
{
    for (u32 y0 = 0; y0 < height; y0 += FLASHBACK_BAND_ROWS)
    {
        u32 rows = height - y0 < FLASHBACK_BAND_ROWS ? height - y0 : FLASHBACK_BAND_ROWS;
        flashbackUprightBand(cols, width, height, y0, rows, dst + y0 * width);
    }
}

#pragma GCC pop_options

static bool flashbackIsSdMode(void)
{
    s64 out = 0;
    // Never write to CTRNAND: only run when Luma boots from the SD card.
    return R_SUCCEEDED(svcGetSystemInfo(&out, 0x10000, 0x203)) && out != 0;
}

// ---------------------------------------------------------------------------
// Capture (kernel mode, like Draw_ConvertFrameBufferLines)
// ---------------------------------------------------------------------------

typedef struct FlashbackCaptureArgs {
    u16 *dst;
    u32 outW, outH;
    u32 kernPaToVa;
    u32 flushedPa;  // framebuffer whose cache lines were flushed for this attempt
    bool ok;
    bool torn;      // the displayed buffer changed before or while we were reading it
} FlashbackCaptureArgs;

// Retries for a torn frame. Each attempt flushes the framebuffer first, so a
// retry never reads lines cached by the previous attempt.
#define FLASHBACK_CAPTURE_ATTEMPTS  3

// -O2 for the C conversion too, like compression (half resolution and the rare
// formats: see flashbackConvert; BGR8 frames convert ~11% faster on an Old 3DS).
#pragma GCC push_options
#pragma GCC optimize ("O2")

static inline u16 flashbackRgb565(u32 r, u32 g, u32 b)
{
    return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline u16 flashbackPixelToRgb565(const u8 *src, u32 fmt)
{
    switch (fmt)
    {
        case GSP_RGBA8_OES:     // bytes in memory: A, B, G, R
        {
            // One aligned 32-bit read instead of three byte reads (like
            // Draw_ConvertPixelToBGR8). 4-byte pixels are always aligned.
            u32 px = *(const u32 *)src;
            return flashbackRgb565(px >> 24, (px >> 16) & 0xFF, (px >> 8) & 0xFF);
        }
        case GSP_BGR8_OES:      // bytes in memory: B, G, R
            return flashbackRgb565(src[2], src[1], src[0]);
        case GSP_RGB565_OES:
            return *(const u16 *)src;
        case GSP_RGB5_A1_OES:
        {
            u16 px = *(const u16 *)src;
            u16 r = px >> 11, g = (px >> 6) & 0x1F, b = (px >> 1) & 0x1F;
            return (u16)((r << 11) | (((g << 1) | (g >> 4)) << 5) | b);
        }
        case GSP_RGBA4_OES:
        {
            u16 px = *(const u16 *)src;
            u16 r = px >> 12, g = (px >> 8) & 0xF, b = (px >> 4) & 0xF;
            return (u16)((((r << 1) | (r >> 3)) << 11) | (((g << 2) | (g >> 2)) << 5) | ((b << 1) | (b >> 3)));
        }
        default:
            return 0;
    }
}

// Converts the whole frame, in the order the screen holds it: column by column
// (left to right), each from the bottom row up. Output column ox is
// args->dst[ox * outH ...], so the frame's top row is the last pixel of each
// column; compressing turns it upright (flashbackCompress). Reads and writes
// both go straight through memory, which the Old 3DS (no L2 cache) handles far
// better than writing an upright frame one row apart per pixel, and the read is
// shorter, so the game swaps buffers mid-read less often.
//
// Always inlined into one call per format with a constant 'fmt' / 'bpp', so the
// per-pixel format switch is resolved at compile time instead of running for
// every pixel.
static inline __attribute__((always_inline)) void flashbackConvertFrame(
    const FlashbackCaptureArgs *args, const u8 *fb, u32 stride, u32 stepX, u32 stepY, u32 fmt, u32 bpp)
{
    const u32 outW = args->outW, outH = args->outH;
    const u32 colStep = stepX * stride;
    const u32 rowStep = stepY * bpp;
    u16 *out = args->dst;

    for (u32 ox = 0; ox < outW; ox++)
    {
        const u8 *src = fb + ox * colStep;      // the column's bottom row (framebuffer y = 0)
        for (u32 k = 0; k < outH; k++, src += rowStep)
        {
            __builtin_prefetch(src + 4 * 32);   // a few cache lines ahead
            *out++ = flashbackPixelToRgb565(src, fmt);
        }
    }
}

// flashback_capture.s: flashbackConvertFrame at full resolution (every
// framebuffer row) in the common formats: 'cols' columns of 240 pixels, one
// after the other ('skip' bytes from the end of one to the start of the next),
// into 'dst' in the same order. 'src', 'skip' and 'dst' must be multiples of 4
// (it loads and stores whole words).
void flashbackConvertRgba8(const u8 *src, u16 *dst, u32 cols, u32 skip);
void flashbackConvertBgr8(const u8 *src, u16 *dst, u32 cols, u32 skip);
void flashbackConvertRgb565(const u8 *src, u16 *dst, u32 cols, u32 skip);

// Converts the frame as flashbackConvertFrame does, with flashback_capture.s
// where it can: full resolution in RGBA8, BGR8 and RGB565, about twice as fast
// on an Old 3DS (RGBA8: 2.8 ms instead of 5.1), which also halves how long the
// capture holds up everything else on this core, the game's threads included.
static void flashbackConvert(const FlashbackCaptureArgs *args, const u8 *fb, u32 stride, u32 stepX, u32 stepY,
                             u32 fmt, u32 bpp)
{
    u32 colStep = stepX * stride;
    if (args->outH == 240 && stepY == 1 && colStep >= 240 * bpp &&
        (((u32)fb | colStep | (u32)args->dst) & 3) == 0)
    {
        switch (fmt)
        {
            case GSP_RGBA8_OES:  flashbackConvertRgba8(fb, args->dst, args->outW, colStep - 240 * 4); return;
            case GSP_BGR8_OES:   flashbackConvertBgr8(fb, args->dst, args->outW, colStep - 240 * 3); return;
            case GSP_RGB565_OES: flashbackConvertRgb565(fb, args->dst, args->outW, colStep - 240 * 2); return;
        }
    }

    switch (fmt)
    {
        case GSP_RGBA8_OES:   flashbackConvertFrame(args, fb, stride, stepX, stepY, GSP_RGBA8_OES, 4); break;
        case GSP_BGR8_OES:    flashbackConvertFrame(args, fb, stride, stepX, stepY, GSP_BGR8_OES, 3); break;
        case GSP_RGB565_OES:  flashbackConvertFrame(args, fb, stride, stepX, stepY, GSP_RGB565_OES, 2); break;
        case GSP_RGB5_A1_OES: flashbackConvertFrame(args, fb, stride, stepX, stepY, GSP_RGB5_A1_OES, 2); break;
        case GSP_RGBA4_OES:   flashbackConvertFrame(args, fb, stride, stepX, stepY, GSP_RGBA4_OES, 2); break;
    }
}

static inline u32 flashbackCurrentTopFramebuffer(void)
{
    return (GPU_FB_TOP_SEL & 1) ? GPU_FB_TOP_LEFT_ADDR_2 : GPU_FB_TOP_LEFT_ADDR_1;
}

static void flashbackCaptureKernel(FlashbackCaptureArgs *args)
{
    static const u8 formatSizes[8] = { 4, 3, 2, 2, 2, 0, 0, 0 };

    u32 fmtReg = GPU_FB_TOP_FMT;
    u32 fmt = fmtReg & 7;
    u32 bpp = formatSizes[fmt];
    u32 pa = flashbackCurrentTopFramebuffer();
    u32 stride = GPU_FB_TOP_STRIDE;

    args->ok = false;
    args->torn = false;
    if (bpp == 0 || pa == 0 || stride == 0)
        return;

    // Only the buffer flushed just before this call is safe to read: if the
    // game swapped since, the other buffer may still have stale cache lines.
    if (pa != args->flushedPa)
    {
        args->torn = true;  // nothing read (ok stays false); the caller retries with a fresh flush
        return;
    }

    // 800px mode (2D, half-width pixels) has twice the columns.
    bool is3d = (fmtReg & BIT(5)) != 0;
    bool isNormal2d = (fmtReg & BIT(6)) != 0;
    u32 width = !is3d && !isNormal2d ? 800 : 400;
    u32 stepX = width / args->outW;
    u32 stepY = 240 / args->outH;

    const u8 *fb = (const u8 *)(pa + args->kernPaToVa);
    flashbackConvert(args, fb, stride, stepX, stepY, fmt, bpp);     // (bpp != 0: a valid format)

    // If the game swapped buffers mid-read, the buffer we were reading may
    // already be getting overwritten with the next frame.
    // (Only compare the format register fields used above: pixel format, 3D, 800px mode.)
    args->torn = flashbackCurrentTopFramebuffer() != pa || ((GPU_FB_TOP_FMT ^ fmtReg) & (7 | BIT(5) | BIT(6))) != 0;
    args->ok = true;
}

#pragma GCC pop_options

// Flushes (cleans + invalidates) the CPU cache lines of the framebuffer that's
// on screen right now, and returns its physical address (0 if there is none).
// The GPU writes the framebuffer behind the caches' back, so lines cached by the
// previous capture would otherwise be read back stale. This used to flush the
// entire data cache, which on every frame also interrupted the game's core and
// threw away its cached data (and all of the New 3DS's L2). It's the same kernel
// routine (svcFlushEntireDataCache passes it an "everything" range), just
// limited to the framebuffer.
static u32 flashbackFlushFramebuffer(u32 kernPaToVa)
{
    static const u8 formatSizes[8] = { 4, 3, 2, 2, 2, 0, 0, 0 };

    u32 fmtReg = GPU_FB_TOP_FMT;
    u32 pa = flashbackCurrentTopFramebuffer();
    u32 stride = GPU_FB_TOP_STRIDE;
    if (formatSizes[fmtReg & 7] == 0 || pa == 0 || stride == 0)
        return 0;

    bool is3d = (fmtReg & BIT(5)) != 0;
    bool isNormal2d = (fmtReg & BIT(6)) != 0;
    u32 columns = !is3d && !isNormal2d ? 800 : 400;

    svcFlushDataCacheRange((void *)(pa + kernPaToVa), columns * stride);
    return pa;
}

static bool flashbackCapture(u16 *dst)
{
    FlashbackCaptureArgs args = { dst, g_w, g_h, KERNPA2VA_OFFSET(), 0, false, false };

    for (u32 attempt = 0; attempt < FLASHBACK_CAPTURE_ATTEMPTS; attempt++)
    {
        args.flushedPa = flashbackFlushFramebuffer(args.kernPaToVa);
        if (args.flushedPa == 0)
            return false;   // no valid framebuffer right now
        svcCustomBackdoor(flashbackCaptureKernel, &args);
        if (!args.torn)
            break;          // a clean frame, or no valid framebuffer
    }

    // Still torn after every attempt: the game is swapping faster than we can
    // read. A frame torn mid-read (ok) is kept, since a slightly torn frame beats
    // a missing one. If the last attempt couldn't read at all (the buffer changed
    // right after the flush, !ok), the frame counts as missed and is filled with
    // a copy of the previous one.
    return args.ok;
}

// ---------------------------------------------------------------------------
// Ring file
// ---------------------------------------------------------------------------

static Result flashbackCreateDirectory(FS_Archive archive, const char *path)
{
    Result res = FSUSER_CreateDirectory(archive, fsMakePath(PATH_ASCII, path), 0);
    return (u32)res == 0xC82044BE ? 0 : res; // already exists
}

// Writes the next whole chunk of staged audio to the audio ring, or with
// 'unfinished', the frames of an unfinished chunk too (for a save; the chunk is
// written again once it's complete). Returns false if there was nothing to write.
static bool flashbackWriteAudio(bool unfinished)
{
    if (!g_audio)
        return false;

    u32 consumed = g_audioConsumed;     // always the start of a chunk
    u32 pending = g_audioProduced - consumed;
    if (pending < FLASHBACK_AUDIO_CHUNK_FRAMES && !(unfinished && pending != 0 && g_audioFlushed != consumed + pending))
        return false;
    __sync_synchronize();   // see the staged frames before writing them

    u32 n = pending < FLASHBACK_AUDIO_CHUNK_FRAMES ? pending : FLASHBACK_AUDIO_CHUNK_FRAMES;
    if (!g_writeFailed)
    {
        u64 total;
        // (An unfinished chunk rounded up to whole sectors also overwrites the
        // start of the chunk's next frames: the ring's oldest audio, outside
        // any clip, and rewritten anyway once the chunk is complete.)
        u32 len = (n * FLASHBACK_AUDIO_FRAME_BYTES + 511) & ~511;
        const u8 *src = g_mem + g_audioStageOffset + (consumed % g_audioStageFrames) * FLASHBACK_AUDIO_FRAME_BYTES;
        g_ring.pos = (u64)(consumed % g_audioCapFrames) * FLASHBACK_AUDIO_FRAME_BYTES;
        if (R_FAILED(IFile_Write(&g_ring, &total, src, len, 0)) || total != len)
            g_writeFailed = true;
    }

    __sync_synchronize();   // finish with the staging before handing it back
    g_audioFlushed = consumed + n;
    if (n == FLASHBACK_AUDIO_CHUNK_FRAMES)
        g_audioConsumed = consumed + n;
    return true;
}

// Writes every staged frame to its ring slot, oldest first. Only the frame's
// (compressed) bytes are written, rounded up to whole SD sectors so the card
// never has to read back a partially covered one. Staged audio goes first: its
// writes are small and its staging is the smaller margin. Runs on the writer
// thread, or inline as frames are packed if the writer couldn't be started.
static void flashbackWritePending(void)
{
    for (;;)
    {
        if (flashbackWriteAudio(g_audioFlushRequest))
            continue;

        u32 consumed = g_stageConsumed;
        if (g_stageProduced == consumed)
            return;
        __sync_synchronize();   // see the staged frame, its ring slot and size before using them

        u32 slot = consumed % g_stageSlots;

        // After a failure, frames are only discarded, so the capture thread
        // can stop recording without waiting on a broken card.
        if (!g_writeFailed)
        {
            u64 total;
            u32 len = (g_stageLen[slot] + 511) & ~511;
            if (len > g_slotStride)
                len = g_slotStride;
            u64 t0 = svcGetSystemTick();

            g_ring.pos = g_videoOffset + (u64)g_stageRingIdx[slot] * g_slotStride;   // pre-sized file: always an overwrite
            if (R_FAILED(IFile_Write(&g_ring, &total, g_mem + slot * g_slotStride, len, 0)) || total != len)
                g_writeFailed = true;
            else
            {
                g_writeTotalUs += (u32)((svcGetSystemTick() - t0) * 1000000 / SYSCLOCK_ARM11);
                g_writeTotalFrames++;
            }
        }

        __sync_synchronize();   // finish with the slot before handing it back
        g_stageConsumed = consumed + 1;
    }
}

static void flashbackWriterThreadMain(void)
{
    // Keep going after termination is requested until everything staged is
    // written; the capture thread waits for that before closing the ring file.
    while (!preTerminationRequested || g_stageProduced != g_stageConsumed)
    {
        LightEvent_WaitTimeout(&g_writeEvent, 100 * 1000 * 1000LL);
        flashbackWritePending();
    }
}

// Waits until every staged frame (and audio, unfinished chunk included) has
// been written, so the ring file and the staging memory can be used or freed
// by the capture thread. Audio capture must be paused (flashbackAudioPause).
static void flashbackDrainWrites(void)
{
    g_audioFlushRequest = true;
    if (!g_writerRunning)
        flashbackWritePending();
    else
    {
        while (g_stageProduced != g_stageConsumed || g_audioFlushed != g_audioProduced)
        {
            LightEvent_Signal(&g_writeEvent);
            svcSleepThread(1 * 1000 * 1000LL);
        }
    }
    g_audioFlushRequest = false;
    __sync_synchronize();
}

// Turns audio capture on or off. The audio thread keeps the audio in step with
// the time spent capturing since the ring started, which is the time the video
// covers too: both skip the pauses in between.
static void flashbackAudioSetCapture(bool on)
{
    if (on == g_audioCapture)
        return;
    u64 now = svcGetSystemTick();
    if (on)
        g_audioCaptureSince = now;
    else
        g_audioCapturedTicks += now - g_audioCaptureSince;
    __sync_synchronize();   // (the audio thread reads these once it sees the change)
    g_audioCapture = on;
}

// Stops audio capture, and waits until the audio thread is no longer touching
// the staging memory or the audio counters.
static void flashbackAudioPause(void)
{
    flashbackAudioSetCapture(false);
    __sync_synchronize();
    while (g_audioRunning && !g_audioIdle)
        svcSleepThread(1 * 1000 * 1000LL);
    __sync_synchronize();
}

// An empty audio buffer (with capture paused).
static void flashbackAudioReset(void)
{
    g_audioProduced = g_audioConsumed = g_audioFlushed = 0;
    g_audioMissed = 0;
    g_audioFull = 0;
    g_audioCapturedTicks = 0;
}

// Audio frames in 'ticks' of time, to the nearest frame.
static inline u32 flashbackAudioFramesIn(u64 ticks)
{
    return (u32)((ticks + FLASHBACK_AUDIO_FRAME_TICKS / 2) / FLASHBACK_AUDIO_FRAME_TICKS);
}

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

static inline bool flashbackCounterNewer(u16 a, u16 b)
{
    return a != b && (u16)(a - b) < 0x8000;
}

// Appends one audio frame to the staging memory: a copy of 'src', or silence
// if it's NULL. Returns false if the staging memory is full (the writer is
// behind): the frame is lost, and the time it covered is filled in later.
static bool flashbackAppendAudio(const u32 *src)
{
    u32 produced = g_audioProduced;
    if (produced - g_audioConsumed >= g_audioStageFrames)
    {
        g_audioFull++;
        return false;
    }

    u32 *dst = (u32 *)(g_mem + g_audioStageOffset + (produced % g_audioStageFrames) * FLASHBACK_AUDIO_FRAME_BYTES);
    if (src != NULL)
        memcpy(dst, src, FLASHBACK_AUDIO_FRAME_BYTES);
    else
        memset(dst, 0, FLASHBACK_AUDIO_FRAME_BYTES);
    __sync_synchronize();   // the frame must be visible before it's counted
    g_audioProduced = produced + 1;

    if ((produced + 1) % FLASHBACK_AUDIO_CHUNK_FRAMES == 0 && g_writerRunning)
        LightEvent_Signal(&g_writeEvent);
    return true;
}

// Captures the DSP's output while the capture thread asks for it. Reads go
// through Luma's uncached mapping of physical memory (PA_PTR), so there's no
// cache to manage. A "run" is a stretch of capturing between pauses (recording
// off, a Rosalina menu open, a save): the audio, like the video, skips pauses.
// It's kept in step with the time captured since the ring started ('ticks'),
// so that it skips exactly as much as the video does each time.
static void flashbackAudioThreadMain(void)
{
    const u8 *region[2] = { (const u8 *)PA_PTR(FLASHBACK_DSP_REGION0), (const u8 *)PA_PTR(FLASHBACK_DSP_REGION1) };
    u32 block[FLASHBACK_AUDIO_FRAME_BYTES / 4];
    bool capturing = false, align = false;
    u16 last = 0;
    u64 lastTick = 0, ticks = 0, behindSince = 0;

    while (!preTerminationRequested)
    {
        // (The capture thread clears g_audioCapture and then waits for
        // g_audioIdle; checking g_audioCapture again after clearing g_audioIdle
        // means it can never miss an iteration that's under way.)
        if (!g_audioCapture)
        {
            // A run ended. The video covers it to the end, but the audio can
            // stop short of that: the next frame was on its way, or the game
            // had stopped playing audio first (it's frozen as soon as a
            // Rosalina menu opens, and recording stops at the next video
            // frame). Make that up with silence, so the next run starts in step.
            if (capturing)
            {
                u32 end = flashbackAudioFramesIn(g_audioCapturedTicks);
                while (g_audioProduced < end && flashbackAppendAudio(NULL))
                    ;
                capturing = false;
            }
            g_audioIdle = true;
            svcSleepThread(10 * 1000 * 1000LL);     // (not recording: nothing to keep up with)
            continue;
        }
        g_audioIdle = false;
        __sync_synchronize();
        if (!g_audioCapture)
            continue;

        u64 now = svcGetSystemTick();
        u16 c0 = *(const vu16 *)(region[0] + FLASHBACK_DSP_COUNTER_OFFSET);
        u16 c1 = *(const vu16 *)(region[1] + FLASHBACK_DSP_COUNTER_OFFSET);
        u32 r = flashbackCounterNewer(c0, c1) ? 0 : 1;
        u16 top = r == 0 ? c0 : c1;

        if (!capturing)
        {
            // A new run: take frames from the next one on. The video started
            // when recording did (or resumed), and this thread can notice up
            // to ~10 ms later; the first frame lines the audio up with the
            // time captured (below).
            capturing = true;
            align = true;
            last = top;
            lastTick = now;
            ticks = g_audioCapturedTicks + (now - g_audioCaptureSince);
            behindSince = 0;
            svcSleepThread(1 * 1000 * 1000LL);
            continue;
        }

        ticks += now - lastTick;
        lastTick = now;
        u32 wanted = (u32)(ticks / FLASHBACK_AUDIO_FRAME_TICKS);   // frames there should be by now
        bool got = false;

        if (flashbackCounterNewer(top, last))
        {
            u16 step = (u16)(top - last);
            const vu32 *src = (const vu32 *)(region[r] + FLASHBACK_DSP_FINAL_OFFSET);
            for (u32 i = 0; i < FLASHBACK_AUDIO_FRAME_BYTES / 4; i++)
                block[i] = src[i];

            // The frame stays put until the counters have moved on twice more;
            // if they have (this thread was held up mid-copy), it may be torn.
            u16 d0 = *(const vu16 *)(region[0] + FLASHBACK_DSP_COUNTER_OFFSET);
            u16 d1 = *(const vu16 *)(region[1] + FLASHBACK_DSP_COUNTER_OFFSET);
            u16 top2 = flashbackCounterNewer(d0, d1) ? d0 : d1;
            bool torn = flashbackCounterNewer(top2, top) && (u16)(top2 - top) >= 2;

            // Frames missed since the last one become silence; a big jump
            // (the DSP restarted, ...) is left to the time check below. Never
            // more than real time allows, in case the DSP had stopped for a
            // while and the time check already filled that in.
            u32 have = g_audioProduced;
            u32 room = wanted + 1 > have + 1 ? wanted - have : 0;
            u32 fill = step > 1 && step <= 16 ? step - 1u : 0;
            for (u32 i = 0; i < fill && i < room && flashbackAppendAudio(NULL); i++)
                g_audioMissed++;

            // Lining up (the run's first frame, or the audio fell behind):
            // with this frame, the audio should reach the time captured, to
            // the nearest frame (this one has only just arrived). Silence
            // makes up the difference.
            if (align)
            {
                u32 target = flashbackAudioFramesIn(ticks);
                while (g_audioProduced + 1 < target && flashbackAppendAudio(NULL))
                    ;
                align = false;
                behindSince = 0;
            }
            if (!flashbackAppendAudio(torn ? NULL : block) || torn)
                g_audioMissed++;
            last = top;
            got = true;
        }
        else if (top != last)
        {
            // The counters went backwards: the game restarted its audio (Ocarina
            // of Time 3D seems to, when loading), and they start over from a low
            // count. Carry on from there; waiting for them to pass the old count
            // again would miss up to 160 s of audio.
            last = top;
        }

        // No frames from the DSP (the game isn't playing audio, or isn't
        // running): keep real time with silence.
        while (g_audioProduced + 3 < wanted && flashbackAppendAudio(NULL))
            ;

        // That leaves the audio up to 3 frames behind real time, for as long as
        // the run lasts (frames come at exactly real time after that), which
        // would put the sound up to 15 ms early in the clip. Normally it's at
        // most a frame behind (the next one is on its way); 2 or more for over
        // 20 ms means frames went missing (the DSP paused, or restarted), so
        // line up again. (A late wake-up doesn't count: the frames it skipped
        // are filled in by 'step' above.)
        if (g_audioProduced + 1 < wanted)
        {
            if (behindSince == 0)
                behindSince = now;
            else if (now - behindSince > SYSCLOCK_ARM11 / 50)
                align = true;
        }
        else
            behindSince = 0;

        // Until the next frame lines it up, silence keeps the audio at the
        // time captured (less the frame on its way), so that a silent stretch
        // (the HOME menu, a loading screen) is in step too.
        if (align)
        {
            u32 target = flashbackAudioFramesIn(ticks);
            while (g_audioProduced + 1 < target && flashbackAppendAudio(NULL))
                ;
        }

        // The next frame is due ~4.9 ms after this one: sleep most of that.
        svcSleepThread(got ? 3500 * 1000LL : 1000 * 1000LL);
    }
    g_audioIdle = true;
}

// Opens (creating it if needed) the ring file at its full size. Allocating the
// whole ring up front claims the SD space now rather than during gameplay, and
// a full card fails here instead of mid-recording. Contents don't matter to a
// new buffer (g_count starts at 0); reopening an existing ring keeps them.
static Result flashbackOpenRing(FS_Archive archive)
{
    Result res = IFile_OpenFromArchive(&g_ring, archive, fsMakePath(PATH_ASCII, FLASHBACK_RING_FILE),
                                       FS_OPEN_CREATE | FS_OPEN_READ | FS_OPEN_WRITE);
    if (R_SUCCEEDED(res))
    {
        res = IFile_SetSize(&g_ring, g_videoOffset + (u64)g_frames * g_slotStride);
        if (R_SUCCEEDED(res))
            FSFILE_SetPriority(g_ring.handle, FLASHBACK_RING_FS_PRIORITY);
        else
            IFile_Close(&g_ring);
    }
    return res;
}

static Result flashbackStart(void)
{
    Result res;
    u32 tmp;
    FS_Archive archive;

    flashbackAudioPause();

    // As many staging slots as there's memory for (halving down to the
    // fewest), then capture buffers (down to 1), and short of memory even then,
    // less audio staging.
    g_stageSlots = FLASHBACK_STAGE_SLOTS_MAX;
    g_capBufs = FLASHBACK_CAPTURE_BUFS_MAX;
    g_audioStageFrames = FLASHBACK_AUDIO_STAGE_FRAMES;
    for (;;)
    {
        flashbackLayout();
        res = svcControlMemoryEx(&tmp, FLASHBACK_MEM_ADDR, 0, g_memSize,
                                 MEMOP_ALLOC | MEMOP_REGION_SYSTEM, MEMPERM_READWRITE, true);
        if (R_SUCCEEDED(res))
            break;
        if (g_stageSlots > FLASHBACK_STAGE_SLOTS_MIN)
            g_stageSlots /= 2;
        else if (g_capBufs > 1)
            g_capBufs /= 2;
        else if (g_audio && g_audioStageFrames > FLASHBACK_AUDIO_STAGE_FRAMES_MIN)
            g_audioStageFrames = FLASHBACK_AUDIO_STAGE_FRAMES_MIN;
        else
            return res;
    }
    g_mem = (u8 *)FLASHBACK_MEM_ADDR;

    res = FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if (R_SUCCEEDED(res))
    {
        res = flashbackCreateDirectory(archive, FLASHBACK_DIR);
        if (R_SUCCEEDED(res))
            res = flashbackOpenRing(archive);
        FSUSER_CloseArchive(archive);
    }

    if (R_FAILED(res))
    {
        svcControlMemory(&tmp, FLASHBACK_MEM_ADDR, 0, g_memSize, MEMOP_FREE, 0);
        g_mem = NULL;
        return res;
    }

    g_head = 0;
    g_count = 0;
    memset(g_repeat, 0, sizeof(g_repeat));
    g_pendingMissed = 0;    // (the capture queue is empty: flashbackStop drained it)
    flashbackAudioReset();
    g_writeFailed = false;
    g_ringLost = false;
    g_active = true;
    flashbackResetStats();
    return 0;
}

static void flashbackFillMissed(u32 missed);
static bool flashbackPackFrame(void);

// Waits until every captured frame is packed, then gives the grid slots missed
// since the last capture to it (as copies). Before the ring's state is used for
// a save, or the memory freed.
static void flashbackDrainCaptures(void)
{
    while (g_capProduced != g_capConsumed)
    {
        if (g_packRunning)
            LightEvent_Signal(&g_packEvent);
        else if (flashbackPackFrame())
            continue;
        svcSleepThread(1 * 1000 * 1000LL);
    }
    __sync_synchronize();
    flashbackFillMissed(g_pendingMissed);
    g_pendingMissed = 0;
}

static void flashbackStop(void)
{
    u32 tmp;
    if (!g_active)
        return;

    flashbackDrainCaptures();   // (before the writer: packing hands it frames)
    flashbackAudioPause();
    flashbackDrainWrites();     // the writer must be done with the file and slots
    IFile_Close(&g_ring);
    svcControlMemory(&tmp, FLASHBACK_MEM_ADDR, 0, g_memSize, MEMOP_FREE, 0);
    g_mem = NULL;
    g_active = false;
    g_count = 0;
}

// Compresses the oldest captured frame into the next staging slot and hands it
// to the writer (packer thread). Returns false if no staging slot is free yet
// (the SD card is behind): try again a little later.
static bool flashbackPackFrame(void)
{
    if (g_stageProduced - g_stageConsumed >= g_stageSlots)
        return false;
    __sync_synchronize();   // (the captured frame and its details, as the capture thread left them)

    u32 b = g_capConsumed % g_capBufs;
    u16 *frame = (u16 *)(g_mem + g_frameOffset + b * g_capStride);
    u32 slot = g_stageProduced % g_stageSlots;   // the oldest free staging slot
    u8 *stage = g_mem + slot * g_slotStride;

    // Frames that wouldn't get smaller are stored as they are (upright). So is a
    // frame when packing is falling behind (the next frame is already captured
    // and waiting) and the SD card has time to spare (at most half the staging
    // slots waiting, which leaves the rest for its pauses): that trades the
    // processor's time for the card's. With the capture queue one frame from
    // full (the next capture would be dropped), up to 3/4 of the slots may wait.
    u64 t1 = svcGetSystemTick();
    u32 capTicks1 = g_capTicksTotal;
    u32 queued = g_capProduced - g_capConsumed, staged = g_stageProduced - g_stageConsumed;
    bool storeRaw = queued >= 2 && (staged <= g_stageSlots / 2 ||
                                    (queued >= g_capBufs - 1 && staged <= g_stageSlots * 3 / 4));
    u32 len = storeRaw ? 0 : flashbackCompress(frame, g_w, g_h, stage, g_slotBytes);
    if (len == 0 || len >= g_slotBytes)
    {
        flashbackColumnsToRows(frame, g_w, g_h, (u16 *)stage);
        len = g_slotBytes;
        g_winRawFrames++;
    }

    // (Captures interrupt the packer: their time is the capture's, not this.)
    u64 t2 = svcGetSystemTick();
    u32 capTicks = g_capTicksTotal - capTicks1;
    g_winCaptureTicks += g_capTicks[b];
    g_winCompressTicks += t2 - t1 > capTicks ? (t2 - t1) - capTicks : 0;
    g_winPackedBytes += len;
    g_winFrames++;

    // Grid slots missed just before this frame: copies of the one before it.
    flashbackFillMissed(g_capMissedBefore[b]);

    g_repeat[g_head] = 0;
    g_slotLen[g_head] = len;
    g_slotAudioFrame[g_head] = g_capAudioFrame[b];
    g_stageRingIdx[slot] = g_head;
    g_stageLen[slot] = len;
    __sync_synchronize();   // frame, ring slot and size must be visible before the frame is handed over
    g_stageProduced++;

    g_head = (g_head + 1) % g_frames;
    if (g_count < g_frames)
        g_count++;

    __sync_synchronize();
    g_capConsumed++;        // the capture buffer is free again

    if (g_writerRunning)
        LightEvent_Signal(&g_writeEvent);
    else
        flashbackWritePending();
    return true;
}

// Captures a frame into the next free capture buffer and queues it for the
// packer (capture thread). Returns false if no frame was captured this grid
// slot (the capture failed, or every buffer still waits to be packed).
static bool flashbackCaptureFrame(void)
{
    // (Without the packer thread, whatever the card has room for now.)
    if (!g_packRunning)
        while (g_capProduced != g_capConsumed && flashbackPackFrame())
            ;

    if (g_capProduced - g_capConsumed >= g_capBufs)
        return false;

    u32 b = g_capProduced % g_capBufs;
    u16 *frame = (u16 *)(g_mem + g_frameOffset + b * g_capStride);

    // Where the audio is as the frame is captured.
    u32 audioFrame = g_audioProduced;

    u64 t0 = svcGetSystemTick();
    if (!flashbackCapture(frame))
        return false;
    g_capTicks[b] = svcGetSystemTick() - t0;
    g_capTicksTotal += (u32)g_capTicks[b];
    g_capAudioFrame[b] = audioFrame;
    g_capMissedBefore[b] = g_pendingMissed;
    g_pendingMissed = 0;
    __sync_synchronize();   // the frame and its details before it's queued
    g_capProduced++;

    if (g_packRunning)
        LightEvent_Signal(&g_packEvent);
    else
        while (g_capProduced != g_capConsumed && flashbackPackFrame())
            ;
    return true;
}

// The packer thread: packs captured frames as they come.
static void flashbackPackThreadMain(void)
{
    while (!preTerminationRequested)
    {
        LightEvent_WaitTimeout(&g_packEvent, 100 * 1000 * 1000LL);
        while (g_capProduced != g_capConsumed)
            if (!flashbackPackFrame())
                svcSleepThread(1 * 1000 * 1000LL);     // no staging slot yet: the card is behind
    }
}

// Fills 'missed' frame times with copies of the most recent frame.
static void flashbackFillMissed(u32 missed)
{
    if (g_count == 0 || missed == 0)
        return;

    u32 last = (g_head + g_frames - 1) % g_frames;
    u32 repeat = g_repeat[last] + missed;
    g_repeat[last] = repeat > 0xFFFF ? 0xFFFF : (u16)repeat;
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

// Which ring slots make up a clip of the chosen length, oldest first,
// with each slot repeated 1 + g_repeat[] times to fill missed frames.
typedef struct FlashbackClipPlan {
    u32 slots;          // number of ring slots used
    u32 trimFirst;      // repeats to drop from the oldest slot (clip would be too long)
    u32 frames;         // frames in the clip, duplicates included
} FlashbackClipPlan;

static FlashbackClipPlan flashbackPlanClip(void)
{
    FlashbackClipPlan plan = { 0, 0, 0 };
    u32 frameTimes = 0;

    // Walk back from the newest slot until the slots (with their repeats)
    // cover the clip length; the oldest one may be partly cut.
    while (plan.slots < g_count && frameTimes < g_frames)
    {
        u32 idx = (g_head + g_frames - 1 - plan.slots) % g_frames;
        frameTimes += 1 + g_repeat[idx];
        plan.slots++;
    }

    plan.trimFirst = frameTimes > g_frames ? frameTimes - g_frames : 0;
    plan.frames = frameTimes - plan.trimFirst;
    return plan;
}

static inline u32 flashbackPlanSlot(const FlashbackClipPlan *plan, u32 i, u32 *copies)
{
    u32 idx = (g_head + g_frames - plan->slots + i) % g_frames;
    *copies = 1 + g_repeat[idx] - (i == 0 ? plan->trimFirst : 0);
    return idx;
}

// The oldest audio frame the ring still holds: all but the oldest chunk, which
// a partly written chunk (see flashbackWriteAudio) may have overwritten.
static u32 flashbackAudioOldest(void)
{
    u32 held = g_audioCapFrames - FLASHBACK_AUDIO_CHUNK_FRAMES;
    return g_audioFlushed > held ? g_audioFlushed - held : 0;
}

// Saved audio positions (in samples) count from this audio frame on: a whole
// number of rings back, so sample i is still at (i % capacity) in the ring,
// and one ring before the oldest frame held, so a frame whose audio is older
// than that still gets a position (where the audio reads as silence). Counted
// from the ring's start, they'd pass 2^32 after ~36 hours of recording.
static u32 flashbackAudioBase(void)
{
    u32 oldest = flashbackAudioOldest();
    return oldest >= g_audioCapFrames ? (oldest / g_audioCapFrames - 1) * g_audioCapFrames : 0;
}

// The saved audio position of audio frame 'frame' (counted since the ring started).
static u32 flashbackAudioSampleOf(u32 frame)
{
    u32 base = flashbackAudioBase();
    return frame > base ? (frame - base) * FLASHBACK_AUDIO_FRAME_SAMPLES : 0;
}

// Clip file format (<clip>.flb): the ring file itself, renamed, with an index
// and a trailer appended. Saving this way copies no frame or audio data, so it
// takes a few seconds however long the clip is; the PC-side converter
// (convert_flashback.bat, in https://github.com/LiamTrader43/Flashback-Studio)
// decodes it into a video, and Flashback Studio plays it on the 3DS. Keep
// those readers in step with any change here.
//
// Version 1, recorded without audio:
//   [ring slots]    slot i at offset i * slotStride: a compressed frame, or the
//                   raw frame if its size is slotBytes (see flashbackCompress)
//   [index]         entryCount x { u32 slot, u32 size, u32 copies }: the clip's
//                   frames oldest first, each slot shown 'copies' times
//   [trailer]       FlashbackClipTrailer, the last 48 bytes of the file
//
// Version 3, recorded with audio:
//   [audio ring]    audio.capacity stereo PCM16 samples: sample i (counting from
//                   when the ring started) at audio.offset + (i % capacity) * 4
//   [ring slots]    as in version 1, after the audio ring (from a 64 KB boundary)
//   [index]         entryCount x { u32 offset, u32 size, u32 copies, u32 audioPos }:
//                   as in version 1, but with each frame's offset in the file,
//                   and the audio sample at which it's first shown (each copy
//                   comes sampleRate / fps samples after the one before)
//   [audio info]    FlashbackClipAudioInfo (32 bytes), including which samples
//                   the ring holds (first .. end - 1)
//   [trailer]       FlashbackClipTrailer, as in version 1
//
// (Version 2 is a clip without audio, compacted by Flashback Studio.)
// All values little-endian. Frames are RGB565, top row first.

#define FLASHBACK_CLIP_MAGIC            0x4B424C46  // "FLBK"
#define FLASHBACK_CLIP_VERSION          1
#define FLASHBACK_CLIP_VERSION_AUDIO    3

typedef struct FlashbackClipTrailer {
    u32 magic;
    u32 version;
    u32 width, height, fps;
    u32 slotStride;     // bytes between ring slots
    u32 slotBytes;      // size of an uncompressed frame
    u32 entryCount;     // index entries
    u32 frameCount;     // frames in the clip, copies included
    u32 indexOffset;    // where the index starts
    u32 reserved[2];
} FlashbackClipTrailer;
_Static_assert(sizeof(FlashbackClipTrailer) == 48, "the clip trailer is 48 bytes");

typedef struct FlashbackClipAudioInfo {
    u32 sampleRate;     // in millihertz
    u16 channels;       // 2
    u16 bitsPerSample;  // 16
    u32 offset;         // where the audio ring starts
    u32 capacity;       // samples the ring has room for: sample i is at offset + (i % capacity) * 4
    u32 first;          // the oldest sample held
    u32 end;            // one past the newest sample held
    u32 missed;         // audio frames (of 160 samples) filled with silence because the recorder missed them...
    u32 full;           // ...of which because its buffer was full (the SD card was busy)
} FlashbackClipAudioInfo;
_Static_assert(sizeof(FlashbackClipAudioInfo) == 32, "the clip audio info is 32 bytes");

// Builds the clip's index, audio info (with audio) and trailer into 'buf';
// returns their size.
static u32 flashbackBuildClipIndex(const FlashbackClipPlan *plan, u32 indexOffset, u8 *buf)
{
    u32 *entry = (u32 *)buf;
    for (u32 i = 0; i < plan->slots; i++)
    {
        u32 copies;
        u32 idx = flashbackPlanSlot(plan, i, &copies);
        if (g_audio)
        {
            *entry++ = g_videoOffset + idx * g_slotStride;
            *entry++ = g_slotLen[idx];
            *entry++ = copies;
            // (The oldest entry may start a few copies in: see flashbackPlanClip.)
            *entry++ = flashbackAudioSampleOf(g_slotAudioFrame[idx]) + flashbackAudioSamplesFor(i == 0 ? plan->trimFirst : 0);
        }
        else
        {
            *entry++ = idx;
            *entry++ = g_slotLen[idx];
            *entry++ = copies;
        }
    }

    if (g_audio)
    {
        FlashbackClipAudioInfo *a = (FlashbackClipAudioInfo *)entry;
        memset(a, 0, sizeof(*a));
        a->sampleRate = FLASHBACK_AUDIO_RATE_MHZ;
        a->channels = 2;
        a->bitsPerSample = 16;
        a->offset = 0;
        a->capacity = g_audioCapFrames * FLASHBACK_AUDIO_FRAME_SAMPLES;
        a->end = flashbackAudioSampleOf(g_audioFlushed);
        a->first = flashbackAudioSampleOf(flashbackAudioOldest());
        a->missed = g_audioMissed;
        a->full = g_audioFull;
        entry = (u32 *)(a + 1);
    }

    FlashbackClipTrailer *t = (FlashbackClipTrailer *)entry;
    memset(t, 0, sizeof(*t));
    t->magic = FLASHBACK_CLIP_MAGIC;
    t->version = g_audio ? FLASHBACK_CLIP_VERSION_AUDIO : FLASHBACK_CLIP_VERSION;
    t->width = g_w;
    t->height = g_h;
    t->fps = g_fps;
    t->slotStride = g_slotStride;
    t->slotBytes = g_slotBytes;
    t->entryCount = plan->slots;
    t->frameCount = plan->frames;
    t->indexOffset = indexOffset;
    return (u32)((u8 *)(t + 1) - buf);
}

// The clip's index is built in the capture buffer, a frame's size.
_Static_assert(FLASHBACK_MAX_FRAMES * 16 + sizeof(FlashbackClipAudioInfo) + sizeof(FlashbackClipTrailer) <= 200 * 120 * 2,
               "a clip's index fits in the capture buffer at half resolution");

// Saves the clip by turning the ring file into it, then starts a fresh ring.
// Needs the writer to be idle (flashbackDrainWrites).
static s32 flashbackSaveClipFile(FS_Archive archive, const char *clip, const FlashbackClipPlan *plan)
{
    char path[128];
    u64 total;
    IFile file = {0};
    sprintf(path, "%s.flb", clip);

    // The ring always starts at slot 0, so until it has wrapped once the clip
    // only uses its first g_count slots: the rest can be trimmed off.
    u32 dataEnd = g_videoOffset + (g_count < g_frames ? g_count : g_frames) * g_slotStride;
    u8 *index = g_mem + g_frameOffset;  // the capture buffer, free while saving
    u32 indexSize = flashbackBuildClipIndex(plan, dataEnd, index);

    IFile_Close(&g_ring);
    if (R_FAILED(FSUSER_RenameFile(archive, fsMakePath(PATH_ASCII, FLASHBACK_RING_FILE),
                                   archive, fsMakePath(PATH_ASCII, path))))
    {
        // Nothing moved: keep recording into the same ring, buffer intact.
        if (R_FAILED(flashbackOpenRing(archive)))
            g_ringLost = true;
        return -2;
    }

    s32 result = (s32)plan->frames;
    bool ok = R_SUCCEEDED(IFile_OpenFromArchive(&file, archive, fsMakePath(PATH_ASCII, path), FS_OPEN_WRITE)) &&
              R_SUCCEEDED(IFile_SetSize(&file, dataEnd));
    if (ok)
    {
        file.pos = dataEnd;
        ok = R_SUCCEEDED(IFile_Write(&file, &total, index, indexSize, 0)) && total == indexSize;
    }
    IFile_Close(&file);
    if (!ok)
    {
        // Without its index the clip can't be decoded: don't leave it behind.
        FSUSER_DeleteFile(archive, fsMakePath(PATH_ASCII, path));
        result = -3;
    }

    // The old buffer is now the clip: continue with an empty one.
    g_head = 0;
    g_count = 0;
    memset(g_repeat, 0, sizeof(g_repeat));
    flashbackAudioReset();
    if (R_FAILED(flashbackOpenRing(archive)))
        g_ringLost = true;  // no space for a new ring: the capture thread stops recording
    return result;
}

// Returns the number of frames saved, or a negative value:
//   -1 nothing recorded, -2 couldn't create the clip file, -3 couldn't write the clip's index.
static s32 flashbackSave(void)
{
    if (g_active)
        flashbackDrainCaptures();   // every captured frame packed, before the ring's state is used
    if (!g_active || g_count == 0)
        return -1;

    // Frames and audio still waiting to be written belong at the end of the
    // clip, and the writer must be done with the ring file before it becomes
    // the clip. (Audio capture resumes along with recording, once the save is done.)
    flashbackAudioPause();
    flashbackDrainWrites();

    FS_Archive archive;
    if (R_FAILED(FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
        return -2;
    if (R_FAILED(flashbackCreateDirectory(archive, FLASHBACK_DIR)))
    {
        FSUSER_CloseArchive(archive);
        return -2;
    }

    char dateTimeStr[32];
    char clipName[48];
    char clip[80];
    dateTimeToString(dateTimeStr, osGetTime(), true);
    sprintf(clipName, "clip_%s", dateTimeStr);
    sprintf(clip, FLASHBACK_DIR "/%s", clipName);

    FlashbackClipPlan plan = flashbackPlanClip();
    s32 result = flashbackSaveClipFile(archive, clip, &plan);

    FSUSER_CloseArchive(archive);
    if (result > 0)
        sprintf(g_lastClip, "%s.flb", clip);
    return result;
}

// ---------------------------------------------------------------------------
// Settings file
// ---------------------------------------------------------------------------

#define FLASHBACK_SETTINGS_FILE     FLASHBACK_DIR "/settings.bin"
#define FLASHBACK_SETTINGS_MAGIC    0x54534246  // "FBST"
// Version 2 added the clip length, as an index into the 5 to 30 second list of
// that time; version 3 stores it in seconds, so the list can change; version 4
// added audio. Older files still load.
#define FLASHBACK_SETTINGS_VERSION  4

typedef struct FlashbackSettings {
    u32 magic;
    u16 version;
    u16 size;           // sizeof(FlashbackSettings), so newer versions can append fields
    u8  enabled;        // unused (always 0): recording is always off at boot
    u8  res;            // FlashbackResolution
    u8  fpsIdx;         // index into g_fpsChoices
    u8  format;         // unused (always 0): was the save format, when BMP frames were one
    u32 saveCombo;
    // Version 2 and later:
    u8  length;         // clip length in seconds (version 2: index into { 5, 10, ..., 30 })
    // Version 4 and later:
    u8  audio;          // record audio: 1 or 0
    u8  reserved[2];
} FlashbackSettings;

// The shortest available length that's at least 'seconds' (or the longest).
static u32 flashbackLengthIdxFor(u32 seconds)
{
    for (u32 i = 0; i < FLASHBACK_LENGTH_CHOICES; i++)
        if (g_lengthChoices[i] >= seconds)
            return i;
    return FLASHBACK_LENGTH_CHOICES - 1;
}

#define FLASHBACK_SETTINGS_V1_SIZE  16  // everything up to and including saveCombo

static const char *flashbackCheckCombo(u32 combo);

static FlashbackSettings flashbackCurrentSettings(void)
{
    FlashbackSettings s;
    memset(&s, 0, sizeof(s));
    s.magic = FLASHBACK_SETTINGS_MAGIC;
    s.version = FLASHBACK_SETTINGS_VERSION;
    s.size = sizeof(s);
    s.enabled = 0;      // not remembered, so toggling recording doesn't rewrite the file
    s.res = (u8)g_wantRes;
    s.fpsIdx = (u8)g_wantFpsIdx;
    s.saveCombo = g_saveCombo;
    s.length = (u8)g_lengthChoices[g_wantLenIdx];
    s.audio = g_wantAudio ? 1 : 0;
    return s;
}

// Old 3DS has a third of the CPU speed, and the system core Flashback runs on
// also serves the game's graphics, input and file requests there, so it gets
// a lighter default: half resolution (at the same 30 fps). Saved settings still
// override it.
static void flashbackApplyDefaults(void)
{
    if (!isN3DS)
        g_wantRes = FLASHBACK_RES_HALF;
}

// Loads saved settings, keeping the defaults for anything missing or invalid.
static void flashbackLoadSettings(void)
{
    FlashbackSettings s;
    IFile file = {0};
    u64 total = 0;

    if (R_FAILED(IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                            fsMakePath(PATH_ASCII, FLASHBACK_SETTINGS_FILE), FS_OPEN_READ)))
        return;

    memset(&s, 0, sizeof(s));
    Result res = IFile_Read(&file, &total, &s, sizeof(s));
    IFile_Close(&file);

    if (R_FAILED(res) || total < FLASHBACK_SETTINGS_V1_SIZE || s.magic != FLASHBACK_SETTINGS_MAGIC ||
        s.version < 1 || s.version > FLASHBACK_SETTINGS_VERSION)
        return;

    // Version 1 files end before the length: it keeps its default.
    if (s.version == 2 && total >= sizeof(s) && s.length < 6)
        g_wantLenIdx = flashbackLengthIdxFor((s.length + 1) * 5);   // 5..30 s, rounded up
    else if (s.version >= 3 && total >= sizeof(s) && s.length != 0)
        g_wantLenIdx = flashbackLengthIdxFor(s.length);
    if (s.res < FLASHBACK_RES_COUNT)
        g_wantRes = s.res;
    if (s.fpsIdx < FLASHBACK_FPS_CHOICES)
        g_wantFpsIdx = s.fpsIdx;
    if (s.version >= 4 && total >= sizeof(s))
        g_wantAudio = s.audio != 0;     // (older files: the default, on)
    // Re-checked in case the Rosalina menu combo was changed since.
    if (flashbackCheckCombo(s.saveCombo) == NULL)
        g_saveCombo = s.saveCombo;
    // Recording itself always starts off at boot, whatever was saved.
}

static void flashbackSaveSettings(void)
{
    FlashbackSettings s = flashbackCurrentSettings();
    FS_Archive archive;
    IFile file = {0};
    u64 total;

    if (!flashbackIsSdMode() || R_FAILED(FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
        return;

    if (R_SUCCEEDED(flashbackCreateDirectory(archive, FLASHBACK_DIR)) &&
        R_SUCCEEDED(IFile_OpenFromArchive(&file, archive, fsMakePath(PATH_ASCII, FLASHBACK_SETTINGS_FILE),
                                          FS_OPEN_CREATE | FS_OPEN_WRITE)))
    {
        IFile_SetSize(&file, sizeof(s));
        IFile_Write(&file, &total, &s, sizeof(s), 0);
        IFile_Close(&file);
    }

    FSUSER_CloseArchive(archive);
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
    flashbackApplyDefaults();
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
            flashbackResetStats();
        if (recording)
        {
            // Only the capture here: the packer thread compresses. A grid
            // slot without a capture becomes a copy of the frame before it,
            // once that's packed (flashbackPackFrame).
            if (!flashbackCaptureFrame())
                g_pendingMissed++;
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
        if (nextTick > now)
            svcSleepThread((s64)((nextTick - now) * 1000000000ULL / SYSCLOCK_ARM11));
        else if (recording)
        {
            // Grid slots that ended before 'now' are lost; record the current
            // slot (the one containing 'now') immediately, slightly late.
            u32 missed = (u32)((now - nextTick) / period);
            g_pendingMissed += missed;      // (copies, as above)
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
    // packer for the few ms that takes. (With the packer a step lower still,
    // at 61, other threads got ahead of it too: an Old 3DS compressed ~30%
    // slower, too slow to keep up.) The writer is level with the capture
    // thread, above the packer: it hardly uses the CPU (it mostly waits on the
    // SD card), and running first means it sends the next write the moment
    // the previous one finishes. (Below the work, it had to wait out each
    // frame's compression with the card idle: an SD speed test on a New 3DS XL
    // put a ~100 KB write at ~20 ms, but recording measured 31-36 ms per
    // write.) None of the three is essential to Rosalina: without the writer,
    // frames are written inline, without the packer they're compressed inline,
    // and without the capture thread the recorder stays off. The audio
    // thread, on the other hand, runs well above them: it has to catch each
    // audio frame within the ~8 ms it stays put, so it wakes every millisecond
    // or few, but it uses very little CPU. Without it, there's no audio.
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

// Returns why 'combo' can't be the save hotkey, or NULL if it can.
static const char *flashbackCheckCombo(u32 combo)
{
    u32 keys = combo & FLASHBACK_COMBO_KEYS;

    if (keys != combo)
        return "Only buttons can be used:\nnot the touchscreen, Circle Pad or C-Stick.";
    if (__builtin_popcount(keys) < 2)
        return "Use at least 2 buttons, so it can't be\npressed by accident during play.";
    // Inside the menu combo: opening the menu would also save. Containing it:
    // the menu would open instead of saving.
    if ((keys & menuCombo) == keys || (keys & menuCombo) == menuCombo)
        return "It overlaps the Rosalina menu combo.";
    return NULL;
}

static void FlashbackMenu_ChangeHotkey(void)
{
    char current[128], entered[128];
    LumaConfig_ConvertComboToString(current, g_saveCombo);

    // Ask for a combo until a valid one is entered, or the user backs out.
    for (;;)
    {
        Draw_Lock();
        Draw_ClearFramebuffer();
        Draw_DrawString(10, 10, COLOR_TITLE, "Flashback recorder: save hotkey");
        u32 posY = Draw_DrawFormattedString(10, 30, COLOR_WHITE, "Current hotkey: %s\n\n", current);
        Draw_DrawString(10, posY, COLOR_WHITE, "Hold the new hotkey (2 or more buttons),\nthen let go of all buttons.");
        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 combo = waitCombo();    // waits for the previous buttons to be released first
        if (combo == 0)             // menu closing (sleep, shell closed...)
            return;

        const char *error = flashbackCheckCombo(combo);
        if (error == NULL)
            g_saveCombo = combo;
        LumaConfig_ConvertComboToString(entered, combo);

        u32 pressed;
        do
        {
            Draw_Lock();
            Draw_ClearFramebuffer();
            Draw_DrawString(10, 10, COLOR_TITLE, "Flashback recorder: save hotkey");
            posY = Draw_DrawFormattedString(10, 30, COLOR_WHITE, "You pressed: %s\n\n", entered);
            if (error == NULL)
            {
                posY = Draw_DrawFormattedString(10, posY, COLOR_GREEN, "Save hotkey changed to %s.\n", entered);
                Draw_DrawString(10, posY + 10, COLOR_WHITE, "Press B to go back.");
            }
            else
            {
                posY = Draw_DrawFormattedString(10, posY, COLOR_RED, "%s\nThe hotkey is still %s.\n", error, current);
                Draw_DrawString(10, posY + 10, COLOR_WHITE, "Press A to try again, or B to go back.");
            }
            Draw_FlushFramebuffer();
            Draw_Unlock();

            pressed = waitInput();
            if (menuShouldExit)
                return;
        }
        while (!(pressed & KEY_B) && !(error != NULL && (pressed & KEY_A)));

        if (pressed & KEY_B)
            return;
        // A after an invalid combo: ask again.
    }
}

// The Flashback screen is redrawn a few times a second, in place: Rosalina
// draws straight onto the screen, so clearing it first would show as a
// flicker. Instead each line is drawn the whole width of the screen (its text,
// then spaces), and the rows between and below the lines are blanked, so
// nothing from before is left over and nothing goes blank in between.
#define FLASHBACK_SCREEN_COLS   ((SCREEN_BOT_WIDTH - 10) / SPACING_X)  // (Draw_DrawString wraps after this many)

static u32 g_screenBlankFrom;   // the first row not drawn yet this time

// Blanks rows y0 .. y1 - 1 of the bottom screen (pixel (x, y) is at
// fb[x * 240 + 239 - y]).
static void flashbackBlankRows(u32 y0, u32 y1)
{
    u16 *fb = (u16 *)FB_BOTTOM_VRAM_ADDR;
    if (y1 > SCREEN_BOT_HEIGHT)
        y1 = SCREEN_BOT_HEIGHT;
    if (y0 >= y1)
        return;
    for (u32 x = 0; x < SCREEN_BOT_WIDTH; x++)
        memset(fb + x * SCREEN_BOT_HEIGHT + (SCREEN_BOT_HEIGHT - y1), 0, (y1 - y0) * 2);
}

// Draw_DrawFormattedString at x = 10, the Flashback screen's way: each line
// padded to the screen's width (a longer one goes on below, in pieces), and
// the rows above it that nothing drew blanked. Returns the y after the last
// '\n', as Draw_DrawFormattedString does.
static u32 flashbackDrawLines(u32 posY, u32 color, const char *fmt, ...)
{
    char text[DRAW_MAX_FORMATTED_STRING_SIZE + 1], line[FLASHBACK_SCREEN_COLS + 1];
    va_list args;
    va_start(args, fmt);
    vsprintf(text, fmt, args);
    va_end(args);

    for (const char *p = text; *p != '\0'; )
    {
        u32 len = 0;
        while (p[len] != '\0' && p[len] != '\n')
            len++;
        for (;;)
        {
            u32 n = len < FLASHBACK_SCREEN_COLS ? len : FLASHBACK_SCREEN_COLS;
            memset(line, ' ', FLASHBACK_SCREEN_COLS);
            memcpy(line, p, n);
            line[FLASHBACK_SCREEN_COLS] = '\0';
            flashbackBlankRows(g_screenBlankFrom, posY);
            Draw_DrawString(10, posY, color, line);
            g_screenBlankFrom = posY + 10;      // (characters are 10 rows high)
            p += n;
            len -= n;
            if (len == 0)
                break;
            posY += SPACING_Y;
        }
        if (*p == '\n')
        {
            posY += SPACING_Y;
            p++;
        }
    }
    return posY;
}

void FlashbackMenu_Show(void)
{
    bool sdMode = flashbackIsSdMode();
    char comboStr[128];
    FlashbackSettings before = flashbackCurrentSettings();

    do
    {
        // What the settings will be once applied (they apply within one frame).
        u32 res = g_wantRes;
        u32 fps = g_fpsChoices[g_wantFpsIdx];
        u32 seconds = g_lengthChoices[g_wantLenIdx];
        u32 frames = seconds * fps;
        bool audio = flashbackAudioWanted();
        bool applied = res == g_res && g_wantFpsIdx == g_fpsIdx && g_wantLenIdx == g_lenIdx && audio == g_audio;

        Draw_Lock();
        g_screenBlankFrom = 0;
        flashbackDrawLines(10, COLOR_TITLE, "Flashback recorder");

        u32 posY = 30;
        if (!g_threadRunning)
            posY = flashbackDrawLines(posY, COLOR_RED, "Recorder thread failed to start.\n\n");
        else if (!sdMode)
            posY = flashbackDrawLines(posY, COLOR_RED, "Only available when booting from the SD card.\n\n");

        posY = flashbackDrawLines(posY, COLOR_WHITE, "Recording:  %s\n", g_enabled ? "on" : "off");
        posY = flashbackDrawLines(posY, COLOR_WHITE, "Resolution: %s\n",
                                  res == FLASHBACK_RES_FULL ? "full (400x240)" : "half (200x120)");
        // Size of a saved clip of the full length: the ring file (the audio
        // ring, and the frames' slots: see flashbackSlotStride).
        u32 frameBytes = (res == FLASHBACK_RES_FULL ? 400 * 240 : 200 * 120) * 2;
        u32 audioBytes = audio ? flashbackAudioFramesFor(seconds) * FLASHBACK_AUDIO_FRAME_BYTES : 0;
        u64 clipBytes = flashbackVideoOffset(audioBytes) + (u64)frames * flashbackSlotStride(frameBytes);
        u32 clipTenthsMb = (u32)((clipBytes + 50000) / 100000);     // in tenths of a MB (1 MB = 1,000,000 bytes)

        if (clipTenthsMb < 100)
            posY = flashbackDrawLines(posY, COLOR_WHITE, "Frame rate: %lu fps   Length: %lu s (%lu.%lu MB)\n",
                                      fps, seconds, clipTenthsMb / 10, clipTenthsMb % 10);
        else
            posY = flashbackDrawLines(posY, COLOR_WHITE, "Frame rate: %lu fps   Length: %lu s (%lu MB)\n",
                                      fps, seconds, (clipTenthsMb + 5) / 10);
        posY = flashbackDrawLines(posY, COLOR_WHITE, "Audio:      %s\n\n",
                                  !g_audioRunning ? "failed" : g_wantAudio ? "on" : "off");

        // Counted the same way a save would be: frame times in the clip, and
        // how many of those are dropped frames filled with a copy.
        u32 buffered = 0, dropped = 0;
        if (applied && g_active)
        {
            FlashbackClipPlan plan = flashbackPlanClip();
            buffered = plan.frames;
            dropped = plan.frames - plan.slots;
        }

        posY = flashbackDrawLines(posY, COLOR_WHITE, "Buffered:  %lu / %lu frames, %lu dropped\n",
                                  buffered, frames, dropped);
        if (applied && g_active && g_audio)
            posY = flashbackDrawLines(posY, COLOR_WHITE, "Rate:      %lu.%lu / %lu fps   audio: %lu missed\n",
                                      g_statFpsX10 / 10, g_statFpsX10 % 10, fps, g_audioMissed);
        else
            posY = flashbackDrawLines(posY, COLOR_WHITE, "Rate:      %lu.%lu / %lu fps\n",
                                      g_statFpsX10 / 10, g_statFpsX10 % 10, fps);
        posY = flashbackDrawLines(posY, COLOR_WHITE, "Per frame: capture %lu.%lu, compress %lu.%lu, write %lu.%lums\n",
                                  g_statCaptureUs / 1000, (g_statCaptureUs / 100) % 10,
                                  g_statCompressUs / 1000, (g_statCompressUs / 100) % 10,
                                  g_statWriteUs / 1000, (g_statWriteUs / 100) % 10);
        posY = flashbackDrawLines(posY, COLOR_WHITE, "Budget:    %lums, compressed to %lu%%, raw %lu%%\n\n",
                                  1000 / fps, g_statSizePct, g_statRawPct);

        const char *recordError = g_recordError;
        if (g_saving || g_saveRequested)
            posY = flashbackDrawLines(posY, COLOR_WHITE, "Saving...\n");
        else if (recordError != NULL)
            posY = flashbackDrawLines(posY, COLOR_RED, "%s\n", recordError);
        else if (g_lastSaveResult > 0)
            posY = flashbackDrawLines(posY, COLOR_GREEN, "Saved %ld frames in %lu.%lus to\n%s\n", g_lastSaveResult,
                                      g_lastSaveMs / 1000, (g_lastSaveMs / 100) % 10, g_lastClip);
        else if (g_lastSaveResult < 0)
            posY = flashbackDrawLines(posY, COLOR_RED, "Save failed (%ld).\n", g_lastSaveResult);

        posY += 10;
        posY = flashbackDrawLines(posY, COLOR_WHITE, "A: recording on/off   X: save clip now   B: back\n");
        posY = flashbackDrawLines(posY, COLOR_WHITE, "Y: resolution   Left/Right: fps   L/R: length\n");
        posY = flashbackDrawLines(posY, COLOR_WHITE, "Start: audio   Select: hotkey\n");
        posY = flashbackDrawLines(posY, COLOR_WHITE, "Off or res/fps/length/audio clear the buffer.\n\n");
        LumaConfig_ConvertComboToString(comboStr, g_saveCombo);
        flashbackDrawLines(posY, COLOR_WHITE, "In game: %s saves a clip.", comboStr);
        flashbackBlankRows(g_screenBlankFrom, SCREEN_BOT_HEIGHT);

        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 pressed = waitInputWithTimeout(250);
        bool busy = g_saving || g_saveRequested;

        if ((pressed & KEY_A) && sdMode && g_threadRunning)
        {
            g_enabled = !g_enabled;
            g_recordError = NULL;
        }
        else if ((pressed & KEY_X) && !busy)
        {
            g_saveFromHotkey = false;
            g_saveRequested = true;
        }
        else if ((pressed & KEY_Y) && !busy)
            g_wantRes = (g_wantRes + 1) % FLASHBACK_RES_COUNT;
        else if ((pressed & KEY_DRIGHT) && !busy)
            g_wantFpsIdx = (g_wantFpsIdx + 1) % FLASHBACK_FPS_CHOICES;
        else if ((pressed & KEY_DLEFT) && !busy)
            g_wantFpsIdx = (g_wantFpsIdx + FLASHBACK_FPS_CHOICES - 1) % FLASHBACK_FPS_CHOICES;
        else if ((pressed & KEY_R) && !busy && g_wantLenIdx + 1 < FLASHBACK_LENGTH_CHOICES)
            g_wantLenIdx = g_wantLenIdx + 1;    // longer (stops at the longest)
        else if ((pressed & KEY_L) && !busy && g_wantLenIdx > 0)
            g_wantLenIdx = g_wantLenIdx - 1;    // shorter (stops at the shortest)
        else if ((pressed & KEY_START) && !busy && g_audioRunning)
            g_wantAudio = !g_wantAudio;
        else if (pressed & KEY_SELECT)
            FlashbackMenu_ChangeHotkey();
        else if (pressed & KEY_B)
            break;
    }
    while (!menuShouldExit);

    // Remember the settings for the next boot (only if something changed).
    FlashbackSettings after = flashbackCurrentSettings();
    if (memcmp(&before, &after, sizeof(before)) != 0)
        flashbackSaveSettings();
}
