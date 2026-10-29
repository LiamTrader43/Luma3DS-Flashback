// Flashback SD test: how much room for waiting frames would an SD card need so
// Flashback never drops a frame on it?
//
// It writes like Flashback recording at full resolution, 30 fps, 60 s, with
// sound: a 362 MB file, 30 frames a second of ~50-85 KB each (about what frames
// compress to), each at the start of its own 192 KB slot, as Flashback does,
// and a 40 KB chunk of audio every 0.31 s into the audio part at the start of
// the file. (An earlier version also tried writing frames back to back: on a
// SanDisk Ultra that dropped about 4 times as many frames, so it's gone.)
//
// Frames that come while earlier ones are still being written wait in line.
// Here every frame is written, however many wait, and the most that ever
// waited is the room the card needs for no drops at all. It also works out
// what room for 16 (Flashback 2.1), 48 (packed staging), 96 and 192 frames
// would have dropped (slightly pessimistic: frames those would have dropped are
// written anyway, which keeps the card busier). Three rounds of 60 s. The
// results are shown and saved to sd:/flashback_sdtest.txt; the test file is
// deleted at the end.
//
// The sustained test (X) writes like an Old 3DS recording Ocarina of Time 3D
// with a 10-minute buffer, for 12 minutes: frames of ~100 KB into a 3.6 GB
// file, and only as much room for waiting frames as an Old 3DS has (3 MB of
// staging, plus its 4 capture buffers), so a frame that comes with no room is
// dropped, as Flashback drops it. It does that twice: as Flashback writes
// (each frame on its own, at the start of its own slot), then with frames back
// to back in the file, writing all that wait (up to 8) in one go. The results,
// minute by minute (write time per frame, frames dropped), are shown and saved
// to sd:/flashback_sdtest_sustained.txt.

#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#define FPS             30
#define FRAMES          (60 * FPS)          // ring slots (60 s)
#define SLOT            196608              // ring slot size at full resolution
#define AUDIO_CHUNK     40960               // 64 audio frames
#define AUDIO_BYTES     (12736 * 640)       // audio ring for 60 s (+2 s)
#define VIDEO_BASE      ((AUDIO_BYTES + 0xFFFF) & ~0xFFFF)
#define FILE_SIZE       ((u64)VIDEO_BASE + (u64)FRAMES * SLOT)
#define AUDIO_TICKS     (64ULL * 160 * 8192)    // ARM11 ticks per audio chunk (~0.31 s)
#define ROUND_SECONDS   60
#define ROUNDS          3
#define WAIT_CAP        1024                // (in case a card can't keep up at all)
#define TEST_PATH       "/flashback_sdtest.bin"
#define RESULT_PATH     "sdmc:/flashback_sdtest.txt"

// The sustained test.
#define SUS_MINUTES     12
#define SUS_FRAMES      (600 * FPS)         // ring slots (a 10-minute buffer)
#define SUS_AUDIO_BYTES (((602ULL * SYSCLOCK_ARM11 / (160 * 8192) + 1 + 63) / 64 * 64) * 640)   // audio ring for 600 s (+2 s)
#define SUS_VIDEO_BASE  ((SUS_AUDIO_BYTES + 0xFFFF) & ~0xFFFFULL)
#define SUS_VIDEO_BYTES ((u64)SUS_FRAMES * SLOT)
#define SUS_FILE_SIZE   (SUS_VIDEO_BASE + SUS_VIDEO_BYTES)
#define SUS_ROOM_BYTES  (16 * SLOT)         // an Old 3DS's staging (3 MB)...
#define SUS_ROOM_FRAMES 512                 // ...and the most frames it holds
#define SUS_CAPTURE     4                   // its capture buffers (uncompressed frames before staging)
#define SUS_BATCH       8                   // frames written in one go at most, back to back
#define SUS_BUF_BYTES   (1024 * 1024)       // (a batch's worth)
#define SUS_RESULT_PATH "sdmc:/flashback_sdtest_sustained.txt"

static const u32 kRooms[] = { 16, 48, 96, 192 };
#define ROOMS (sizeof(kRooms) / sizeof(kRooms[0]))

typedef struct {
    u32 frames, mostWaiting, audioWrites, capped;
    u32 dropped[ROOMS];
    u32 over100, over250, slowestUs;
    u64 totalUs, bytes;
} Round;

static PrintConsole g_top, g_bottom;
static u8 *g_buf;

static u32 g_rng;
static u32 nextRandom(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

// A frame's size: 50-85 KB, in whole sectors (as Flashback writes them).
static u32 frameSize(void)
{
    return (50 * 1024 + nextRandom() % (36 * 1024) + 511) & ~511u;
}

static bool userQuit(void)
{
    hidScanInput();
    return (hidKeysDown() & KEY_START) != 0 || !aptMainLoop();
}

// One round. Returns false if the user quit or a write failed.
static bool runRound(Handle file, Round *r, int n)
{
    u64 period = SYSCLOCK_ARM11 / FPS;
    u64 t0 = svcGetSystemTick(), end = t0 + (u64)ROUND_SECONDS * SYSCLOCK_ARM11, shown = 0;
    u32 arrived = 0, waiting = 0, audioDone = 0, slot = 0;
    u32 room[ROOMS] = { 0 };    // frames waiting with each room
    g_rng = 0x12345678 + n;

    for (;;)
    {
        u64 now = svcGetSystemTick();
        if (now >= end)
            return true;

        // Frames that came since: all wait here; with each room, one that
        // finds it full is dropped.
        u32 due = (u32)((now - t0) / period) + 1;
        for (; arrived < due; arrived++)
        {
            if (waiting >= WAIT_CAP)
                r->capped++;
            else
                waiting++;
            for (u32 k = 0; k < ROOMS; k++)
            {
                if (room[k] >= kRooms[k])
                    r->dropped[k]++;
                else
                    room[k]++;
            }
        }
        if (waiting > r->mostWaiting)
            r->mostWaiting = waiting;

        if (now - shown >= SYSCLOCK_ARM11 / 2)
        {
            shown = now;
            consoleSelect(&g_bottom);
            printf("\x1b[8;1HRound %d of %d: %2lu / %d s       \n", n, ROUNDS,
                   (unsigned long)((now - t0) / SYSCLOCK_ARM11), ROUND_SECONDS);
            printf("Waiting now %3lu, most %3lu       \n", (unsigned long)waiting, (unsigned long)r->mostWaiting);
            printf("Slowest write %lu ms          \n", (unsigned long)(r->slowestUs / 1000));
            if (userQuit())
                return false;
        }

        // Audio first (as Flashback's writer does), then the oldest frame.
        u32 audioDue = (u32)((now - t0) / AUDIO_TICKS);
        u64 offset;
        u32 size;
        bool video = false;
        if (audioDone < audioDue)
        {
            offset = (u64)(audioDone % (AUDIO_BYTES / AUDIO_CHUNK)) * AUDIO_CHUNK;
            size = AUDIO_CHUNK;
            audioDone++;
            r->audioWrites++;
        }
        else if (waiting != 0)
        {
            size = frameSize();
            offset = VIDEO_BASE + (u64)(slot++ % FRAMES) * SLOT;
            video = true;
        }
        else
        {
            // Nothing to write: sleep until the next frame or audio chunk.
            u64 next = t0 + (u64)due * period, nextAudio = t0 + (u64)(audioDone + 1) * AUDIO_TICKS;
            if (nextAudio < next)
                next = nextAudio;
            now = svcGetSystemTick();
            if (next > now)
                svcSleepThread((s64)((next - now) * 1000000000ULL / SYSCLOCK_ARM11));
            continue;
        }

        u32 written = 0;
        u64 w0 = svcGetSystemTick();
        Result res = FSFILE_Write(file, &written, offset, g_buf, size, 0);
        u32 us = (u32)((svcGetSystemTick() - w0) * 1000000 / SYSCLOCK_ARM11);
        if (R_FAILED(res) || written != size)
        {
            consoleSelect(&g_bottom);
            printf("\nWrite failed (%08lX).\n", (unsigned long)res);
            return false;
        }
        if (video)
        {
            r->frames++;
            r->bytes += size;
            r->totalUs += us;
            r->over100 += us > 100000;
            r->over250 += us > 250000;
            if (us > r->slowestUs)
                r->slowestUs = us;
            waiting--;
            for (u32 k = 0; k < ROOMS; k++)
                if (room[k] != 0)
                    room[k]--;
        }
    }
}

static void emit(FILE *out, const char *text)
{
    printf("%s", text);
    if (out != NULL)
        fprintf(out, "%s", text);
}

static void printResults(FILE *out, const Round *rounds, int count)
{
    char line[128];
    // (The top screen is 50 columns.)
    snprintf(line, sizeof(line), "%1s %5s %5s %4s %4s %4s |%4s %4s %4s %4s\n", "", "avg", "slow", ">100", ">250", "most",
             "16", "48", "96", "192");
    emit(out, line);
    u32 drop[ROOMS] = { 0 }, most = 0;
    u64 bytes = 0, frames = 0;
    for (int i = 0; i < count; i++)
    {
        const Round *r = &rounds[i];
        snprintf(line, sizeof(line), "%d %5.1f %5lu %4lu %4lu %4lu |%4lu %4lu %4lu %4lu%s\n", i + 1,
                 r->frames ? r->totalUs / 1000.0 / r->frames : 0.0, (unsigned long)(r->slowestUs / 1000),
                 (unsigned long)r->over100, (unsigned long)r->over250, (unsigned long)r->mostWaiting,
                 (unsigned long)r->dropped[0], (unsigned long)r->dropped[1], (unsigned long)r->dropped[2],
                 (unsigned long)r->dropped[3], r->capped ? "!" : "");
        emit(out, line);
        for (u32 k = 0; k < ROOMS; k++)
            drop[k] += r->dropped[k];
        if (r->mostWaiting > most)
            most = r->mostWaiting;
        bytes += r->bytes;
        frames += r->frames;
    }
    emit(out, "(avg, slow: write times in ms; >100, >250: writes\n"
              "over that many ms; most: frames waiting at most;\n"
              "16-192: frames dropped with room for that many;\n"
              "!: over 1024 waited, the card couldn't keep up)\n");
    snprintf(line, sizeof(line), "\nDropped in all, room for 16 / 48 / 96 / 192:\n  %lu / %lu / %lu / %lu\n",
             (unsigned long)drop[0], (unsigned long)drop[1], (unsigned long)drop[2], (unsigned long)drop[3]);
    emit(out, line);
    // The room for no drops at all, in memory: the frames that waited at
    // most, at the average frame size, each rounded up to a 4 KB page.
    u64 avgFrame = frames ? bytes / frames : 0;
    u64 needKb = (u64)most * ((avgFrame + 4095) / 4096) * 4;
    snprintf(line, sizeof(line), "No drops at all needs room for %lu frames\n(about %lu.%lu MB at these sizes).\n",
             (unsigned long)most, (unsigned long)(needKb / 1024), (unsigned long)(needKb % 1024 * 10 / 1024));
    emit(out, line);
}

// ---------------------------------------------------------------------------
// The sustained test
// ---------------------------------------------------------------------------

typedef struct {
    u32 arrived, written, dropped, writes, audioWrites, slowestUs;
    u64 videoUs, audioUs, bytes;
} Minute;

// A frame's size: ~100 KB (Ocarina of Time 3D's compress to that, at full
// resolution), in whole sectors.
static u32 sustainedFrameSize(void)
{
    return (101376 + nextRandom() % 2048 + 511) & ~511u;
}

static u32 pageRound(u32 size)
{
    return (size + 4095) & ~4095u;
}

// One way of writing, for SUS_MINUTES. Returns false if the user quit or a
// write failed.
static bool runSustained(Handle file, bool batched, Minute *m)
{
    u64 period = SYSCLOCK_ARM11 / FPS;
    u64 t0 = svcGetSystemTick(), end = t0 + (u64)SUS_MINUTES * 60 * SYSCLOCK_ARM11, shown = 0;
    u32 arrived = 0, audioDone = 0, slot = 0;
    u64 logPos = 0;                         // batched: where the next frame goes in the video part
    u32 cap[SUS_CAPTURE], capHead = 0, capCount = 0;            // frames in the capture buffers
    static u32 staged[SUS_ROOM_FRAMES];
    u32 stageHead = 0, stageCount = 0, stageBytes = 0;          // frames in staging, oldest first
    g_rng = 0x5EED0000 + batched;

    for (;;)
    {
        u64 now = svcGetSystemTick();
        if (now >= end)
            return true;
        Minute *cur = &m[(now - t0) / (60ULL * SYSCLOCK_ARM11)];

        // Frames that came since: into a capture buffer, or dropped if none is
        // free; then into staging while it has room (as the packer does).
        u32 due = (u32)((now - t0) / period) + 1;
        for (; arrived < due; arrived++)
        {
            Minute *at = &m[arrived / (60 * FPS) < SUS_MINUTES ? arrived / (60 * FPS) : SUS_MINUTES - 1];
            at->arrived++;
            if (capCount < SUS_CAPTURE)
                cap[(capHead + capCount++) % SUS_CAPTURE] = sustainedFrameSize();
            else
                at->dropped++;
        }
        while (capCount != 0 && stageCount < SUS_ROOM_FRAMES &&
               stageBytes + pageRound(cap[capHead]) <= SUS_ROOM_BYTES)
        {
            staged[(stageHead + stageCount++) % SUS_ROOM_FRAMES] = cap[capHead];
            stageBytes += pageRound(cap[capHead]);
            capHead = (capHead + 1) % SUS_CAPTURE;
            capCount--;
        }

        if (now - shown >= SYSCLOCK_ARM11 / 2)
        {
            shown = now;
            u32 secs = (u32)((now - t0) / SYSCLOCK_ARM11), dropped = 0;
            for (u32 i = 0; i < SUS_MINUTES; i++)
                dropped += m[i].dropped;
            consoleSelect(&g_bottom);
            printf("\x1b[8;1H%s: %2lu:%02lu / %d:00      \n", batched ? "Back to back" : "As Flashback",
                   (unsigned long)(secs / 60), (unsigned long)(secs % 60), SUS_MINUTES);
            printf("Dropped so far %5lu       \n", (unsigned long)dropped);
            printf("This minute: %5.1f ms a frame       \n",
                   cur->written ? cur->videoUs / 1000.0 / cur->written : 0.0);
            if (userQuit())
                return false;
        }

        // Audio first (as Flashback's writer does), then the oldest frames.
        u32 audioDue = (u32)((now - t0) / AUDIO_TICKS);
        u64 offset;
        u32 size, frames = 0;
        if (audioDone < audioDue)
        {
            offset = (u64)(audioDone % (SUS_AUDIO_BYTES / AUDIO_CHUNK)) * AUDIO_CHUNK;
            size = AUDIO_CHUNK;
            audioDone++;
        }
        else if (stageCount != 0)
        {
            if (!batched)
            {
                size = staged[stageHead];
                offset = SUS_VIDEO_BASE + (u64)(slot++ % SUS_FRAMES) * SLOT;
                frames = 1;
            }
            else
            {
                // Back to back from logPos (round to the start when the next
                // doesn't fit before the end), as many as wait, up to SUS_BATCH.
                if (logPos + staged[stageHead] > SUS_VIDEO_BYTES)
                    logPos = 0;
                offset = SUS_VIDEO_BASE + logPos;
                size = 0;
                while (frames < stageCount && frames < SUS_BATCH)
                {
                    u32 next = staged[(stageHead + frames) % SUS_ROOM_FRAMES];
                    if (logPos + size + next > SUS_VIDEO_BYTES)
                        break;
                    size += next;
                    frames++;
                }
                logPos += size;
            }
        }
        else
        {
            // Nothing to write: sleep until the next frame or audio chunk.
            u64 next = t0 + (u64)due * period, nextAudio = t0 + (u64)(audioDone + 1) * AUDIO_TICKS;
            if (nextAudio < next)
                next = nextAudio;
            now = svcGetSystemTick();
            if (next > now)
                svcSleepThread((s64)((next - now) * 1000000000ULL / SYSCLOCK_ARM11));
            continue;
        }

        u32 written = 0;
        u64 w0 = svcGetSystemTick();
        Result res = FSFILE_Write(file, &written, offset, g_buf, size, 0);
        u64 w1 = svcGetSystemTick();
        u32 us = (u32)((w1 - w0) * 1000000 / SYSCLOCK_ARM11);
        if (R_FAILED(res) || written != size)
        {
            consoleSelect(&g_bottom);
            printf("\nWrite failed (%08lX).\n", (unsigned long)res);
            return false;
        }
        Minute *done = &m[w1 < end ? (w1 - t0) / (60ULL * SYSCLOCK_ARM11) : SUS_MINUTES - 1];
        if (frames == 0)
        {
            done->audioWrites++;
            done->audioUs += us;
            continue;
        }
        done->writes++;
        done->written += frames;
        done->videoUs += us;
        done->bytes += size;
        if (us > done->slowestUs)
            done->slowestUs = us;
        for (u32 i = 0; i < frames; i++)
        {
            stageBytes -= pageRound(staged[stageHead]);
            stageHead = (stageHead + 1) % SUS_ROOM_FRAMES;
            stageCount--;
        }
    }
}

static void printSustained(FILE *out, const Minute *a, const Minute *b, u32 minutes)
{
    char line[128];
    // (The top screen is 50 columns.)
    emit(out, "      As Flashback        Back to back\n");
    emit(out, "min  ms/frame dropped   ms/frame dropped per write\n");
    Minute ta = { 0 }, tb = { 0 };
    for (u32 i = 0; i < SUS_MINUTES; i++)
    {
        const Minute *x = &a[i], *y = &b[i];
        snprintf(line, sizeof(line), "%3lu  %8.1f %7lu   %8.1f %7lu %9.1f\n", (unsigned long)(i + 1),
                 x->written ? x->videoUs / 1000.0 / x->written : 0.0, (unsigned long)x->dropped,
                 y->written ? y->videoUs / 1000.0 / y->written : 0.0, (unsigned long)y->dropped,
                 y->writes ? (double)y->written / y->writes : 0.0);
        if (i < minutes)
            emit(out, line);
        ta.written += x->written; ta.dropped += x->dropped; ta.videoUs += x->videoUs; ta.bytes += x->bytes;
        ta.audioWrites += x->audioWrites; ta.audioUs += x->audioUs; ta.writes += x->writes;
        tb.written += y->written; tb.dropped += y->dropped; tb.videoUs += y->videoUs; tb.bytes += y->bytes;
        tb.audioWrites += y->audioWrites; tb.audioUs += y->audioUs; tb.writes += y->writes;
        if (x->slowestUs > ta.slowestUs) ta.slowestUs = x->slowestUs;
        if (y->slowestUs > tb.slowestUs) tb.slowestUs = y->slowestUs;
    }
    snprintf(line, sizeof(line), "all  %8.1f %7lu   %8.1f %7lu %9.1f\n",
             ta.written ? ta.videoUs / 1000.0 / ta.written : 0.0, (unsigned long)ta.dropped,
             tb.written ? tb.videoUs / 1000.0 / tb.written : 0.0, (unsigned long)tb.dropped,
             tb.writes ? (double)tb.written / tb.writes : 0.0);
    emit(out, line);
    emit(out, "(ms/frame: writing time per frame, 33.3 at most to\n"
              "keep up; dropped: frames that found no room)\n");
    snprintf(line, sizeof(line), "Audio %.1f / %.1f ms, slowest write %lu / %lu ms\n",
             ta.audioWrites ? ta.audioUs / 1000.0 / ta.audioWrites : 0.0,
             tb.audioWrites ? tb.audioUs / 1000.0 / tb.audioWrites : 0.0,
             (unsigned long)(ta.slowestUs / 1000), (unsigned long)(tb.slowestUs / 1000));
    emit(out, line);
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, &g_top);
    consoleInit(GFX_BOTTOM, &g_bottom);

    consoleSelect(&g_bottom);
    printf("Flashback SD test\n\n");
    printf("A: room test (3 rounds of 60 s)\n");
    printf("X: sustained test (2 x 12 minutes)\n");
    printf("START: quit\n");

    consoleSelect(&g_top);
    printf("A writes to the SD card like Flashback recording\n");
    printf("(full resolution, 30 fps, 60 s, with sound) and\n");
    printf("measures how much room for waiting frames the\n");
    printf("card needs so no frame is ever dropped.\n");
    printf("Needs 363 MB free on the SD card.\n\n");
    printf("X writes like an Old 3DS recording Ocarina of\n");
    printf("Time 3D with a 10-minute buffer, for 12 minutes,\n");
    printf("first as Flashback does, then with frames back to\n");
    printf("back, and shows write times and drops minute by\n");
    printf("minute. Needs 3.7 GB free on the SD card.\n\n");
    printf("Turn Flashback recording off first. Leave the 3DS\n");
    printf("alone while it runs. The test file is deleted at\n");
    printf("the end.\n");

    bool start = false, sustained = false;
    while (aptMainLoop())
    {
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START)
            break;
        if (down & (KEY_A | KEY_X))
        {
            start = true;
            sustained = (down & KEY_X) != 0;
            break;
        }
        gspWaitForVBlank();
    }

    if (start)
    {
        FS_Archive sdmc = 0;
        Handle file = 0;
        u64 freeBytes = 0;
        u64 fileSize = sustained ? SUS_FILE_SIZE : FILE_SIZE;
        FS_ArchiveResource resource;
        bool haveArchive = R_SUCCEEDED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")));
        if (haveArchive && R_SUCCEEDED(FSUSER_GetArchiveResource(&resource, SYSTEM_MEDIATYPE_SD)))
            freeBytes = (u64)resource.freeClusters * resource.clusterSize;

        g_buf = memalign(0x1000, SUS_BUF_BYTES);
        consoleSelect(&g_bottom);
        if (!haveArchive || g_buf == NULL)
            printf("\nCouldn't open the SD card.\n");
        else if (freeBytes < fileSize + 1000000)
            printf("\nNot enough free space: needs %lu MB,\nhas %lu MB.\n", (unsigned long)(fileSize / 1000000 + 1),
                   (unsigned long)(freeBytes / 1000000));
        else
        {
            for (u32 i = 0; i < SUS_BUF_BYTES; i++)
                g_buf[i] = (u8)(i * 37 + (i >> 8));

            printf(sustained ? "\nCreating the test file (3.6 GB,\nthat can take a minute)...\n" : "\nCreating the test file...\n");
            FSUSER_DeleteFile(sdmc, fsMakePath(PATH_ASCII, TEST_PATH));
            Result res = FSUSER_OpenFile(&file, sdmc, fsMakePath(PATH_ASCII, TEST_PATH), FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
            if (R_SUCCEEDED(res))
                res = FSFILE_SetSize(file, fileSize);
            if (R_FAILED(res))
                printf("Couldn't create it (%08lX).\n", (unsigned long)res);
            else if (sustained)
            {
                static Minute asFlashback[SUS_MINUTES], backToBack[SUS_MINUTES];
                bool done = runSustained(file, false, asFlashback) && runSustained(file, true, backToBack);

                consoleSelect(&g_top);
                consoleClear();
                printf("Flashback SD sustained test results\n\n");
                FILE *out = fopen(SUS_RESULT_PATH, "w");
                printSustained(out, asFlashback, backToBack, SUS_MINUTES);
                if (out != NULL)
                    fclose(out);
                consoleSelect(&g_bottom);
                printf("\n%s.\n", done ? "Done" : "Stopped early");
                printf("Results saved to\n%s\n", SUS_RESULT_PATH);
            }
            else
            {
                Round rounds[ROUNDS];
                memset(rounds, 0, sizeof(rounds));
                int done = 0;
                for (int i = 0; i < ROUNDS; i++)
                {
                    if (!runRound(file, &rounds[i], i + 1))
                        break;
                    done++;
                }

                consoleSelect(&g_top);
                consoleClear();
                printf("Flashback SD test results\n\n");
                FILE *out = fopen(RESULT_PATH, "w");
                printResults(out, rounds, done);
                if (out != NULL)
                    fclose(out);
                consoleSelect(&g_bottom);
                printf("\n%s.\n", done == ROUNDS ? "Done" : "Stopped early");
                printf("Results saved to %s\n", RESULT_PATH);
            }
            if (file != 0)
                FSFILE_Close(file);
            FSUSER_DeleteFile(sdmc, fsMakePath(PATH_ASCII, TEST_PATH));
        }
        if (haveArchive)
            FSUSER_CloseArchive(sdmc);
        free(g_buf);

        consoleSelect(&g_bottom);
        printf("\nSTART: quit\n");
        while (aptMainLoop())
        {
            hidScanInput();
            if (hidKeysDown() & KEY_START)
                break;
            gspWaitForVBlank();
        }
    }

    gfxExit();
    return 0;
}
