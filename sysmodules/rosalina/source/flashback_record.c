/*
*   This file is part of Luma3DS
*   Copyright (C) 2026 LiamTrader
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*   Additional Terms 7.b and 7.c of GPLv3 apply to this file:
*       * Requiring preservation of specified reasonable legal notices or
*         author attributions in that material or in the Appropriate Legal
*         Notices displayed by works containing it.
*       * Prohibiting misrepresentation of the origin of that material,
*         or requiring that modified versions of such material be marked in
*         reasonable ways as different from the original version.
*/

// Flashback: the capture queue, staging, the writer, and starting and
// stopping recording (see flashback_internal.h).

#include <3ds.h>
#include <string.h>
#include "flashback_internal.h"
#include "csvc.h"
#include "menu.h"

// Work memory (staging for frames and audio, and a capture buffer), allocated
// from the SYSTEM region when recording starts, never from the running game,
// and sized for the current resolution. The address is clear of the
// screenshot/menu framebuffer cache at 0x0D000000.
#define FLASHBACK_MEM_ADDR      0x0D800000

// FS priority for the ring file. Rosalina's FS session runs at real-time
// priority; the ring file's constant writes must not get ahead of the game,
// and the lowest priority costs them nothing (see docs/flashback-design.md,
// "SD card writes").
#define FLASHBACK_RING_FS_PRIORITY  8

// Owned by the flashback thread.
bool  g_active;                         // memory allocated + ring file open
u8   *g_mem;
IFile g_ring;
u32   g_head;                           // next ring slot to write
volatile u32 g_count;                   // valid ring slots

// Frames that should have been recorded after each slot but weren't (the
// thread fell behind, or a capture/write failed). Saving repeats the slot's
// frame that many extra times so clips keep real-time speed. Kept in RAM
// rather than writing duplicates, since a slow SD write is the usual cause.
// (In g_mem, like g_slotLen and g_slotAudioFrame: see flashbackUseMem.)
u16  *g_repeat;

// Size of each ring slot's contents: the compressed frame, or g_slotBytes if
// the frame was stored uncompressed (it wouldn't have gotten smaller).
u32  *g_slotLen;

// Staged frames are packed one after another, each taking only what it
// compressed to (rounded up to FLASHBACK_STAGE_ALIGN), and staging is used
// round and round: a frame that might not fit before the end goes at the start.
// Frames typically compress to a third or a half, so two or three times as
// many fit as uncompressed, and a card's slow spell (see
// docs/flashback-design.md, "Staging") is waited out for two or three times as
// long. A frame is only started where there's room for it uncompressed: its
// size is only known once it's compressed. Frame number p (counting every
// frame ever staged) has entry p % FLASHBACK_STAGE_ENTRIES: where it starts,
// its ring slot (so any ring size works) and its size. g_stageProduced -
// g_stageConsumed frames are waiting, oldest first; both counters only ever
// increase (and stay in step with the entries when they wrap, there being a
// power of two of them).
#ifndef FLASHBACK_STAGE_ALIGN
#define FLASHBACK_STAGE_ALIGN       0x1000      // a page; a multiple of the SD sector (writes are rounded up to whole ones)
#endif
#define FLASHBACK_STAGE_ENTRIES     512         // frames that can wait at most
u32 g_stageSlots;                       // staging size, in uncompressed frames (flashbackStart)
u32 g_stageBytes;                       // ...and in bytes (flashbackLayout)
static u32 g_stageHead;                 // where the frame after the newest staged one may go (packer thread)
static u32 g_stageOff[FLASHBACK_STAGE_ENTRIES];      // where each staged frame starts in staging
static u32 g_stageRingIdx[FLASHBACK_STAGE_ENTRIES];  // ring slot of each staged frame
static u32 g_stageLen[FLASHBACK_STAGE_ENTRIES];      // bytes to write for each staged frame
static volatile u32 g_stageProduced;    // frames staged (packer thread)
static volatile u32 g_stageConsumed;    // frames written or given up on (writer thread)
volatile bool g_writeFailed;            // a ring write failed (writer -> capture thread)
bool g_ringLost;                        // a save couldn't reopen/recreate the ring file
LightEvent g_writeEvent;                // signaled when a frame is staged

// The capture queue (see FLASHBACK_CAPTURE_BUFS_MAX).
u32 g_capBufs;                          // capture buffers in use (flashbackStart)
u32 g_capStride;                        // bytes between them (the first at g_frameOffset)
static volatile u32 g_capProduced;      // frames captured (capture thread)
static volatile u32 g_capConsumed;      // frames packed (packer thread): their buffers are free again
static u32 g_capAudioFrame[FLASHBACK_CAPTURE_BUFS_MAX];    // where the audio was at each capture
static u32 g_capMissedBefore[FLASHBACK_CAPTURE_BUFS_MAX];  // grid slots missed just before it (copies of the frame before)
static u64 g_capTicks[FLASHBACK_CAPTURE_BUFS_MAX];         // how long each capture took (stats)
static volatile u32 g_capTicksTotal;    // all captures' time, wrapping (stats: compressing leaves it out)
u32 g_pendingMissed;                    // capture thread: grid slots missed since the last capture
LightEvent g_packEvent;                 // signaled when a frame is captured

// ---------------------------------------------------------------------------
// Ring file
// ---------------------------------------------------------------------------

Result flashbackCreateDirectory(FS_Archive archive, const char *path)
{
    Result res = FSUSER_CreateDirectory(archive, fsMakePath(PATH_ASCII, path), 0);
    return (u32)res == 0xC82044BE ? 0 : res; // already exists
}

// Writes the next whole chunk of staged audio to the audio ring, or with
// 'unfinished', the frames of an unfinished chunk too (for a save; the chunk is
// written again once it's complete). Returns false if there was nothing to write.
static bool flashbackWriteAudio(bool unfinished)
{
    if (!g_audio)
        return false;

    u32 consumed = g_audioConsumed;     // always the start of a chunk
    u32 pending = g_audioProduced - consumed;
    if (pending < FLASHBACK_AUDIO_CHUNK_FRAMES && !(unfinished && pending != 0 && g_audioFlushed != consumed + pending))
        return false;
    __sync_synchronize();   // see the staged frames before writing them

    u32 n = pending < FLASHBACK_AUDIO_CHUNK_FRAMES ? pending : FLASHBACK_AUDIO_CHUNK_FRAMES;
    if (!g_writeFailed)
    {
        u64 total;
        // (An unfinished chunk rounded up to whole sectors also overwrites the
        // start of the chunk's next frames: the ring's oldest audio, outside
        // any clip, and rewritten anyway once the chunk is complete.)
        u32 len = (n * FLASHBACK_AUDIO_FRAME_BYTES + 511) & ~511;
        const u8 *src = g_mem + g_audioStageOffset + (consumed % g_audioStageFrames) * FLASHBACK_AUDIO_FRAME_BYTES;
        g_ring.pos = (u64)(consumed % g_audioCapFrames) * FLASHBACK_AUDIO_FRAME_BYTES;
        if (R_FAILED(IFile_Write(&g_ring, &total, src, len, 0)) || total != len)
            g_writeFailed = true;
    }

    __sync_synchronize();   // finish with the staging before handing it back
    g_audioFlushed = consumed + n;
    if (n == FLASHBACK_AUDIO_CHUNK_FRAMES)
        g_audioConsumed = consumed + n;
    return true;
}

// Writes every staged frame to its ring slot, oldest first. Only the frame's
// (compressed) bytes are written, rounded up to whole SD sectors so the card
// never has to read back a partially covered one. Staged audio goes first: its
// writes are small and its staging is the smaller margin. Runs on the writer
// thread, or inline as frames are packed if the writer couldn't be started.
static void flashbackWritePending(void)
{
    for (;;)
    {
        if (flashbackWriteAudio(g_audioFlushRequest))
            continue;

        u32 consumed = g_stageConsumed;
        if (g_stageProduced == consumed)
            return;
        __sync_synchronize();   // see the staged frame, its ring slot and size before using them

        u32 e = consumed % FLASHBACK_STAGE_ENTRIES;

        // After a failure, frames are only discarded, so the capture thread
        // can stop recording without waiting on a broken card.
        if (!g_writeFailed)
        {
            u64 total;
            u32 len = (g_stageLen[e] + 511) & ~511;     // (within the frame's space in staging: see FLASHBACK_STAGE_ALIGN)
            if (len > g_slotStride)
                len = g_slotStride;
            u64 t0 = svcGetSystemTick();

            g_ring.pos = g_videoOffset + (u64)g_stageRingIdx[e] * g_slotStride;   // pre-sized file: always an overwrite
            if (R_FAILED(IFile_Write(&g_ring, &total, g_mem + g_stageOff[e], len, 0)) || total != len)
                g_writeFailed = true;
            else
            {
                g_writeTotalUs += (u32)((svcGetSystemTick() - t0) * 1000000 / SYSCLOCK_ARM11);
                g_writeTotalFrames++;
            }
        }

        __sync_synchronize();   // finish with the frame's staging before handing it back
        g_stageConsumed = consumed + 1;
    }
}

void flashbackWriterThreadMain(void)
{
    // Keep going after termination is requested until everything staged is
    // written; the capture thread waits for that before closing the ring file.
    while (!preTerminationRequested || g_stageProduced != g_stageConsumed)
    {
        LightEvent_WaitTimeout(&g_writeEvent, 100 * 1000 * 1000LL);
        flashbackWritePending();
    }
}

// Waits until every staged frame (and audio, unfinished chunk included) has
// been written, so the ring file and the staging memory can be used or freed
// by the capture thread. Audio capture must be paused (flashbackAudioPause).
void flashbackDrainWrites(void)
{
    g_audioFlushRequest = true;
    if (!g_writerRunning)
        flashbackWritePending();
    else
    {
        while (g_stageProduced != g_stageConsumed || g_audioFlushed != g_audioProduced)
        {
            LightEvent_Signal(&g_writeEvent);
            svcSleepThread(1 * 1000 * 1000LL);
        }
    }
    g_audioFlushRequest = false;
    __sync_synchronize();
}

// ---------------------------------------------------------------------------
// Starting and stopping, capturing and packing
// ---------------------------------------------------------------------------

// Opens (creating it if needed) the ring file at its full size. Allocating the
// whole ring up front claims the SD space now rather than during gameplay, and
// a full card fails here instead of mid-recording. Contents don't matter to a
// new buffer (g_count starts at 0); reopening an existing ring keeps them.
Result flashbackOpenRing(FS_Archive archive)
{
    Result res = IFile_OpenFromArchive(&g_ring, archive, fsMakePath(PATH_ASCII, FLASHBACK_RING_FILE),
                                       FS_OPEN_CREATE | FS_OPEN_READ | FS_OPEN_WRITE);
    if (R_SUCCEEDED(res))
    {
        res = IFile_SetSize(&g_ring, g_videoOffset + (u64)g_frames * g_slotStride);
        if (R_SUCCEEDED(res))
            FSFILE_SetPriority(g_ring.handle, FLASHBACK_RING_FS_PRIORITY);
        else
            IFile_Close(&g_ring);
    }
    return res;
}

// The recorder's memory (g_memSize at FLASHBACK_MEM_ADDR) is taken in pieces of
// at most FLASHBACK_MEM_CHUNK, one after another, so it's still one range of
// addresses: a New 3DS's kernel turns down single requests of ~14 MB and more
// ("out of range") with that much memory free. Returns the first failure,
// with whatever was taken given back.
#define FLASHBACK_MEM_CHUNK     (4 * 1024 * 1024)

static Result flashbackAllocMem(u32 size)
{
    u32 tmp;
    for (u32 off = 0; off < size; off += FLASHBACK_MEM_CHUNK)
    {
        u32 n = size - off < FLASHBACK_MEM_CHUNK ? size - off : FLASHBACK_MEM_CHUNK;
        Result res = svcControlMemoryEx(&tmp, FLASHBACK_MEM_ADDR + off, 0, n,
                                        MEMOP_ALLOC | MEMOP_REGION_SYSTEM, MEMPERM_READWRITE, true);
        if (R_FAILED(res))
        {
            for (u32 o = 0; o < off; o += FLASHBACK_MEM_CHUNK)
                svcControlMemory(&tmp, FLASHBACK_MEM_ADDR + o, 0, FLASHBACK_MEM_CHUNK, MEMOP_FREE, 0);
            return res;
        }
    }
    return 0;
}

static void flashbackFreeMem(u32 size)
{
    u32 tmp;
    for (u32 off = 0; off < size; off += FLASHBACK_MEM_CHUNK)
        svcControlMemory(&tmp, FLASHBACK_MEM_ADDR + off, 0, size - off < FLASHBACK_MEM_CHUNK ? size - off : FLASHBACK_MEM_CHUNK,
                         MEMOP_FREE, 0);
}

Result flashbackStart(void)
{
    Result res;
    FS_Archive archive;

    flashbackAudioPause();

    // As much staging as there's memory for: more than the base only while
    // that leaves FLASHBACK_MEM_MARGIN free; then the base, halving down to the
    // least, then capture buffers (down to 1), and short of memory even then,
    // less audio staging.
    u64 sysFree = (u64)osGetMemRegionFree(MEMREGION_SYSTEM);
    g_stageSlots = FLASHBACK_STAGE_SLOTS_MAX;
    g_capBufs = FLASHBACK_CAPTURE_BUFS_MAX;
    g_audioStageFrames = FLASHBACK_AUDIO_STAGE_FRAMES;
    for (;;)
    {
        flashbackLayout();
        if (g_stageSlots > FLASHBACK_STAGE_SLOTS_BASE && (u64)g_memSize + FLASHBACK_MEM_MARGIN > sysFree)
        {
            g_stageSlots /= 2;
            continue;
        }
        res = flashbackAllocMem(g_memSize);
        if (R_SUCCEEDED(res))
            break;
        if (g_stageSlots > FLASHBACK_STAGE_SLOTS_MIN)
            g_stageSlots /= 2;
        else if (g_capBufs > 1)
            g_capBufs /= 2;
        else if (g_audio && g_audioStageFrames > FLASHBACK_AUDIO_STAGE_FRAMES_MIN)
            g_audioStageFrames = FLASHBACK_AUDIO_STAGE_FRAMES_MIN;
        else
            return res;
    }
    flashbackUseMem((u8 *)FLASHBACK_MEM_ADDR);

    res = FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if (R_SUCCEEDED(res))
    {
        res = flashbackCreateDirectory(archive, FLASHBACK_DIR);
        if (R_SUCCEEDED(res))
            res = flashbackOpenRing(archive);
        FSUSER_CloseArchive(archive);
    }

    if (R_FAILED(res))
    {
        flashbackFreeMem(g_memSize);
        g_mem = NULL;
        return res;
    }

    g_head = 0;
    g_count = 0;
    memset(g_repeat, 0, g_frames * sizeof(*g_repeat));
    g_pendingMissed = 0;    // (the capture queue is empty: flashbackStop drained it)
    flashbackAudioReset();
    flashbackInputReset();
    g_writeFailed = false;
    g_ringLost = false;
    g_active = true;
    flashbackResetStats();
    return 0;
}

static void flashbackFillMissed(u32 missed);
static bool flashbackPackFrame(void);

// Waits until every captured frame is packed, then gives the grid slots missed
// since the last capture to it (as copies). Before the ring's state is used for
// a save, or the memory freed.
void flashbackDrainCaptures(void)
{
    while (g_capProduced != g_capConsumed)
    {
        if (g_packRunning)
            LightEvent_Signal(&g_packEvent);
        else if (flashbackPackFrame())
            continue;
        svcSleepThread(1 * 1000 * 1000LL);
    }
    __sync_synchronize();
    flashbackFillMissed(g_pendingMissed);
    g_pendingMissed = 0;
}

void flashbackStop(void)
{
    if (!g_active)
        return;

    flashbackDrainCaptures();   // (before the writer: packing hands it frames)
    flashbackAudioPause();
    flashbackDrainWrites();     // the writer must be done with the file and slots
    IFile_Close(&g_ring);
    flashbackFreeMem(g_memSize);
    g_mem = NULL;
    g_active = false;
    g_count = 0;
}

// Compresses the oldest captured frame into staging, after the newest staged
// one, and hands it to the writer (packer thread). Returns false if staging has
// no room for it yet (the SD card is behind): try again a little later.
static bool flashbackPackFrame(void)
{
    // Room for the frame uncompressed: after the newest staged frame, or at the
    // start of staging if not before its end (the oldest frame, perhaps being
    // written, starts where the free space ends).
    u32 consumed = g_stageConsumed, staged = g_stageProduced - consumed;
    u32 need = (g_slotBytes + FLASHBACK_STAGE_ALIGN - 1) & ~(FLASHBACK_STAGE_ALIGN - 1);
    u32 off = staged == 0 ? 0 : g_stageHead;    // (nothing waiting: start over at the start)
    if (staged != 0)
    {
        u32 tail = g_stageOff[consumed % FLASHBACK_STAGE_ENTRIES];
        if (staged >= FLASHBACK_STAGE_ENTRIES)
            return false;
        if (off > tail && off + need > g_stageBytes)
            off = 0;                            // (on round to the start)
        if (off <= tail && off + need > tail)   // (off == tail with frames waiting: staging is full)
            return false;
    }
    __sync_synchronize();   // (the captured frame and its details, as the capture thread left them)

    u32 b = g_capConsumed % g_capBufs;
    u16 *frame = (u16 *)(g_mem + g_frameOffset + b * g_capStride);
    u8 *stage = g_mem + off;

    // Frames that wouldn't get smaller are stored as they are (upright). So is a
    // frame when packing is falling behind (the next frame is already captured
    // and waiting) and the SD card has time to spare (at most half as many
    // frames waiting as the base staging holds uncompressed, which leaves the
    // rest for its pauses): that trades the processor's time for the card's.
    // With the capture queue one frame from full (the next capture would be
    // dropped), up to 3/4 as many may wait. (Counted from the base, at most:
    // staging beyond it is for the card's slow spells, not for this.)
    u64 t1 = svcGetSystemTick();
    u32 capTicks1 = g_capTicksTotal;
    u32 queued = g_capProduced - g_capConsumed;
    u32 rawRoom = g_stageSlots < FLASHBACK_STAGE_SLOTS_BASE ? g_stageSlots : FLASHBACK_STAGE_SLOTS_BASE;
    bool storeRaw = queued >= 2 && (staged <= rawRoom / 2 ||
                                    (queued >= g_capBufs - 1 && staged <= rawRoom * 3 / 4));
    u32 len = storeRaw ? 0 : flashbackCompress(frame, g_w, g_h, stage, g_slotBytes);
    if (len == 0 || len >= g_slotBytes)
    {
        flashbackColumnsToRows(frame, g_w, g_h, (u16 *)stage);
        len = g_slotBytes;
        g_winRawFrames++;
    }

    // (Captures interrupt the packer: their time is the capture's, not this.)
    u64 t2 = svcGetSystemTick();
    u32 capTicks = g_capTicksTotal - capTicks1;
    g_winCaptureTicks += g_capTicks[b];
    g_winCompressTicks += t2 - t1 > capTicks ? (t2 - t1) - capTicks : 0;
    g_winPackedBytes += len;
    g_winFrames++;

    // Grid slots missed just before this frame: copies of the one before it.
    flashbackFillMissed(g_capMissedBefore[b]);

    g_repeat[g_head] = 0;
    g_slotLen[g_head] = len;
    g_slotAudioFrame[g_head] = g_capAudioFrame[b];
    u32 e = g_stageProduced % FLASHBACK_STAGE_ENTRIES;
    g_stageOff[e] = off;
    g_stageRingIdx[e] = g_head;
    g_stageLen[e] = len;
    g_stageHead = off + ((len + FLASHBACK_STAGE_ALIGN - 1) & ~(FLASHBACK_STAGE_ALIGN - 1));
    __sync_synchronize();   // frame, place, ring slot and size must be visible before the frame is handed over
    g_stageProduced++;

    g_head = (g_head + 1) % g_frames;
    if (g_count < g_frames)
        g_count++;

    __sync_synchronize();
    g_capConsumed++;        // the capture buffer is free again

    if (g_writerRunning)
        LightEvent_Signal(&g_writeEvent);
    else
        flashbackWritePending();
    return true;
}

// Captures a frame into the next free capture buffer and queues it for the
// packer (capture thread). Returns false if no frame was captured this grid
// slot (the capture failed, or every buffer still waits to be packed).
bool flashbackCaptureFrame(void)
{
    // (Without the packer thread, whatever the card has room for now.)
    if (!g_packRunning)
        while (g_capProduced != g_capConsumed && flashbackPackFrame())
            ;

    if (g_capProduced - g_capConsumed >= g_capBufs)
        return false;

    u32 b = g_capProduced % g_capBufs;
    u16 *frame = (u16 *)(g_mem + g_frameOffset + b * g_capStride);

    // Where the audio is as the frame is captured.
    u32 audioFrame = g_audioProduced;

    u64 t0 = svcGetSystemTick();
    if (!flashbackCapture(frame))
        return false;
    g_capTicks[b] = svcGetSystemTick() - t0;
    g_capTicksTotal += (u32)g_capTicks[b];
    g_capAudioFrame[b] = audioFrame;
    g_capMissedBefore[b] = g_pendingMissed;
    g_pendingMissed = 0;
    __sync_synchronize();   // the frame and its details before it's queued
    g_capProduced++;

    if (g_packRunning)
        LightEvent_Signal(&g_packEvent);
    else
        while (g_capProduced != g_capConsumed && flashbackPackFrame())
            ;
    return true;
}

// The packer thread: packs captured frames as they come.
void flashbackPackThreadMain(void)
{
    while (!preTerminationRequested)
    {
        LightEvent_WaitTimeout(&g_packEvent, 100 * 1000 * 1000LL);
        while (g_capProduced != g_capConsumed)
            if (!flashbackPackFrame())
                svcSleepThread(1 * 1000 * 1000LL);     // no room in staging yet: the card is behind
    }
}

// Fills 'missed' frame times with copies of the most recent frame.
static void flashbackFillMissed(u32 missed)
{
    if (g_count == 0 || missed == 0)
        return;

    u32 last = (g_head + g_frames - 1) % g_frames;
    u32 repeat = g_repeat[last] + missed;
    g_repeat[last] = repeat > 0xFFFF ? 0xFFFF : (u16)repeat;
}
