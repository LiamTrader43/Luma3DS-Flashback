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
     the middle and recording on afterwards.
   Then the saved clips are parsed like a reader would and checked. */
#include "stubs_e2e.h"
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

static bool flashbackCapture(u16 *dst)
{
    u32 id = g_nextFrameId++;
    if (g_beeps)
    {
        /* white while the DSP is beeping, black otherwise (compresses well) */
        u16 v = g_dspFrames % 205 < 4 ? 0xFFFF : 0x0000;
        for (u32 i = 0; i < g_w * g_h; i++) dst[i] = v;
    }
    else
        for (u32 i = 0; i < g_w * g_h; i++) dst[i] = (u16)rand();
    /* the id in the upright frame's first two pixels: in the screen's order the
       capture hands over (column by column, bottom row up), the top row is the
       last pixel of each column */
    dst[g_h - 1] = (u16)id;
    dst[2 * g_h - 1] = (u16)(id >> 16);
    g_frameDsp[id] = g_dspFrames;
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
    g_fakeHid[2] = g_fakeHid[0];    /* (when the one before was taken, and this one) */
    g_fakeHid[3] = g_fakeHid[1];
    g_fakeHid[0] = r;
    g_fakeHid[1] = 0;
    g_fakeHid[4] = i;
}


static void record(double seconds, const Events *ev)
{
    u64 period = SYSCLOCK_ARM11 / g_fps, start = svcGetSystemTick(), next = start;
    bool held12 = false, held30 = false, didReset = false, didStall = false, wasRecording = false;
    g_recordStart = start;
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
    u32 dataEnd = RING_VIDEO_BASE + (g_count < g_frames ? g_count : g_frames) * g_slotStride;
    u8 *tail = malloc(128 * 1024);
    u32 inputSize = flashbackBuildClipInputs(&plan, tail);
    u32 tailSize = inputSize + flashbackBuildClipIndex(&plan, dataEnd + inputSize, dataEnd, tail + inputSize);
    u8 *clip = malloc(dataEnd + tailSize);
    memcpy(clip, g_ring.data, dataEnd);
    memcpy(clip + dataEnd, tail, tailSize);
    free(tail);
    *size = dataEnd + tailSize;
    g_head = 0;
    g_count = 0;
    memset(g_repeat, 0, sizeof(g_repeat));
    flashbackAudioReset();
    flashbackInputReset();
    return clip;
}

static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static int magic_check(const u8 *t) { return rd32(t) != 0x4B424C46; }

/* Reads a saved clip as a reader would and checks it. */
static void check(const char *name, const u8 *clip, u32 size, int expectJumps)
{
    const u8 *t = clip + size - 48;
    u32 version = rd32(t + 4), fps = rd32(t + 16), stride = rd32(t + 20), slotBytes = rd32(t + 24);
    u32 entries = rd32(t + 28), frames = rd32(t + 32), indexOffset = rd32(t + 36);
    const u8 *ai = clip + indexOffset + entries * 16;
    u32 rateMhz = rd32(ai), aoff = rd32(ai + 8), cap = rd32(ai + 12), first = rd32(ai + 16), end = rd32(ai + 20), missed = rd32(ai + 24);
    printf("%s: version %u, %u frames (%u stored), stride %u, audio: rate %u mHz, ring %u samples, held %u..%u, missed %u\n",
           name, version, frames, entries, stride, rateMhz, cap, first, end, missed);
    /* (positions count from a whole number of rings back: 'first' is 0 only if the ring holds everything since it started) */
    if (end - first != (first == 0 ? end : cap - FLASHBACK_AUDIO_CHUNK_FRAMES * 160) || end > 3 * cap) FAIL("held range\n");
    if (version != 3 || magic_check(t) || indexOffset + entries * 16 + 32 + 48 != size) FAIL("layout\n");

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

    /* video: ids in order; audio at each clip frame lines up with the DSP at capture time */
    u32 lastId = 0, f = 0, silentFrames = 0, maxOff = 0;
    double spf = (double)SYSCLOCK_ARM11 / (fps * 8192.0);
    for (u32 e = 0; e < entries; e++)
    {
        const u8 *en = clip + indexOffset + e * 16;
        u32 off = rd32(en), sz = rd32(en + 4), copies = rd32(en + 8), apos = rd32(en + 12);
        if (sz != slotBytes || off + sz > indexOffset) FAIL("entry %u\n", e);
        const u16 *px = (const u16 *)(clip + off);
        u32 id = px[0] | (u32)px[1] << 16;
        if (e && id <= lastId) FAIL("video ids out of order\n");
        lastId = id;
        for (u32 c = 0; c < copies; c++, f++)
        {
            u32 pos = apos + (u32)(c * spf);
            if (pos < first || pos >= end) { FAIL("frame %u's audio %u isn't in the ring (end %u)\n", f, pos, end); continue; }
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
        u32 start = rd32(clip + indexOffset + 12), fr = 0;
        double maxDev = 0, endDev = 0;
        for (u32 e = 0; e < entries; e++)
        {
            const u8 *en = clip + indexOffset + e * 16;
            double dev = ((double)rd32(en + 12) - (start + fr * spf)) / 32.7285;
            if (fabs(dev) > maxDev) maxDev = fabs(dev);
            endDev = dev;
            fr += rd32(en + 8);
        }
        g_lastMaxDevMs = maxDev;
        g_lastEndDevMs = endDev;
        printf("  straight-through audio vs each frame: max %.1f ms, at the end %+.1f ms\n", maxDev, endDev);
    }
    if (maxOff > 4) FAIL("audio and video out of step\n");

    /* the clip's audio, start to end: in order, gaps only where expected */
    u32 start = rd32(clip + indexOffset + 12), len = (u32)(frames * spf);
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
    for (u32 i = 0; i < FLASHBACK_LENGTH_CHOICES; i++)
    {
        u32 secs = g_lengthChoices[i], frames = flashbackAudioFramesFor(secs);
        double need = (secs + FLASHBACK_AUDIO_EXTRA_SECONDS) * 32728.498 / 160;
        printf("%2u s: audio ring %u frames (%.2f s)\n", secs, frames, frames * 160 / 32728.498);
        if (frames < need || frames > need + FLASHBACK_AUDIO_CHUNK_FRAMES + 1) FAIL("audio ring size for %u s\n", secs);
    }

    flashbackApplyFormat(FLASHBACK_RES_HALF, 2, 0, true);     /* 200x120, 30 fps, 10 s, audio */
    printf("ring: audio %u frames (%u bytes), video %u slots of %u; memory %u bytes\n",
           g_audioCapFrames, g_audioBytes, g_frames, g_slotStride, g_memSize);
    g_mem = calloc(1, g_memSize);
    g_ring.size = RING_VIDEO_BASE + (u64)g_frames * g_slotStride;
    g_ring.data = calloc(1, g_ring.size);
    g_head = 0; g_count = 0; flashbackAudioReset();
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

    /* 4: beeps with white frames, for checking A/V sync after conversion */
    g_beeps = true;
    record(12.0, NULL);
    u32 size4;
    u8 *clip4 = saveClip(&size4);
    g_beeps = false;

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
    }
    printf("wrote out/clip_test1..4.flb (clip 4: a beep and white frames once a second)\n");

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
