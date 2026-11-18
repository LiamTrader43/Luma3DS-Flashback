/* Deterministic fuzz test of Flashback's recording, with the recorder's own
   code (recorder_extracted.c, made by extract.py): the capture queue, packed
   staging (every size it can have), audio staging, the writer (audio chunks
   and video frames), unfinished-chunk flushes for saves, saves that reset the
   ring and saves that fail and carry on, write failures, staging running
   full, every clip length and frame rate, audio on and off, one screen and
   both (the top one first, or the bottom one), and the inputs' records (HID
   taking readings between grid slots and
   during them, more than it holds at times, and the recorder waking up late),
   and frames packed as they're captured or by a packer that lags behind.

   The screens change the way real ones do: often not at all, often a little
   (a few pixels, a box), sometimes altogether, and the bottom screen now and
   then can't be captured. So frames go unstored for being the same, are stored
   as what changed, whole, packed with Huffman codes or as they are, and the
   bottom screen's sometimes find no room in their slot: then they aren't
   stored, or when a whole one is due, get a slot of their own.

   The producers (audio frames, video frames) run in the middle of the
   writer's SD writes, and the "card" only takes the data at the end of each
   write, so a producer touching data that's being written would show up.

   After every drain it checks, exactly: the audio ring holds every audio frame
   it should (by content); the ring's slots, read in order the way a reader
   reads a clip (whole frames, then what changed, on each screen), show the
   right picture on both screens at every slot, for as many grid slots as they
   should, with the right audio position; and the clip's inputs, index and
   audio info a save would write, and that the clip's own slots, read from its
   first, show the same. */
#include "stubs_fuzz.h"
/* Staged frames start on 512-byte boundaries here (a page on the 3DS): with the
   tiny frames below, that packs 1 to 4 to a slot, as real frames do to a page
   multiple, and the frames waiting can outnumber the slots' worth of staging. */
#define FLASHBACK_STAGE_ALIGN 512
/* What became of each frame packed (see flashbackPackFrame): the model's way in */
static void modelPackedFrame(u32 priLen, u32 secLen, u32 ownLen, bool secKept, bool secTried);
#define FLASHBACK_PACKED_HOOK(priLen, secLen, ownLen, secKept, secTried) modelPackedFrame(priLen, secLen, ownLen, secKept, secTried)
#include "recorder_extracted.c"
#include "asm_standin.h"
#ifndef RING_VIDEO_BASE
#define RING_VIDEO_BASE g_videoOffset  /* where the ring's video slots start */
#endif
#define FLASHBACK_MAX_FRAMES (FLASHBACK_MAX_SECONDS * 30)  /* the longest clip at the highest frame rate */
#define MAX_SLOTS (FLASHBACK_MAX_FRAMES + FLASHBACK_LEAD_MAX + FLASHBACK_MAX_FRAMES / (FLASHBACK_KEY_SLOTS - 1) + 4)

/* Flashback Studio's decoder (a copy of 3ds-app/source/flb.c's: decode.c), linked in */
bool testDecodeFrame(const uint8_t *src, uint32_t len, uint16_t *dst, uint32_t w, uint32_t h, bool delta, bool coded);

#ifndef FUZZ_RUNS
#define FUZZ_RUNS 400   /* (fewer for a quick look: -DFUZZ_RUNS=40) */
#endif

#define TINY_W 32
#define TINY_H 32
#define TINY_BOT_W 24   /* the bottom screen: narrower, as on the 3DS */
#define TINY_BOT_H 32

static long errors, checks, audioChecked, videoChecked, saves, failedSaves, failStops, stagingFull, runs;
/* what the slots checked held, and what became of the frames packed */
static long slotsCoded, slotsDelta, slotsRaw, slotsTopOnly, slotsBotOnly, slotsBoth, clipsWithLead, leadSlots;
static long framesPacked, framesSame, framesBotLost, framesBotSame, framesBotStored, framesBotMissing, copiesChecked, missedChecked;
static long framesBotOwn, framesBotLostStale, framesBotWasted, framesTopKey, framesBotKey, slotsUnshown, botLate, clipsChecked;
static long framesBottomFirst, slotsUnshownTop, framesBehind, framesSecYielded, framesBotLostRoom;
#define FAIL(...) do { errors++; if (errors < 20) printf("  ERROR: " __VA_ARGS__); } while (0)

static u32 g_epoch;
static u32 *g_audioModel;           /* tag of each appended audio frame since g_kBase; 0 = silence */
static u32 g_kBase;                 /* audio frame count the ring started at (high: as if ~36 hours in) */
static int g_inProducer, g_budget, g_reentrantPct;

static u32 tagFor(u32 k) { return (g_epoch << 22) | (k & 0x3FFFFF) | 0x80000000u; }

/* ---- the screens, and what a reader of the ring should see on them */

/* A picture's fingerprint (never 0: that's "no picture"). */
static u64 hashOf(const u16 *px, u32 n)
{
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < n; i++)
        h = (h ^ px[i]) * 1099511628211ull;
    return h | 1;
}

static u16 g_scrTop[TINY_W * TINY_H], g_scrBot[TINY_BOT_W * TINY_BOT_H];   /* the screens now, upright */
static int g_topStillPct, g_botStillPct;    /* how often each stays as it is from one capture to the next */
static bool g_stillNext;                    /* the next capture finds both as they are (see check) */

/* The screen changes: a few pixels, a box, a scroll, or a new picture (noise,
   two colors dithered, flat, a ramp). */
static void changeScreen(u16 *scr, u32 w, u32 h)
{
    u32 n = w * h;
    switch (rand() % 10)
    {
    case 0: case 1: case 2:     /* a few pixels */
        for (int k = 1 + rand() % 6; k > 0; k--)
            scr[rand() % n] = (u16)rand();
        break;
    case 3: case 4:             /* a box, flat or noise */
    {
        u32 bw = 1 + rand() % w, bh = 1 + rand() % h, x0 = rand() % (w - bw + 1), y0 = rand() % (h - bh + 1);
        bool flat = rand() % 2;
        u16 color = (u16)rand();
        for (u32 y = y0; y < y0 + bh; y++)
            for (u32 x = x0; x < x0 + bw; x++)
                scr[y * w + x] = flat ? color : (u16)rand();
        break;
    }
    case 5:                     /* a scroll */
        memmove(scr, scr + w, (n - w) * 2);
        break;
    case 6:                     /* noise: stored as it is */
        for (u32 i = 0; i < n; i++) scr[i] = (u16)rand();
        break;
    case 7:                     /* two colors dithered: about a byte a pixel, nearly all two INDEX ops (packed with codes) */
        for (u32 i = 0; i < n; i++) scr[i] = (u16)(i % 2 ? 0x1234 : 0x4321);
        scr[rand() % n] = (u16)rand();
        break;
    case 8:                     /* flat */
    {
        u16 color = (u16)rand();
        for (u32 i = 0; i < n; i++) scr[i] = color;
        break;
    }
    case 9:                     /* a ramp, with some noise */
    {
        u16 base = (u16)rand();
        for (u32 i = 0; i < n; i++) scr[i] = i % 5 ? (u16)(base + i / 9) : (u16)rand();
        break;
    }
    }
}

/* captured frames waiting to be packed, oldest first: the first screen's
   picture and the second screen's, if that was captured (which screen is which:
   g_priBottom), where the audio was, and the grid slots missed just before it */
typedef struct { u64 pri, sec; bool secOk; u32 frame, missed; } ModelFrame;
static ModelFrame g_modelQ[64];
static u32 g_modelQHead, g_modelQTail;
static long g_captures;

/* A capture, handed over in the screen's order as the real one does: column by
   column, bottom row up. */
static void toColumns(const u16 *up, u32 w, u32 h, u16 *cols)
{
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
            cols[x * h + (h - 1 - y)] = up[y * w + x];
}

static bool flashbackCapture(u16 *first, u16 *second, bool *secondOk)
{
    bool still = g_stillNext;
    g_stillNext = false;
    if (!still && rand() % 100 >= g_topStillPct)
        changeScreen(g_scrTop, TINY_W, TINY_H);
    if (g_botBytes != 0 && !still && rand() % 100 >= g_botStillPct)
        changeScreen(g_scrBot, TINY_BOT_W, TINY_BOT_H);
    const u16 *priScr = g_priBottom ? g_scrBot : g_scrTop, *secScr = g_priBottom ? g_scrTop : g_scrBot;
    toColumns(priScr, g_priW, g_priH, first);

    ModelFrame f = { hashOf(priScr, g_priW * g_priH), 0, false, g_audioProduced, g_pendingMissed };
    if (second != NULL)
    {
        /* (often it isn't captured, several frames in a row at times, and now
           and then for a second or more: its buffer is left as it is) */
        static int skipping;
        if (skipping == 0 && rand() % 12 == 0) skipping = rand() % 30 == 0 ? 25 + rand() % 20 : 1 + rand() % 6;
        f.secOk = *secondOk = still || skipping == 0;
        if (skipping > 0) skipping--;
        if (f.secOk)
        {
            toColumns(secScr, g_secW, g_secH, second);
            f.sec = hashOf(secScr, g_secW * g_secH);
        }
    }
    g_modelQ[g_modelQTail++ % 64] = f;
    g_captures++;
    return true;
}

/* The ring as it should be: each slot's pictures (what a reader has on each
   screen once it has read it; 0: none), its audio position, and the grid slots
   it's shown for after its own, and how many of those were missed captures. */
static u64 g_mSlotTop[MAX_SLOTS], g_mSlotBot[MAX_SLOTS];
static u32 g_mSlotFrame[MAX_SLOTS], g_mRepeat[MAX_SLOTS], g_mMissed[MAX_SLOTS];
static bool g_mUnshown[MAX_SLOTS];  /* the slot is only read: shown for no grid slot */
static u32 g_mHead, g_mCount;
static u64 g_mTop, g_mBot;      /* what a reader has now */
static bool g_mSecDue;          /* the second screen's next frame is to be stored whole */
static bool g_mSecLostLast;     /* its last frame captured wasn't stored */
static u32 g_mStageHead;        /* where the recorder's next staged frame could go, as the last frame packed left it */

static bool packingSecOk(void) { return g_modelQ[g_modelQHead % 64].secOk; }   /* (of the frame being packed) */

/* a new slot in the ring as it should be, showing what a reader has now */
static u32 modelSlot(u32 frame, bool unshown)
{
    u32 r = g_mHead;
    g_mSlotTop[r] = g_mTop;
    g_mSlotBot[r] = g_mBot;
    g_mSlotFrame[r] = frame;
    g_mRepeat[r] = g_mMissed[r] = 0;
    g_mUnshown[r] = unshown;
    g_mHead = (g_mHead + 1) % g_ringSlots;
    if (g_mCount < g_ringSlots) g_mCount++;
    return r;
}

static void modelFillMissed(u32 missed)
{
    if (g_mCount == 0 || missed == 0) return;
    u32 last = (g_mHead + g_ringSlots - 1) % g_ringSlots;
    g_mRepeat[last] = g_mRepeat[last] + missed > 0xFFFF ? 0xFFFF : g_mRepeat[last] + missed;
    g_mMissed[last] = g_mMissed[last] + missed > 0xFFFF ? 0xFFFF : g_mMissed[last] + missed;
}

/* The recorder packed its oldest captured frame: it stored 'priLen' bytes for
   the first screen and 'secLen' for the second one in a slot (0: nothing), or
   'ownLen' for the second one in a slot of its own before that, and says a
   reader has the second screen's frame ('secKept': stored, or the same as
   before), and whether it compressed it ('secTried': for nothing, if it isn't
   stored). Nothing may go unstored unless it's what a reader already has, and
   the first screen's frame is always stored if it changed.

   Whole frames are due with every FLASHBACK_KEY_SLOTS-th slot, and the ring's
   first: the first screen's then, the second screen's with its next frame
   captured, which is always stored (in a slot of its own if need be, the one
   before the first screen's: the first screen's frame is whole there too, if
   that takes it to a FLASHBACK_KEY_SLOTS-th slot).

   While packing was falling behind as the frame was packed (the next frame
   already captured, or staging over half full), the second screen's frame
   isn't even compressed, unless its whole frame is due. Otherwise, a changed
   one is stored if there's room for it, which there always is with slots
   twice the first screen's frame.

   In a slot, the top screen's frame comes first, whichever screen is first. */
static void modelPackedFrame(u32 priLen, u32 secLen, u32 ownLen, bool secKept, bool secTried)
{
    if (g_modelQHead == g_modelQTail) { FAIL("packed a frame that wasn't captured\n"); return; }
    /* (how far behind packing was: the frames captured and not packed, this one included; and how much of
       staging was taken, from the oldest staged frame, before this one's slots were staged) */
    u32 queued = g_modelQTail - g_modelQHead;
    u32 slotsNow = priLen == 0 && secLen == 0 ? 0 : ownLen != 0 ? 2 : 1;
    u32 stagedBefore = g_stageProduced - slotsNow - g_stageConsumed, used = 0;
    if (stagedBefore != 0)
    {
        u32 tail = g_stageOff[g_stageConsumed % FLASHBACK_STAGE_ENTRIES];
        used = g_mStageHead > tail ? g_mStageHead - tail : g_stageBytes - tail + g_mStageHead;
    }
    bool behind = queued >= 2 || used > g_stageBytes / 2;
    g_mStageHead = g_stageHead;
    framesBehind += behind;
    if (secTried ? !packingSecOk() : (secLen != 0 || ownLen != 0))
        FAIL("the second screen's frame: %u + %u bytes, %s\n", secLen, ownLen, secTried ? "compressed with none captured" : "not compressed");
    framesBotWasted += secTried && secLen == 0 && ownLen == 0;
    ModelFrame f = g_modelQ[g_modelQHead++ % 64];
    framesPacked++;
    modelFillMissed(f.missed);

    /* which screen is which: what a reader has on each, and their bits in a slot's flags */
    u64 *mPri = g_priBottom ? &g_mBot : &g_mTop, *mSec = g_priBottom ? &g_mTop : &g_mBot;
    u32 priDelta = g_priBottom ? FLASHBACK_FRAME_BOTTOM_DELTA : FLASHBACK_FRAME_TOP_DELTA;
    u32 secDelta = g_priBottom ? FLASHBACK_FRAME_TOP_DELTA : FLASHBACK_FRAME_BOTTOM_DELTA;
    u32 secCodes = g_priBottom ? FLASHBACK_FRAME_TOP_CODES : FLASHBACK_FRAME_BOTTOM_CODES;

    bool key = g_mCount == 0 || g_mHead % FLASHBACK_KEY_SLOTS == 0 ||
               (f.secOk && g_mSecDue && (g_mHead + 1) % g_ringSlots % FLASHBACK_KEY_SLOTS == 0);
    if (key) g_mSecDue = true;
    bool secDue = f.secOk && g_mSecDue;
    bool secChanged = f.secOk && f.sec != *mSec;    /* (against what a reader has) */
    if (behind && !secDue && (secTried || secLen != 0 || ownLen != 0))
        FAIL("the second screen's frame %s while packing was falling behind (%u waiting, %u of %u bytes of staging taken)\n",
             secLen != 0 || ownLen != 0 ? "stored" : "compressed", queued, used, g_stageBytes);
    if (!behind && !secDue && secChanged && g_slotStride >= 2 * g_slotBytes && secLen == 0 && ownLen == 0)
        FAIL("the second screen's frame not stored, with room for it and packing keeping up\n");
    framesSecYielded += behind && !secDue && secChanged;
    u32 newest = (g_head + g_ringSlots - 1) % g_ringSlots;      /* (the recorder's slot for this frame, if it has one) */
    if (key)
    {
        framesTopKey++;
        if (priLen == 0 || (g_slotFlags[newest] & priDelta)) FAIL("the first screen's whole frame was due: %u bytes, flags %x\n", priLen, g_slotFlags[newest]);
    }

    if (ownLen != 0)
    {
        /* the second screen's whole frame in a slot of its own, before the first screen's */
        if (!secDue) FAIL("a slot for the second screen's frame alone, with no whole frame due\n");
        if (priLen == 0 || secLen != 0) FAIL("a slot for the second screen's frame alone, beside %u and %u bytes\n", priLen, secLen);
        *mSec = f.sec;
        u32 r = modelSlot(f.frame, true);
        if ((g_head + g_ringSlots - 2) % g_ringSlots != r) FAIL("the second screen's own slot is %u, not %u\n", (g_head + g_ringSlots - 2) % g_ringSlots, r);
        if (g_slotLen[r] != (g_priBottom ? ownLen : 0) || g_slotBotLen[r] != (g_priBottom ? 0 : ownLen) ||
            (g_slotFlags[r] & ~secCodes) != FLASHBACK_FRAME_UNSHOWN)
            FAIL("the second screen's own slot %u: %u + %u bytes, flags %x\n", r, g_slotLen[r], g_slotBotLen[r], g_slotFlags[r]);
        framesBotOwn++;
    }

    if (priLen != 0) *mPri = f.pri;
    else if (f.pri != *mPri) FAIL("the first screen changed, but nothing was stored for it\n");
    if (secLen != 0 || ownLen != 0)
    {
        if (!f.secOk) FAIL("a second screen's frame stored that wasn't captured\n");
        if (!secKept) FAIL("a second screen's frame stored, but said not to be kept\n");
        *mSec = f.sec;
        framesBotStored++;
        if (secDue)
        {
            if (secLen != 0 && (g_slotFlags[newest] & secDelta)) FAIL("the second screen's whole frame was due: stored as what changed\n");
            framesBotKey++;
            g_mSecDue = false;
        }
        g_mSecLostLast = false;
    }
    else if (secKept)
    {
        if (!f.secOk || f.sec != *mSec) FAIL("the second screen changed, but it's said to be as it was\n");
        if (secDue) FAIL("the second screen's whole frame was due: not stored for being the same\n");
        framesBotSame++;
        g_mSecLostLast = false;
    }
    else if (f.secOk)
    {
        if (secDue) FAIL("the second screen's whole frame was due: not stored\n");
        /* (not stored: a reader keeps the one before, and the next is stored against that one) */
        framesBotLost++;
        framesBotLostRoom += !behind;   /* (for lack of room, not time) */
        framesBotLostStale += g_mSecLostLast;
        g_mSecLostLast = true;
    }
    else if (g_secBytes != 0) framesBotMissing++;
    if (secKept && g_secBytes == 0) FAIL("a second screen's frame kept, with one screen\n");

    if (priLen == 0 && secLen == 0)
    {
        /* nothing new: the newest slot once more */
        u32 last = (g_mHead + g_ringSlots - 1) % g_ringSlots;
        if (g_mCount == 0) { FAIL("nothing stored for the ring's first frame\n"); return; }
        if (g_mRepeat[last] >= 0xFFFF) FAIL("a slot shown more times than it can count\n");
        g_mRepeat[last]++;
        framesSame++;
        return;
    }
    u32 r = modelSlot(f.frame, false);
    if (newest != r) FAIL("the frame went to slot %u, not %u\n", newest, r);
    if (g_slotFlags[r] & FLASHBACK_FRAME_UNSHOWN) FAIL("slot %u, with the first screen's frame, isn't shown\n", r);
    if (g_slotLen[r] != (g_priBottom ? secLen : priLen) || g_slotBotLen[r] != (g_priBottom ? priLen : secLen))
        FAIL("slot %u holds %u + %u bytes; the first screen's frame is %u, the second's %u\n", r, g_slotLen[r], g_slotBotLen[r], priLen, secLen);
    framesBottomFirst += g_priBottom;
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

    long captures = g_captures;
    if (flashbackCaptureFrame())
    {
        if (g_captures != captures + 1) FAIL("capture not taken\n");
    }
    else
    {
        if (g_captures != captures) FAIL("a capture taken, and refused\n");
        if (g_capProduced - g_capConsumed >= flashbackCaptureQueue())
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
    stageStats();
    u32 depth = g_capProduced - g_capConsumed;
    if (depth) g_queuedAfterCapture++;
    if ((long)depth > g_queueDepthMax) g_queueDepthMax = depth;
    if (depth > flashbackCaptureQueue()) FAIL("%u frames waiting, with %u buffers\n", depth, g_capBufs);
}

/* With a packer thread (g_packRunning, a third of the runs), captured frames are
   packed now and then, this often (% of producer ticks): a processor that keeps
   up, or lags behind and lets frames wait. Otherwise they're packed as they're
   captured. */
static int g_packPct;

static void producerTick(void)
{
    g_inProducer = 1;
    if (g_packRunning && g_capProduced != g_capConsumed && rand() % 100 < g_packPct)
        flashbackPackFrame();
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
    bool w = g_writerRunning, p = g_packRunning;
    g_writerRunning = false;    /* (inline: there's no writer thread here) */
    g_packRunning = false;      /* (nor a packer thread) */
    while (g_stageProduced != g_stageConsumed)   /* (the writer's part: staging may be full, and packing needs a slot) */
        flashbackWritePending();
    u32 pending = g_pendingMissed;
    flashbackDrainCaptures();   /* (as flashbackSave: every captured frame packed first) */
    modelFillMissed(pending);   /* (the grid slots missed since the last capture: copies too) */
    if (g_capProduced != g_capConsumed || g_modelQHead != g_modelQTail) FAIL("captures left unpacked\n");
    flashbackAudioPause();
    flashbackDrainWrites();
    g_writerRunning = w;
    g_packRunning = p;
}

/* Reads 'n' ring slots from slot 'start' on, as a reader reads a clip's entries:
   a screen has no picture until its first whole frame, and a frame stored as
   what changed goes over the picture it has. After each slot, both screens must
   show what they should; in a clip, the first screen (g_priBottom) has its
   picture by the first slot that's shown, from the 'shownFrom'-th on
   (UINT32_MAX: not a clip). Returns whether the second screen has its picture
   by then too (by the first slot, if not a clip). */
static bool readSlots(u32 start, u32 n, u32 shownFrom)
{
    static u16 top[TINY_W * TINY_H], bot[TINY_BOT_W * TINY_BOT_H];
    bool topKnown = false, botKnown = false, shown = false, secByThen = false;
    for (u32 i = 0; i < n; i++)
    {
        u32 r = (start + i) % g_ringSlots;
        const u8 *stored = g_ring.data + RING_VIDEO_BASE + (u64)r * g_slotStride;
        u32 tl = g_slotLen[r], bl = g_slotBotLen[r], fl = g_slotFlags[r];
        bool unshown = fl & FLASHBACK_FRAME_UNSHOWN;
        fl &= ~(u32)FLASHBACK_FRAME_UNSHOWN;
        if (unshown != g_mUnshown[r]) FAIL("slot %u: %s\n", r, unshown ? "not shown, and should be" : "shown, and shouldn't be");
        /* (a slot that isn't shown holds the second screen's whole frame, alone) */
        if (unshown && (g_priBottom ? tl == 0 || bl != 0 || (fl & FLASHBACK_FRAME_TOP_DELTA)
                                    : tl != 0 || bl == 0 || (fl & FLASHBACK_FRAME_BOTTOM_DELTA)))
            FAIL("slot %u isn't shown, with %u + %u bytes, flags %x\n", r, tl, bl, fl);
        slotsUnshown += unshown;
        slotsUnshownTop += unshown && tl != 0;
        if (tl == 0 && bl == 0) FAIL("slot %u holds nothing\n", r);
        if (bl != 0 && g_botBytes == 0) FAIL("slot %u has a bottom screen's frame\n", r);
        if (((tl + 3) & ~3u) + bl > g_slotStride || tl > g_slotBytes || bl > g_botBytes) FAIL("slot %u: %u + %u bytes\n", r, tl, bl);
        if (!isN3DS && (fl & (FLASHBACK_FRAME_TOP_CODES | FLASHBACK_FRAME_BOTTOM_CODES)))
            FAIL("an Old 3DS packed slot %u with Huffman codes\n", r);
        if ((tl == 0 && (fl & 3)) || (bl == 0 && (fl & 12)) || (fl & ~15u)) FAIL("slot %u: flags %x\n", r, fl);

        for (int s = 0; s < 2; s++)
        {
            u32 len = s ? bl : tl, rawBytes = s ? g_botBytes : g_slotBytes, w = s ? g_botW : g_w, h = s ? g_botH : g_h;
            const u8 *at = s ? stored + ((tl + 3) & ~3u) : stored;
            bool delta = fl >> (2 * s) & 1, coded = fl >> (2 * s) & 2, *known = s ? &botKnown : &topKnown;
            u16 *px = s ? bot : top;
            if (len == 0) continue;
            bool ok = true;
            if (len == rawBytes)
            {
                if (delta || coded) FAIL("slot %u: a frame as it is, flagged %x\n", r, fl);
                memcpy(px, at, len);
                *known = true;
                slotsRaw++;
            }
            else if (!delta)
            {
                ok = testDecodeFrame(at, len, px, w, h, false, coded);
                *known = true;
            }
            else if (*known)
                ok = testDecodeFrame(at, len, px, w, h, true, coded);
            if (!ok) FAIL("slot %u: the %s screen's frame doesn't decode\n", r, s ? "bottom" : "top");
            slotsCoded += coded;
            slotsDelta += delta;
        }
        slotsTopOnly += tl != 0 && bl == 0 && g_botBytes != 0;
        slotsBotOnly += tl == 0;
        slotsBoth += tl != 0 && bl != 0;

        if (topKnown && hashOf(top, g_w * g_h) != g_mSlotTop[r]) FAIL("slot %u: the top screen is wrong\n", r);
        if (botKnown && hashOf(bot, g_botW * g_botH) != g_mSlotBot[r]) FAIL("slot %u: the bottom screen is wrong\n", r);
        if (!shown && (shownFrom == UINT32_MAX || (i >= shownFrom && !unshown)))
        {
            shown = true;
            secByThen = g_priBottom ? topKnown : botKnown;
            if (shownFrom != UINT32_MAX && !(g_priBottom ? botKnown : topKnown))
                FAIL("slot %u, %u into the clip, is shown with no whole frame of the first screen's yet\n", r, i);
        }
        videoChecked++;
    }
    return secByThen;
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

    /* video: the ring is what it should be (as many slots, each shown as long,
       with its audio position), and read from its oldest slot shows the right
       pictures (from the first whole frame on) */
    if (g_count != g_mCount || g_head != g_mHead) FAIL("the ring has %u slots, head %u (want %u, %u)\n", g_count, g_head, g_mCount, g_mHead);
    for (u32 i = 0; i < g_count; i++)
    {
        u32 r = (g_head + g_ringSlots - 1 - i) % g_ringSlots;
        if (g_repeat[r] != g_mRepeat[r] || g_missed[r] != g_mMissed[r])
            FAIL("slot %u shown %u more times, %u of them missed (want %u, %u)\n", r, g_repeat[r], g_missed[r], g_mRepeat[r], g_mMissed[r]);
        if (g_missed[r] > g_repeat[r]) FAIL("slot %u: more missed than shown\n", r);
        if (g_mUnshown[r] && g_repeat[r] != 0) FAIL("slot %u isn't shown, %u times\n", r, g_repeat[r]);
        if (g_audio && g_slotAudioFrame[r] != g_mSlotFrame[r])
            FAIL("slot %u audio frame %u, want %u\n", r, g_slotAudioFrame[r], g_mSlotFrame[r]);
    }
    readSlots((g_head + g_ringSlots - g_count) % g_ringSlots, g_count, UINT32_MAX);

    /* what a save would append: the inputs (a record for each of the clip's
       frames: the last ones, by grid slot), then the index */
    if (g_count == 0) return;
    FlashbackClipPlan plan = flashbackPlanClip();
    u32 dataEnd = RING_VIDEO_BASE + (g_count < g_ringSlots ? g_count : g_ringSlots) * g_slotStride;
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
    u32 entries = plan.lead + plan.slots;
    if (isize + size > flashbackClipTailMax(g_frames) || isize + size > g_audioStageOffset)
        FAIL("the clip's inputs and index (%u bytes) are past the room for them\n", isize + size);
    if (size != entries * 24 + (g_audio ? 32 : 0) + 48) FAIL("index size\n");
    const u32 *t = (const u32 *)(buf + size - 48);
    if (t[0] != 0x4B424C46 || t[1] != ((g_audio ? 3u : 1u) | FLASHBACK_CLIP_HUFFMAN | FLASHBACK_CLIP_SCREENS) || t[7] != entries ||
        t[8] != plan.frames || t[9] != dataEnd + isize || t[10] != dataEnd || t[11] != (g_botW | g_botH << 16))
        FAIL("trailer\n");
    if (plan.lead >= FLASHBACK_LEAD_MAX) FAIL("%u slots read before the clip's first\n", plan.lead);
    if (entries > flashbackRingSlots(g_frames, g_botBytes != 0)) FAIL("%u index entries\n", entries);
    if (plan.frames > g_frames || (plan.frames < g_frames && plan.slots != g_count)) FAIL("the clip has %u frames\n", plan.frames);
    u32 frames = 0, missed = 0;
    for (u32 i = 0; i < entries; i++)
    {
        u32 copies, idx = flashbackPlanSlot(&plan, i, &copies);
        const u32 *e = (const u32 *)(buf + i * 24);
        u32 wasMissed = copies == 0 ? 0 : g_mMissed[idx] < copies ? g_mMissed[idx] : copies - 1;
        if ((i < plan.lead || g_mUnshown[idx]) != (copies == 0)) FAIL("index entry %u: %u copies\n", i, copies);
        if (i > plan.lead && !g_mUnshown[idx] && copies != 1 + g_mRepeat[idx]) FAIL("index entry %u: %u copies, want %u\n", i, copies, 1 + g_mRepeat[idx]);
        if (e[0] != RING_VIDEO_BASE + idx * g_slotStride || e[1] != g_slotLen[idx] || e[2] != g_slotBotLen[idx] || e[3] != copies ||
            e[5] != ((g_slotFlags[idx] & 15u) | wasMissed << 16))
            FAIL("index entry %u\n", i);
        /* (saved positions count from 'base', a whole number of rings back) */
        u32 f = g_mSlotFrame[idx];
        u32 wantPos = !g_audio || copies == 0 ? 0
                    : (f > base ? (f - base) * 160 : 0) + flashbackAudioSamplesFor(i == plan.lead ? plan.trimFirst : 0);
        if (e[4] != wantPos) FAIL("index entry %u audio position\n", i);
        frames += copies;
        missed += wasMissed;
    }
    if (frames != plan.frames || missed != plan.missed) FAIL("the index adds up to %u frames, %u missed (plan: %u, %u)\n", frames, missed, plan.frames, plan.missed);
    copiesChecked += frames;
    missedChecked += missed;
    if (g_audio)
    {
        const u32 *a = (const u32 *)(buf + entries * 24);
        if (a[0] != FLASHBACK_AUDIO_RATE_MHZ || a[1] != (2 | 16 << 16) || a[2] != 0 ||
            a[3] != g_audioCapFrames * 160 || a[4] != (lo - base) * 160 || a[5] != (g_audioFlushed - base) * 160)
            FAIL("audio info: cap %u first %u end %u (want %u %u %u)\n", a[3], a[4], a[5],
                 g_audioCapFrames * 160, (lo - base) * 160, (g_audioFlushed - base) * 160);
        /* the saved positions are small, and still name the same places in the ring */
        if (a[5] > 3 * a[3] || (a[4] / 160) % g_audioCapFrames != lo % g_audioCapFrames)
            FAIL("audio info: first %u end %u don't map onto the ring (cap %u, oldest frame %u)\n", a[4], a[5], a[3], lo);
        /* every entry's audio is inside what the ring holds (it was captured in the last ~clip length) */
        const u32 *e0 = (const u32 *)(buf + plan.lead * 24);
        if (plan.frames >= g_frames && e0[4] < a[4]) FAIL("the clip's first frame's audio is gone\n");
    }

    /* ...and the clip's slots, read from its first as a reader does: the right
       pictures all the way, the first screen's from the first slot that's
       shown. The second screen has its picture by then too, if any of the
       slots a clip can start from (the FLASHBACK_LEAD_MAX up to its oldest)
       has its whole frame: it was last stored whole before that only if it
       wasn't captured for a while. */
    u32 first, copies0;
    first = flashbackPlanSlot(&plan, 0, &copies0);
    bool secByThen = readSlots(first, entries, plan.lead);
    clipsChecked++;
    clipsWithLead += plan.lead != 0;
    leadSlots += plan.lead;
    if (g_secBytes != 0 && !secByThen)
    {
        botLate++;
        for (u32 back = 0; back < FLASHBACK_LEAD_MAX && plan.slots + back <= g_count; back++)
        {
            u32 r = (g_head + g_ringSlots - plan.slots - back) % g_ringSlots;
            if (g_priBottom ? g_slotLen[r] != 0 && !(g_slotFlags[r] & FLASHBACK_FRAME_TOP_DELTA)
                            : g_slotBotLen[r] != 0 && !(g_slotFlags[r] & FLASHBACK_FRAME_BOTTOM_DELTA))
            {
                FAIL("the clip starts without the second screen, with its whole frame %u slots before the clip's first\n", back);
                break;
            }
        }
    }

    /* As a save: the index was built over the capture buffers, and the frame
       before with them. So that nothing goes by what they hold from here on,
       every buffer is left holding just what the next capture will find: a
       recorder that still took one for the frame before would find nothing
       changed, and store nothing. */
    changeScreen(g_scrTop, TINY_W, TINY_H);
    if (g_botBytes != 0)
        changeScreen(g_scrBot, TINY_BOT_W, TINY_BOT_H);
    for (u32 b = 0; b < g_capBufs; b++)
    {
        toColumns(g_priBottom ? g_scrBot : g_scrTop, g_priW, g_priH, (u16 *)(g_mem + g_frameOffset + b * g_capStride));
        if (g_secBytes != 0)
            toColumns(g_priBottom ? g_scrTop : g_scrBot, g_secW, g_secH, (u16 *)(g_mem + g_secOffset + b * g_secStride));
    }
    g_stillNext = true;
    flashbackForgetFrames();
    g_mSecDue = true;       /* (its next frame whole, then: a reader's is no longer in a buffer) */
}

static void startRing(void)
{
    g_epoch++;
    memset(g_ring.data, 0xEE, g_ring.size);
    flashbackNewRing();
    g_mHead = g_mCount = 0;
    g_mTop = g_mBot = 0;
    g_mSecDue = true;
    g_mSecLostLast = false;
    g_mStageHead = g_stageHead;
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

    for (int run = 0; run < FUZZ_RUNS; run++)
    {
        runs++;
        bool audio = rand() % 5 != 0;
        /* clip lengths: mostly up to 2 minutes (a long one takes a while to
           check), every 20th run any length, and the shortest and the longest
           every 50th */
        u32 seconds = run % 50 == 24 ? FLASHBACK_MIN_SECONDS : run % 50 == 49 ? FLASHBACK_MAX_SECONDS
                    : run % 20 == 19 ? FLASHBACK_MIN_SECONDS + rand() % (FLASHBACK_MAX_SECONDS - FLASHBACK_MIN_SECONDS + 1)
                    : FLASHBACK_MIN_SECONDS + rand() % 120;
        isN3DS = rand() % 4 != 0;   /* (an Old 3DS a quarter of the time: no Huffman codes, nor memory for them) */
        bool both = rand() % 3 != 0;
        u32 priority = rand() % FLASHBACK_PRIORITY_COUNT;
        flashbackApplyFormat(rand() % 2, both ? FLASHBACK_SCREENS_BOTH : FLASHBACK_SCREENS_TOP, priority, rand() % 3, seconds, audio);
        /* tiny frames, so the video ring stays small (the audio ring is real
           size); a slot as big as the top screen's frame as it is, or twice
           that (then the bottom screen's always finds room) */
        g_w = TINY_W; g_h = TINY_H;
        g_slotBytes = TINY_W * TINY_H * 2; g_slotStride = rand() % 4 == 0 ? 4096 : 2048;
        g_botW = both ? TINY_BOT_W : 0; g_botH = both ? TINY_BOT_H : 0; g_botBytes = g_botW * g_botH * 2;
        g_stageSlots = FLASHBACK_STAGE_SLOTS_MIN << (rand() % 6);   /* (up to 128) */
        g_capBufs = 1u << (rand() % 3);                                                                     /* 1, 2 or 4 */
        g_audioStageFrames = rand() % 2 ? FLASHBACK_AUDIO_STAGE_FRAMES : FLASHBACK_AUDIO_STAGE_FRAMES_MIN;   /* (both sizes) */
        flashbackLayout();
        /* (the bottom screen comes first only if it's recorded, and chosen to) */
        if (g_priBottom != (both && priority == FLASHBACK_PRIORITY_BOTTOM) ||
            g_priBytes != (g_priBottom ? g_botBytes : g_slotBytes) || g_priW != (g_priBottom ? g_botW : g_w) ||
            g_secBytes != (!both ? 0 : g_priBottom ? g_slotBytes : g_botBytes) || g_secW != (!both ? 0 : g_priBottom ? g_w : g_botW))
            FAIL("which screen comes first: %d, with the bottom screen %s and priority %u\n", g_priBottom, both ? "recorded" : "not recorded", priority);
        /* screens that mostly change, mostly stay, or anything between (the
           bottom one mostly stays, as on the 3DS; or the other way round, half
           the time when it's the first screen) */
        static const int still[] = { 0, 20, 60, 90, 98, 100 };
        g_topStillPct = still[rand() % 5];
        g_botStillPct = still[1 + rand() % 5];
        if (g_priBottom && rand() % 2)
        {
            int t = g_topStillPct;
            g_topStillPct = g_botStillPct;
            g_botStillPct = t;
        }

        flashbackUseMem(calloc(1, g_memSize));
        g_active = true;
        g_ring.size = RING_VIDEO_BASE + (u64)g_ringSlots * g_slotStride;
        g_ring.data = malloc(g_ring.size);
        startRing();
        g_failPct = run % 9 == 8 ? 3 : 0;
        g_reentrantPct = rand() % 90;
        g_packRunning = rand() % 3 == 0;
        g_packPct = 15 + rand() % 50;

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
    printf("audio frames checked %ld, appends refused (staging full) %ld\n", audioChecked, stagingFull);
    printf("frames packed %ld (%ld with the bottom screen first): %ld with nothing new on either screen (no slot); the second screen's: "
           "%ld stored (%ld in a slot of their own), %ld the same, %ld not stored (%ld of them after another, %ld compressed for nothing), "
           "%ld not captured\n", framesPacked, framesBottomFirst, framesSame, framesBotStored, framesBotOwn, framesBotSame, framesBotLost,
           framesBotLostStale, framesBotWasted, framesBotMissing);
    printf("whole frames due: the first screen's %ld times, the second screen's %ld\n", framesTopKey, framesBotKey);
    printf("packing falling behind for %ld frames: %ld of the second screen's that had changed not stored for it\n",
           framesBehind, framesSecYielded);
    printf("slots read %ld: frames as what changed %ld, packed with Huffman codes %ld, as they are %ld; slots with the top screen's only %ld, "
           "the bottom screen's only %ld, both %ld; not shown %ld (%ld of them the top screen's)\n",
           videoChecked, slotsDelta, slotsCoded, slotsRaw, slotsTopOnly, slotsBotOnly, slotsBoth, slotsUnshown, slotsUnshownTop);
    printf("clips: %ld, of %ld frames (%ld of them missed captures); %ld started before their first frame (%ld slots), "
           "%ld without the second screen at first\n", clipsChecked, copiesChecked, missedChecked, clipsWithLead, leadSlots, botLate);
    if (framesSame == 0 || framesBotLost == 0 || framesBotSame == 0 || framesBotMissing == 0 || slotsDelta == 0 || slotsCoded == 0 ||
        slotsRaw == 0 || slotsTopOnly == 0 || slotsBotOnly == 0 || slotsBoth == 0 || clipsWithLead == 0 || missedChecked == 0 ||
        framesBotOwn == 0 || framesBotLostStale == 0 || slotsUnshown == 0 || framesBotKey == 0 || framesBotWasted == 0 ||
        framesBottomFirst == 0 || slotsUnshownTop == 0 || slotsUnshownTop == slotsUnshown || framesSecYielded == 0)
    {
        printf("  ERROR: not every case came up\n");
        errors++;
    }
    /* (A frame of the bottom screen's that didn't fit isn't tried again in as little room: most of the ones
       not stored are never compressed.) */
    if (framesBotWasted * 2 > framesBotLostRoom)
    {
        printf("  ERROR: too many of the second screen's frames compressed for nothing\n");
        errors++;
    }
    /* (The bottom screen is only missing at a clip's start when it wasn't captured for more than
       FLASHBACK_LEAD_MAX - FLASHBACK_KEY_SLOTS slots running, which isn't often here.) */
    if (botLate * 5 > clipsChecked)
    {
        printf("  ERROR: too many clips start without the second screen\n");
        errors++;
    }
    printf("inputs: records checked %ld, times HID took more readings than it holds between looks %ld\n",
           g_inputsChecked, g_inputOverruns);
    printf("SD writes %ld, not sector aligned %ld\n", g_writes, g_unaligned);
    if (g_unaligned) errors++;
    printf("queue: frames left waiting after a capture %ld, deepest %ld; refused with it full: %ld (card behind too), %ld (not)\n", g_queuedAfterCapture, g_queueDepthMax, g_fullCard, g_fullCpu);
    printf("staging: most frames waiting %ld (of %d entries), times more than the slots' worth %ld, wraps %ld\n", g_stagedMax, FLASHBACK_STAGE_ENTRIES, g_stagedOverSlots, g_wraps);
    printf("%s (%ld errors)\n", errors ? "FAILED" : "all good", errors);
    return errors != 0;
}
