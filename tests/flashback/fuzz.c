/* Deterministic fuzz test of Flashback's recording, with the recorder's own
   code (recorder_extracted.c, made by extract.py): the capture queue, packed
   staging (every size it can have), audio staging, the writer (audio chunks
   and video frames), unfinished-chunk flushes for saves, saves that reset the
   ring and saves that fail and carry on, write failures, staging running
   full, every clip length and frame rate, audio on and off, and the inputs'
   records (HID taking readings between grid slots and during them, more than
   it holds at times, and the recorder waking up late).

   The producers (audio frames, video frames) run in the middle of the
   writer's SD writes, and the "card" only takes the data at the end of each
   write, so a producer touching data that's being written would show up.

   After every drain it checks, exactly: the audio ring holds every audio frame
   it should (by content), every buffered video slot holds the right frame,
   each slot's audio position is where the audio was when the frame was
   captured, and the clip's inputs, index and audio info a save would write. */
#include "stubs_fuzz.h"
/* Staged frames start on 512-byte boundaries here (a page on the 3DS): with the
   tiny frames below, that packs 1 to 4 to a slot, as real frames do to a page
   multiple, and the frames waiting can outnumber the slots' worth of staging. */
#define FLASHBACK_STAGE_ALIGN 512
#include "recorder_extracted.c"
#include "asm_standin.h"
#ifndef RING_VIDEO_BASE
#define RING_VIDEO_BASE g_videoOffset  /* where the ring's video slots start */
#endif
#define FLASHBACK_MAX_FRAMES (FLASHBACK_MAX_SECONDS * 30)  /* the longest clip at the highest frame rate */

/* Flashback Studio's decoder (a copy of 3ds-app/source/flb.c's: decode.c), linked in */
bool flbDecompress(const uint8_t *src, uint32_t len, uint16_t *dst, uint32_t pixels);

#define TINY_W 32
#define TINY_H 32

static long errors, checks, audioChecked, videoChecked, saves, failedSaves, failStops, stagingFull, runs;
#define FAIL(...) do { errors++; if (errors < 20) printf("  ERROR: " __VA_ARGS__); } while (0)

static u32 g_epoch;
static u32 *g_audioModel;           /* tag of each appended audio frame since g_kBase; 0 = silence */
static u32 g_kBase;                 /* audio frame count the ring started at (high: as if ~36 hours in) */
static u32 g_modelSlotId[FLASHBACK_MAX_FRAMES], g_modelSlotFrame[FLASHBACK_MAX_FRAMES];
static u32 g_nextId;
static int g_inProducer, g_budget, g_reentrantPct;

static u32 tagFor(u32 k) { return (g_epoch << 22) | (k & 0x3FFFFF) | 0x80000000u; }

/* The frame (upright: its first two pixels carry its id), handed over in the
   screen's order as the real capture does: column by column, bottom row up. */
static bool flashbackCapture(u16 *dst)
{
    u16 up[TINY_W * TINY_H];
    u32 id = ++g_nextId;
    int style = rand() % 3;
    for (u32 i = 0; i < TINY_W * TINY_H; i++)
        up[i] = style == 0 ? (u16)(id * 7 + i / 9) : style == 1 ? (u16)rand() : (i % 5 ? up[i - 1] : (u16)rand());
    up[0] = (u16)id;
    up[1] = (u16)(id >> 16);
    for (u32 y = 0; y < TINY_H; y++)
        for (u32 x = 0; x < TINY_W; x++)
            dst[x * TINY_H + (TINY_H - 1 - y)] = up[y * TINY_W + x];
    return true;
}

static void appendAudio(bool silent)
{
    if (!g_audio) return;
    u32 k = g_audioProduced, block[160];
    for (int i = 0; i < 160; i++) block[i] = tagFor(k);
    if (flashbackAppendAudio(silent ? NULL : block))
        g_audioModel[k - g_kBase] = silent ? 0 : tagFor(k);
    else
        stagingFull++;
}

/* captured frames waiting to be packed (id, audio position), oldest first */
static u32 g_modelQId[64], g_modelQFrame[64], g_modelQHead, g_modelQTail;

/* frames the recorder packed since g_capConsumed was 'before': the latest ring slots, in order */
static void modelPacked(u32 before)
{
    u32 n = g_capConsumed - before;
    if (n > g_modelQTail - g_modelQHead) { FAIL("packed more frames than were captured\n"); n = g_modelQTail - g_modelQHead; }
    for (u32 i = 0; i < n; i++, g_modelQHead++)
    {
        u32 slot = (g_head + g_frames - n + i) % g_frames;
        g_modelSlotId[slot] = g_modelQId[g_modelQHead % 64];
        g_modelSlotFrame[slot] = g_modelQFrame[g_modelQHead % 64];
    }
}

/* ---- inputs: a fake of HID's shared memory (as flashback_input.c reads it),
   and what the records should be */
static u32 g_padTotal, g_padSeen, g_touchTotal, g_touchSeen;   /* readings so far, and the newest looked at */
static FlashbackInput g_mInputNow, g_mInputNewest;
static FlashbackInput g_mInputs[FLASHBACK_MAX_FRAMES];           /* the records, one per grid slot (round) */
static u32 g_mInputCount;
static long g_inputsChecked, g_inputOverruns;

/* what reading r holds (reading 0: HID's memory as it starts, all zero) */
static u32 heldOf(u32 r)     { u32 h = r * 2654435761u; return r ? (h >> 9 & 0xFFF) | (h & 0xF0000000) : 0; }
static u32 circleOf(u32 r)   { return r ? (r & 0xFFFF) | (r * 3 & 0xFFFF) << 16 : 0; }
static bool touchedOf(u32 t) { return t && t / 4 % 3 == 0; }
static u32 posOf(u32 t)      { return t ? t % 320 | (t * 7 % 240) << 16 : 0; }
static void put64(u32 *w, u64 v) { w[0] = (u32)v; w[1] = (u32)(v >> 32); }
static u64 get64(const u32 *w)   { return w[0] | (u64)w[1] << 32; }

/* A section of HID's memory has taken reading 'index' at g_fakeTicks. As on
   the 3DS, its tick only changes when index 0 is taken (once a round of 8),
   and the tick before it moves up to the next field. */
static void sectionTook(u32 *section, u32 index)
{
    if (index == 0)
    {
        put64(section + 2, get64(section));
        put64(section, g_fakeTicks);
    }
    section[4] = index;
}

/* HID takes a reading of the pad (and, every other time or so, the touch screen) */
#define HID_TICKS (SYSCLOCK_ARM11 / 250)    /* (4 ms apart) */
static void hidReading(void)
{
    g_fakeTicks += HID_TICKS;
    u32 r = ++g_padTotal, i = r % 8;
    g_fakeHid[10 + i * 4] = heldOf(r);
    g_fakeHid[10 + i * 4 + 3] = circleOf(r);
    sectionTook(g_fakeHid, i);
    if (rand() % 2)
    {
        u32 t = ++g_touchTotal, j = t % 8;
        g_fakeHid[50 + j * 2] = posOf(t);
        g_fakeHid[51 + j * 2] = touchedOf(t);
        sectionTook(g_fakeHid + 42, j);
    }
}

/* What a look at HID gives: the readings since the last look that it still
   holds (its 8 newest), added to the record under way. */
static void modelLook(bool discard)
{
    u32 held = 0, pos = 0;
    bool touched = false;
    if (g_padTotal > g_padSeen + 8) g_inputOverruns++;
    for (u32 r = g_padTotal > g_padSeen + 8 ? g_padTotal - 7 : g_padSeen + 1; r <= g_padTotal; r++)
        held |= heldOf(r);
    for (u32 t = g_touchTotal > g_touchSeen + 8 ? g_touchTotal - 7 : g_touchSeen + 1; t <= g_touchTotal; t++)
        if (touchedOf(t)) { touched = true; pos = posOf(t); }  /* (the newest touched one) */
    g_padSeen = g_padTotal;
    g_touchSeen = g_touchTotal;

    bool on = touchedOf(g_touchTotal);
    FlashbackInput newest = { heldOf(g_padTotal) | (on ? KEY_TOUCH : 0),
                              (s16)circleOf(g_padTotal), (s16)(circleOf(g_padTotal) >> 16),
                              on ? (u16)posOf(g_touchTotal) : 0, on ? (u16)(posOf(g_touchTotal) >> 16) : 0, 0, 0 };
    g_mInputNewest = newest;
    if (discard)
        g_mInputNow = newest;
    else
    {
        g_mInputNow.buttons |= held | (touched ? KEY_TOUCH : 0);
        g_mInputNow.circleX = newest.circleX;
        g_mInputNow.circleY = newest.circleY;
        if (touched) { g_mInputNow.touchX = (u16)pos; g_mInputNow.touchY = (u16)(pos >> 16); }
    }
}

/* records for 'slots' grid slots: the one under way, then copies of the newest readings */
static void modelSample(u32 slots)
{
    if (slots == 0) return;
    modelLook(false);
    for (u32 i = 0; i < slots; i++)
    {
        g_mInputs[g_mInputCount++ % FLASHBACK_MAX_FRAMES] = g_mInputNow;
        g_mInputNow = g_mInputNewest;
    }
}

static long g_queuedAfterCapture, g_queueDepthMax, g_fullCard, g_fullCpu;
static long g_stagedMax, g_stagedOverSlots, g_wraps;
static u32 g_lastHead;
static void stageStats(void)
{
    u32 st = g_stageProduced - g_stageConsumed;
    if ((long)st > g_stagedMax) g_stagedMax = st;
    if (st > g_stageSlots) g_stagedOverSlots++;
    if (g_stageHead < g_lastHead) g_wraps++;
    g_lastHead = g_stageHead;
}
/* One grid slot, as flashbackThreadMain records it */
static void recordVideo(void)
{
    /* HID's readings since the last grid slot (looked at now and then
       meanwhile, as the recorder thread does while it waits): sometimes more
       than it holds */
    for (int n = rand() % 20 == 0 ? 9 + rand() % 12 : rand() % 4; n > 0; n--)
    {
        hidReading();
        if (rand() % 6 == 0) { flashbackInputPoll(); modelLook(false); }
    }

    u32 frame = g_audioProduced, before = g_capConsumed, id = g_nextId;
    if (flashbackCaptureFrame())
    {
        if (g_nextId != id + 1) FAIL("capture not taken\n");
        g_modelQId[g_modelQTail % 64] = g_nextId;
        g_modelQFrame[g_modelQTail % 64] = frame;
        g_modelQTail++;
    }
    else
    {
        if (g_capProduced - g_capConsumed >= g_capBufs)
        {
            if (g_stageProduced - g_stageConsumed >= g_stageSlots) g_fullCard++;
            else g_fullCpu++;
        }
        g_pendingMissed++;
    }
    flashbackInputSample(1);
    modelSample(1);

    /* now and then the thread wakes up late: grid slots missed after this one */
    if (rand() % 40 == 0)
    {
        u32 n = 1 + rand() % 3;
        if (rand() % 2) hidReading();
        g_pendingMissed += n;
        flashbackInputSample(n);
        modelSample(n);
    }
    modelPacked(before);
    stageStats();
    u32 depth = g_capProduced - g_capConsumed;
    if (depth) g_queuedAfterCapture++;
    if ((long)depth > g_queueDepthMax) g_queueDepthMax = depth;
}

static void producerTick(void)
{
    g_inProducer = 1;
    int r = rand() % 10;
    if (r < 6) appendAudio(rand() % 8 == 0);
    else recordVideo();
    g_inProducer = 0;
}

static void writeHook(IFile *f)
{
    (void)f;
    if (g_inProducer) return;
    for (int n = 0; n < 6 && g_budget > 0 && rand() % 100 < g_reentrantPct; n++, g_budget--)
        producerTick();
}

/* As flashbackSave does before writing a clip: pause audio, write everything. */
static void drain(void)
{
    g_budget = 0;
    bool w = g_writerRunning;
    g_writerRunning = false;    /* (inline: there's no writer thread here) */
    while (g_stageProduced != g_stageConsumed)   /* (the writer's part: staging may be full, and packing needs a slot) */
        flashbackWritePending();
    u32 before = g_capConsumed;
    flashbackDrainCaptures();   /* (as flashbackSave: every captured frame packed first) */
    modelPacked(before);
    if (g_capProduced != g_capConsumed) FAIL("captures left unpacked\n");
    flashbackAudioPause();
    flashbackDrainWrites();
    g_writerRunning = w;
}

static void check(void)
{
    checks++;
    if (g_stageProduced != g_stageConsumed) FAIL("video left unwritten\n");
    if (g_audioFlushed != g_audioProduced) FAIL("audio left unwritten (%u of %u)\n", g_audioFlushed, g_audioProduced);
    if (g_writeFailed) return;  /* (the ring's contents don't count any more) */

    /* audio: every frame the ring should still hold, by content (from the
       ring's real start: with a high g_kBase the counts start part way) */
    u32 lo = 0, base = 0;
    if (g_audio)
    {
        u32 held = g_audioCapFrames - FLASHBACK_AUDIO_CHUNK_FRAMES;
        lo = g_audioFlushed > held ? g_audioFlushed - held : 0;
        base = lo >= g_audioCapFrames ? (lo / g_audioCapFrames - 1) * g_audioCapFrames : 0;
        for (u32 k = lo > g_kBase ? lo : g_kBase; k < g_audioFlushed; k++)
        {
            const u32 *p = (const u32 *)(g_ring.data + (u64)(k % g_audioCapFrames) * FLASHBACK_AUDIO_FRAME_BYTES);
            u32 want = g_audioModel[k - g_kBase];
            for (int i = 0; i < 160; i++)
                if (p[i] != want) { FAIL("audio frame %u: %08x, want %08x\n", k, p[i], want); break; }
            audioChecked++;
        }
    }

    /* video: every buffered slot (decoded by the 3DS app's decoder), and its audio position */
    u8 one[TINY_W * TINY_H * 2];
    for (u32 i = 0; i < g_count; i++)
    {
        u32 r = (g_head + g_frames - 1 - i) % g_frames;
        const u16 *px = (const u16 *)one;
        const u8 *stored = g_ring.data + RING_VIDEO_BASE + (u64)r * g_slotStride;
        bool ok = g_slotLen[r] == g_slotBytes ? (memcpy(one, stored, g_slotBytes), true)
                                              : flbDecompress(stored, g_slotLen[r], (u16 *)one, TINY_W * TINY_H);
        if (!ok || (px[0] | (u32)px[1] << 16) != g_modelSlotId[r])
            FAIL("video slot %u wrong\n", r);
        if (g_audio && g_slotAudioFrame[r] != g_modelSlotFrame[r])
            FAIL("slot %u audio frame %u, want %u\n", r, g_slotAudioFrame[r], g_modelSlotFrame[r]);
        videoChecked++;
    }

    /* what a save would append: the inputs (a record for each of the clip's
       frames: the last ones, by grid slot), then the index */
    if (g_count == 0) return;
    FlashbackClipPlan plan = flashbackPlanClip();
    u32 dataEnd = RING_VIDEO_BASE + (g_count < g_frames ? g_count : g_frames) * g_slotStride;
    u8 *ibuf = g_mem;   /* (where a save builds them, over staging and the capture buffers) */
    u32 isize = flashbackBuildClipInputs(&plan, ibuf);
    const u32 *ih = (const u32 *)ibuf;
    if (isize != 16 + plan.frames * 16 || ih[0] != 0x49424C46 || ih[1] != 1 || ih[2] != plan.frames || ih[3] != 16)
        FAIL("inputs header\n");
    if (plan.frames > g_mInputCount)
        FAIL("%u frames, but only %u input records\n", plan.frames, g_mInputCount);
    else
        for (u32 f = 0; f < plan.frames; f++)
        {
            const FlashbackInput *got = (const FlashbackInput *)(ibuf + 16) + f;
            const FlashbackInput *want = &g_mInputs[(g_mInputCount - plan.frames + f) % FLASHBACK_MAX_FRAMES];
            if (memcmp(got, want, sizeof(*got)) != 0)
            {
                FAIL("frame %u of %u's inputs: %08x %d,%d %u,%u; want %08x %d,%d %u,%u\n", f, plan.frames,
                     got->buttons, got->circleX, got->circleY, got->touchX, got->touchY,
                     want->buttons, want->circleX, want->circleY, want->touchX, want->touchY);
                break;
            }
            g_inputsChecked++;
        }

    u8 *buf = g_mem + isize;
    u32 size = flashbackBuildClipIndex(&plan, dataEnd + isize, dataEnd, buf);
    if (isize + size > flashbackClipTailMax(g_frames) || isize + size > g_audioStageOffset)
        FAIL("the clip's inputs and index (%u bytes) are past the room for them\n", isize + size);
    u32 es = g_audio ? 16 : 12;
    if (size != plan.slots * es + (g_audio ? 32 : 0) + 48) FAIL("index size\n");
    const u32 *t = (const u32 *)(buf + size - 48);
    if (t[0] != 0x4B424C46 || t[1] != (g_audio ? 3u : 1u) || t[7] != plan.slots || t[8] != plan.frames ||
        t[9] != dataEnd + isize || t[10] != dataEnd || t[11] != 0)
        FAIL("trailer\n");
    for (u32 i = 0; i < plan.slots; i++)
    {
        u32 copies, idx = flashbackPlanSlot(&plan, i, &copies);
        const u32 *e = (const u32 *)(buf + i * es);
        u32 wantOff = g_audio ? RING_VIDEO_BASE + idx * g_slotStride : idx;
        if (e[0] != wantOff || e[1] != g_slotLen[idx] || e[2] != copies) FAIL("index entry %u\n", i);
        /* (saved positions count from 'base', a whole number of rings back) */
        u32 f = g_modelSlotFrame[idx];
        if (g_audio && e[3] != (f > base ? (f - base) * 160 : 0) + flashbackAudioSamplesFor(i == 0 ? plan.trimFirst : 0))
            FAIL("index entry %u audio position\n", i);
    }
    if (g_audio)
    {
        const u32 *a = (const u32 *)(buf + plan.slots * 16);
        if (a[0] != FLASHBACK_AUDIO_RATE_MHZ || a[1] != (2 | 16 << 16) || a[2] != 0 ||
            a[3] != g_audioCapFrames * 160 || a[4] != (lo - base) * 160 || a[5] != (g_audioFlushed - base) * 160)
            FAIL("audio info: cap %u first %u end %u (want %u %u %u)\n", a[3], a[4], a[5],
                 g_audioCapFrames * 160, (lo - base) * 160, (g_audioFlushed - base) * 160);
        /* the saved positions are small, and still name the same places in the ring */
        if (a[5] > 3 * a[3] || (a[4] / 160) % g_audioCapFrames != lo % g_audioCapFrames)
            FAIL("audio info: first %u end %u don't map onto the ring (cap %u, oldest frame %u)\n", a[4], a[5], a[3], lo);
        /* every entry's audio is inside what the ring holds (it was captured in the last ~clip length) */
        const u32 *e0 = (const u32 *)buf;
        if (plan.frames >= g_frames && e0[3] < a[4]) FAIL("the clip's first frame's audio is gone\n");
    }
}

static void startRing(void)
{
    g_epoch++;
    memset(g_ring.data, 0xEE, g_ring.size);
    g_head = 0; g_count = 0;
    memset(g_repeat, 0, g_frames * sizeof(*g_repeat));
    flashbackAudioReset();
    flashbackInputReset();
    g_mInputCount = 0;
    modelLook(true);
    g_writeFailed = false;
    /* every other ring: counts start just before 2^32 samples (~36.4 hours in),
       so the run crosses it */
    g_kBase = rand() % 2 ? ((0xFFFFFFFFu / 160 - (u32)(rand() % 40000)) & ~(FLASHBACK_AUDIO_CHUNK_FRAMES - 1u)) : 0;
    g_audioProduced = g_audioConsumed = g_audioFlushed = g_kBase;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    srand(424242);
    g_audioModel = calloc(4u << 20, 4);
    g_writerRunning = true;     /* producers only stage; the test plays the writer */
    g_audioRunning = false;     /* no audio thread to wait for */

    for (int run = 0; run < 400; run++)
    {
        runs++;
        bool audio = rand() % 5 != 0;
        /* clip lengths: mostly up to 2 minutes (a long one takes a while to
           check), every 20th run any length, and the shortest and the longest
           every 50th */
        u32 seconds = run % 50 == 24 ? FLASHBACK_MIN_SECONDS : run % 50 == 49 ? FLASHBACK_MAX_SECONDS
                    : run % 20 == 19 ? FLASHBACK_MIN_SECONDS + rand() % (FLASHBACK_MAX_SECONDS - FLASHBACK_MIN_SECONDS + 1)
                    : FLASHBACK_MIN_SECONDS + rand() % 120;
        flashbackApplyFormat(rand() % 2, rand() % 3, seconds, audio);
        /* tiny frames, so the video ring stays small (the audio ring is real size) */
        g_w = TINY_W; g_h = TINY_H;
        g_slotBytes = TINY_W * TINY_H * 2; g_slotStride = 2048;
        g_stageSlots = FLASHBACK_STAGE_SLOTS_MIN << (rand() % 6);   /* (up to 128) */                                          /* 4, 8 or 16 */
        g_capBufs = 1u << (rand() % 3);                                                                     /* 1, 2 or 4 */
        g_audioStageFrames = rand() % 2 ? FLASHBACK_AUDIO_STAGE_FRAMES : FLASHBACK_AUDIO_STAGE_FRAMES_MIN;   /* (both sizes) */
        flashbackLayout();

        flashbackUseMem(calloc(1, g_memSize));
        g_active = true;
        g_ring.size = RING_VIDEO_BASE + (u64)g_frames * g_slotStride;
        g_ring.data = malloc(g_ring.size);
        startRing();
        g_failPct = run % 9 == 8 ? 3 : 0;
        g_reentrantPct = rand() % 90;

        /* about 0.5 to 3 times the clip length */
        int steps = (int)(g_frames * (1 + rand() % 6) / 2) * 8;
        for (int s = 0; s < steps; s++)
        {
            g_budget = 12;
            int r = rand() % 1000;
            if (r < 400) producerTick();
            else if (r < 430) { for (int k = 0; k < 40; k++) producerTick(); }    /* the writer falls behind */
            else if (r < 997) flashbackWritePending();
            else if (r < 998) { drain(); check(); saves++; startRing(); }          /* a save */
            else { drain(); check(); failedSaves++; }                              /* a save that failed: carry on */
            if (g_writeFailed)
            {
                drain(); check(); failStops++;  /* the recorder stops; the next start is a fresh ring */
                startRing();
            }
        }
        drain();
        check();
        free(g_mem);
        free(g_ring.data);
    }

    printf("%ld runs, %ld checks (%ld saves, %ld failed saves, %ld write-failure stops)\n",
           runs, checks, saves, failedSaves, failStops);
    printf("audio frames checked %ld, video slots checked %ld, appends refused (staging full) %ld\n",
           audioChecked, videoChecked, stagingFull);
    printf("inputs: records checked %ld, times HID took more readings than it holds between looks %ld\n",
           g_inputsChecked, g_inputOverruns);
    printf("SD writes %ld, not sector aligned %ld\n", g_writes, g_unaligned);
    if (g_unaligned) errors++;
    printf("queue: frames left waiting after a capture %ld, deepest %ld; refused with it full: %ld (card behind too), %ld (not)\n", g_queuedAfterCapture, g_queueDepthMax, g_fullCard, g_fullCpu);
    printf("staging: most frames waiting %ld (of %d entries), times more than the slots' worth %ld, wraps %ld\n", g_stagedMax, FLASHBACK_STAGE_ENTRIES, g_stagedOverSlots, g_wraps);
    printf("%s (%ld errors)\n", errors ? "FAILED" : "all good", errors);
    return errors != 0;
}
