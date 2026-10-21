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

// Work memory (one frame + one BMP), allocated from the SYSTEM region when
// recording starts, never from the running game, and sized for the current
// resolution. The address is clear of the screenshot/menu framebuffer cache
// at 0x0D000000.
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

typedef enum FlashbackFormat {
    FLASHBACK_FORMAT_RAW = 0,   // one .flb clip file, converted on a PC (fastest: the ring file is renamed)
    FLASHBACK_FORMAT_BMP,       // folder of 16-bit BMP frames
    FLASHBACK_FORMAT_COUNT,
} FlashbackFormat;

// 14-byte file header + 40-byte BITMAPINFOHEADER + 3 RGB565 channel masks.
#define FLASHBACK_BMP_HEADER    (14 + 40 + 12)

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
// Defaults when no settings are saved: full resolution, 30 fps, raw format on
// New 3DS; half resolution, 30 fps on Old 3DS (see flashbackApplyDefaults).
static volatile u32  g_wantRes = FLASHBACK_RES_FULL;   // FlashbackResolution chosen in the menu
static volatile u32  g_wantFpsIdx = 2;                 // index into g_fpsChoices chosen in the menu (30 fps)
static volatile u32  g_wantLenIdx = FLASHBACK_DEFAULT_LENGTH_IDX; // index into g_lengthChoices chosen in the menu
static volatile u32  g_saveFormat = FLASHBACK_FORMAT_RAW; // FlashbackFormat; takes effect on the next save
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
static u32 g_slotBytes, g_bmpBytes, g_bmpOffset, g_memSize;
static u32 g_slotStride;                // ring and staging slot size: g_slotBytes rounded up to whole SD sectors

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
// Audio is written in chunks of this many frames (20480 bytes, a whole number
// of SD sectors). The ring and the staging memory hold whole chunks, so a chunk
// never wraps around in either.
#define FLASHBACK_AUDIO_CHUNK_FRAMES    32
#define FLASHBACK_AUDIO_STAGE_FRAMES    128         // ~0.6 s of staging
// The audio ring holds this much more than the clip length, so a clip's audio
// is all still there when it's saved.
#define FLASHBACK_AUDIO_EXTRA_SECONDS   2

static volatile bool g_wantAudio = true;    // chosen in the menu
static volatile bool g_audio;               // in effect (part of the recording format)
static u32 g_audioCapFrames;                // audio ring capacity in frames (0 without audio)
static u32 g_audioBytes;                    // the audio ring's size: the video slots start after it
static u32 g_audioStageOffset;              // audio staging memory, in g_mem

// Audio frames since the ring started: captured (audio thread), written in
// whole chunks (writer; frees their staging), and on the SD card (writer; ahead
// of g_audioConsumed after an unfinished chunk was written for a save).
static volatile u32 g_audioProduced, g_audioConsumed, g_audioFlushed;
static volatile u32 g_audioMissed;          // frames filled with silence because the thread missed them
static volatile bool g_audioCapture;        // capture thread -> audio thread: capture now
static volatile bool g_audioIdle = true;    // audio thread -> capture thread: not capturing
static volatile bool g_audioFlushRequest;   // writer: write unfinished chunks too (for a save)

// Audio position (samples since the ring started) of each ring slot's frame.
static u32   g_slotAudioPos[FLASHBACK_MAX_FRAMES];

static MyThread flashbackAudioThread;
static u8 CTR_ALIGN(8) flashbackAudioStack[0x1000];
static bool g_audioRunning;                 // the audio thread exists

// Frames waiting to be written to the ring file. The capture thread captures
// and compresses frames into staging slots, and the writer thread writes them
// out, so capturing the next frame overlaps with writing the previous ones.
// On a New 3DS XL the SD path tops out around 5 MB/s, which is less than full
// resolution at 30 fps needs uncompressed (5.8 MB/s); compression is what
// closes that gap. (Batching several frames per SD request was tried and
// didn't help: the time is in moving the bytes, not in each request.)
#define FLASHBACK_STAGE_SLOTS   4

// Staging slots are used in order: frame number p (counting every frame ever
// staged) goes to staging slot p % FLASHBACK_STAGE_SLOTS, independent of its
// ring slot (stored next to it), so any ring size works.
// g_stageProduced - g_stageConsumed frames are waiting, oldest first, starting
// at staging slot g_stageConsumed % FLASHBACK_STAGE_SLOTS. Both counters only
// ever increase.
static u32 g_stageRingIdx[FLASHBACK_STAGE_SLOTS];  // ring slot of each staged frame
static u32 g_stageLen[FLASHBACK_STAGE_SLOTS];      // bytes to write for each staged frame
static volatile u32 g_stageProduced;    // frames staged (capture thread)
static volatile u32 g_stageConsumed;    // frames written or given up on (writer thread)
static volatile bool g_writeFailed;     // a ring write failed (writer -> capture thread)
static bool g_ringLost;                 // a save couldn't reopen/recreate the ring file
static LightEvent g_writeEvent;         // signaled when a frame is staged

static MyThread flashbackWriterThread;
static u8 CTR_ALIGN(8) flashbackWriterStack[0x1000];
static bool g_writerRunning;            // false: frames are written inline by the capture thread

// Performance stats shown in the menu, updated about once a second while recording.
static volatile u32 g_statFpsX10;       // frames actually recorded per second, x10
static volatile u32 g_statCaptureUs;    // average time to read + convert one frame
static volatile u32 g_statCompressUs;   // average time to compress one frame
static volatile u32 g_statSizePct;      // average compressed size, % of the uncompressed frame
static volatile u32 g_statWriteUs;      // average SD write time per frame
static volatile u32 g_lastSaveMs;       // duration of the last save
static volatile u32 g_lastSaveExpected; // frames the last save should have written
static const char *volatile g_recordError; // why recording switched itself off, or NULL

// Written by the writer thread only, never reset (wrap-around safe deltas).
static volatile u32 g_writeTotalUs, g_writeTotalFrames;

static u64 g_winStart, g_winCaptureTicks, g_winCompressTicks, g_winPackedBytes;
static u32 g_winFrames, g_winWriteUs0, g_winWriteFrames0;

static u32 flashbackAudioFramesFor(u32 seconds)
{
    // (64-bit: SYSCLOCK_ARM11 is a 32-bit constant, and 16 s of ticks don't fit in 32 bits.)
    u32 frames = (u32)((u64)(seconds + FLASHBACK_AUDIO_EXTRA_SECONDS) * SYSCLOCK_ARM11 / FLASHBACK_AUDIO_FRAME_TICKS) + 1;
    return (frames + FLASHBACK_AUDIO_CHUNK_FRAMES - 1) / FLASHBACK_AUDIO_CHUNK_FRAMES * FLASHBACK_AUDIO_CHUNK_FRAMES;
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
    // Every ring slot starts on a 512-byte SD sector (half resolution frames,
    // 48000 bytes, otherwise don't), so writes never need the card to read
    // and rewrite a partially covered sector.
    g_slotStride = (g_slotBytes + 511) & ~511;
    g_bmpBytes = FLASHBACK_BMP_HEADER + g_slotBytes;
    // Staging slots first, then a buffer that holds the frame being captured
    // (before compression) while recording, or a BMP / decompression scratch
    // area while saving; the two never overlap in time.
    g_bmpOffset = (FLASHBACK_STAGE_SLOTS * g_slotStride + 0xFFF) & ~0xFFF;
    g_memSize = (g_bmpOffset + g_bmpBytes + 0xFFF) & ~0xFFF;

    // Audio: a ring at the start of the ring file (a whole number of chunks,
    // so the video slots after it stay sector-aligned), and staging memory.
    g_audio = audio;
    g_audioCapFrames = audio ? flashbackAudioFramesFor(g_lengthChoices[lenIdx]) : 0;
    g_audioBytes = g_audioCapFrames * FLASHBACK_AUDIO_FRAME_BYTES;
    g_audioStageOffset = g_memSize;
    if (audio)
        g_memSize += (FLASHBACK_AUDIO_STAGE_FRAMES * FLASHBACK_AUDIO_FRAME_BYTES + 0xFFF) & ~0xFFF;
}

static void flashbackResetStats(void)
{
    g_statFpsX10 = g_statCaptureUs = g_statCompressUs = g_statSizePct = g_statWriteUs = 0;
    g_winStart = svcGetSystemTick();
    g_winCaptureTicks = g_winCompressTicks = g_winPackedBytes = 0;
    g_winFrames = 0;
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
    }

    u32 writeUs = g_writeTotalUs, writeFrames = g_writeTotalFrames;
    if (writeFrames != g_winWriteFrames0)
        g_statWriteUs = (writeUs - g_winWriteUs0) / (writeFrames - g_winWriteFrames0);

    g_winStart = now;
    g_winCaptureTicks = g_winCompressTicks = g_winPackedBytes = 0;
    g_winFrames = 0;
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
// The previous pixel starts as 0 and the table as all 0, for each frame.

#define FLASHBACK_PACK_RUN_MAX  62
#define FLASHBACK_PACK_RAW      0xFE

static inline u32 flashbackPackHash(u16 px)
{
    return ((u32)px * 0x9E3779B1u) >> 26;   // 6 bits
}

// Wraps a channel difference into the signed range of a 'bits'-wide channel.
static inline s32 flashbackPackWrap(s32 d, u32 bits)
{
    d &= (1 << bits) - 1;
    return d >= (1 << (bits - 1)) ? d - (1 << bits) : d;
}

// Compresses 'pixels' RGB565 pixels into 'dst'. Returns the compressed size,
// or 0 if it wouldn't be smaller than 'cap' bytes (store the frame as is).
static u32 flashbackCompress(const u16 *src, u32 pixels, u8 *dst, u32 cap)
{
    u16 index[64];
    memset(index, 0, sizeof(index));
    u16 prev = 0;
    u32 run = 0;
    u8 *out = dst;
    const u8 *limit = dst + cap - 4;    // room for a run flush plus the largest op

    for (u32 i = 0; i < pixels; i++)
    {
        u16 px = src[i];
        if (px == prev)
        {
            if (++run == FLASHBACK_PACK_RUN_MAX)
            {
                if (out > limit)
                    return 0;
                *out++ = 0xC0 | (FLASHBACK_PACK_RUN_MAX - 1);
                run = 0;
            }
            continue;
        }

        if (out > limit)
            return 0;
        if (run != 0)
        {
            *out++ = 0xC0 | (run - 1);
            run = 0;
        }

        u32 h = flashbackPackHash(px);
        if (index[h] == px)
            *out++ = (u8)h;
        else
        {
            index[h] = px;
            s32 dr = flashbackPackWrap((s32)(px >> 11) - (s32)(prev >> 11), 5);
            s32 dg = flashbackPackWrap((s32)((px >> 5) & 0x3F) - (s32)((prev >> 5) & 0x3F), 6);
            s32 db = flashbackPackWrap((s32)(px & 0x1F) - (s32)(prev & 0x1F), 5);
            s32 tr = dr - (dg >> 1), tb = db - (dg >> 1);

            if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1)
                *out++ = 0x40 | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2);
            else if (tr >= -8 && tr <= 7 && tb >= -8 && tb <= 7)
            {
                *out++ = 0x80 | (dg + 32);
                *out++ = ((tr + 8) << 4) | (tb + 8);
            }
            else
            {
                *out++ = FLASHBACK_PACK_RAW;
                *out++ = (u8)px;
                *out++ = (u8)(px >> 8);
            }
        }
        prev = px;
    }

    if (run != 0)
    {
        if (out > limit)
            return 0;
        *out++ = 0xC0 | (run - 1);
    }
    return (u32)(out - dst);
}

// Decompresses exactly 'pixels' pixels from exactly 'len' bytes. Returns false
// if the data is malformed (never writes past 'dst' + 'pixels').
static bool flashbackDecompress(const u8 *src, u32 len, u16 *dst, u32 pixels)
{
    u16 index[64];
    memset(index, 0, sizeof(index));
    u16 px = 0;
    const u8 *p = src, *end = src + len;
    u32 i = 0;

    while (i < pixels)
    {
        if (p >= end)
            return false;
        u8 b = *p++;

        if (b == FLASHBACK_PACK_RAW)
        {
            if (end - p < 2)
                return false;
            px = p[0] | (p[1] << 8);
            p += 2;
        }
        else if ((b & 0xC0) == 0xC0)
        {
            u32 run = (b & 0x3F) + 1;
            if (run > pixels - i)
                return false;
            while (run-- != 0)
                dst[i++] = px;
            continue;
        }
        else if ((b & 0xC0) == 0x00)
        {
            px = index[b];
            dst[i++] = px;
            continue;
        }
        else
        {
            s32 dr, dg, db;
            if ((b & 0xC0) == 0x40)
            {
                dr = ((b >> 4) & 3) - 2;
                dg = ((b >> 2) & 3) - 2;
                db = (b & 3) - 2;
            }
            else
            {
                if (p >= end)
                    return false;
                u8 b2 = *p++;
                dg = (b & 0x3F) - 32;
                dr = ((b2 >> 4) - 8) + (dg >> 1);
                db = ((b2 & 0xF) - 8) + (dg >> 1);
            }
            u32 r = ((px >> 11) + dr) & 0x1F;
            u32 g = (((px >> 5) & 0x3F) + dg) & 0x3F;
            u32 bl = ((px & 0x1F) + db) & 0x1F;
            px = (u16)((r << 11) | (g << 5) | bl);
        }

        index[flashbackPackHash(px)] = px;
        dst[i++] = px;
    }

    return p == end;
}

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

// Converts the whole frame. Always inlined into one call per format with a
// constant 'fmt' / 'bpp', so the per-pixel format switch is resolved at compile
// time instead of running for every pixel.
static inline __attribute__((always_inline)) void flashbackConvertFrame(
    const FlashbackCaptureArgs *args, const u8 *fb, u32 stride, u32 stepX, u32 stepY, u32 fmt, u32 bpp)
{
    const u32 outW = args->outW, outH = args->outH;
    const u32 colStep = stepX * stride;
    const u32 rowStep = stepY * bpp;

    for (u32 ox = 0; ox < outW; ox++)
    {
        // Output row 0 (top) is framebuffer y = 239, the end of the column;
        // walk the column backwards from there.
        const u8 *src = fb + ox * colStep + (outH - 1) * rowStep;
        u16 *out = args->dst + ox;
        for (u32 oy = 0; oy < outH; oy++)
        {
            *out = flashbackPixelToRgb565(src, fmt);
            out += outW;
            src -= rowStep;
        }
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
    switch (fmt)
    {
        case GSP_RGBA8_OES:   flashbackConvertFrame(args, fb, stride, stepX, stepY, GSP_RGBA8_OES, 4); break;
        case GSP_BGR8_OES:    flashbackConvertFrame(args, fb, stride, stepX, stepY, GSP_BGR8_OES, 3); break;
        case GSP_RGB565_OES:  flashbackConvertFrame(args, fb, stride, stepX, stepY, GSP_RGB565_OES, 2); break;
        case GSP_RGB5_A1_OES: flashbackConvertFrame(args, fb, stride, stepX, stepY, GSP_RGB5_A1_OES, 2); break;
        case GSP_RGBA4_OES:   flashbackConvertFrame(args, fb, stride, stepX, stepY, GSP_RGBA4_OES, 2); break;
        default: return;
    }

    // If the game swapped buffers mid-read, the buffer we were reading may
    // already be getting overwritten with the next frame.
    // (Only compare the format register fields used above: pixel format, 3D, 800px mode.)
    args->torn = flashbackCurrentTopFramebuffer() != pa || ((GPU_FB_TOP_FMT ^ fmtReg) & (7 | BIT(5) | BIT(6))) != 0;
    args->ok = true;
}

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
        const u8 *src = g_mem + g_audioStageOffset + (consumed % FLASHBACK_AUDIO_STAGE_FRAMES) * FLASHBACK_AUDIO_FRAME_BYTES;
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
// thread, or inline on the capture thread if the writer couldn't be started.
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

        u32 slot = consumed % FLASHBACK_STAGE_SLOTS;

        // After a failure, frames are only discarded, so the capture thread
        // can stop recording without waiting on a broken card.
        if (!g_writeFailed)
        {
            u64 total;
            u32 len = (g_stageLen[slot] + 511) & ~511;
            if (len > g_slotStride)
                len = g_slotStride;
            u64 t0 = svcGetSystemTick();

            g_ring.pos = g_audioBytes + (u64)g_stageRingIdx[slot] * g_slotStride;   // pre-sized file: always an overwrite
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

// Stops audio capture, and waits until the audio thread is no longer touching
// the staging memory or the audio counters.
static void flashbackAudioPause(void)
{
    g_audioCapture = false;
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
    if (produced - g_audioConsumed >= FLASHBACK_AUDIO_STAGE_FRAMES)
        return false;

    u32 *dst = (u32 *)(g_mem + g_audioStageOffset + (produced % FLASHBACK_AUDIO_STAGE_FRAMES) * FLASHBACK_AUDIO_FRAME_BYTES);
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
static void flashbackAudioThreadMain(void)
{
    const u8 *region[2] = { (const u8 *)PA_PTR(FLASHBACK_DSP_REGION0), (const u8 *)PA_PTR(FLASHBACK_DSP_REGION1) };
    u32 block[FLASHBACK_AUDIO_FRAME_BYTES / 4];
    bool capturing = false;
    u16 last = 0;
    u64 lastTick = 0, runTicks = 0;
    u32 runStart = 0;

    while (!preTerminationRequested)
    {
        // (The capture thread clears g_audioCapture and then waits for
        // g_audioIdle; checking g_audioCapture again after clearing g_audioIdle
        // means it can never miss an iteration that's under way.)
        if (!g_audioCapture)
        {
            capturing = false;
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
            // A new run: take frames from the next one on.
            capturing = true;
            last = top;
            lastTick = now;
            runTicks = 0;
            runStart = g_audioProduced;
            svcSleepThread(1 * 1000 * 1000LL);
            continue;
        }

        runTicks += now - lastTick;
        lastTick = now;
        u32 wanted = (u32)(runTicks / FLASHBACK_AUDIO_FRAME_TICKS);   // frames this run should have by now
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
            u32 have = g_audioProduced - runStart;
            u32 room = wanted + 1 > have + 1 ? wanted - have : 0;
            u32 fill = step > 1 && step <= 16 ? step - 1u : 0;
            for (u32 i = 0; i < fill && i < room && flashbackAppendAudio(NULL); i++)
                g_audioMissed++;
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
        while (g_audioProduced - runStart + 3 < wanted && flashbackAppendAudio(NULL))
            ;

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
        res = IFile_SetSize(&g_ring, g_audioBytes + (u64)g_frames * g_slotStride);
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

    res = svcControlMemoryEx(&tmp, FLASHBACK_MEM_ADDR, 0, g_memSize,
                             MEMOP_ALLOC | MEMOP_REGION_SYSTEM, MEMPERM_READWRITE, true);
    if (R_FAILED(res))
        return res;
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
    flashbackAudioReset();
    g_writeFailed = false;
    g_ringLost = false;
    g_active = true;
    flashbackResetStats();
    return 0;
}

static void flashbackStop(void)
{
    u32 tmp;
    if (!g_active)
        return;

    flashbackAudioPause();
    flashbackDrainWrites();     // the writer must be done with the file and slots
    IFile_Close(&g_ring);
    svcControlMemory(&tmp, FLASHBACK_MEM_ADDR, 0, g_memSize, MEMOP_FREE, 0);
    g_mem = NULL;
    g_active = false;
    g_count = 0;
}

// Captures and compresses a frame into the next staging slot and hands it to
// the writer. Returns false if no frame was recorded this tick (capture failed,
// or every staging slot is still waiting to be written because the SD card is
// behind).
static bool flashbackRecordFrame(void)
{
    if (g_stageProduced - g_stageConsumed >= FLASHBACK_STAGE_SLOTS)
        return false;

    u32 slot = g_stageProduced % FLASHBACK_STAGE_SLOTS;   // the oldest free staging slot
    u8 *stage = g_mem + slot * g_slotStride;
    u16 *frame = (u16 *)(g_mem + g_bmpOffset);  // capture buffer (only saves use it otherwise)

    // Where the audio is as the frame is captured (taken after the capture and
    // compression, it put the audio up to ~20 ms early in the clip).
    u32 audioPos = g_audioProduced * FLASHBACK_AUDIO_FRAME_SAMPLES;

    u64 t0 = svcGetSystemTick();
    if (!flashbackCapture(frame))
        return false;
    u64 t1 = svcGetSystemTick();

    // Frames that wouldn't get smaller are stored as they are.
    u32 len = flashbackCompress(frame, g_w * g_h, stage, g_slotBytes);
    if (len == 0 || len >= g_slotBytes)
    {
        memcpy(stage, frame, g_slotBytes);
        len = g_slotBytes;
    }

    g_winCaptureTicks += t1 - t0;
    g_winCompressTicks += svcGetSystemTick() - t1;
    g_winPackedBytes += len;
    g_winFrames++;

    g_repeat[g_head] = 0;
    g_slotLen[g_head] = len;
    g_slotAudioPos[g_head] = audioPos;
    g_stageRingIdx[slot] = g_head;
    g_stageLen[slot] = len;
    __sync_synchronize();   // frame, ring slot and size must be visible before the frame is handed over
    g_stageProduced++;

    g_head = (g_head + 1) % g_frames;
    if (g_count < g_frames)
        g_count++;

    if (g_writerRunning)
        LightEvent_Signal(&g_writeEvent);
    else
        flashbackWritePending();
    return true;
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

static inline void flashbackPut16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static inline void flashbackPut32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

// Converts one upright RGB565 frame into a complete 16-bit RGB565 BMP
// (BI_BITFIELDS). Lossless: frames are already RGB565, so this is just a header
// and the rows in bottom-up order.
static void flashbackEncodeBmp(const u16 *src, u8 *out)
{
    const u32 rowBytes = g_w * 2;               // 400 or 800: already a multiple of 4
    const u32 pixBytes = rowBytes * g_h;

    memset(out, 0, FLASHBACK_BMP_HEADER);
    out[0] = 'B'; out[1] = 'M';
    flashbackPut32(out + 2,  FLASHBACK_BMP_HEADER + pixBytes); // file size
    flashbackPut32(out + 10, FLASHBACK_BMP_HEADER);            // pixel data offset
    flashbackPut32(out + 14, 40);               // BITMAPINFOHEADER size
    flashbackPut32(out + 18, g_w);
    flashbackPut32(out + 22, g_h);              // positive height = rows stored bottom-up
    flashbackPut16(out + 26, 1);                // planes
    flashbackPut16(out + 28, 16);               // bits per pixel
    flashbackPut32(out + 30, 3);                // BI_BITFIELDS
    flashbackPut32(out + 34, pixBytes);
    flashbackPut32(out + 38, 2835);             // ~72 dpi
    flashbackPut32(out + 42, 2835);
    flashbackPut32(out + 54, 0xF800);           // red mask
    flashbackPut32(out + 58, 0x07E0);           // green mask
    flashbackPut32(out + 62, 0x001F);           // blue mask

    u8 *dst = out + FLASHBACK_BMP_HEADER;
    for (u32 y = g_h; y-- > 0; dst += rowBytes) // bottom row first
        memcpy(dst, src + y * g_w, rowBytes);
}

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

// Reads 'count' ring slots starting at 'idx' (which must not wrap) as
// uncompressed frames, g_slotBytes apart, into 'dst'. Only each slot's actual
// contents are read; compressed ones go through the scratch buffer (free while
// saving) and are decompressed into place.
static bool flashbackReadSlots(u32 idx, u32 count, u8 *dst)
{
    u8 *scratch = g_mem + g_bmpOffset;

    for (u32 i = 0; i < count; i++)
    {
        u64 total;
        u32 len = g_slotLen[idx + i];
        u8 *frame = dst + i * g_slotBytes;
        bool packed = len < g_slotBytes;

        g_ring.pos = g_audioBytes + (u64)(idx + i) * g_slotStride;
        if (R_FAILED(IFile_Read(&g_ring, &total, packed ? scratch : frame, len)) || total != len)
            return false;
        if (packed && !flashbackDecompress(scratch, len, (u16 *)frame, g_w * g_h))
            return false;
    }
    return true;
}

// How many plan slots starting at 'i' sit next to each other in the ring file
// (i.e. stop where the ring wraps), at most 'max'.
static u32 flashbackContiguousSlots(const FlashbackClipPlan *plan, u32 i, u32 max)
{
    u32 copies;
    u32 idx0 = flashbackPlanSlot(plan, i, &copies);
    u32 k = 1;
    while (k < max && i + k < plan->slots && idx0 + k < g_frames)
        k++;
    return k;
}

// Temporary buffer used only while saving, so reads and writes can move many
// frames per SD request. Taken from the SYSTEM region and freed right after.
#define FLASHBACK_SAVE_BUF_ADDR 0x0DA00000
#define FLASHBACK_SAVE_BUF_MAX  (1024 * 1024)

typedef struct FlashbackSaveBuffer {
    u8 *data;
    u32 frames;     // capacity in frames
    u32 allocSize;  // 0 if we're using the recorder's own staging slots
} FlashbackSaveBuffer;

// Needs the staging slots to be idle (flashbackDrainWrites), which they are
// during a save. A separate buffer is only borrowed when it's meaningfully
// bigger than the staging slots (at half resolution); at full resolution the
// staging slots are used directly, keeping peak memory use down.
static FlashbackSaveBuffer flashbackAllocSaveBuffer(void)
{
    FlashbackSaveBuffer buf = { g_mem, FLASHBACK_STAGE_SLOTS, 0 };
    u32 tmp;

    for (u32 frames = FLASHBACK_SAVE_BUF_MAX / g_slotBytes; frames > 2 * FLASHBACK_STAGE_SLOTS; frames /= 2)
    {
        u32 size = (frames * g_slotBytes + 0xFFF) & ~0xFFF;
        if (R_SUCCEEDED(svcControlMemoryEx(&tmp, FLASHBACK_SAVE_BUF_ADDR, 0, size,
                                           MEMOP_ALLOC | MEMOP_REGION_SYSTEM, MEMPERM_READWRITE, true)))
        {
            buf.data = (u8 *)FLASHBACK_SAVE_BUF_ADDR;
            buf.frames = frames;
            buf.allocSize = size;
            break;
        }
    }

    return buf;
}

static void flashbackFreeSaveBuffer(FlashbackSaveBuffer *buf)
{
    u32 tmp;
    if (buf->allocSize != 0)
        svcControlMemory(&tmp, FLASHBACK_SAVE_BUF_ADDR, 0, buf->allocSize, MEMOP_FREE, 0);
    buf->allocSize = 0;
}

// The oldest sample the audio ring still holds, when 'end' is one past the
// newest: all but the oldest chunk, which a partly written chunk (see
// flashbackWriteAudio) may have overwritten.
static u32 flashbackAudioOldest(u32 end)
{
    u32 held = (g_audioCapFrames - FLASHBACK_AUDIO_CHUNK_FRAMES) * FLASHBACK_AUDIO_FRAME_SAMPLES;
    return end > held ? end - held : 0;
}

// Reads audio samples first .. first + count - 1 from the audio ring into 'dst'
// (stereo PCM16); samples the ring doesn't hold (any more, or yet) are silence.
static bool flashbackReadAudio(u32 first, u32 count, u8 *dst)
{
    u32 cap = g_audioCapFrames * FLASHBACK_AUDIO_FRAME_SAMPLES;
    u32 end = g_audioFlushed * FLASHBACK_AUDIO_FRAME_SAMPLES;
    u32 oldest = flashbackAudioOldest(end);

    memset(dst, 0, count * 4);
    u32 a = first < oldest ? oldest : first;
    u32 b = first + count < end ? first + count : end;
    while (a < b)
    {
        u64 total;
        u32 pos = a % cap;
        u32 n = b - a < cap - pos ? b - a : cap - pos;
        g_ring.pos = (u64)pos * 4;
        if (R_FAILED(IFile_Read(&g_ring, &total, dst + (a - first) * 4, n * 4)) || total != n * 4)
            return false;
        a += n;
    }
    return true;
}

// With BMP frames, the clip's audio goes in <clip>/audio.wav.
static bool flashbackSaveWav(FS_Archive archive, const char *clip, const FlashbackClipPlan *plan, u8 *buf, u32 bufBytes)
{
    u32 copies;
    u32 idx = flashbackPlanSlot(plan, 0, &copies);
    u32 first = g_slotAudioPos[idx] + flashbackAudioSamplesFor(plan->trimFirst);
    u32 count = flashbackAudioSamplesFor(plan->frames);
    u32 rate = FLASHBACK_AUDIO_RATE_MHZ / 1000;
    char path[128];
    IFile file = {0};
    u64 total;

    sprintf(path, "%s/audio.wav", clip);
    if (R_FAILED(IFile_OpenFromArchive(&file, archive, fsMakePath(PATH_ASCII, path), FS_OPEN_CREATE | FS_OPEN_WRITE)))
        return false;

    u8 header[44];
    memcpy(header, "RIFF", 4);
    flashbackPut32(header + 4, 36 + count * 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    flashbackPut32(header + 16, 16);
    flashbackPut16(header + 20, 1);             // PCM
    flashbackPut16(header + 22, 2);             // stereo
    flashbackPut32(header + 24, rate);
    flashbackPut32(header + 28, rate * 4);      // bytes per second
    flashbackPut16(header + 32, 4);             // bytes per sample frame
    flashbackPut16(header + 34, 16);            // bits per sample
    memcpy(header + 36, "data", 4);
    flashbackPut32(header + 40, count * 4);

    bool ok = R_SUCCEEDED(IFile_Write(&file, &total, header, sizeof(header), 0)) && total == sizeof(header);
    u32 perRead = bufBytes / 4;
    for (u32 done = 0; ok && done < count; )
    {
        u32 n = count - done < perRead ? count - done : perRead;
        ok = flashbackReadAudio(first + done, n, buf) &&
             R_SUCCEEDED(IFile_Write(&file, &total, buf, n * 4, 0)) && total == n * 4;
        done += n;
    }
    IFile_Close(&file);
    return ok;
}

// BMP format: <clip>/frame_000.bmp, frame_001.bmp, ... (and audio.wav with
// audio). Frames are read from the ring in batches; each BMP is still its own file.
static s32 flashbackSaveBmp(FS_Archive archive, const char *clip, const FlashbackClipPlan *plan)
{
    u8 *bmp = g_mem + g_bmpOffset;
    char path[128];
    s32 written = 0;

    if (R_FAILED(flashbackCreateDirectory(archive, clip)))
        return -2;

    FlashbackSaveBuffer buf = flashbackAllocSaveBuffer();

    for (u32 i = 0; i < plan->slots; )
    {
        u32 copies;
        u32 idx = flashbackPlanSlot(plan, i, &copies);
        u32 k = flashbackContiguousSlots(plan, i, buf.frames);
        if (!flashbackReadSlots(idx, k, buf.data))
            goto end;

        for (u32 j = 0; j < k; j++, i++)
        {
            flashbackPlanSlot(plan, i, &copies);
            flashbackEncodeBmp((const u16 *)(buf.data + j * g_slotBytes), bmp);

            for (u32 c = 0; c < copies; c++)
            {
                u64 total;
                IFile file = {0};
                sprintf(path, "%s/frame_%03ld.bmp", clip, written);
                if (R_FAILED(IFile_OpenFromArchive(&file, archive, fsMakePath(PATH_ASCII, path), FS_OPEN_CREATE | FS_OPEN_WRITE)))
                    goto end;
                Result res = IFile_Write(&file, &total, bmp, g_bmpBytes, 0);
                IFile_Close(&file);
                if (R_FAILED(res) || total != g_bmpBytes)
                    goto end;
                written++;
            }
        }
    }

    if (g_audio)
        flashbackSaveWav(archive, clip, plan, buf.data, buf.frames * g_slotBytes);

end:
    flashbackFreeSaveBuffer(&buf);
    return written;
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
//   [ring slots]    as in version 1, after the audio ring
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
    u32 missed;         // audio frames (of 160 samples) filled with silence because the recorder missed them
    u32 reserved;
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
            *entry++ = g_audioBytes + idx * g_slotStride;
            *entry++ = g_slotLen[idx];
            *entry++ = copies;
            // (The oldest entry may start a few copies in: see flashbackPlanClip.)
            *entry++ = g_slotAudioPos[idx] + flashbackAudioSamplesFor(i == 0 ? plan->trimFirst : 0);
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
        a->end = g_audioFlushed * FLASHBACK_AUDIO_FRAME_SAMPLES;
        a->first = flashbackAudioOldest(a->end);
        a->missed = g_audioMissed;
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
    u32 dataEnd = g_audioBytes + (g_count < g_frames ? g_count : g_frames) * g_slotStride;
    u8 *index = g_mem + g_bmpOffset;    // scratch buffer, free while saving
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

// Returns the number of frames written, or a negative value:
//   -1 nothing recorded, -2 couldn't create the clip file/directory, -3 couldn't write any frame.
static s32 flashbackSave(void)
{
    if (!g_active || g_count == 0)
        return -1;

    // Frames and audio still waiting to be written belong at the end of the
    // clip, and the save reads the ring file and reuses the staging slots.
    // (Audio capture resumes along with recording, once the save is done.)
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
    g_lastSaveExpected = plan.frames;
    bool clipFile = g_saveFormat == FLASHBACK_FORMAT_RAW;
    s32 written = clipFile ? flashbackSaveClipFile(archive, clip, &plan) : flashbackSaveBmp(archive, clip, &plan);

    if (written == 0 && !clipFile)
        FSUSER_DeleteDirectory(archive, fsMakePath(PATH_ASCII, clip)); // empty clip folder

    FSUSER_CloseArchive(archive);
    if (written > 0)
        sprintf(g_lastClip, "%s%s", clip, clipFile ? ".flb" : "/");
    return written != 0 ? written : -3;
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
    u8  format;         // FlashbackFormat
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
    s.format = (u8)g_saveFormat;
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
    if (s.format < FLASHBACK_FORMAT_COUNT)
        g_saveFormat = s.format;
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
                bool complete = res > 0 && (u32)res == g_lastSaveExpected;
                if (!menuIsOpen())
                {
                    flashbackSetTint(complete ? FLASHBACK_TINT_OK : FLASHBACK_TINT_FAILED);
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
        g_audioCapture = recording && g_audio;
        if (recording && !wasRecording)
            flashbackResetStats();
        if (recording)
        {
            if (!flashbackRecordFrame())
                flashbackFillMissed(1);
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
            flashbackFillMissed(missed);
            nextTick += (u64)missed * period;
        }
        else
            nextTick = now;
    }

    flashbackStop();    // waits for the writer to finish first

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

    // Both threads have the lowest priorities among Rosalina's threads and run
    // on the system core. The writer is one step *above* the capture thread:
    // it hardly uses the CPU (it mostly waits on the SD card), and running
    // first means it sends the next write the moment the previous one finishes.
    // Below the capture thread, it had to wait out each frame's capture and
    // compression (~15 ms) with the card idle; an SD speed test on a New 3DS XL
    // put a ~100 KB write at ~20 ms, but recording measured 31-36 ms per write.
    // Neither thread is essential to Rosalina: without the writer, frames are
    // written inline, and without the capture thread the recorder stays off.
    // The audio thread, on the other hand, runs well above them: it has to
    // catch each audio frame within the ~8 ms it stays put, so it wakes every
    // millisecond or few, but it uses very little CPU. Without it, there's no
    // audio.
    g_audioRunning = R_SUCCEEDED(MyThread_Create(&flashbackAudioThread, flashbackAudioThreadMain, flashbackAudioStack,
                                                 sizeof(flashbackAudioStack), 25, CORE_SYSTEM));
    g_writerRunning = R_SUCCEEDED(MyThread_Create(&flashbackWriterThread, flashbackWriterThreadMain, flashbackWriterStack,
                                                  sizeof(flashbackWriterStack), 59, CORE_SYSTEM));
    g_threadRunning = R_SUCCEEDED(MyThread_Create(&flashbackThread, flashbackThreadMain, flashbackThreadStack,
                                                  sizeof(flashbackThreadStack), 60, CORE_SYSTEM));
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
        Draw_ClearFramebuffer();
        Draw_DrawString(10, 10, COLOR_TITLE, "Flashback recorder");

        u32 posY = 30;
        if (!g_threadRunning)
            posY = Draw_DrawString(10, posY, COLOR_RED, "Recorder thread failed to start.\n\n");
        else if (!sdMode)
            posY = Draw_DrawString(10, posY, COLOR_RED, "Only available when booting from the SD card.\n\n");

        posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Recording:  %s\n", g_enabled ? "on" : "off");
        posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Resolution: %s\n",
                                        res == FLASHBACK_RES_FULL ? "full (400x240)" : "half (200x120)");
        // Size of a saved clip of the full length: a .flb is the ring file
        // (frames rounded up to whole SD sectors, and the audio ring), BMPs are
        // frames plus headers (and a WAV).
        u32 frameBytes = (res == FLASHBACK_RES_FULL ? 400 * 240 : 200 * 120) * 2;
        u32 perFrame = g_saveFormat == FLASHBACK_FORMAT_RAW ? (frameBytes + 511) & ~511 : frameBytes + FLASHBACK_BMP_HEADER;
        u64 clipBytes = (u64)frames * perFrame;
        if (audio && g_saveFormat == FLASHBACK_FORMAT_RAW)
            clipBytes += (u64)flashbackAudioFramesFor(seconds) * FLASHBACK_AUDIO_FRAME_BYTES;
        else if (audio)
            clipBytes += (u64)seconds * FLASHBACK_AUDIO_RATE_MHZ / 1000 * 4;
        u32 clipTenthsMb = (u32)((clipBytes + 50000) / 100000);     // in tenths of a MB (1 MB = 1,000,000 bytes)

        if (clipTenthsMb < 100)
            posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Frame rate: %lu fps   Length: %lu s (%lu.%lu MB)\n",
                                            fps, seconds, clipTenthsMb / 10, clipTenthsMb % 10);
        else
            posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Frame rate: %lu fps   Length: %lu s (%lu MB)\n",
                                            fps, seconds, (clipTenthsMb + 5) / 10);
        // (One line for both: the screen has no room for another.)
        posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Format:     %s   Audio: %s\n\n",
                                        g_saveFormat == FLASHBACK_FORMAT_RAW ? "clip file (.flb, fast)" : "BMP frames (slow)",
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

        posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Buffered:  %lu / %lu frames, %lu dropped\n",
                                        buffered, frames, dropped);
        if (applied && g_active && g_audio)
            posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Rate:      %lu.%lu / %lu fps   audio: %lu missed\n",
                                            g_statFpsX10 / 10, g_statFpsX10 % 10, fps, g_audioMissed);
        else
            posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Rate:      %lu.%lu / %lu fps\n",
                                            g_statFpsX10 / 10, g_statFpsX10 % 10, fps);
        posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Per frame: capture %lu.%lu, compress %lu.%lu, write %lu.%lums\n",
                                        g_statCaptureUs / 1000, (g_statCaptureUs / 100) % 10,
                                        g_statCompressUs / 1000, (g_statCompressUs / 100) % 10,
                                        g_statWriteUs / 1000, (g_statWriteUs / 100) % 10);
        posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Budget:    %lums per frame, compressed to %lu%%\n\n",
                                        1000 / fps, g_statSizePct);

        const char *recordError = g_recordError;
        if (g_saving || g_saveRequested)
            posY = Draw_DrawString(10, posY, COLOR_WHITE, "Saving...\n");
        else if (recordError != NULL)
            posY = Draw_DrawFormattedString(10, posY, COLOR_RED, "%s\n", recordError);
        else if (g_lastSaveResult > 0 && (u32)g_lastSaveResult < g_lastSaveExpected)
            posY = Draw_DrawFormattedString(10, posY, COLOR_RED, "Incomplete: saved %ld of %lu frames to\n%s\n",
                                            g_lastSaveResult, g_lastSaveExpected, g_lastClip);
        else if (g_lastSaveResult > 0)
            posY = Draw_DrawFormattedString(10, posY, COLOR_GREEN, "Saved %ld frames in %lu.%lus to\n%s\n", g_lastSaveResult,
                                            g_lastSaveMs / 1000, (g_lastSaveMs / 100) % 10, g_lastClip);
        else if (g_lastSaveResult < 0)
            posY = Draw_DrawFormattedString(10, posY, COLOR_RED, "Save failed (%ld).\n", g_lastSaveResult);

        posY += 10;
        posY = Draw_DrawString(10, posY, COLOR_WHITE, "A: recording on/off   X: save clip now   B: back\n");
        posY = Draw_DrawString(10, posY, COLOR_WHITE, "Y: resolution   Left/Right: fps   L/R: length\n");
        posY = Draw_DrawString(10, posY, COLOR_WHITE, "Up/Down: format   Start: audio   Select: hotkey\n");
        posY = Draw_DrawString(10, posY, COLOR_WHITE, "Off or res/fps/length/audio clear the buffer.\n\n");
        LumaConfig_ConvertComboToString(comboStr, g_saveCombo);
        Draw_DrawFormattedString(10, posY, COLOR_WHITE, "In game: %s saves a clip.", comboStr);

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
        else if ((pressed & (KEY_DUP | KEY_DDOWN)) && !busy)
            g_saveFormat = (g_saveFormat + 1) % FLASHBACK_FORMAT_COUNT;
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
