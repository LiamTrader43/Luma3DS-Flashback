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

// Flashback: capturing frames from the screen (see flashback_internal.h).

#include <3ds.h>
#include <string.h>
#include "flashback_internal.h"
#include "csvc.h"
#include "draw.h"

#define KERNPA2VA_OFFSET()      (GET_VERSION_MINOR(osGetKernelVersion()) < 44 ? 0xD0000000 : 0xC0000000)

// ---------------------------------------------------------------------------
// Capture (kernel mode, like Draw_ConvertFrameBufferLines)
// ---------------------------------------------------------------------------

// The top screen's framebuffer, as it was on screen (flashbackFlushFramebuffer).
typedef struct FlashbackScreen {
    u32 pa;         // physical address
    u32 fmtReg;     // GPU_FB_TOP_FMT
    u32 stride;     // bytes from one column to the next
} FlashbackScreen;

typedef struct FlashbackCaptureArgs {
    const u8 *fb;   // the framebuffer, through the kernel's mapping of physical memory
    u32 fmt, bpp, stride, stepX, stepY;
    u16 *dst;
    u32 outW, outH;
    u32 col, cols;  // the output columns to convert in this call
} FlashbackCaptureArgs;

// Retries for a torn frame. Each attempt flushes the framebuffer first, so a
// retry never reads lines cached by the previous attempt.
#define FLASHBACK_CAPTURE_ATTEMPTS  3

// A frame is read in this many pieces (of output columns), with a check for a
// buffer swap before each, so a read that a swap spoils stops there instead of
// running to the end (see flashbackCapture).
#define FLASHBACK_CAPTURE_PIECES    20

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

// Converts output columns args->col to args->col + args->cols - 1 of the frame,
// in the order the screen holds it: column by column (left to right), each from
// the bottom row up. Output column ox is args->dst[ox * outH ...], so the
// frame's top row is the last pixel of each column; compressing turns it
// upright (flashbackCompress). Reads and writes both go straight through
// memory, which the Old 3DS (no L2 cache) handles far better than writing an
// upright frame one row apart per pixel, and the read is shorter, so the game
// swaps buffers mid-read less often.
//
// Always inlined into one call per format with a constant 'fmt' / 'bpp', so the
// per-pixel format switch is resolved at compile time instead of running for
// every pixel.
static inline __attribute__((always_inline)) void flashbackConvertFrame(
    const FlashbackCaptureArgs *args, u32 fmt, u32 bpp)
{
    const u32 outH = args->outH;
    const u32 colStep = args->stepX * args->stride;
    const u32 rowStep = args->stepY * bpp;
    u16 *out = args->dst + args->col * outH;

    for (u32 ox = args->col; ox < args->col + args->cols; ox++)
    {
        const u8 *src = args->fb + ox * colStep;    // the column's bottom row (framebuffer y = 0)
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

// Converts the columns as flashbackConvertFrame does, with flashback_capture.s
// where it can: full resolution in RGBA8, BGR8 and RGB565, about twice as fast
// on an Old 3DS (RGBA8: 2.8 ms instead of 5.1), which also halves how long the
// capture holds up everything else on this core, the game's threads included.
static void flashbackConvert(const FlashbackCaptureArgs *args)
{
    u32 colStep = args->stepX * args->stride;
    const u8 *src = args->fb + args->col * colStep;
    u16 *dst = args->dst + args->col * args->outH;
    if (args->outH == 240 && args->stepY == 1 && colStep >= 240 * args->bpp &&
        (((u32)src | colStep | (u32)dst) & 3) == 0)
    {
        switch (args->fmt)
        {
            case GSP_RGBA8_OES:  flashbackConvertRgba8(src, dst, args->cols, colStep - 240 * 4); return;
            case GSP_BGR8_OES:   flashbackConvertBgr8(src, dst, args->cols, colStep - 240 * 3); return;
            case GSP_RGB565_OES: flashbackConvertRgb565(src, dst, args->cols, colStep - 240 * 2); return;
        }
    }

    switch (args->fmt)
    {
        case GSP_RGBA8_OES:   flashbackConvertFrame(args, GSP_RGBA8_OES, 4); break;
        case GSP_BGR8_OES:    flashbackConvertFrame(args, GSP_BGR8_OES, 3); break;
        case GSP_RGB565_OES:  flashbackConvertFrame(args, GSP_RGB565_OES, 2); break;
        case GSP_RGB5_A1_OES: flashbackConvertFrame(args, GSP_RGB5_A1_OES, 2); break;
        case GSP_RGBA4_OES:   flashbackConvertFrame(args, GSP_RGBA4_OES, 2); break;
    }
}

// (The framebuffer is read through the kernel's mapping of physical memory.)
static void flashbackCaptureKernel(FlashbackCaptureArgs *args)
{
    flashbackConvert(args);
}

#pragma GCC pop_options

static inline u32 flashbackCurrentTopFramebuffer(void)
{
    return (GPU_FB_TOP_SEL & 1) ? GPU_FB_TOP_LEFT_ADDR_2 : GPU_FB_TOP_LEFT_ADDR_1;
}

// 800px mode (2D, half-width pixels) has twice the columns.
static inline u32 flashbackScreenColumns(u32 fmtReg)
{
    bool is3d = (fmtReg & BIT(5)) != 0;
    bool isNormal2d = (fmtReg & BIT(6)) != 0;
    return !is3d && !isNormal2d ? 800 : 400;
}

// Flushes (cleans + invalidates) the CPU cache lines of the framebuffer that's
// on screen right now, and says which it is (false if there is none). The GPU
// writes the framebuffer behind the caches' back, so lines cached by the
// previous capture would otherwise be read back stale. Only the framebuffer:
// flushing the entire data cache would also interrupt the game's core and
// throw away its cached data (see docs/flashback-design.md, "Capture").
static bool flashbackFlushFramebuffer(u32 kernPaToVa, FlashbackScreen *screen)
{
    static const u8 formatSizes[8] = { 4, 3, 2, 2, 2, 0, 0, 0 };

    screen->fmtReg = GPU_FB_TOP_FMT;
    screen->pa = flashbackCurrentTopFramebuffer();
    screen->stride = GPU_FB_TOP_STRIDE;
    if (formatSizes[screen->fmtReg & 7] == 0 || screen->pa == 0 || screen->stride == 0)
        return false;

    svcFlushDataCacheRange((void *)(screen->pa + kernPaToVa), flashbackScreenColumns(screen->fmtReg) * screen->stride);
    return true;
}

// The game swapped buffers (or changed the format) since 'screen' was flushed:
// the buffer it was may still have stale cache lines, or be getting overwritten
// with a later frame already. (Only the format register fields used: pixel
// format, 3D, 800px mode.)
static bool flashbackScreenChanged(const FlashbackScreen *screen)
{
    return flashbackCurrentTopFramebuffer() != screen->pa ||
           ((GPU_FB_TOP_FMT ^ screen->fmtReg) & (7 | BIT(5) | BIT(6))) != 0;
}

// ---------------------------------------------------------------------------
// Keeping reads clear of buffer swaps
// ---------------------------------------------------------------------------

// Games swap buffers on the screens' refreshes (GSP makes a swap at the next
// VBlank), so once two swaps have been seen a whole number of refreshes apart,
// the later ones can be expected that many refreshes on too. A read that one
// would land in waits for it to pass instead (flashbackCaptureWait): the
// recorder's 30 fps and a game's 29.92 (half the refresh rate) drift past each
// other every ~12 s, and for a few seconds of each, nearly every frame used to
// be read twice (see docs/flashback-design.md, "Capture").
#define FLASHBACK_REFRESH_TICKS     4481136ULL                      // a refresh (59.83 Hz), in system ticks
#define FLASHBACK_SWAP_MARGIN       (SYSCLOCK_ARM11 / 1000 * 3 / 2) // a swap is seen up to a piece's read late (1.5 ms)
#define FLASHBACK_SWAP_SETTLE       (SYSCLOCK_ARM11 / 1000)         // waiting past it (1 ms): GSP takes a moment to swap

static u64 g_swapSeenAt;            // when a swap was last seen (it came since the check before)
static bool g_swapsOnRefreshes;     // the last two seen were whole refreshes apart
static u32 g_readTicks;             // how long a whole read takes, flush included (smoothed)

static void flashbackSawSwap(u64 now)
{
    if (g_swapSeenAt != 0)
    {
        u64 off = (now - g_swapSeenAt) % FLASHBACK_REFRESH_TICKS;
        g_swapsOnRefreshes = off < 2 * FLASHBACK_SWAP_MARGIN || off > FLASHBACK_REFRESH_TICKS - 2 * FLASHBACK_SWAP_MARGIN;
    }
    g_swapSeenAt = now;
}

void flashbackCaptureWait(void)
{
    u64 need = g_readTicks + g_readTicks / 4 + FLASHBACK_SWAP_MARGIN;  // the read, with some to spare
    if (!g_swapsOnRefreshes || g_readTicks == 0 || need + FLASHBACK_SWAP_SETTLE >= FLASHBACK_REFRESH_TICKS)
        return;     // (nothing to go by, or no read fits between two refreshes)

    u64 untilSwap = FLASHBACK_REFRESH_TICKS - (svcGetSystemTick() - g_swapSeenAt) % FLASHBACK_REFRESH_TICKS;
    if (untilSwap < need)
        svcSleepThread((s64)((untilSwap + FLASHBACK_SWAP_SETTLE) * 1000000000ULL / SYSCLOCK_ARM11));
}

bool flashbackCapture(u16 *dst)
{
    static const u8 formatSizes[8] = { 4, 3, 2, 2, 2, 0, 0, 0 };

    u32 kernPaToVa = KERNPA2VA_OFFSET();
    u32 piece = (g_w + FLASHBACK_CAPTURE_PIECES - 1) / FLASHBACK_CAPTURE_PIECES;
    FlashbackCaptureArgs args = { .dst = dst, .outW = g_w, .outH = g_h };

    for (u32 attempt = 0; attempt < FLASHBACK_CAPTURE_ATTEMPTS; attempt++)
    {
        bool lastAttempt = attempt + 1 == FLASHBACK_CAPTURE_ATTEMPTS;
        u64 start = svcGetSystemTick();
        FlashbackScreen screen;
        if (!flashbackFlushFramebuffer(kernPaToVa, &screen))
            return false;   // no valid framebuffer right now

        args.fb = (const u8 *)(screen.pa + kernPaToVa);
        args.fmt = screen.fmtReg & 7;
        args.bpp = formatSizes[args.fmt];
        args.stride = screen.stride;
        args.stepX = flashbackScreenColumns(screen.fmtReg) / g_w;
        args.stepY = 240 / g_h;

        // Piece by piece, stopping at the first sign of a swap: before the
        // first, the buffer flushed may not be the one on screen any more, and
        // after that, the one being read may be getting overwritten.
        bool torn = false;
        for (args.col = 0; ; args.col += args.cols)
        {
            if (!torn && flashbackScreenChanged(&screen))
            {
                flashbackSawSwap(svcGetSystemTick());
                torn = true;
                if (!lastAttempt)
                    break;          // try again, on the buffer just swapped in
                if (args.col == 0)
                    return false;   // (nothing read at all: the frame counts as missed)
            }
            if (args.col >= g_w)
                break;
            args.cols = g_w - args.col < piece ? g_w - args.col : piece;
            svcCustomBackdoor(flashbackCaptureKernel, &args);
        }

        if (!torn)
        {
            u32 took = (u32)(svcGetSystemTick() - start);
            g_readTicks = g_readTicks == 0 ? took : g_readTicks - g_readTicks / 8 + took / 8;
            return true;
        }
    }

    // Still torn after every attempt: the game is swapping faster than we can
    // read. The last attempt reads on to the end regardless, since a slightly
    // torn frame beats a missing one (filled with a copy of the previous one).
    return true;
}
