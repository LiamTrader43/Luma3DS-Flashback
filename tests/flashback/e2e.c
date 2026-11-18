/* End-to-end test of Flashback's recording, using the recorder's own code
   (recorder_extracted.c, made by extract.py) with real threads in real time:
   - a fake DSP produces an audio frame every 160 * 8192 ticks into the region
     that isn't newest, then (0.4 ms later, as the game does) advances that
     region's counter; every sample carries its own index, so the saved audio
     can be checked sample by sample;
   - the audio thread, the writer thread and a 30 fps capture loop run as on
     the 3DS; the "SD card" is memory that takes 0-2 ms per write;
   - events: Rosalina menu pauses, a DSP stall (no audio produced), and the
     audio thread held up 12 ms and 30 ms (like a long kernel call), a save in
     the middle and recording on afterwards;
   - both screens are recorded: the top one all new at every capture (noise),
     or mostly as it was, and the bottom one mostly as it was, so the packer
     thread compares each frame with the one before while the capture loop
     fills the other buffers;
   - and a bottom screen that's new at every capture, beside a top screen that
     leaves it no room: its frames mustn't cost the top screen's theirs. And
     the same the other way round, with the bottom screen first (see
     FlashbackPriority).
   Then the saved clips are parsed like a reader would and checked: every
   picture a clip shows, on both screens, must be one that was captured, in
   order, and the sound in step with it. */
#include "stubs_e2e.h"
/* What became of the second screen's frames as they were packed (see
   flashbackPackFrame; here it's captured with every frame): not stored, and of
   those, compressed for nothing; and stored whole in a slot of their own. */
static volatile u32 g_packLost, g_packWasted, g_packOwn;
#define FLASHBACK_PACKED_HOOK(priLen, secLen, ownLen, secKept, secTried) \
    do { (void)(priLen); g_packLost += !(secKept); g_packWasted += (secTried) && (secLen) == 0 && (ownLen) == 0; \
         g_packOwn += (ownLen) != 0; } while (0)
#include "recorder_extracted.c"
#include "asm_standin.h"
#ifndef RING_VIDEO_BASE
#define RING_VIDEO_BASE g_videoOffset  /* where the ring's video slots start */
#endif
#include <mmsystem.h>

#define FRAME_TICKS (160ULL * 8192)

static volatile bool g_dspStop;
static volatile u32 g_dspFrames;            /* frames the fake DSP has produced */
static volatile bool g_dspStalled;
static volatile bool g_beeps;               /* the A/V sync scenario */
static volatile bool g_dspReset;            /* the game restarts its audio: the counters start over low */
static u32 g_lastSilentSamples;             /* (from check()) */
static double g_lastMaxDevMs, g_lastEndDevMs;
#include <math.h>
static volatile s64 g_audioHoldNs;          /* hold the audio thread up once, this long */
static u32 g_frameDsp[100000];              /* DSP frames produced when each video frame was captured */

static u16 *counter(int r) { return (u16 *)(g_fakeDsp + r * 0x20000 + 0x7FFE); }

/* Flashback Studio's decoder (a copy of 3ds-app/source/flb.c's: decode.c), linked in */
bool testDecodeFrame(const uint8_t *src, uint32_t len, uint16_t *dst, uint32_t w, uint32_t h, bool delta, bool coded);

/* The screens. Each picture has an id in its first two pixels (upright), and
   its fingerprint is kept, so that what a clip shows can be checked. */
enum { TOP_NOISE, TOP_BEEPS, TOP_STILL };
static volatile int g_topMode;          /* all new at every capture; white or black; or new every third capture */
static volatile bool g_freshNext;       /* the next capture is a new picture whatever the mode (a recording starts) */
static volatile bool g_botBusy;         /* the bottom screen is new at every capture, and takes a lot to store */
static u16 g_scrTop[400 * 240], g_scrBot[320 * 240];   /* the screens now, upright */
static u32 g_botId;                     /* the bottom screen's picture now (0: none yet) */
static u64 g_topHash[100000], g_botHash[100000];
static u32 g_frameBot[100000];          /* the bottom screen's picture when each of the top one's was captured */
static u32 g_captureNo;

static u64 hashOf(const u16 *px, u32 n)
{
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < n; i++)
        h = (h ^ px[i]) * 1099511628211ull;
    return h;
}

/* Records for 'seconds' at 30 fps; events at the given times (seconds from the start).
   'freeze': the game is frozen during the pauses, as under a real Rosalina menu: its
   audio stops at exactly those times, and recording at the next video frame. */
typedef struct { double pauseFrom[8], pauseTo[8], stallFrom, stallTo, hold12, hold30, resetAt, stallSdAt; bool freeze; } Events;
static const Events *volatile g_freezeEv;
static volatile u64 g_recordStart;

static bool gameFrozen(void)
{
    const Events *ev = g_freezeEv;
    if (!ev || !ev->freeze) return false;
    double t = (svcGetSystemTick() - g_recordStart) / (double)SYSCLOCK_ARM11;
    for (int i = 0; i < 8; i++) if (t >= ev->pauseFrom[i] && t < ev->pauseTo[i]) return true;
    return false;
}

static void *dspThread(void *arg)
{
    (void)arg;
    *counter(0) = 1000;
    *counter(1) = 1001;
    u64 next = svcGetSystemTick();
    u32 n = 0;
    while (!g_dspStop)
    {
        next += FRAME_TICKS;
        while (svcGetSystemTick() < next) SwitchToThread();
        if (g_dspStalled || gameFrozen())
            continue;
        if (g_dspReset)
        {
            *counter(0) = 10;
            *counter(1) = 11;
            g_dspReset = false;
        }
        int newest = flashbackCounterNewer(*counter(0), *counter(1)) ? 0 : 1, w = 1 - newest;
        s16 *blk = (s16 *)(g_fakeDsp + w * 0x20000 + 0xA80);
        for (u32 i = 0; i < 160; i++)
        {
            u32 idx = n * 160 + i;
            if (g_beeps)
            {
                /* a 1 kHz beep for 4 frames (~20 ms) every 205 frames (~1 s) */
                s16 v = n % 205 < 4 ? (s16)(12000 * sin(2 * 3.14159265 * 1000.0 * idx / 32728.5)) : 0;
                blk[2 * i] = v;
                blk[2 * i + 1] = v;
                continue;
            }
            blk[2 * i] = (s16)(idx & 0xFFFF);
            blk[2 * i + 1] = (s16)(0x8000 | ((idx >> 16) & 0x7FFF));
        }
        u64 t = svcGetSystemTick() + SYSCLOCK_ARM11 / 2500;     /* 0.4 ms */
        while (svcGetSystemTick() < t) SwitchToThread();
        *counter(w) = (u16)(*counter(newest) + 1);
        g_dspFrames = ++n;
    }
    return NULL;
}

static void *audioThread(void *arg) { (void)arg; t_extraSleepNs = &g_audioHoldNs; flashbackAudioThreadMain(); return NULL; }
static void *writerThread(void *arg) { (void)arg; flashbackWriterThreadMain(); return NULL; }
static void *packThread(void *arg) { (void)arg; flashbackPackThreadMain(); return NULL; }

static bool flashbackCapture(u16 *first, u16 *second, bool *secondOk)
{
    /* (which screen is which: the first one is the top one, unless g_priBottom) */
    u16 *top = g_priBottom ? second : first, *bottom = g_priBottom ? first : second;
    u32 n = g_w * g_h, no = g_captureNo++;
    bool fresh = true;
    if (g_topMode == TOP_BEEPS)
    {
        /* white while the DSP is beeping, black otherwise */
        u16 v = g_dspFrames % 205 < 4 ? 0xFFFF : 0x0000;
        for (u32 i = 0; i < n; i++) g_scrTop[i] = v;
    }
    else if (g_topMode == TOP_STILL)
    {
        /* as it was two captures in three; then a box of noise over it */
        fresh = no % 3 == 0 || g_freshNext;
        if (fresh)
            for (u32 y = 20; y < 50; y++)
                for (u32 x = 30; x < 90; x++)
                    g_scrTop[y * g_w + x] = (u16)rand();
    }
    else
        for (u32 i = 0; i < n; i++) g_scrTop[i] = (u16)rand();
    g_freshNext = false;

    u32 id = g_nextFrameId;
    if (fresh)
    {
        id = ++g_nextFrameId;
        g_scrTop[0] = (u16)id;
        g_scrTop[1] = (u16)(id >> 16);
        g_topHash[id] = hashOf(g_scrTop, n);
        g_frameDsp[id] = g_dspFrames;
    }

    /* the bottom screen: bands, and a box on them that moves every 20th capture
       (little enough that its whole frame fits beside a top screen's frame
       stored as it is). Or (g_botBusy) at every capture, a big box of noise
       where the last one was: neither its whole frame nor what changed fits
       there, though under half of it changes. */
    if (bottom != NULL)
    {
        u32 bn = g_botW * g_botH;
        if (g_botBusy || no % 20 == 0 || g_botId == 0)
        {
            u32 bid = ++g_botId;
            for (u32 i = 0; i < bn; i++) g_scrBot[i] = (u16)((i / g_botW / 8) * 0x0841);
            u32 x0 = bid * 13 % (g_botW - 16), y0 = bid * 7 % (g_botH - 16);
            for (u32 y = y0; y < y0 + 12; y++)
                for (u32 x = x0; x < x0 + 12; x++)
                    g_scrBot[y * g_botW + x] = (u16)(bid * 2654435761u >> 16);
            if (g_botBusy)
                for (u32 y = 20; y < 100; y++)
                    for (u32 x = 20; x < 110; x++)
                        g_scrBot[y * g_botW + x] = (u16)rand();
            g_scrBot[0] = (u16)bid;
            g_scrBot[1] = (u16)(bid >> 16);
            g_botHash[bid] = hashOf(g_scrBot, bn);
        }
        for (u32 y = 0; y < g_botH; y++)
            for (u32 x = 0; x < g_botW; x++)
                bottom[x * g_botH + (g_botH - 1 - y)] = g_scrBot[y * g_botW + x];
    }
    if (fresh)
        g_frameBot[id] = g_botId;

    /* in the screen's order, as the capture hands frames over: column by column, bottom row up */
    for (u32 y = 0; top != NULL && y < g_h; y++)
        for (u32 x = 0; x < g_w; x++)
            top[x * g_h + (g_h - 1 - y)] = g_scrTop[y * g_w + x];
    if (second != NULL)
        *secondOk = true;
    return true;
}

static long errors;
#define FAIL(...) do { errors++; if (errors < 30) printf("  ERROR: " __VA_ARGS__); } while (0)

/* HID: a reading each time round the recording loop, its number in the
   Circle Pad's x. While a menu is open, SELECT is held (as when choosing in
   the menu): that must never show in a clip. */
static u32 g_hidN;
static void hidReading(bool inMenu)
{
    u32 r = ++g_hidN, i = r % 8;
    g_fakeHid[10 + i * 4] = inMenu ? KEY_SELECT : (r & 1 ? KEY_A : 0);
    g_fakeHid[10 + i * 4 + 3] = r & 0x7FFF;
    if (i == 0)                     /* (as on the 3DS, the tick only changes with index 0, the one before moving up) */
    {
        g_fakeHid[2] = g_fakeHid[0];
        g_fakeHid[3] = g_fakeHid[1];
        g_fakeHid[0] = r;
        g_fakeHid[1] = 0;
    }
    g_fakeHid[4] = i;
}


static void record(double seconds, const Events *ev)
{
    u64 period = SYSCLOCK_ARM11 / g_fps, start = svcGetSystemTick(), next = start;
    bool held12 = false, held30 = false, didReset = false, didStall = false, wasRecording = false;
    g_recordStart = start;
    g_freshNext = true;
    g_freezeEv = ev;
    for (;;)
    {
        double t = (svcGetSystemTick() - start) / (double)SYSCLOCK_ARM11;
        if (t >= seconds) break;
        bool paused = false;
        if (ev)
        {
            for (int i = 0; i < 8; i++) if (t >= ev->pauseFrom[i] && t < ev->pauseTo[i]) paused = true;
            g_dspStalled = t >= ev->stallFrom && t < ev->stallTo;
            if (!held12 && ev->hold12 > 0 && t >= ev->hold12) { g_audioHoldNs = 12000000; held12 = true; }
            if (!held30 && ev->hold30 > 0 && t >= ev->hold30) { g_audioHoldNs = 30000000; held30 = true; }
            if (!didReset && ev->resetAt > 0 && t >= ev->resetAt) { g_dspReset = true; didReset = true; }
        }
        bool recording = !paused;
        hidReading(paused);
        flashbackAudioSetCapture(recording && g_audio);     /* (as flashbackThreadMain) */
        if (recording && !wasRecording)
            flashbackInputResync();
        if (ev && ev->stallSdAt > 0 && !didStall && t >= ev->stallSdAt) { g_sdStallMs = 800; didStall = true; }
        if (recording && !flashbackCaptureFrame())
            g_pendingMissed++;
        if (recording)
            flashbackInputSample(1);
        wasRecording = recording;
        next += period;
        u64 now = svcGetSystemTick();
        if (next > now && recording) flashbackInputSleepUntil(next);
        else if (next > now) svcSleepThread((s64)((next - now) * 1e9 / SYSCLOCK_ARM11));
        else if (recording)
        {
            u32 missed = (u32)((now - next) / period);
            g_pendingMissed += missed;
            flashbackInputSample(missed);
            next += (u64)missed * period;
        }
        else next = now;
    }
    g_dspStalled = false;
    g_freezeEv = NULL;
}

/* As flashbackSave + flashbackSaveClipFile do it (the file being memory). */
static u8 *saveClip(u32 *size)
{
    flashbackDrainCaptures();
    flashbackAudioPause();
    flashbackDrainWrites();
    FlashbackClipPlan plan = flashbackPlanClip();
    u32 dataEnd = RING_VIDEO_BASE + (g_count < g_ringSlots ? g_count : g_ringSlots) * g_slotStride;
    u8 *tail = malloc(128 * 1024);
    u32 inputSize = flashbackBuildClipInputs(&plan, tail);
    u32 tailSize = inputSize + flashbackBuildClipIndex(&plan, dataEnd + inputSize, dataEnd, tail + inputSize);
    u8 *clip = malloc(dataEnd + tailSize);
    memcpy(clip, g_ring.data, dataEnd);
    memcpy(clip + dataEnd, tail, tailSize);
    free(tail);
    *size = dataEnd + tailSize;
    flashbackNewRing();
    flashbackAudioReset();
    flashbackInputReset();
    return clip;
}

static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static int magic_check(const u8 *t) { return rd32(t) != 0x4B424C46; }

/* What reading a clip's pictures found (readPictures). */
typedef struct {
    u32 lead, shown, frames, missed;        /* entries read before the first one shown; entries shown, for how many frames, how many of them missed */
    u32 topDelta, botStored, botDelta, botOnly, botStale, botUnknown;
    u32 own;                                /* entries after that with the second screen's whole frame only, shown for no frame */
    u32 topUnknown;                         /* frames the top screen has no picture for (it's the second screen, and has no whole frame yet) */
} Pictures;
#define MAX_ENTRIES 4096
static u32 g_entryId[MAX_ENTRIES];          /* the top screen's picture at each entry shown */
static Pictures g_lastPictures;             /* (from check()) */
/* With this set, readPictures writes a line for each entry shown instead of
   printing: how many frames it's shown for, and each screen's picture's
   fingerprint (hashOf; 0: the bottom screen has none yet). Flashback Studio's
   readers can be checked against those. */
static FILE *g_fingerprints;

/* Reads a saved clip's pictures as a reader does: entry by entry, whole frames
   and what changed, on both screens (a screen has no picture until its first
   whole frame). Each picture shown must be one that was captured, the top
   screen's and the bottom screen's each in order, and the second screen's the
   one that was on screen with the first one's (or an earlier one, if a frame
   of its found no room: counted). Which screen is first: as the recorder is
   set now (g_priBottom). */
static void readPictures(const char *name, const u8 *clip, u32 size, Pictures *st)
{
    bool bf = g_priBottom;
    static u16 top[400 * 240], bot[320 * 240];
    const u8 *t = clip + size - 48;
    u32 w = rd32(t + 8), h = rd32(t + 12), slotBytes = rd32(t + 24), entries = rd32(t + 28), indexOffset = rd32(t + 36);
    u32 bw = rd32(t + 44) & 0xFFFF, bh = rd32(t + 44) >> 16, lastTop = 0, lastBot = 0;
    bool topKnown = false, botKnown = false;
    memset(st, 0, sizeof(*st));
    if (entries > MAX_ENTRIES) { FAIL("%s: %u entries\n", name, entries); return; }
    if (bw != g_botW || bh != g_botH) FAIL("%s: bottom screen %ux%u\n", name, bw, bh);
    for (u32 e = 0; e < entries; e++)
    {
        const u8 *en = clip + indexOffset + e * 24;
        u32 off = rd32(en), tl = rd32(en + 4), bl = rd32(en + 8), copies = rd32(en + 12), flags = rd32(en + 20);
        if ((tl == 0 && bl == 0) || off + ((tl + 3) & ~3u) + bl > indexOffset || (flags & 0xFFF0) || (flags >> 16 && flags >> 16 >= copies))
        {
            FAIL("%s: entry %u: %u + %u bytes at %u, %u copies, flags %x\n", name, e, tl, bl, off, copies, flags);
            continue;
        }
        for (int s = 0; s < 2; s++)
        {
            u32 len = s ? bl : tl, rawBytes = s ? bw * bh * 2 : slotBytes;
            const u8 *at = clip + off + (s ? (tl + 3) & ~3u : 0);
            bool delta = flags >> (2 * s) & 1, coded = flags >> (2 * s) & 2, *known = s ? &botKnown : &topKnown, ok = true;
            u16 *px = s ? bot : top;
            if (len == 0) continue;
            if (len == rawBytes) { memcpy(px, at, len); *known = true; }
            else if (!delta) { ok = testDecodeFrame(at, len, px, s ? bw : w, s ? bh : h, false, coded); *known = true; }
            else if (*known) ok = testDecodeFrame(at, len, px, s ? bw : w, s ? bh : h, true, coded);
            if (!ok) FAIL("%s: entry %u: the %s screen's frame doesn't decode\n", name, e, s ? "bottom" : "top");
        }
        st->topDelta += flags & 1;
        st->botStored += bl != 0;
        st->botDelta += flags >> 2 & 1;
        st->botOnly += tl == 0;
        u32 id = topKnown ? top[0] | (u32)top[1] << 16 : 0;
        if (copies == 0)
        {
            /* (only read: the entries before the first one shown, and after that, the
               second screen's whole frame where it has a slot of its own) */
            if (st->shown != 0 && (bf ? tl == 0 || bl != 0 || (flags & 1) : tl != 0 || bl == 0 || (flags & 4)))
                FAIL("%s: entry %u isn't shown, after ones that are\n", name, e);
            if (st->shown == 0) st->lead++;
            else st->own++;
            g_entryId[e] = id;
            lastTop = id;
            if (botKnown) lastBot = bot[0] | (u32)bot[1] << 16;
            continue;
        }
        g_entryId[e] = 0;
        if (!(bf ? botKnown : topKnown)) { FAIL("%s: entry %u is shown before any whole frame of the first screen's\n", name, e); continue; }
        if (!topKnown)
            st->topUnknown += copies;
        else if (id == 0 || id > g_nextFrameId || hashOf(top, w * h) != g_topHash[id])
            FAIL("%s: entry %u: the top screen isn't a picture that was captured\n", name, e);
        else if (id < lastTop || (id != lastTop && tl == 0))
            FAIL("%s: entry %u: the top screen's picture %u after %u (%u bytes)\n", name, e, id, lastTop, tl);
        else
        {
            g_entryId[e] = id;
            lastTop = id;
        }
        if (bw != 0 && !botKnown)
            st->botUnknown += copies;
        else if (bw != 0)
        {
            u32 bid = bot[0] | (u32)bot[1] << 16;
            /* (the two pictures against each other, where the top screen's is a good one: the
               second screen's may be from before the first's, never from after it) */
            bool both = g_entryId[e] != 0;
            bool ahead = both && (bf ? bid < g_frameBot[id] : tl != 0 && bid > g_frameBot[id]);
            bool behind = both && (bf ? bid > g_frameBot[id] : tl != 0 && bid != g_frameBot[id]);
            if (bid == 0 || bid > g_botId || hashOf(bot, bw * bh) != g_botHash[bid])
                FAIL("%s: entry %u: the bottom screen isn't a picture that was captured\n", name, e);
            else if (bid < lastBot || ahead)
                FAIL("%s: entry %u: the bottom screen's picture %u (after %u; with the top one's it was %u)\n", name, e, bid, lastBot, g_frameBot[id]);
            else
            {
                st->botStale += behind;
                lastBot = bid;
            }
        }
        st->shown++;
        st->frames += copies;
        st->missed += flags >> 16;
        if (g_fingerprints != NULL)
            fprintf(g_fingerprints, "%u %016llx %016llx\n", copies, topKnown ? (unsigned long long)hashOf(top, w * h) : 0ull,
                    bw != 0 && botKnown ? (unsigned long long)hashOf(bot, bw * bh) : 0ull);
    }
    if (g_fingerprints != NULL)
        return;
    printf("  pictures: %u entries shown for %u frames (%u of them missed), %u read first; the top screen's stored as what changed %u times;\n"
           "    the bottom screen's stored %u times (%u as what changed, %u on their own); %u entries with the second screen's whole frame alone, shown for no frame;\n"
           "    the second screen behind the first %u times; no picture yet for %u frames (the top screen), %u (the bottom screen)\n",
           st->shown, st->frames, st->missed, st->lead, st->topDelta, st->botStored, st->botDelta, st->botOnly, st->own, st->botStale,
           st->topUnknown, st->botUnknown);
    if (st->frames != rd32(t + 32)) FAIL("%s: the entries are shown for %u frames, not %u\n", name, st->frames, rd32(t + 32));
    if (st->lead >= FLASHBACK_LEAD_MAX) FAIL("%s: %u entries read first\n", name, st->lead);
}

/* Reads a saved clip as a reader would and checks it. */
static void check(const char *name, const u8 *clip, u32 size, int expectJumps)
{
    const u8 *t = clip + size - 48;
    u32 version = rd32(t + 4), fps = rd32(t + 16), stride = rd32(t + 20);
    u32 entries = rd32(t + 28), frames = rd32(t + 32), indexOffset = rd32(t + 36);
    const u8 *ai = clip + indexOffset + entries * 24;
    u32 rateMhz = rd32(ai), aoff = rd32(ai + 8), cap = rd32(ai + 12), first = rd32(ai + 16), end = rd32(ai + 20), missed = rd32(ai + 24);
    printf("%s: version %x, %u frames (%u stored), stride %u, audio: rate %u mHz, ring %u samples, held %u..%u, missed %u\n",
           name, version, frames, entries, stride, rateMhz, cap, first, end, missed);
    /* (positions count from a whole number of rings back: 'first' is 0 only if the ring holds everything since it started) */
    if (end - first != (first == 0 ? end : cap - FLASHBACK_AUDIO_CHUNK_FRAMES * 160) || end > 3 * cap) FAIL("held range\n");
    if (version != (FLASHBACK_CLIP_VERSION_AUDIO | FLASHBACK_CLIP_HUFFMAN | FLASHBACK_CLIP_SCREENS) || magic_check(t) ||
        indexOffset + entries * 24 + 32 + 48 != size)
        FAIL("layout\n");
    Pictures pictures;
    readPictures(name, clip, size, &pictures);
    g_lastPictures = pictures;
    u32 lead = pictures.lead;   /* (the entries read before the first one shown: they have no place in time) */

    /* inputs: a record for every frame, just before the index; in order (the
       Circle Pad's x is the reading's number: the same again only for grid
       slots missed by waking late, a jump only across a menu pause), none
       from while a menu was open, and the last one the newest reading */
    u32 inputOffset = rd32(t + 40), inputJumps = 0;
    const u8 *ih = clip + inputOffset;
    if (inputOffset == 0 || rd32(ih) != 0x49424C46 || rd32(ih + 4) != 1 || rd32(ih + 8) != frames || rd32(ih + 12) != 16 ||
        inputOffset + 16 + frames * 16 != indexOffset)
        FAIL("inputs: layout\n");
    else
    {
        const FlashbackInput *in = (const FlashbackInput *)(ih + 16);
        for (u32 i = 0; i < frames; i++)
        {
            if (in[i].buttons & KEY_SELECT) { FAIL("inputs: frame %u has buttons pressed in a menu\n", i); break; }
            if (i > 0 && in[i].circleX < in[i - 1].circleX) { FAIL("inputs: frame %u out of order\n", i); break; }
            if (i > 0 && in[i].circleX > in[i - 1].circleX + 1) inputJumps++;
        }
        if (in[frames - 1].circleX != (s16)(g_hidN & 0x7FFF)) FAIL("inputs: the last frame's aren't the newest\n");
        if (expectJumps >= 0 && (int)inputJumps > expectJumps) FAIL("inputs: %u jumps, more than the pauses\n", inputJumps);
    }
    printf("  inputs: %u records, %u jumps (menu pauses)\n", frames, inputJumps);

    /* audio at each clip frame lines up with the DSP at capture time (for the
       entries with a new picture on the top screen: the pictures' ids say when
       they were captured) */
    u32 f = 0, silentFrames = 0, maxOff = 0;
    double spf = (double)SYSCLOCK_ARM11 / (fps * 8192.0);
    for (u32 e = lead; e < entries; e++)
    {
        const u8 *en = clip + indexOffset + e * 24;
        u32 copies = rd32(en + 12), apos = rd32(en + 16), id = g_entryId[e];
        /* (an entry with the picture the one before had was captured later
           than that picture first was: the bottom screen's frame alone, or the
           top screen's stored whole again for a clip to start from) */
        bool newPicture = id != 0 && id != (e > 0 ? g_entryId[e - 1] : 0);
        for (u32 c = 0; c < copies; c++, f++)
        {
            u32 pos = apos + (u32)(c * spf);
            if (pos < first || pos >= end) { FAIL("frame %u's audio %u isn't in the ring (end %u)\n", f, pos, end); continue; }
            if (!newPicture) continue;
            const s16 *smp = (const s16 *)(clip + aoff + (u64)(pos % cap) * 4);
            if (!(smp[1] & 0x8000)) { silentFrames++; continue; }
            u32 idx = (u16)smp[0] | (u32)(smp[1] & 0x7FFF) << 16;
            double expect = g_frameDsp[id] + c * (g_fps * 0 + 204.55 / g_fps);
            double d = idx / 160.0 - expect;
            u32 ad = (u32)(d < 0 ? -d : d);
            if (ad > maxOff) maxOff = ad;
        }
    }
    printf("  video frame -> audio: max %u DSP frames from the DSP at capture time, %u frames on silence\n", maxOff, silentFrames);

    /* Flashback Studio (3DS and Windows) plays the audio straight through from the first
       frame: how far is each frame from its recorded audio position, played so? */
    {
        u32 start = rd32(clip + indexOffset + lead * 24 + 16), fr = 0;
        double maxDev = 0, endDev = 0;
        for (u32 e = lead; e < entries; e++)
        {
            const u8 *en = clip + indexOffset + e * 24;
            double dev = ((double)rd32(en + 16) - (start + fr * spf)) / 32.7285;
            if (fabs(dev) > maxDev) maxDev = fabs(dev);
            endDev = dev;
            fr += rd32(en + 12);
        }
        g_lastMaxDevMs = maxDev;
        g_lastEndDevMs = endDev;
        printf("  straight-through audio vs each frame: max %.1f ms, at the end %+.1f ms\n", maxDev, endDev);
    }
    if (maxOff > 4) FAIL("audio and video out of step\n");

    /* the clip's audio, start to end: in order, gaps only where expected */
    u32 start = rd32(clip + indexOffset + lead * 24 + 16), len = (u32)(frames * spf);
    u32 prev = 0xFFFFFFFF, jumps = 0, silence = 0, backwards = 0;
    for (u32 i = 0; i < len; i++)
    {
        u32 pos = start + i;
        if (pos >= end) break;
        const s16 *smp = (const s16 *)(clip + aoff + (u64)(pos % cap) * 4);
        if (!(smp[1] & 0x8000)) { silence++; prev = 0xFFFFFFFF; continue; }
        u32 idx = (u16)smp[0] | (u32)(smp[1] & 0x7FFF) << 16;
        if (prev != 0xFFFFFFFF && idx != prev + 1) { if (idx > prev) jumps++; else backwards++; }
        prev = idx;
    }
    g_lastSilentSamples = silence;
    printf("  audio: %u samples, %u silent (%.1f frames), %d jumps forward (pauses), %u backwards\n",
           len, silence, silence / 160.0, jumps, backwards);
    if (backwards) FAIL("audio out of order\n");
    if (expectJumps >= 0 && (int)jumps > expectJumps) FAIL("more audio jumps than pauses\n");
}


int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    timeBeginPeriod(1);
    srand(7);

    /* the audio ring must hold every clip length, plus its 2 s of margin */
    for (u32 secs = FLASHBACK_MIN_SECONDS; secs <= FLASHBACK_MAX_SECONDS; secs++)
    {
        u32 frames = flashbackAudioFramesFor(secs);
        double need = (secs + FLASHBACK_AUDIO_EXTRA_SECONDS) * 32728.498 / 160;
        if (secs == FLASHBACK_MIN_SECONDS || secs % 120 == 0)
            printf("%3u s: audio ring %u frames (%.2f s)\n", secs, frames, frames * 160 / 32728.498);
        if (frames < need || frames > need + FLASHBACK_AUDIO_CHUNK_FRAMES + 1) FAIL("audio ring size for %u s\n", secs);
    }

    /* 200x120 and 160x120, the top screen first, 30 fps, 10 s, audio */
    flashbackApplyFormat(FLASHBACK_RES_HALF, FLASHBACK_SCREENS_BOTH, FLASHBACK_PRIORITY_TOP, 2, 10, true);
    printf("ring: audio %u frames (%u bytes), video %u slots of %u; memory %u bytes\n",
           g_audioCapFrames, g_audioBytes, g_ringSlots, g_slotStride, g_memSize);
    flashbackUseMem(calloc(1, g_memSize));
    g_ring.size = RING_VIDEO_BASE + (u64)g_ringSlots * g_slotStride;
    g_ring.data = calloc(1, g_ring.size);
    flashbackNewRing(); flashbackAudioReset();
    g_active = true;
    flashbackInputReset();
    g_writerRunning = true;
    g_audioRunning = true;
    LightEvent_Init(&g_writeEvent);
    LightEvent_Init(&g_packEvent);
    g_packRunning = true;

    pthread_t dsp, aud, wr, pk;
    pthread_create(&dsp, NULL, dspThread, NULL);
    pthread_create(&aud, NULL, audioThread, NULL);
    pthread_create(&wr, NULL, writerThread, NULL);
    pthread_create(&pk, NULL, packThread, NULL);
    svcSleepThread(50000000);

    /* 1: 13 s with events; the clip is the last 10 s */
    Events ev = { { 3.0, 8.0 }, { 3.5, 8.3 }, 5.0, 5.4, 6.0, 7.0 };
    record(13.0, &ev);
    u32 size1;
    u8 *clip1 = saveClip(&size1);
    check("clip 1 (pauses, DSP stall, hold-ups)", clip1, size1, 2);
    /* (the top screen is all new at every capture: its frames are stored as they
       are, which leaves the bottom screen's just over 1 KB of the slot) */
    if (g_lastPictures.botStored == 0 || g_lastPictures.botDelta == 0) FAIL("the bottom screen's frames weren't stored\n");

    /* 2: recording goes on after the save; nothing happens for 11 s */
    record(11.0, NULL);
    u32 size2;
    u8 *clip2 = saveClip(&size2);
    check("clip 2 (after a save, undisturbed)", clip2, size2, 0);
    u32 missed2 = rd32(clip2 + size2 - 48 - 32 + 24);
    if (missed2 != 0) FAIL("frames missed with nothing holding the audio thread up\n");

    /* 3: a short clip (the ring never filled) */
    record(3.0, NULL);
    u32 size3;
    u8 *clip3 = saveClip(&size3);
    check("clip 3 (3 s, ring not full)", clip3, size3, 0);

    /* 4: beeps with white frames, for checking A/V sync after conversion (the
       frames differ by their ids only, and the flashes: stored as what changed) */
    g_beeps = true;
    g_topMode = TOP_BEEPS;
    record(12.0, NULL);
    u32 size4;
    u8 *clip4 = saveClip(&size4);
    g_beeps = false;
    g_topMode = TOP_NOISE;
    {
        Pictures p;
        printf("clip 4 (beeps and white frames)\n");
        readPictures("clip 4", clip4, size4, &p);
        if (p.topDelta < p.shown / 2) FAIL("clip 4: frames that differ by a few pixels weren't stored as what changed\n");
    }

    /* 11 (out of order: it's short): the top screen as it was two captures in
       three, the bottom one nineteen in twenty. Nothing is stored for those. */
    g_topMode = TOP_STILL;
    record(8.0, NULL);
    u32 size11;
    u8 *clip11 = saveClip(&size11);
    g_topMode = TOP_NOISE;
    check("clip 11 (screens that mostly stay as they are)", clip11, size11, 0);
    if (g_lastPictures.frames < 2 * g_lastPictures.shown || g_lastPictures.missed > g_lastPictures.frames / 20)
        FAIL("clip 11: frames the same as the one before were stored, or counted as missed\n");
    if (g_lastPictures.botOnly == 0 || g_lastPictures.botStale > g_lastPictures.shown / 10 || g_lastPictures.botUnknown != 0)
        FAIL("clip 11: the bottom screen's frames (stored on their own %u times, behind %u times, missing for %u frames)\n",
             g_lastPictures.botOnly, g_lastPictures.botStale, g_lastPictures.botUnknown);
    {
        FILE *f = fopen("out/clip_test5.flb", "wb");
        fwrite(clip11, 1, size11, f);
        fclose(f);
        Pictures p;
        g_fingerprints = fopen("out/clip_test5.txt", "w");
        readPictures("clip 11", clip11, size11, &p);
        fclose(g_fingerprints);
        g_fingerprints = NULL;
    }
    free(clip11);

    /* 12 (short too): the bottom screen new at every capture, with a lot to
       store, beside a top screen stored as it is, which leaves it just over
       1 KB of the slot. Its whole frame gets a slot of its own every
       FLASHBACK_KEY_SLOTS slots; its other frames aren't stored, and nearly
       all of those aren't compressed either. */
    {
        u32 lost0 = g_packLost, wasted0 = g_packWasted, own0 = g_packOwn;
        g_botBusy = true;
        record(8.0, NULL);
        u32 size12;
        u8 *clip12 = saveClip(&size12);
        g_botBusy = false;
        u32 lost = g_packLost - lost0, wasted = g_packWasted - wasted0, own = g_packOwn - own0;
        check("clip 12 (a busy bottom screen with no room in the top screen's slots)", clip12, size12, 0);
        printf("  the bottom screen's frames: %u not stored (%u of them compressed for nothing), %u whole in a slot of their own\n",
               lost, wasted, own);
        if (g_lastPictures.own < g_lastPictures.shown / (FLASHBACK_KEY_SLOTS + 2) || g_lastPictures.botUnknown != 0)
            FAIL("clip 12: the bottom screen's whole frames (%u in entries of their own, for %u entries shown; missing for %u frames)\n",
                 g_lastPictures.own, g_lastPictures.shown, g_lastPictures.botUnknown);
        if (lost < g_lastPictures.shown / 2 || wasted * 8 > lost)
            FAIL("clip 12: %u of the bottom screen's frames not stored, %u of them compressed for nothing\n", lost, wasted);
        FILE *f = fopen("out/clip_test6.flb", "wb");
        fwrite(clip12, 1, size12, f);
        fclose(f);
        Pictures p;
        g_fingerprints = fopen("out/clip_test6.txt", "w");
        readPictures("clip 12", clip12, size12, &p);
        fclose(g_fingerprints);
        g_fingerprints = NULL;
        free(clip12);
    }

    /* 5: the game restarts its audio (its counters start over, low), 12 s in */
    Events ev5 = { { 0, 0 }, { 0, 0 }, 0, 0, 0, 0, 12.0 };
    record(16.0, &ev5);
    u32 size5;
    u8 *clip5 = saveClip(&size5);
    check("clip 5 (the game's audio counters start over)", clip5, size5, 1);   /* (a reset can cost one frame) */
    if (g_lastSilentSamples > 3 * 160) FAIL("audio lost after the counters started over\n");
    if (fabs(g_lastEndDevMs) > 10) FAIL("the audio isn't back in line after the counters started over\n");

    /* 6: the game's audio pauses for 30 ms (no frames), 6 s in */
    Events ev6 = { { 0, 0 }, { 0, 0 }, 6.0, 6.03, 0, 0, 0 };
    record(12.0, &ev6);
    u32 size6;
    u8 *clip6 = saveClip(&size6);
    check("clip 6 (the game's audio pauses for 30 ms)", clip6, size6, 1);
    if (fabs(g_lastEndDevMs) > 10) FAIL("the audio isn't back in line after a pause\n");
    free(clip6);

    /* 7: the SD card stalls for 0.8 s (the game reading it), 6 s in */
    Events ev7 = { { 0, 0 }, { 0, 0 }, 0, 0, 0, 0, 0, 6.0 };
    record(12.0, &ev7);
    u32 full7 = g_audioFull;
    u32 size7;
    u8 *clip7 = saveClip(&size7);
    check("clip 7 (the SD card stalls for 0.8 s)", clip7, size7, 0);
    printf("  frames lost to full staging: %u, silent samples %u\n", full7, g_lastSilentSamples);
    if (full7 != 0 || g_lastSilentSamples > 3 * 160) FAIL("audio lost while the SD card stalled\n");
    free(clip7);

    /* 8: eight Rosalina menu pauses (the game frozen meanwhile) in 10 s: each
       pause must leave the sound where it was against the picture */
    Events ev8 = { { 1.0, 2.13, 3.27, 4.41, 5.52, 6.66, 7.79, 8.93 },
                   { 1.3, 2.44, 3.61, 4.70, 5.83, 6.98, 8.11, 9.25 } };
    ev8.freeze = true;
    record(12.7, &ev8);
    u32 size8;
    u8 *clip8 = saveClip(&size8);
    check("clip 8 (eight menu pauses, the game frozen meanwhile)", clip8, size8, 8);
    if (fabs(g_lastEndDevMs) > 6) FAIL("menu pauses move the sound against the picture\n");
    free(clip8);

    /* 9: the clip starts while the game plays no audio (the DSP gives no frames
       from 1 s to 4.5 s; the clip is 2 s to 12 s): the sound that follows must
       still be in step with the picture */
    Events ev9 = { { 0, 0 }, { 0, 0 }, 1.0, 4.5 };
    record(12.0, &ev9);
    u32 size9;
    u8 *clip9 = saveClip(&size9);
    check("clip 9 (starts in a silent stretch)", clip9, size9, 1);
    /* (to about a frame: the clip's first frame is in the silence, which is only kept to the frame) */
    if (fabs(g_lastEndDevMs) > 8) FAIL("a clip that starts in silence has the sound out of step\n");
    free(clip9);

    /* 10: as if recording had gone on for ~36.4 hours: the audio count passes
       2^32 samples during the clip */
    {
        u32 k = (0xFFFFFFFFu / 160 - 1000) & ~(FLASHBACK_AUDIO_CHUNK_FRAMES - 1u);     /* ~5 s before */
        g_audioProduced = g_audioConsumed = g_audioFlushed = k;
        g_audioCapturedTicks = (u64)k * FRAME_TICKS;
    }
    record(12.0, NULL);
    u32 size10;
    u8 *clip10 = saveClip(&size10);
    check("clip 10 (the audio count passes 2^32 samples, ~36.4 hours in)", clip10, size10, 0);
    if (fabs(g_lastEndDevMs) > 6 || g_lastSilentSamples > 3 * 160) FAIL("audio wrong past 2^32 samples\n");
    free(clip10);

    const u8 *clips[4] = { clip1, clip2, clip3, clip4 };
    u32 sizes[4] = { size1, size2, size3, size4 };
    for (int i = 0; i < 4; i++)
    {
        char name[64];
        snprintf(name, sizeof name, "out/clip_test%d.flb", i + 1);
        FILE *f = fopen(name, "wb");
        fwrite(clips[i], 1, sizes[i], f);
        fclose(f);
        Pictures p;
        snprintf(name, sizeof name, "out/clip_test%d.txt", i + 1);
        g_fingerprints = fopen(name, "w");
        readPictures(name, clips[i], sizes[i], &p);
        fclose(g_fingerprints);
        g_fingerprints = NULL;
    }
    /* 13: the bottom screen first (the recorder set up again for that, as when
       the setting changes), and busy; the top screen all new at every capture,
       so its frame never fits in what the bottom screen's leaves of the slot.
       It's the top screen's frames that go unstored now, but for a whole one
       in a slot of its own every FLASHBACK_KEY_SLOTS slots, and they aren't
       compressed for nothing either. */
    {
        flashbackApplyFormat(FLASHBACK_RES_HALF, FLASHBACK_SCREENS_BOTH, FLASHBACK_PRIORITY_BOTTOM, 2, 10, true);
        flashbackUseMem(calloc(1, g_memSize));  /* (the memory from before isn't freed: the threads are only idle) */
        flashbackNewRing(); flashbackAudioReset();
        flashbackInputReset();
        u32 lost0 = g_packLost, wasted0 = g_packWasted, own0 = g_packOwn;
        g_botBusy = true;
        record(8.0, NULL);
        u32 size13;
        u8 *clip13 = saveClip(&size13);
        g_botBusy = false;
        u32 lost = g_packLost - lost0, wasted = g_packWasted - wasted0, own = g_packOwn - own0;
        check("clip 13 (the bottom screen first, with no room for the top screen's frames)", clip13, size13, 0);
        printf("  the top screen's frames: %u not stored (%u of them compressed for nothing), %u whole in a slot of their own\n",
               lost, wasted, own);
        if (g_lastPictures.own < g_lastPictures.shown / (FLASHBACK_KEY_SLOTS + 2) || g_lastPictures.topUnknown != 0 ||
            g_lastPictures.botUnknown != 0)
            FAIL("clip 13: the top screen's whole frames (%u in entries of their own, for %u entries shown; missing for %u frames)\n",
                 g_lastPictures.own, g_lastPictures.shown, g_lastPictures.topUnknown);
        if (g_lastPictures.botStored < g_lastPictures.shown || g_lastPictures.missed > g_lastPictures.frames / 20)
            FAIL("clip 13: the bottom screen comes first, but only %u of its %u frames were stored (%u frames missed)\n",
                 g_lastPictures.botStored, g_lastPictures.shown, g_lastPictures.missed);
        if (lost < g_lastPictures.shown / 2 || wasted * 8 > lost)
            FAIL("clip 13: %u of the top screen's frames not stored, %u of them compressed for nothing\n", lost, wasted);
        FILE *f = fopen("out/clip_test7.flb", "wb");
        fwrite(clip13, 1, size13, f);
        fclose(f);
        Pictures p;
        g_fingerprints = fopen("out/clip_test7.txt", "w");
        readPictures("clip 13", clip13, size13, &p);
        fclose(g_fingerprints);
        g_fingerprints = NULL;
        free(clip13);
    }

    printf("wrote out/clip_test1..7.flb (clip 4: a beep and white frames once a second; clip 5: screens that mostly stay as they are;\n"
           "  clip 6: a busy bottom screen with slots of its own; clip 7: the bottom screen first), and each one's pictures'\n"
           "  fingerprints in a .txt next to it\n");

    printf("SD writes: %ld, not sector aligned: %ld\n", g_ifileWrites, g_ifileUnaligned);
    if (g_ifileUnaligned) FAIL("unaligned writes\n");

    preTerminationRequested = true;
    g_dspStop = true;
    pthread_join(aud, NULL);
    LightEvent_Signal(&g_packEvent);
    pthread_join(pk, NULL);
    LightEvent_Signal(&g_writeEvent);
    pthread_join(wr, NULL);
    pthread_join(dsp, NULL);
    printf("%s (%ld errors)\n", errors ? "FAILED" : "all good", errors);
    return errors != 0;
}
