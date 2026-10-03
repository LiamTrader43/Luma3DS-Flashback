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
#define FLASHBACK_RING_FS_PRIORITY  8

// Bottom screen fill colors (0x00BBGGRR) used as feedback for the hotkey.
#define FLASHBACK_FILL_SAVING   0x00808000
#define FLASHBACK_FILL_OK       0x0000C000
#define FLASHBACK_FILL_FAILED   0x000000E0

#define KERNPA2VA_OFFSET()      (GET_VERSION_MINOR(osGetKernelVersion()) < 44 ? 0xD0000000 : 0xC0000000)

// User settings.
typedef enum FlashbackResolution {
    FLASHBACK_RES_HALF = 0,     // 200 x 120
    FLASHBACK_RES_FULL,         // 400 x 240
    FLASHBACK_RES_COUNT,
} FlashbackResolution;

typedef enum FlashbackFormat {
    FLASHBACK_FORMAT_RAW = 0,   // one .raw video file + .txt with the ffmpeg command (fastest)
    FLASHBACK_FORMAT_BMP,       // folder of 16-bit BMP frames
    FLASHBACK_FORMAT_COUNT,
} FlashbackFormat;

// 14-byte file header + 40-byte BITMAPINFOHEADER + 3 RGB565 channel masks.
#define FLASHBACK_BMP_HEADER    (14 + 40 + 12)

static const u32 g_fpsChoices[] = { 10, 20, 30 };
#define FLASHBACK_FPS_CHOICES   (sizeof(g_fpsChoices) / sizeof(g_fpsChoices[0]))
#define FLASHBACK_MAX_FRAMES    (FLASHBACK_SECONDS * 30)

static MyThread flashbackThread;
static u8 CTR_ALIGN(8) flashbackThreadStack[0x2000];
static bool g_threadRunning;

static volatile bool g_enabled;         // user toggle
// Defaults when no settings are saved: full resolution, 30 fps, raw format on
// New 3DS; half resolution, 20 fps on Old 3DS (see flashbackApplyDefaults).
static volatile u32  g_wantRes = FLASHBACK_RES_FULL;   // FlashbackResolution chosen in the menu
static volatile u32  g_wantFpsIdx = 2;                 // index into g_fpsChoices chosen in the menu (30 fps)
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
static u32 g_w, g_h, g_fps, g_frames;   // derived from g_res / g_fpsIdx
static u32 g_slotBytes, g_bmpBytes, g_bmpOffset, g_memSize;

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

// Performance stats shown in the menu, updated about once a second while recording.
static volatile u32 g_statFpsX10;       // frames actually recorded per second, x10
static volatile u32 g_statDropped;      // frames skipped because we fell behind, since recording started
static volatile u32 g_statCaptureUs;    // average time to read + convert one frame
static volatile u32 g_statWriteUs;      // average time to write one frame to the SD card
static volatile u32 g_lastSaveMs;       // duration of the last save
static volatile u32 g_lastSaveExpected; // frames the last save should have written

static u64 g_winStart, g_winCaptureTicks, g_winWriteTicks;
static u32 g_winFrames;

static void flashbackApplyFormat(u32 res, u32 fpsIdx)
{
    g_res = res;
    g_fpsIdx = fpsIdx;

    g_w = res == FLASHBACK_RES_FULL ? 400 : 200;
    g_h = res == FLASHBACK_RES_FULL ? 240 : 120;
    g_fps = g_fpsChoices[fpsIdx];
    g_frames = FLASHBACK_SECONDS * g_fps;

    g_slotBytes = g_w * g_h * 2;
    g_bmpBytes = FLASHBACK_BMP_HEADER + g_slotBytes;
    g_bmpOffset = (g_slotBytes + 0xFFF) & ~0xFFF;
    g_memSize = (g_bmpOffset + g_bmpBytes + 0xFFF) & ~0xFFF;
}

static void flashbackResetStats(void)
{
    g_statFpsX10 = g_statDropped = g_statCaptureUs = g_statWriteUs = 0;
    g_winStart = svcGetSystemTick();
    g_winCaptureTicks = g_winWriteTicks = 0;
    g_winFrames = 0;
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
        g_statWriteUs = (u32)(g_winWriteTicks * 1000000 / SYSCLOCK_ARM11 / g_winFrames);
    }

    g_winStart = now;
    g_winCaptureTicks = g_winWriteTicks = 0;
    g_winFrames = 0;
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
    bool ok;
    bool torn;      // the displayed buffer changed while we were reading it
} FlashbackCaptureArgs;

// Retries for a torn frame. Each attempt flushes the caches first, so a retry
// never reads lines cached by the previous attempt.
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
            return flashbackRgb565(src[3], src[2], src[1]);
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

static bool flashbackCapture(u16 *dst)
{
    FlashbackCaptureArgs args = { dst, g_w, g_h, KERNPA2VA_OFFSET(), false, false };

    for (u32 attempt = 0; attempt < FLASHBACK_CAPTURE_ATTEMPTS; attempt++)
    {
        // The GPU wrote the framebuffer behind the CPU caches' back.
        svcFlushEntireDataCache();
        svcCustomBackdoor(flashbackCaptureKernel, &args);
        if (!args.ok || !args.torn)
            break;
    }

    // A frame still torn after every attempt is kept: the game is swapping
    // faster than we can read, and a slightly torn frame beats a missing one.
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

static Result flashbackStart(void)
{
    Result res;
    u32 tmp;
    FS_Archive archive;

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
            res = IFile_OpenFromArchive(&g_ring, archive, fsMakePath(PATH_ASCII, FLASHBACK_RING_FILE),
                                        FS_OPEN_CREATE | FS_OPEN_READ | FS_OPEN_WRITE);
        if (R_SUCCEEDED(res))
        {
            // Allocate the whole ring up front, so SD space is claimed now
            // rather than during gameplay, and a full card fails here instead
            // of mid-recording. Old contents don't matter: g_count starts at 0.
            res = IFile_SetSize(&g_ring, (u64)g_frames * g_slotBytes);
            if (R_SUCCEEDED(res))
                FSFILE_SetPriority(g_ring.handle, FLASHBACK_RING_FS_PRIORITY);
            else
                IFile_Close(&g_ring);
        }
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
    g_active = true;
    flashbackResetStats();
    return 0;
}

static void flashbackStop(void)
{
    u32 tmp;
    if (!g_active)
        return;

    IFile_Close(&g_ring);
    svcControlMemory(&tmp, FLASHBACK_MEM_ADDR, 0, g_memSize, MEMOP_FREE, 0);
    g_mem = NULL;
    g_active = false;
    g_count = 0;
}

// Returns false if no frame was recorded this tick.
static bool flashbackRecordFrame(void)
{
    u64 total;
    u16 *frame = (u16 *)g_mem;

    u64 t0 = svcGetSystemTick();
    if (!flashbackCapture(frame))
        return false;
    u64 t1 = svcGetSystemTick();

    g_ring.pos = (u64)g_head * g_slotBytes; // the file is pre-sized, so this is always an overwrite
    if (R_FAILED(IFile_Write(&g_ring, &total, frame, g_slotBytes, 0)) || total != g_slotBytes)
        return false;

    g_winCaptureTicks += t1 - t0;
    g_winWriteTicks += svcGetSystemTick() - t1;
    g_winFrames++;

    g_repeat[g_head] = 0;
    g_head = (g_head + 1) % g_frames;
    if (g_count < g_frames)
        g_count++;
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
    g_statDropped += missed;
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

// Which ring slots make up a clip of the last FLASHBACK_SECONDS, oldest first,
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

// Reads 'count' ring slots starting at 'idx' (which must not wrap) in one request.
static bool flashbackReadSlots(u32 idx, u32 count, u8 *dst)
{
    u64 total;
    u32 len = count * g_slotBytes;
    g_ring.pos = (u64)idx * g_slotBytes;
    return R_SUCCEEDED(IFile_Read(&g_ring, &total, dst, len)) && total == len;
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
    u32 allocSize;  // 0 if we fell back to the recorder's own one-frame buffer
} FlashbackSaveBuffer;

static FlashbackSaveBuffer flashbackAllocSaveBuffer(void)
{
    FlashbackSaveBuffer buf = { g_mem, 1, 0 };
    u32 tmp;

    for (u32 frames = FLASHBACK_SAVE_BUF_MAX / g_slotBytes; frames > 1; frames /= 2)
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

// BMP format: <clip>/frame_000.bmp, frame_001.bmp, ... Frames are read from the
// ring in batches; each BMP is still its own file.
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

end:
    flashbackFreeSaveBuffer(&buf);
    return written;
}

static void flashbackWriteTextFile(FS_Archive archive, const char *path, const char *text)
{
    IFile file = {0};
    u64 total;
    if (R_SUCCEEDED(IFile_OpenFromArchive(&file, archive, fsMakePath(PATH_ASCII, path), FS_OPEN_CREATE | FS_OPEN_WRITE)))
    {
        IFile_Write(&file, &total, text, strlen(text), 0);
        IFile_Close(&file);
    }
}

// Raw format: <clip>.raw holds every frame back to back (RGB565 little-endian,
// top row first), plus <clip>.txt describing it (read by the PC-side
// convert_flashback.bat) with the ffmpeg command to convert it.
// Writes the first 'frames' frames of the save buffer to the clip in one request.
static bool flashbackWriteFrames(IFile *file, const u8 *data, u32 frames)
{
    u64 total;
    u32 len = frames * g_slotBytes;
    return frames == 0 || (R_SUCCEEDED(IFile_Write(file, &total, data, len, 0)) && total == len);
}

// Fills the raw clip, moving as many frames per SD request as the save buffer
// holds: runs of neighbouring ring slots are read in one request, spread out
// in the buffer to make room for their duplicates, and written in one request.
// Returns the number of frames written.
static s32 flashbackWriteRawFrames(IFile *file, const FlashbackClipPlan *plan, FlashbackSaveBuffer *buf)
{
    u8 *data = buf->data;
    const u32 cap = buf->frames;
    u32 n = 0;          // frames waiting in the buffer
    s32 written = 0;

    for (u32 i = 0; i < plan->slots; )
    {
        u32 copies;
        u32 idx = flashbackPlanSlot(plan, i, &copies);

        if (copies > cap - n)
        {
            if (n != 0)
            {
                // Make room first.
                if (!flashbackWriteFrames(file, data, n))
                    return written;
                written += n;
                n = 0;
                continue;
            }

            // A long run of duplicates (bigger than the whole buffer): fill the
            // buffer with copies of this frame and write it as many times as needed.
            if (!flashbackReadSlots(idx, 1, data))
                return written;
            for (u32 r = 1; r < cap; r++)
                memcpy(data + r * g_slotBytes, data, g_slotBytes);
            for (u32 left = copies; left != 0; )
            {
                u32 w = left < cap ? left : cap;
                if (!flashbackWriteFrames(file, data, w))
                    return written;
                written += w;
                left -= w;
            }
            i++;
            continue;
        }

        // Gather neighbouring ring slots whose frames (with duplicates) fit.
        u32 maxRun = flashbackContiguousSlots(plan, i, cap - n);
        u32 k = 0, need = 0;
        while (k < maxRun)
        {
            u32 c;
            flashbackPlanSlot(plan, i + k, &c);
            if (need + c > cap - n)
                break;
            need += c;
            k++;
        }

        // One read for the whole run, into the next free buffer slots...
        if (!flashbackReadSlots(idx, k, data + n * g_slotBytes))
            return written;

        // ...then spread it out back to front, so each frame only ever moves
        // forward and nothing is overwritten before it is copied.
        u32 dest = n + need;
        for (u32 j = k; j-- > 0; )
        {
            u32 c;
            flashbackPlanSlot(plan, i + j, &c);
            dest -= c;
            const u8 *src = data + (n + j) * g_slotBytes;
            for (u32 r = c; r-- > 0; )
            {
                u8 *d = data + (dest + r) * g_slotBytes;
                if (d != src)
                    memcpy(d, src, g_slotBytes);
            }
        }

        n += need;
        i += k;

        if (n == cap)
        {
            if (!flashbackWriteFrames(file, data, n))
                return written;
            written += n;
            n = 0;
        }
    }

    if (flashbackWriteFrames(file, data, n))
        written += n;
    return written;
}

static s32 flashbackSaveRaw(FS_Archive archive, const char *clip, const char *clipName, const FlashbackClipPlan *plan)
{
    char path[128];
    char *text = (char *)(g_mem + g_bmpOffset);    // the BMP buffer is free in this format
    IFile file = {0};

    sprintf(path, "%s.raw", clip);
    if (R_FAILED(IFile_OpenFromArchive(&file, archive, fsMakePath(PATH_ASCII, path), FS_OPEN_CREATE | FS_OPEN_WRITE)))
        return -2;

    // One allocation up front instead of growing the file frame by frame.
    IFile_SetSize(&file, (u64)plan->frames * g_slotBytes);

    FlashbackSaveBuffer buf = flashbackAllocSaveBuffer();
    s32 written = flashbackWriteRawFrames(&file, plan, &buf);
    flashbackFreeSaveBuffer(&buf);

    if ((u32)written != plan->frames)
        IFile_SetSize(&file, (u64)written * g_slotBytes); // don't leave unwritten frames at the end
    IFile_Close(&file);

    if (written == 0)
    {
        // Nothing usable: don't leave an empty .raw behind.
        FSUSER_DeleteFile(archive, fsMakePath(PATH_ASCII, path));
        return written;
    }

    char command[320];
    sprintf(command,
        "ffmpeg -f rawvideo -pixel_format rgb565le -video_size %lux%lu -framerate %lu -i \"%s.raw\" "
        "-vf \"scale=iw*2:ih*2:flags=neighbor\" -c:v libx264 -crf 12 -pix_fmt yuv420p \"%s.mp4\"",
        g_w, g_h, g_fps, clipName, clipName);

    sprintf(text,
        "Flashback clip\r\n"
        "Resolution:   %lux%lu\r\n"
        "Frame rate:   %lu fps\r\n"
        "Frames:       %ld\r\n"
        "Pixel format: RGB565 little-endian, top row first, no header\r\n"
        "\r\n"
        "Convert to MP4 with ffmpeg (run in this folder):\r\n"
        "%s\r\n",
        g_w, g_h, g_fps, written, command);
    sprintf(path, "%s.txt", clip);
    flashbackWriteTextFile(archive, path, text);

    return written;
}

// Returns the number of frames written, or a negative value:
//   -1 nothing recorded, -2 couldn't create the clip file/directory, -3 couldn't write any frame.
static s32 flashbackSave(void)
{
    if (!g_active || g_count == 0)
        return -1;

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
    bool raw = g_saveFormat == FLASHBACK_FORMAT_RAW;
    s32 written = raw ? flashbackSaveRaw(archive, clip, clipName, &plan) : flashbackSaveBmp(archive, clip, &plan);

    if (written == 0 && !raw)
        FSUSER_DeleteDirectory(archive, fsMakePath(PATH_ASCII, clip)); // empty clip folder

    FSUSER_CloseArchive(archive);
    if (written > 0)
        sprintf(g_lastClip, "%s%s", clip, raw ? ".raw" : "/");
    return written != 0 ? written : -3;
}

// ---------------------------------------------------------------------------
// Settings file
// ---------------------------------------------------------------------------

#define FLASHBACK_SETTINGS_FILE     FLASHBACK_DIR "/settings.bin"
#define FLASHBACK_SETTINGS_MAGIC    0x54534246  // "FBST"
#define FLASHBACK_SETTINGS_VERSION  1

typedef struct FlashbackSettings {
    u32 magic;
    u16 version;
    u16 size;           // sizeof(FlashbackSettings), so newer versions can append fields
    u8  enabled;        // unused (always 0): recording is always off at boot
    u8  res;            // FlashbackResolution
    u8  fpsIdx;         // index into g_fpsChoices
    u8  format;         // FlashbackFormat
    u32 saveCombo;
} FlashbackSettings;

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
    return s;
}

// Old 3DS has a third of the CPU speed, and the system core Flashback runs on
// also serves the game's graphics, input and file requests there, so it gets
// lighter defaults. Saved settings still override these.
static void flashbackApplyDefaults(void)
{
    if (!isN3DS)
    {
        g_wantRes = FLASHBACK_RES_HALF;
        g_wantFpsIdx = 1;   // 20 fps
    }
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

    if (R_FAILED(res) || total < sizeof(s) || s.magic != FLASHBACK_SETTINGS_MAGIC || s.version != FLASHBACK_SETTINGS_VERSION)
        return;

    if (s.res < FLASHBACK_RES_COUNT)
        g_wantRes = s.res;
    if (s.fpsIdx < FLASHBACK_FPS_CHOICES)
        g_wantFpsIdx = s.fpsIdx;
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

static void flashbackSetBottomFill(u32 color)
{
    LCD_BOT_FILLCOLOR = color == 0 ? 0 : (LCD_FILLCOLOR_ENABLE | color);
}

static void flashbackThreadMain(void)
{
    bool sdMode = flashbackIsSdMode();
    flashbackApplyDefaults();
    if (sdMode)
        flashbackLoadSettings();
    flashbackApplyFormat(g_wantRes, g_wantFpsIdx);

    u64 period = SYSCLOCK_ARM11 / g_fps;
    u64 nextTick = svcGetSystemTick();
    bool wasRecording = false;

    while (!preTerminationRequested)
    {
        if (g_saveRequested)
        {
            bool feedback = g_saveFromHotkey;
            u32 savedFill = LCD_BOT_FILLCOLOR;

            g_saving = true;
            if (feedback)
                flashbackSetBottomFill(FLASHBACK_FILL_SAVING);

            u64 t0 = svcGetSystemTick();
            s32 res = flashbackSave();
            g_lastSaveMs = (u32)((svcGetSystemTick() - t0) * 1000 / SYSCLOCK_ARM11);
            g_lastSaveResult = res;

            if (feedback)
            {
                bool complete = res > 0 && (u32)res == g_lastSaveExpected;
                flashbackSetBottomFill(complete ? FLASHBACK_FILL_OK : FLASHBACK_FILL_FAILED);
                svcSleepThread(300 * 1000 * 1000LL);
                LCD_BOT_FILLCOLOR = savedFill;
            }

            g_saveFromHotkey = false;
            g_saving = false;
            g_saveRequested = false;
            nextTick = svcGetSystemTick(); // don't try to catch up on missed frames
            wasRecording = false;          // ...or count them as dropped
        }

        // Settings changed in the menu: the ring layout depends on them, so
        // drop the buffer and restart with the new format.
        if (g_wantRes != g_res || g_wantFpsIdx != g_fpsIdx)
        {
            flashbackStop();
            flashbackApplyFormat(g_wantRes, g_wantFpsIdx);
            period = SYSCLOCK_ARM11 / g_fps;
            nextTick = svcGetSystemTick();
            wasRecording = false;
        }

        if (g_enabled && !g_active && sdMode && R_FAILED(flashbackStart()))
            g_enabled = false;

        // menuShouldExit: going to sleep / shell closed / terminating.
        // menuIsOpen: the game is paused under a Rosalina menu, so recording
        // would only add frozen frames. Pausing here (rather than counting
        // missed frames) leaves no gap or duplicates in the clip.
        bool recording = g_enabled && g_active && !menuShouldExit && !menuIsOpen();
        if (recording && !wasRecording)
            flashbackResetStats();
        if (recording)
        {
            if (!flashbackRecordFrame())
                flashbackFillMissed(1);
            flashbackUpdateStats();
        }
        wasRecording = recording;

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

    flashbackStop();
}

MyThread *flashbackCreateThread(void)
{
    // Lowest priority of Rosalina's threads, on the system core. Unlike the
    // core threads, failing to start is not fatal: the recorder just stays off.
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

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_DrawString(10, 10, COLOR_TITLE, "Flashback recorder: save hotkey");
    u32 posY = Draw_DrawFormattedString(10, 30, COLOR_WHITE, "Current hotkey: %s\n\n", current);
    Draw_DrawString(10, posY, COLOR_WHITE, "Hold the new hotkey (2 or more buttons),\nthen let go of all buttons.");
    Draw_FlushFramebuffer();
    Draw_Unlock();

    u32 combo = waitCombo();    // waits for Select to be released first
    if (combo == 0)             // menu closing (sleep, shell closed...)
        return;

    const char *error = flashbackCheckCombo(combo);
    if (error == NULL)
        g_saveCombo = combo;
    LumaConfig_ConvertComboToString(entered, combo);

    do
    {
        Draw_Lock();
        Draw_ClearFramebuffer();
        Draw_DrawString(10, 10, COLOR_TITLE, "Flashback recorder: save hotkey");
        posY = Draw_DrawFormattedString(10, 30, COLOR_WHITE, "You pressed: %s\n\n", entered);
        if (error == NULL)
            posY = Draw_DrawFormattedString(10, posY, COLOR_GREEN, "Save hotkey changed to %s.\n", entered);
        else
            posY = Draw_DrawFormattedString(10, posY, COLOR_RED, "%s\nThe hotkey is still %s.\n", error, current);
        Draw_DrawString(10, posY + 10, COLOR_WHITE, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
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
        u32 frames = FLASHBACK_SECONDS * fps;
        bool applied = res == g_res && g_wantFpsIdx == g_fpsIdx;

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
        posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Frame rate: %lu fps\n", fps);
        posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Format:     %s\n\n",
                                        g_saveFormat == FLASHBACK_FORMAT_RAW ? "raw video (.raw + .txt)" : "16-bit BMP frames");

        posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Buffered:  %lu / %lu frames\n",
                                        applied ? g_count : 0, frames);
        posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Rate:      %lu.%lu / %lu fps, %lu duplicated\n",
                                        g_statFpsX10 / 10, g_statFpsX10 % 10, fps, g_statDropped);
        posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Per frame: capture %lu.%lums, write %lu.%lums\n",
                                        g_statCaptureUs / 1000, (g_statCaptureUs / 100) % 10,
                                        g_statWriteUs / 1000, (g_statWriteUs / 100) % 10);
        posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Budget:    %lums per frame\n\n", 1000 / fps);

        if (g_saving || g_saveRequested)
            posY = Draw_DrawString(10, posY, COLOR_WHITE, "Saving...\n");
        else if (g_lastSaveResult > 0 && (u32)g_lastSaveResult < g_lastSaveExpected)
            posY = Draw_DrawFormattedString(10, posY, COLOR_RED, "Incomplete: saved %ld of %lu frames to\n%s\n",
                                            g_lastSaveResult, g_lastSaveExpected, g_lastClip);
        else if (g_lastSaveResult > 0)
            posY = Draw_DrawFormattedString(10, posY, COLOR_GREEN, "Saved %ld frames in %lu.%lus to\n%s\n", g_lastSaveResult,
                                            g_lastSaveMs / 1000, (g_lastSaveMs / 100) % 10, g_lastClip);
        else if (g_lastSaveResult < 0)
            posY = Draw_DrawFormattedString(10, posY, COLOR_RED, "Save failed (%ld).\n", g_lastSaveResult);

        posY += 10;
        posY = Draw_DrawString(10, posY, COLOR_WHITE, "A: recording on/off   X: save clip now\n");
        posY = Draw_DrawString(10, posY, COLOR_WHITE, "Y: resolution   Left/Right: frame rate\n");
        posY = Draw_DrawString(10, posY, COLOR_WHITE, "Up/Down: format   Select: hotkey   B: back\n");
        posY = Draw_DrawString(10, posY, COLOR_WHITE, "Resolution/frame rate changes clear the buffer.\n\n");
        LumaConfig_ConvertComboToString(comboStr, g_saveCombo);
        Draw_DrawFormattedString(10, posY, COLOR_WHITE, "In game: %s saves a clip.", comboStr);

        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 pressed = waitInputWithTimeout(250);
        bool busy = g_saving || g_saveRequested;

        if ((pressed & KEY_A) && sdMode && g_threadRunning)
            g_enabled = !g_enabled;
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
