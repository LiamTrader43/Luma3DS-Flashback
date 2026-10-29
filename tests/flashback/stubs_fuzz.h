/* libctru/Rosalina stubs for the deterministic fuzz test: no threads, no real
   time. The "SD card" lets the producers run in the middle of each write, the
   way the capture and audio threads keep going while the writer waits on the
   card, and can be made to fail. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int16_t s16; typedef int32_t s32; typedef int64_t s64; typedef int32_t Result;
typedef volatile u16 vu16; typedef volatile u32 vu32;
#define R_FAILED(r) ((r) < 0)
#define R_SUCCEEDED(r) ((r) >= 0)
#define CTR_ALIGN(n) __attribute__((aligned(n)))
#define SYSCLOCK_SOC (16756991u)   /* as libctru defines them: 32-bit */
#define SYSCLOCK_ARM11 (SYSCLOCK_SOC * 2 * 4 * 2)
#define _Static_assert(a, b)
enum { KEY_A = 1, KEY_B = 2, KEY_SELECT = 4, KEY_START = 8, KEY_DRIGHT = 16, KEY_DLEFT = 32, KEY_DUP = 64,
       KEY_DDOWN = 128, KEY_R = 256, KEY_L = 512, KEY_X = 1024, KEY_Y = 2048, KEY_ZL = 1 << 14, KEY_ZR = 1 << 15, KEY_TOUCH = 1 << 20 };
#define FLASHBACK_DEFAULT_SAVE_COMBO (KEY_L | KEY_R | KEY_DDOWN)

typedef struct MyThread { int dummy; } MyThread;
static volatile bool preTerminationRequested;
static u64 g_fakeTicks;
static u64 svcGetSystemTick(void) { return g_fakeTicks += 1000; }
static void svcSleepThread(s64 ns) { (void)ns; }
typedef struct { int dummy; } LightEvent;
static void LightEvent_Signal(LightEvent *e) { (void)e; }
static int LightEvent_WaitTimeout(LightEvent *e, s64 ns) { (void)e; (void)ns; return 0; }

typedef struct { u8 *data; u64 pos; u64 size; } IFile;
typedef u64 FS_Archive;                       /* (in the prototypes of code the tests leave out) */
static void writeHook(IFile *f);              /* the test's producers, run mid-write */
static int g_failPct;                         /* chance a write fails */
static long g_writes, g_unaligned;
static Result IFile_Write(IFile *f, u64 *total, const void *buf, u32 len, u32 flags)
{
    (void)flags;
    if (f->pos + len > f->size) { printf("WRITE PAST END %llu + %u > %llu\n", (unsigned long long)f->pos, len, (unsigned long long)f->size); exit(3); }
    if (f->pos % 512 || len % 512) g_unaligned++;
    u64 pos = f->pos;
    writeHook(f);                       /* the producers carry on while the card is busy... */
    if (rand() % 100 < g_failPct) return -1;
    memcpy(f->data + pos, buf, len);    /* ...and the card only takes the data at the end (worst case) */
    f->pos = pos + len; *total = len; g_writes++;
    return 0;
}
static Result IFile_Read(IFile *f, u64 *total, void *buf, u32 len)
{
    if (f->pos + len > f->size) { printf("READ PAST END\n"); exit(3); }
    memcpy(buf, f->data + f->pos, len); f->pos += len; *total = len; return 0;
}

static u8 g_fakeDsp[0x28000];
#define PA_PTR(addr) ((void *)(g_fakeDsp + ((u32)(addr) - 0x1FF50000)))

/* HID's shared memory (flashback_input.c reads it; the tests write it), and
   the Rosalina lock it's read under */
static u32 g_fakeHid[0x80];
static vu32 *hidSharedMem = g_fakeHid;
static bool isHidInitialized = true;
static void Draw_Lock(void) {}
static void Draw_Unlock(void) {}

static void flashbackCaptureWait(void) {}
static bool flashbackCapture(u16 *dst);
