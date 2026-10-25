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
// previous capture would otherwise be read back stale. Only the framebuffer:
// flushing the entire data cache would also interrupt the game's core and
// throw away its cached data (see docs/flashback-design.md, "Capture").
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

bool flashbackCapture(u16 *dst)
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
