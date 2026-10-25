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

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, &g_top);
    consoleInit(GFX_BOTTOM, &g_bottom);

    consoleSelect(&g_bottom);
    printf("Flashback SD test\n\n");
    printf("A: start (3 rounds of 60 s)\n");
    printf("START: quit\n");

    consoleSelect(&g_top);
    printf("Writes to the SD card like Flashback recording\n");
    printf("(full resolution, 30 fps, 60 s, with sound) and\n");
    printf("measures how much room for waiting frames the\n");
    printf("card needs so no frame is ever dropped.\n\n");
    printf("Needs 363 MB free on the SD card. Turn Flashback\n");
    printf("recording off first. Leave the 3DS alone while it\n");
    printf("runs. The test file is deleted at the end.\n");

    bool start = false;
    while (aptMainLoop())
    {
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START)
            break;
        if (down & KEY_A)
        {
            start = true;
            break;
        }
        gspWaitForVBlank();
    }

    if (start)
    {
        FS_Archive sdmc = 0;
        Handle file = 0;
        u64 freeBytes = 0;
        FS_ArchiveResource resource;
        bool haveArchive = R_SUCCEEDED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")));
        if (haveArchive && R_SUCCEEDED(FSUSER_GetArchiveResource(&resource, SYSTEM_MEDIATYPE_SD)))
            freeBytes = (u64)resource.freeClusters * resource.clusterSize;

        g_buf = memalign(0x1000, SLOT);
        consoleSelect(&g_bottom);
        if (!haveArchive || g_buf == NULL)
            printf("\nCouldn't open the SD card.\n");
        else if (freeBytes < FILE_SIZE + 1000000)
            printf("\nNot enough free space: needs 363 MB,\nhas %lu MB.\n", (unsigned long)(freeBytes / 1000000));
        else
        {
            for (u32 i = 0; i < SLOT; i++)
                g_buf[i] = (u8)(i * 37 + (i >> 8));

            printf("\nCreating the test file...\n");
            FSUSER_DeleteFile(sdmc, fsMakePath(PATH_ASCII, TEST_PATH));
            Result res = FSUSER_OpenFile(&file, sdmc, fsMakePath(PATH_ASCII, TEST_PATH), FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
            if (R_SUCCEEDED(res))
                res = FSFILE_SetSize(file, FILE_SIZE);
            if (R_FAILED(res))
                printf("Couldn't create it (%08lX).\n", (unsigned long)res);
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
