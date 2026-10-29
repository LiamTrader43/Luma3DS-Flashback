/* PC stand-in for libctru, for the Flashback SD test's logic: a virtual clock
   (sleeps and writes move it on), and a simulated card whose writes take 12 ms,
   or 120 ms during a 3-second slow stretch every 10 seconds. Every write is
   checked to stay inside the test file and in the right part of it. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int32_t s32; typedef int64_t s64; typedef s32 Result; typedef u32 Handle; typedef u64 FS_Archive;
#define R_FAILED(r) ((r) < 0)
#define R_SUCCEEDED(r) ((r) >= 0)
#define SYSCLOCK_ARM11 268111856ULL
#define KEY_A 1
#define KEY_X 2
#define KEY_START 8
typedef struct { int dummy; } PrintConsole;
typedef struct { int type; } FS_Path;
typedef struct { u32 sectorSize, clusterSize, totalClusters, freeClusters; } FS_ArchiveResource;
enum { GFX_TOP, GFX_BOTTOM, ARCHIVE_SDMC = 9, PATH_EMPTY = 1, PATH_ASCII = 3, SYSTEM_MEDIATYPE_SD = 1,
       FS_OPEN_WRITE = 2, FS_OPEN_CREATE = 4 };

static u64 g_vt;                    /* virtual ticks */
static long g_writes, g_badWrites;
static u64 g_fileSize;
static u64 svcGetSystemTick(void) { return g_vt; }
static void svcSleepThread(s64 ns) { u64 t = (u64)ns * SYSCLOCK_ARM11 / 1000000000ULL; g_vt += t ? t : 1; }   /* (time always moves on) */
static void hidScanInput(void) {}
static u32 hidKeysDown(void) { return 0; }
static bool aptMainLoop(void) { return true; }
static void consoleSelect(PrintConsole *c) { (void)c; }
static FS_Path fsMakePath(int t, const char *p) { (void)p; FS_Path f = { t }; return f; }

/* where the test's audio part ends (set by the test before running) */
static u64 g_videoBase;
static Result FSFILE_Write(Handle h, u32 *written, u64 offset, const void *buf, u32 size, u32 flags)
{
    (void)h; (void)buf; (void)flags;
    g_writes++;
    bool audio = size == 40960;
    if (offset + size > g_fileSize || (audio && offset + size > g_videoBase) || (!audio && offset < g_videoBase) || size % 512)
        g_badWrites++;
    double t = (double)g_vt / SYSCLOCK_ARM11;
    bool slow = ((long)t % 10) >= 6 && ((long)t % 10) < 9;     /* 3 s of every 10 */
    g_vt += (slow ? 120 : 12) * SYSCLOCK_ARM11 / 1000;
    *written = size;
    return 0;
}
/* (the menu part of main, not run here) */
static void gfxInitDefault(void) {}
static void gfxExit(void) {}
static void consoleInit(int s, PrintConsole *c) { (void)s; (void)c; }
static void consoleClear(void) {}
static void gspWaitForVBlank(void) {}
static Result FSUSER_OpenArchive(FS_Archive *a, int t, FS_Path p) { (void)a; (void)t; (void)p; return -1; }
static Result FSUSER_GetArchiveResource(FS_ArchiveResource *r, int m) { (void)r; (void)m; return -1; }
static Result FSUSER_OpenFile(Handle *h, FS_Archive a, FS_Path p, u32 f, u32 at) { (void)h; (void)a; (void)p; (void)f; (void)at; return -1; }
static Result FSUSER_DeleteFile(FS_Archive a, FS_Path p) { (void)a; (void)p; return -1; }
static Result FSUSER_CloseArchive(FS_Archive a) { (void)a; return 0; }
static Result FSFILE_SetSize(Handle h, u64 s) { (void)h; (void)s; return -1; }
static Result FSFILE_Close(Handle h) { (void)h; return 0; }
