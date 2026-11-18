/* A copy of Flashback Studio's frame decoder (flbDecodeFrame and what it uses
   in its 3ds-app/source/flb.c, and FlbCodes from flb.h), so the tests here can
   check what the recorder writes the way the readers decode it, without that
   repository. Keep it in step with the original. (testDecompress and
   testDecodeFrame, at the end, are the tests' way in.) */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define FLB_CODE_BITS       12      // the longest of those codes

// What decoding a frame packed with Huffman codes takes (flbDecodeFrame): each
// set of codes by their next FLB_CODE_BITS bits, and the compressed bytes they
// stand for. A clip has its own, so clips can be decoded on different threads.
typedef struct FlbCodes {
    uint16_t table[2][1 << FLB_CODE_BITS];  // byte value | code length << 8, or 0 where no code starts
    uint8_t bytes[400 * 240 * 2];           // (a frame's compressed bytes are fewer than the frame's)
} FlbCodes;

static uint32_t flbU32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Where the k-th pixel a frame's ops give goes in the upright w x h frame. A
// whole frame's come upright, top row first. A frame stored as what changed
// since the one before ('delta') has them in the screen's own order: column by
// column, left to right, each from the bottom row up.
static inline uint32_t flbPixelAt(uint32_t k, uint32_t w, uint32_t h, bool delta)
{
    return delta ? (h - 1 - k % h) * w + k / h : k;
}

// Decodes the recorder's frame compression, its ops (see flashbackCompress and
// flashbackCompressDelta in Luma3DS-Flashback's flashback_compress.c), into
// the upright w x h frame at 'dst'. For a 'delta' frame, 'dst' holds the frame
// before it on the same screen, and the pixels SKIP ops pass over stay as they
// are; only such frames have SKIP ops.
static bool flbDecodeOps(const uint8_t *src, uint32_t len, uint16_t *dst, uint32_t w, uint32_t h, bool delta)
{
    uint16_t index[64];
    memset(index, 0, sizeof(index));
    uint16_t px = 0;
    const uint8_t *p = src, *end = src + len;
    uint32_t i = 0, pixels = w * h;

    while (i < pixels)
    {
        if (p >= end)
            return false;
        uint8_t b = *p++;

        if (b == 0xFF)
        {
            if (!delta || p >= end)
                return false;
            uint32_t count = *p++;
            if (count == 0xFF)
            {
                if (end - p < 2)
                    return false;
                count = p[0] | (p[1] << 8);
                p += 2;
            }
            count++;
            if (count > pixels - i)
                return false;
            i += count;
            px = dst[flbPixelAt(i - 1, w, h, delta)];
            continue;
        }
        else if (b == 0xFE)
        {
            if (end - p < 2)
                return false;
            px = p[0] | (p[1] << 8);
            p += 2;
        }
        else if ((b & 0xC0) == 0xC0)
        {
            uint32_t run = (b & 0x3F) + 1;
            if (run > pixels - i)
                return false;
            while (run-- != 0)
                dst[flbPixelAt(i++, w, h, delta)] = px;
            continue;
        }
        else if ((b & 0xC0) == 0x00)
        {
            px = index[b];
            dst[flbPixelAt(i++, w, h, delta)] = px;
            continue;
        }
        else
        {
            int32_t dr, dg, db;
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
                uint8_t b2 = *p++;
                dg = (b & 0x3F) - 32;
                dr = ((b2 >> 4) - 8) + (dg >> 1);
                db = ((b2 & 0xF) - 8) + (dg >> 1);
            }
            uint32_t r = ((px >> 11) + dr) & 0x1F;
            uint32_t g = (((px >> 5) & 0x3F) + dg) & 0x3F;
            uint32_t bl = ((px & 0x1F) + db) & 0x1F;
            px = (uint16_t)((r << 11) | (g << 5) | bl);
        }
        index[((uint32_t)px * 0x9E3779B1u) >> 26] = px;
        dst[flbPixelAt(i++, w, h, delta)] = px;
    }
    return p == end;
}

#define FLB_CODES_HEADER    264     // a frame packed with Huffman codes: bytes before the codes

// Turns a frame packed with Huffman codes (see flashbackHuffman in
// Luma3DS-Flashback's flashback_compress.c) back into the compressed bytes they
// stand for, at most 'cap' of them, into codes->bytes; sets *count.
static bool flbDecodeCodes(const uint8_t *src, uint32_t len, FlbCodes *codes, uint32_t cap, uint32_t *count)
{
    if (len < FLB_CODES_HEADER || src[0] != 0xFF || src[1] != 1)
        return false;
    uint32_t n = flbU32(src + 4);
    if (n > cap || n > sizeof(codes->bytes))
        return false;

    // Each set's codes by their next FLB_CODE_BITS bits (first bit lowest). The
    // codes are canonical: shorter first, and among the same length, lower
    // byte values first.
    for (uint32_t set = 0; set < 2; set++)
    {
        const uint8_t *lengths = src + 8 + set * 128;
        uint16_t *table = codes->table[set];
        uint32_t perLength[FLB_CODE_BITS + 1] = { 0 }, next[FLB_CODE_BITS + 1];
        for (uint32_t v = 0; v < 256; v++)
        {
            uint32_t length = lengths[v / 2] >> (v & 1) * 4 & 0xF;
            if (length > FLB_CODE_BITS)
                return false;
            perLength[length]++;
        }
        uint32_t code = 0;
        perLength[0] = 0;
        for (uint32_t length = 1; length <= FLB_CODE_BITS; length++)
        {
            code = (code + perLength[length - 1]) << 1;
            next[length] = code;
        }
        memset(table, 0, sizeof(codes->table[set]));
        for (uint32_t v = 0; v < 256; v++)
        {
            uint32_t length = lengths[v / 2] >> (v & 1) * 4 & 0xF;
            if (length == 0)
                continue;
            uint32_t c = next[length]++, reversed = 0;
            if (c >= 1u << length)
                return false;   // (more codes than there's room for)
            for (uint32_t k = 0; k < length; k++)
                reversed = reversed << 1 | (c >> k & 1);
            for (uint32_t k = reversed; k < 1u << FLB_CODE_BITS; k += 1u << length)
                table[k] = (uint16_t)(v | length << 8);
        }
    }

    // The bytes, each set's codes in turn: an op byte, then the bytes after it
    // (LUMA's second, RAW's two, SKIP's count: one, or three if the first is 0xFF).
    uint32_t pos = FLB_CODES_HEADER, bits = 0, have = 0, after = 0;
    bool skipCount = false;     // the next byte is the first after a SKIP
    for (uint32_t i = 0; i < n; i++)
    {
        while (have <= 24)
        {
            bits |= (uint32_t)(pos < len ? src[pos] : 0) << have;
            pos++;
            have += 8;
        }
        uint32_t e = codes->table[after != 0][bits & ((1u << FLB_CODE_BITS) - 1)], used = e >> 8;
        if (used == 0)
            return false;
        bits >>= used;
        have -= used;
        uint8_t b = (uint8_t)e;
        codes->bytes[i] = b;
        if (after != 0)
        {
            after--;
            if (skipCount && b == 0xFF)
                after += 2;
            skipCount = false;
        }
        else
        {
            after = b >= 0x80 && b < 0xC0 ? 1 : b == 0xFE ? 2 : b == 0xFF ? 1 : 0;
            skipCount = b == 0xFF;
        }
    }
    // (The codes must end in the frame's last 2 bytes: it's padded to 2.)
    uint64_t used = (uint64_t)(pos - FLB_CODES_HEADER) * 8 - have, held = (uint64_t)(len - FLB_CODES_HEADER) * 8;
    if (used > held || used + 16 <= held)
        return false;
    *count = n;
    return true;
}

// Decodes a stored frame (not one stored raw) of a w x h screen into 'dst',
// upright: exactly 'len' bytes, packed with Huffman codes if 'coded' (through
// 'codes'), and if 'delta', what changed since the frame before it on the same
// screen, which 'dst' holds. Returns false if the data is malformed (never
// writes outside the frame; a delta frame that fails can leave 'dst' part
// changed).
bool flbDecodeFrame(const uint8_t *src, uint32_t len, uint16_t *dst, uint32_t w, uint32_t h, bool delta, bool coded,
                    FlbCodes *codes)
{
    if (coded)
    {
        uint32_t count;
        return codes != NULL && flbDecodeCodes(src, len, codes, w * h * 2, &count) &&
               flbDecodeOps(codes->bytes, count, dst, w, h, delta);
    }
    return flbDecodeOps(src, len, dst, w, h, delta);
}

// A whole frame from a clip of before frames could be deltas: one that starts
// with 0xFF is packed with Huffman codes. 'pixels' pixels, upright.
bool flbDecompress(const uint8_t *src, uint32_t len, uint16_t *dst, uint32_t pixels, FlbCodes *codes)
{
    return flbDecodeFrame(src, len, dst, pixels, 1, false, len != 0 && src[0] == 0xFF, codes);
}

// The tests decode on one thread: one FlbCodes is enough.
static FlbCodes g_testCodes;

bool testDecompress(const uint8_t *src, uint32_t len, uint16_t *dst, uint32_t pixels)
{
    return flbDecompress(src, len, dst, pixels, &g_testCodes);
}

bool testDecodeFrame(const uint8_t *src, uint32_t len, uint16_t *dst, uint32_t w, uint32_t h, bool delta, bool coded)
{
    return flbDecodeFrame(src, len, dst, w, h, delta, coded, &g_testCodes);
}
