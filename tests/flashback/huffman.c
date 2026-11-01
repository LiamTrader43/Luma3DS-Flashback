/* Test of the recorder's Huffman codes over compressed frames
   (flashbackHuffman, after flashbackCompress), with its own code
   (recorder_extracted.c, made by extract.py).

   Frames of many kinds, at both resolutions, are packed one after another as
   recording does (each frame's codes come from the counts of the frame
   before). Every frame packed must decode, with the readers' decoder
   (decode.c), to exactly the frame, come out smaller than its ops, and leave
   the counts of its own bytes for the next frame. One that doesn't come out
   smaller must give 0. Either way nothing may be written past the room it had
   (as many bytes as its ops).

   And the code lengths, from any counts (skewed far past what 12-bit codes
   can follow), must be 1 to 12 bits, fill the code space exactly, and be no
   longer for a more common byte value. */
#include "stubs_fuzz.h"
#include "recorder_extracted.c"
#include "asm_standin.h"

/* Flashback Studio's decoder (a copy of 3ds-app/source/flb.c's: decode.c), linked in */
bool testDecompress(const uint8_t *src, uint32_t len, uint16_t *dst, uint32_t pixels);

/* (The recorder's code calls these, but nothing here captures or writes.) */
static bool flashbackCapture(u16 *dst) { (void)dst; return false; }
static void writeHook(IFile *f) { (void)f; }

static long errors;
#define FAIL(...) do { errors++; if (errors < 20) printf("  ERROR: " __VA_ARGS__); } while (0)

static u32 g_rng = 12345;
static u32 rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static u16 rgb(int r, int g, int b) { return (u16)((r & 31) << 11 | (g & 63) << 5 | (b & 31)); }
static int clampTo(int v, int max) { return v < 0 ? 0 : v > max ? max : v; }

enum { GAME, TEXTURE, FLAT, PALETTE, NOISE, SKEWED, SOLID, KINDS };
static const char *g_kindNames[KINDS] = { "game-like", "texture", "flat", "palette", "noise", "skewed", "solid" };

/* Frame 't' of a kind, upright (top row first). */
static void makeFrame(u16 *up, u32 w, u32 h, int kind, u32 t)
{
    static u16 palette[16];
    if (t == 0 || kind == PALETTE)
        for (int i = 0; i < 16; i++)
            palette[i] = (u16)rnd();
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
        {
            u16 *p = &up[y * w + x];
            int n = (int)(rnd() % 7) - 3;
            switch (kind)
            {
            case GAME:      /* sky, textured ground, a HUD box and a sprite */
                if (x < w / 5 && y > h * 4 / 5)
                    *p = rgb(4, 40, 20);
                else if (x > w / 2 && x < w / 2 + 24 && y > h / 3 && y < h / 3 + 40)
                    *p = (u16)rnd();
                else if (y < h / 3)
                    *p = rgb(8 + (int)(y * 12 / h), 30 + (int)((y + t) * 20 / h), 28);
                else
                    *p = rgb(clampTo(6 + n + (int)((x + t) % 9), 31), clampTo(30 + 2 * n + (int)(y % 13), 63),
                             clampTo(5 + n, 31));
                break;
            case TEXTURE:   /* bark: a slow base color, and 3D noise in every channel */
                *p = rgb(clampTo(12 + (int)(x / 40) + n * 2, 31), clampTo(24 + (int)(y / 20) + n * 3, 63),
                         clampTo(10 + n * 2, 31));
                break;
            case FLAT:      /* big blocks of a few colors */
                *p = rgb((int)((x / 100 + t) % 4) * 8, (int)(y / 60) * 16, 12);
                break;
            case PALETTE:   /* dithered: 16 colors */
                *p = palette[rnd() % 16];
                break;
            case NOISE:
                *p = (u16)rnd();
                break;
            case SKEWED:    /* almost all one DIFF op (a ramp), now and then something else */
                *p = rnd() % 97 == 0 ? (u16)rnd() : rgb((int)(x + y), (int)(x + y), (int)(x + y));
                break;
            case SOLID:     /* one color (a fade to black): all runs, too few bytes at half resolution */
                *p = rgb((int)(t % 32), 0, 0);
                break;
            }
        }
}

/* The ops' bytes counted as flashbackHuffman counts them: op bytes, and the
   bytes after ops. */
static void countOps(const u8 *ops, u32 len, u32 counts[2][256])
{
    memset(counts, 0, 2 * 256 * sizeof(u32));
    for (u32 i = 0; i < len;)
    {
        u8 b = ops[i++];
        counts[0][b]++;
        for (u32 k = b >= 0x80 && b < 0xC0 ? 1 : b == 0xFE ? 2 : 0; k != 0; k--)
            counts[1][ops[i++]]++;
    }
}

static void checkLengths(const u32 *counts, const char *what)
{
    u8 lengths[256];
    flashbackHuffLengths(counts, lengths);
    u32 space = 0;
    for (u32 v = 0; v < 256; v++)
    {
        if (lengths[v] < 1 || lengths[v] > FLASHBACK_HUFF_MAX_BITS)
        {
            FAIL("%s: value %u has a %u-bit code\n", what, v, lengths[v]);
            return;
        }
        space += 1u << (FLASHBACK_HUFF_MAX_BITS - lengths[v]);
    }
    if (space != 1u << FLASHBACK_HUFF_MAX_BITS)
        FAIL("%s: the codes fill %u of %u\n", what, space, 1u << FLASHBACK_HUFF_MAX_BITS);
    for (u32 v = 0; v < 256; v++)
        for (u32 u = 0; u < 256; u++)
            if (counts[v] > counts[u] && lengths[v] > lengths[u])
            {
                FAIL("%s: value %u (%u of them) has a longer code than %u (%u)\n", what, v, counts[v], u, counts[u]);
                return;
            }
}

int main(void)
{
    g_huff = calloc(1, sizeof(FlashbackHuffman));

    /* code lengths from all sorts of counts */
    long lengthChecks = 0;
    for (int trial = 0; trial < 3000; trial++)
    {
        u32 counts[256];
        int shape = trial % 6;
        u32 a = 1, b = 1;
        for (u32 v = 0; v < 256; v++)
        {
            switch (shape)
            {
            case 0: counts[v] = rnd() % (1u << (rnd() % 18)); break;           /* random, all scales */
            case 1: counts[v] = a; { u32 c = a + b; a = b; b = c > 100000 ? 1 : c; } break;   /* Fibonacci: deep trees */
            case 2: counts[v] = 1u << (v % 18); break;                         /* powers of two */
            case 3: counts[v] = v == trial % 256 ? 190000 : 0; break;          /* one value only */
            case 4: counts[v] = 0; break;                                      /* nothing yet */
            case 5: counts[v] = rnd() % 3 == 0 ? rnd() % 190000 : 0; break;    /* a few */
            }
        }
        checkLengths(counts, "lengths");
        lengthChecks++;
    }

    /* frames, one after another */
    u16 *up = malloc(400 * 240 * 2), *cols = malloc((400 * 240 + 240) * 2), *back = malloc(400 * 240 * 2);
    u8 *ops = malloc(400 * 240 * 2), *dst = malloc(400 * 240 * 2 + 4096);
    long frames = 0, raw = 0, small = 0, packed = 0, notSmaller = 0, perKind[KINDS] = { 0 }, packedKind[KINDS] = { 0 };
    double opsBytes = 0, packedBytes = 0;
    int kind = GAME;
    u32 t = 0;
    for (int f = 0; f < 3000; f++, t++)
    {
        /* mostly runs of one kind (as video is), and now and then a change */
        if (rnd() % 40 == 0)
        {
            kind = (int)(rnd() % KINDS);
            t = 0;
        }
        bool full = (f / 500) % 2 == 0;
        u32 w = full ? 400 : 200, h = full ? 240 : 120, pixels = w * h;
        makeFrame(up, w, h, kind, t);
        /* in the screen's order (as captured): column by column, bottom row first */
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
                cols[x * h + (h - 1 - y)] = up[y * w + x];
        frames++;
        perKind[kind]++;

        u32 opsLen = flashbackCompress(cols, w, h, ops, pixels * 2);
        if (opsLen == 0)
        {
            raw++;
            continue;
        }
        if (!testDecompress(ops, opsLen, back, pixels) || memcmp(back, up, pixels * 2) != 0)
            FAIL("frame %d: its ops don't decode to it\n", f);

        memset(dst + opsLen, 0xA5, 4096);
        u32 before[2][256];
        memcpy(before, g_huff->counts, sizeof(before));
        u32 len = flashbackHuffman(ops, opsLen, dst);
        for (u32 i = 0; i < 4096; i++)
            if (dst[opsLen + i] != 0xA5)
            {
                FAIL("frame %d (%s): written past its room\n", f, g_kindNames[kind]);
                break;
            }
        if (len == 0)
        {
            if (opsLen <= FLASHBACK_HUFF_HEADER + 2 * FLASHBACK_HUFF_BLOCK)
            {
                small++;
                if (memcmp(before, g_huff->counts, sizeof(before)) != 0)
                    FAIL("frame %d: too small to pack, but the counts changed\n", f);
            }
            else
                notSmaller++;
            continue;
        }
        packed++;
        packedKind[kind]++;
        opsBytes += opsLen;
        packedBytes += len;
        if (len >= opsLen)
            FAIL("frame %d: packed to %u bytes, from %u\n", f, len, opsLen);
        if (dst[0] != 0xFF)
            FAIL("frame %d: packed, but it doesn't start with 0xFF\n", f);
        if (!testDecompress(dst, len, back, pixels) || memcmp(back, up, pixels * 2) != 0)
            FAIL("frame %d (%s): packed, it doesn't decode to the frame\n", f, g_kindNames[kind]);
        u32 counts[2][256];
        countOps(ops, opsLen, counts);
        if (memcmp(counts, g_huff->counts, sizeof(counts)) != 0)
            FAIL("frame %d: the counts left aren't its own bytes'\n", f);
        /* (and the lengths built from them are sound, the frames' counts being
           the ones that matter) */
        checkLengths(counts[0], "a frame's op bytes");
        checkLengths(counts[1], "a frame's other bytes");
    }

    printf("code lengths checked from %ld sets of counts, and from every packed frame's\n", lengthChecks);
    printf("%ld frames: %ld packed with codes (to %.1f%% of their ops), %ld no smaller with them, %ld too small to try, %ld stored raw\n",
           frames, packed, packed ? 100.0 * packedBytes / opsBytes : 0.0, notSmaller, small, raw);
    for (int k = 0; k < KINDS; k++)
        printf("  %-10s %4ld frames, %4ld packed\n", g_kindNames[k], perKind[k], packedKind[k]);
    if (packed == 0 || notSmaller == 0 || small == 0)
    {
        printf("  ERROR: not every case came up\n");
        errors++;
    }
    printf("%s (%ld errors)\n", errors ? "FAILED" : "all good", errors);
    return errors != 0;
}
