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

// Flashback: compressing frames (see flashback_internal.h).

#include <3ds.h>
#include <string.h>
#include "flashback_internal.h"

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
//
// On a New 3DS, those bytes are then packed tighter with Huffman codes
// (flashbackHuffman): each byte value gets a code of 1 to
// FLASHBACK_HUFF_MAX_BITS bits, the more common the shorter, which makes
// frames about a fifth smaller (on 7 clips, 18-24%), so the SD card has a
// fifth less to write, for a few more milliseconds of the processor's time
// (4 in Ocarina of Time 3D; an Old 3DS hasn't the time: see
// flashbackPackFrame). Op bytes have one set of codes, and the
// bytes after ops (LUMA's second, RAW's two) another. A frame packed this way:
//   byte 0       0xFF: no op starts with it, so no frame of ops does
//   byte 1       1 (this format)
//   bytes 2-3    0
//   bytes 4-7    how many bytes the codes stand for
//   bytes 8-135  the op bytes' code lengths, 4 bits each: byte value v's in
//                byte 8 + v / 2, in its low 4 bits for an even v
//   bytes 136-263  the other bytes' code lengths, the same way
//   then         the codes, as in Deflate (RFC 1951, 3.1.1 and 3.2.2): each
//                set's codes are canonical (from the lengths: shorter codes
//                first, and among the same length, lower byte values first),
//                each one written first bit first, and the bits fill each
//                byte from its lowest bit; padded with 0s to a whole number
//                of 2-byte words (readers check the codes end in the last 2)
// A frame that wouldn't come out smaller this way is stored as its ops.
// (Flashback-Studio's tests/codes_test.c makes frames with a copy of this
// compression: keep it in step.)

// Rows of a frame stored as it is turned upright at a time (see
// flashbackUprightBand): a cache line holds 16 pixels of a column.
#define FLASHBACK_BAND_ROWS     16

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
u32 flashbackCompress(const u16 *cols, u32 width, u32 height, u8 *dst, u32 cap)
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

// ---------------------------------------------------------------------------
// Huffman codes over the compressed bytes (the format: see the top of this file)
// ---------------------------------------------------------------------------

#define FLASHBACK_HUFF_HEADER   264     // bytes before the codes
#define FLASHBACK_HUFF_BLOCK    64      // bytes packed between checks that the frame still comes out smaller

// The code lengths for the 256 byte values, from how many of each the last
// frame had: Huffman's algorithm, then any code longer than
// FLASHBACK_HUFF_MAX_BITS shortened, and others lengthened to make room, as
// zlib does. Every value is counted once more, so it gets a code even if the
// last frame had none of it (this one may).
static void flashbackHuffLengths(const u32 *counts, u8 *lengths)
{
    FlashbackHuffman *h = g_huff;
    u32 *order = h->order, *weight = h->weight;

    // The values by count, fewest first (a Shell sort: 256 of them).
    for (u32 v = 0; v < 256; v++)
        order[v] = (counts[v] + 1) << 8 | v;
    static const u8 gaps[] = { 132, 57, 23, 10, 4, 1 };
    for (u32 g = 0; g < sizeof(gaps); g++)
        for (u32 i = gaps[g]; i < 256; i++)
        {
            u32 key = order[i], j = i;
            for (; j >= gaps[g] && order[j - gaps[g]] > key; j -= gaps[g])
                order[j] = order[j - gaps[g]];
            order[j] = key;
        }

    // The tree: the leaves are weight[0..255] (in that order), and each node
    // made joins the two lightest of the leaves and nodes not yet joined (the
    // nodes are made lightest first, so those are at the front of each).
    for (u32 i = 0; i < 256; i++)
        weight[i] = order[i] >> 8;
    u32 leaf = 0, node = 256;
    for (u32 made = 256; made < 511; made++)
    {
        u32 a = leaf < 256 && (node == made || weight[leaf] <= weight[node]) ? leaf++ : node++;
        u32 b = leaf < 256 && (node == made || weight[leaf] <= weight[node]) ? leaf++ : node++;
        weight[made] = weight[a] + weight[b];
        h->parent[a] = h->parent[b] = (u16)made;
    }
    h->depth[510] = 0;
    for (u32 i = 510; i-- != 0;)
        h->depth[i] = h->depth[h->parent[i]] + 1;

    // How many codes of each length, the longest ones cut down to the limit:
    // while the codes don't fit (their share of the code space, sum(2^-length),
    // is over 1), one at the limit goes, and a shorter one splits in two one
    // bit longer (it's still one, and the one that went fits back in).
    u32 perLength[FLASHBACK_HUFF_MAX_BITS + 1] = { 0 };
    for (u32 i = 0; i < 256; i++)
        perLength[h->depth[i] < FLASHBACK_HUFF_MAX_BITS ? h->depth[i] : FLASHBACK_HUFF_MAX_BITS]++;
    u32 space = 0;
    for (u32 n = 1; n <= FLASHBACK_HUFF_MAX_BITS; n++)
        space += perLength[n] << (FLASHBACK_HUFF_MAX_BITS - n);
    for (; space > 1u << FLASHBACK_HUFF_MAX_BITS; space--)
    {
        perLength[FLASHBACK_HUFF_MAX_BITS]--;
        for (u32 n = FLASHBACK_HUFF_MAX_BITS - 1; n > 0; n--)
            if (perLength[n] != 0)
            {
                perLength[n]--;
                perLength[n + 1] += 2;
                break;
            }
    }

    // The longest codes to the rarest values.
    u32 i = 0;
    for (u32 n = FLASHBACK_HUFF_MAX_BITS; n > 0; n--)
        for (u32 k = perLength[n]; k != 0; k--)
            lengths[order[i++] & 0xFF] = (u8)n;
}

// The canonical codes for those lengths (see the top of this file), each
// reversed to go out first bit first from the lowest bit: code | length << 16.
static void flashbackHuffCodes(const u8 *lengths, u32 *codes)
{
    u32 perLength[FLASHBACK_HUFF_MAX_BITS + 1] = { 0 }, next[FLASHBACK_HUFF_MAX_BITS + 1];
    for (u32 v = 0; v < 256; v++)
        perLength[lengths[v]]++;
    u32 code = 0;
    for (u32 n = 1; n <= FLASHBACK_HUFF_MAX_BITS; n++)
    {
        code = (code + perLength[n - 1]) << 1;
        next[n] = code;
    }
    for (u32 v = 0; v < 256; v++)
    {
        u32 n = lengths[v], c = next[n]++, reversed = 0;
        for (u32 k = 0; k < n; k++)
            reversed = reversed << 1 | (c >> k & 1);
        codes[v] = reversed | n << 16;
    }
}

// Packs the 'len' bytes at 'src' (a frame from flashbackCompress) into 'dst'
// with Huffman codes from the last frame's counts, counting this frame's for
// the next (see the top of this file). Returns the packed size, or 0 if it
// wouldn't be smaller than 'len' (store the bytes as they are). 'dst' has room
// for 'len' bytes. Packer thread (g_huff).
u32 flashbackHuffman(const u8 *src, u32 len, u8 *dst)
{
    if (len <= FLASHBACK_HUFF_HEADER + 2 * FLASHBACK_HUFF_BLOCK)
        return 0;   // (too small to come out smaller)

    FlashbackHuffman *h = g_huff;
    u8 lengths[256];
    dst[0] = 0xFF;
    dst[1] = 1;
    dst[2] = dst[3] = 0;
    memcpy(dst + 4, &len, 4);
    for (u32 set = 0; set < 2; set++)
    {
        flashbackHuffLengths(h->counts[set], lengths);
        flashbackHuffCodes(lengths, h->codes[set]);
        for (u32 v = 0; v < 256; v += 2)
            dst[8 + set * 128 + v / 2] = (u8)(lengths[v] | lengths[v + 1] << 4);
    }
    memset(h->counts, 0, sizeof(h->counts));

    // The codes go into 'bits' (n of them so far), out 16 at a time: under 16
    // are left after each code, and a code is at most 12 more. A block's codes
    // (FLASHBACK_HUFF_BLOCK bytes, and the up to 2 after its last op) take at
    // most 1.5 bytes each, and the last bits 2 more: as long as each block
    // starts that far (and 1 more) before 'len', the frame comes out smaller.
    u32 *opCounts = h->counts[0], *argCounts = h->counts[1];
    const u32 *opCodes = h->codes[0], *argCodes = h->codes[1];
    const u8 *p = src, *end = src + len;
    u16 *out = (u16 *)(dst + FLASHBACK_HUFF_HEADER);
    const u16 *last = (const u16 *)(dst + len - (FLASHBACK_HUFF_BLOCK + 2) * 3 / 2 - 2 - 1);
    u32 bits = 0, n = 0;
    while (p < end)
    {
        if (out > last)
            return 0;
        const u8 *stop = (u32)(end - p) > FLASHBACK_HUFF_BLOCK ? p + FLASHBACK_HUFF_BLOCK : end;
        do
        {
            u32 b = *p++;
            opCounts[b]++;
            u32 c = opCodes[b];
            bits |= (c & 0xFFFF) << n;
            n += c >> 16;
            if (n >= 16)
            {
                *out++ = (u16)bits;
                bits >>= 16;
                n -= 16;
            }
            if (b - 0x80 < 0x40 || b == FLASHBACK_PACK_RAW)     // LUMA's second byte, or RAW's two
            {
                for (u32 k = b == FLASHBACK_PACK_RAW ? 2 : 1; k != 0; k--)
                {
                    b = *p++;
                    argCounts[b]++;
                    c = argCodes[b];
                    bits |= (c & 0xFFFF) << n;
                    n += c >> 16;
                    if (n >= 16)
                    {
                        *out++ = (u16)bits;
                        bits >>= 16;
                        n -= 16;
                    }
                }
            }
        } while (p < stop);
    }
    if (n != 0)
        *out++ = (u16)bits;
    return (u32)((u8 *)out - dst);
}

// The upright frame (top row first), from a frame in the screen's order (see
// flashbackCompress): for frames stored as they are. Turned upright a band at a
// time (flashbackUprightBand), straight into 'dst'.
void flashbackColumnsToRows(const u16 *cols, u32 width, u32 height, u16 *dst)
{
    for (u32 y0 = 0; y0 < height; y0 += FLASHBACK_BAND_ROWS)
    {
        u32 rows = height - y0 < FLASHBACK_BAND_ROWS ? height - y0 : FLASHBACK_BAND_ROWS;
        flashbackUprightBand(cols, width, height, y0, rows, dst + y0 * width);
    }
}

#pragma GCC pop_options
