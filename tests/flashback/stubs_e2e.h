/* Just enough of libctru/Rosalina for the recorder's audio, writer and clip
   code to run on a PC, with real threads and real time. */
#pragma once
#include <windows.h>
#include <pthread.h>
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
       KEY_DDOWN = 128, KEY_R = 256, KEY_L = 512, KEY_X = 1024, KEY_Y = 2048, KEY_ZL = 4096, KEY_ZR = 8192, KEY_TOUCH = 1 << 20 };
#define FLASHBACK_DEFAULT_SAVE_COMBO (KEY_L | KEY_R | KEY_DDOWN)

typedef struct MyThread { int dummy; } MyThread;
static volatile bool preTerminationRequested;

/* time */
static u64 svcGetSystemTick(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (u64)((double)now.QuadPart * SYSCLOCK_ARM11 / (double)freq.QuadPart);
}
/* A thread can be held up on purpose (like a long kernel call on the 3DS). */
static __thread volatile s64 *t_extraSleepNs;
static void svcSleepThread(s64 ns)
{
    if (t_extraSleepNs && *t_extraSleepNs) { ns += *t_extraSleepNs; *t_extraSleepNs = 0; }
    u64 end = svcGetSystemTick() + (u64)(ns * (double)SYSCLOCK_ARM11 / 1e9);
    /* (Windows 11 can ignore timeBeginPeriod for a process with no visible window,
       making Sleep(1) last ~15 ms: only Sleep for the bulk of long waits) */
    while (svcGetSystemTick() + 268111ULL * 20 < end) Sleep(1);
    while (svcGetSystemTick() < end) SwitchToThread();
}

/* LightEvent (one-shot) */
typedef struct { pthread_mutex_t m; pthread_cond_t c; int set; } LightEvent;
static void LightEvent_Init(LightEvent *e) { pthread_mutex_init(&e->m, NULL); pthread_cond_init(&e->c, NULL); e->set = 0; }
static void LightEvent_Signal(LightEvent *e) { pthread_mutex_lock(&e->m); e->set = 1; pthread_cond_signal(&e->c); pthread_mutex_unlock(&e->m); }
static int LightEvent_WaitTimeout(LightEvent *e, s64 ns)
{
    pthread_mutex_lock(&e->m);
    if (!e->set)
    {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += ns % 1000000000; ts.tv_sec += ns / 1000000000 + ts.tv_nsec / 1000000000; ts.tv_nsec %= 1000000000;
        pthread_cond_timedwait(&e->c, &e->m, &ts);
    }
    int was = e->set; e->set = 0;
    pthread_mutex_unlock(&e->m);
    return was;
}

/* IFile on a memory buffer */
typedef struct { u8 *data; u64 pos; u64 size; } IFile;
typedef u64 FS_Archive;     /* (in the prototypes of code the tests leave out) */
static volatile long g_ifileWrites, g_ifileUnaligned;
static volatile int g_sdStallMs;
static Result IFile_Write(IFile *f, u64 *total, const void *buf, u32 len, u32 flags)
{
    (void)flags;
    if (f->pos + len > f->size) { printf("WRITE PAST END %llu+%u > %llu\n", (unsigned long long)f->pos, len, (unsigned long long)f->size); exit(3); }
    if (f->pos % 512 || len % 512) g_ifileUnaligned++;
    memcpy(f->data + f->pos, buf, len); f->pos += len; *total = len; g_ifileWrites++;
    Sleep(rand() % 3);  /* SD writes take a while */
    if (g_sdStallMs) { int ms = g_sdStallMs; g_sdStallMs = 0; Sleep(ms); }  /* ...and sometimes much longer (the game reading the card) */
    return 0;
}
static Result IFile_Read(IFile *f, u64 *total, void *buf, u32 len)
{
    if (f->pos + len > f->size) { printf("READ PAST END\n"); exit(3); }
    memcpy(buf, f->data + f->pos, len); f->pos += len; *total = len; return 0;
}

/* The DSP's two shared regions (0x1FF50000 and 0x1FF70000), faked */
static u8 g_fakeDsp[0x28000];
#define PA_PTR(addr) ((void *)(g_fakeDsp + ((u32)(addr) - 0x1FF50000)))

/* Video frames: noise (so they're stored uncompressed) carrying an id */
static volatile u32 g_nextFrameId;

/* HID's shared memory (flashback_input.c reads it; the tests write it), and
   the Rosalina lock it's read under */
static u32 g_fakeHid[0x80];
static vu32 *hidSharedMem = g_fakeHid;
static bool isHidInitialized = true;
static void Draw_Lock(void) {}
static void Draw_Unlock(void) {}

static bool flashbackCapture(u16 *dst);
