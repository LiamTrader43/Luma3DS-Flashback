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

// Owned by the flashback thread.
bool  g_active;                         // memory allocated + ring file open
u8   *g_mem;
IFile g_ring;
u32   g_head;                           // next ring slot to write
volatile u32 g_count;                   // valid ring slots

// Grid slots each ring slot is shown for after its own: the ones where neither
// screen had changed (nothing to store), and the ones whose frame should have
// been recorded but wasn't (the thread fell behind, or a capture failed), which
// g_missed counts. Saving repeats the slot that many extra times, so clips
// keep real-time speed. (In g_mem, like the rest of what's known about the
// slots: see flashbackUseMem.)
u16  *g_repeat;
u16  *g_missed;

// What each ring slot holds: the top screen's frame (its size, or 0 if it was
// the same as before), then the bottom screen's on the next word (the same
// way), and how each is stored (FLASHBACK_FRAME_*). A frame as big as its
// screen's pixels is stored as it is (it wouldn't have gotten smaller).
u32  *g_slotLen;
u32  *g_slotBotLen;
u8   *g_slotFlags;

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
static u8 g_capSecBuf[FLASHBACK_CAPTURE_BUFS_MAX];         // which of its buffers the second screen was captured into with it
                                                           // (FLASHBACK_NO_BUFFER: it wasn't)
static u64 g_capTicks[FLASHBACK_CAPTURE_BUFS_MAX];         // how long each capture took (stats)
static volatile u32 g_capTicksTotal;    // all captures' time, wrapping (stats: compressing leaves it out)
u32 g_pendingMissed;                    // capture thread: grid slots missed since the last capture
u64 g_capWaited;                        // how long the last capture slept, waiting out a buffer swap (flashbackCapture)
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
// (At FLASHBACK_FS_PRIORITY before it's sized: claiming that much of the card
// takes it a while.)
Result flashbackOpenRing(FS_Archive archive)
{
    Result res = IFile_OpenFromArchive(&g_ring, archive, fsMakePath(PATH_ASCII, FLASHBACK_RING_FILE),
                                       FS_OPEN_CREATE | FS_OPEN_READ | FS_OPEN_WRITE);
    if (R_SUCCEEDED(res))
    {
        FSFILE_SetPriority(g_ring.handle, FLASHBACK_FS_PRIORITY);
        res = IFile_SetSize(&g_ring, g_videoOffset + (u64)g_ringSlots * g_slotStride);
        if (R_FAILED(res))
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

    flashbackNewRing();
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

// What a reader of the ring has on each screen, once it has read the newest
// slot. Only against that frame can one go unstored for being the same, or be
// stored as what changed. Packer thread (and the capture thread once it has
// drained it).
//
// The first screen's (see FlashbackPriority) is the frame in the capture
// buffer before the one being packed (g_priRef), or not (nothing stored yet, or
// the buffers have held something else since).
//
// The second screen's is the last of its frames that was stored: not every one
// is (see flashbackPackFrame), and a reader keeps the one before. It's in the
// second screen's buffer g_secRefBuf, which the capture thread leaves alone
// (flashbackCaptureFrame), or nowhere (FLASHBACK_NO_BUFFER, as for the first
// screen's).
static bool g_priRef;
static volatile u8 g_secRefBuf = FLASHBACK_NO_BUFFER;

// The second screen's next frame is to be stored whole, in a slot of its own
// if need be: set with every FLASHBACK_KEY_SLOTS-th slot, until that's done.
// (The capture thread reads it: flashbackCapture.)
volatile bool g_secKeyDue;

// The room a frame of the second screen's is taken to need beside the first
// screen's in a slot: whole (what the last one took and an eighth, or more
// than the room that the last one tried didn't fit), and as what changed (more
// than the room that didn't do for the last one tried, against the same frame).
// With less room, it isn't tried: it would be compressed for nothing, frame
// after frame, at the first screen's expense.
static u32 g_secWholeNeed, g_secDeltaNeed;

// How much of the frame being packed is as it was in the frame before, for
// each screen (flashbackSameShare): worked out once, though packing it can
// take several tries (while staging has no room).
static bool g_cmpDone;
static u32 g_cmpPri, g_cmpSec;
static u32 g_cmpPriUpTo, g_cmpSecUpTo;  // ...and how many of its first pixels are known to be the same
static u64 g_cmpTicks;

// The capture buffers no longer hold the frame before (they're new, or a save
// built the clip's index over them): each screen's next frame is stored whole.
void flashbackForgetFrames(void)
{
    g_priRef = false;
    g_secRefBuf = FLASHBACK_NO_BUFFER;
    g_secKeyDue = true;
    g_secWholeNeed = g_secDeltaNeed = 0;
    g_cmpDone = false;
}

// An empty ring: the first frame stored starts it.
void flashbackNewRing(void)
{
    g_head = 0;
    g_count = 0;
    memset(g_repeat, 0, g_ringSlots * sizeof(*g_repeat));
    memset(g_missed, 0, g_ringSlots * sizeof(*g_missed));
    flashbackForgetFrames();
}

// As many captured frames can wait as there are buffers, less the one that
// keeps the frame before (see FLASHBACK_CAPTURE_BUFS_MAX).
static inline u32 flashbackCaptureQueue(void)
{
    return g_capBufs > 1 ? g_capBufs - 1 : 1;
}

// Whether packing is falling behind (packer thread): 'queued' frames are
// captured and not packed yet, this one included, and 'staged' staged and not
// written, the oldest of them the 'consumed'-th. It is when the next frame is
// already captured and waiting (the processor is behind), or staging is over
// half full (the SD card is). Half of all of it: a New 3DS's staging, up to
// ~25 MB, lets frames wait out a slow card for seconds first, and an Old 3DS's,
// ~3 MB, for under one. The second screen's frames give way then (see
// flashbackPackFrame), before the first screen's have to.
static bool flashbackFallingBehind(u32 queued, u32 staged, u32 consumed)
{
    if (queued >= 2)
        return true;
    if (staged == 0)
        return false;
    // (From the oldest staged frame to where the next one may go, round the
    // end if it's there: all of staging when the two meet.)
    u32 tail = g_stageOff[consumed % FLASHBACK_STAGE_ENTRIES];
    u32 used = g_stageHead > tail ? g_stageHead - tail : g_stageBytes - tail + g_stageHead;
    return used > g_stageBytes / 2;
}

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

// Stores a screen's frame at 'dst', in 'room' bytes at most: as what changed
// since 'ref', the frame before it (if 'ref' isn't NULL, and that comes out
// smaller than the frame; its first 'sameUpTo' pixels are known to be the
// same), or else, if 'whole', whole: compressed, or if 'raw' (or it wouldn't
// get smaller) as it is, upright. With 'huff' it's compressed into
// g_packedOffset first, and packed tighter with Huffman codes from there.
// Returns its size, or 0 if there's no room for it, and in *flags how it's
// stored (as the top screen's FLASHBACK_FRAME_ bits).
static u32 flashbackPackScreen(FlashbackHuffman *huff, const u16 *frame, const u16 *ref, u32 sameUpTo, u32 w, u32 h,
                               u8 *dst, u32 room, bool raw, bool whole, u32 *flags)
{
    u32 rawBytes = w * h * 2;
    *flags = 0;
    if (room < 16)
        return 0;

    if (!raw)
    {
        u8 *ops = huff != NULL ? g_mem + g_packedOffset : dst;
        u32 cap = huff != NULL || room > rawBytes ? rawBytes : room;
        u32 opsLen = ref != NULL ? flashbackCompressDelta(frame, ref, w * h, ops, cap, sameUpTo)
                                 : flashbackCompress(frame, w, h, ops, cap);
        if (opsLen != 0 && opsLen < rawBytes && opsLen <= room)
        {
            u32 len = huff != NULL ? flashbackHuffman(huff, ops, opsLen, dst) : opsLen;
            if (huff != NULL && len != 0)
                *flags = FLASHBACK_FRAME_TOP_CODES;
            else if (huff != NULL)
            {
                memcpy(dst, ops, opsLen);   // (no smaller with codes)
                len = opsLen;
            }
            if (ref != NULL)
                *flags |= FLASHBACK_FRAME_TOP_DELTA;
            return len;
        }
        if (ref != NULL)    // (what changed takes more than the frame does, or than there's room for: whole, then)
            return whole ? flashbackPackScreen(huff, frame, NULL, 0, w, h, dst, room, false, true, flags) : 0;
    }

    if (room < rawBytes)
        return 0;
    flashbackColumnsToRows(frame, w, h, (u16 *)dst);
    return rawBytes;
}

// The PC tests' way in: what became of each frame packed (see tests/flashback).
#ifndef FLASHBACK_PACKED_HOOK
#define FLASHBACK_PACKED_HOOK(priLen, secLen, ownLen, secKept, secTried) ((void)(secKept), (void)(secTried))
#endif

// Hands a slot's worth at 'off' in staging to the writer, for ring slot
// 'slot': the top screen's frame (topLen bytes; 0: none), and after it the
// bottom screen's (botLen bytes; 0: none).
static void flashbackStageSlot(u32 off, u32 slot, u32 topLen, u32 botLen, u32 flags, u32 audioFrame)
{
    u32 len = botLen != 0 ? ((topLen + 3) & ~3u) + botLen : topLen;
    g_repeat[slot] = 0;
    g_missed[slot] = 0;
    g_slotLen[slot] = topLen;
    g_slotBotLen[slot] = botLen;
    g_slotFlags[slot] = (u8)flags;
    g_slotAudioFrame[slot] = audioFrame;
    u32 e = g_stageProduced % FLASHBACK_STAGE_ENTRIES;
    g_stageOff[e] = off;
    g_stageRingIdx[e] = slot;
    g_stageLen[e] = len;
    g_stageHead = off + ((len + FLASHBACK_STAGE_ALIGN - 1) & ~(FLASHBACK_STAGE_ALIGN - 1));
    __sync_synchronize();   // frame, place, ring slot and size must be visible before the frame is handed over
    g_stageProduced++;
}

// Packs the oldest captured frame (packer thread): what's new on each screen
// goes into staging, after the newest staged frame, for the writer to put in
// the ring's next slot. Returns false if staging has no room for it yet (the
// SD card is behind): try again a little later.
//
// Each screen is first compared with the frame a reader has (the first
// screen's: the one before, which the capture buffer before this one still
// holds; the second screen's: the last of its frames that was stored, see
// g_secRefBuf):
// - The same: nothing is stored for it. If that goes for both screens, there's
//   no slot either: the newest one is shown for one more grid slot (g_repeat),
//   and nothing is compressed or written. A still screen costs a compare.
// - Much the same (FLASHBACK_DELTA_MIN_SAME): stored as what changed
//   (flashbackCompressDelta).
// - Otherwise whole, as every frame used to be.
// Every FLASHBACK_KEY_SLOTS slots, both screens are stored whole whatever they
// show, so that a clip can start there.
//
// Both screens' frames go in the same slot (the top screen's, then the bottom
// screen's), so the ring is hardly bigger for the second screen, and the SD
// card gets one write. A slot has room for the top screen's frame as it is,
// and frames mostly compress to under half, so there's usually room, the more
// so when the second screen mostly stays put and takes next to nothing. The
// first screen's frames come first, though (see FlashbackPriority):
// - A frame of the second screen's that there isn't room for isn't stored: a
//   clip shows the one before it for longer. The next one is compared with
//   that one, the one a reader still has, so it can still be stored as what
//   changed.
// - It isn't even compressed where it's unlikely to fit (g_secWholeNeed,
//   g_secDeltaNeed).
// - Nor while packing is falling behind (flashbackFallingBehind: the processor
//   or the SD card is): then it would take time or room on the card that the
//   first screen's frames need. On an Old 3DS, in Mario Kart 7, that's where
//   the frames dropped with both screens recorded were, while it dropped none
//   with the top screen alone.
// - Its whole frame, when one is due, is always stored: in a slot of its own
//   if there's no room for it (FLASHBACK_FRAME_UNSHOWN, the slot before the
//   one with the first screen's frame). So however little room the first
//   screen leaves, the second screen has a new picture every
//   FLASHBACK_KEY_SLOTS slots at least, and a clip never starts without one.
static bool flashbackPackFrame(void)
{
    __sync_synchronize();   // (the captured frame and its details, as the capture thread left them)
    u32 c = g_capConsumed, b = c % g_capBufs;
    const u16 *pri = (const u16 *)(g_mem + g_frameOffset + b * g_capStride);
    const u16 *priBefore = (const u16 *)(g_mem + g_frameOffset + ((c - 1) % g_capBufs) * g_capStride);
    bool refs = g_capBufs > 1;      // (with one buffer, the frame before is gone)
    // (The second screen's frame, if one was captured with this one, and the last one stored.)
    u32 secBuf = g_secBytes != 0 ? g_capSecBuf[b] : FLASHBACK_NO_BUFFER, secRefBuf = g_secRefBuf;
    bool haveSec = secBuf != FLASHBACK_NO_BUFFER;
    const u16 *sec = haveSec ? (const u16 *)(g_mem + g_secOffset + secBuf * g_secStride) : NULL;
    const u16 *secBefore = secRefBuf != FLASHBACK_NO_BUFFER ? (const u16 *)(g_mem + g_secOffset + secRefBuf * g_secStride) : NULL;
    u32 last = (g_head + g_ringSlots - 1) % g_ringSlots;

    // Whole frames are due with every FLASHBACK_KEY_SLOTS-th slot (and the
    // ring's first): the first screen's in it, the second screen's with its
    // next frame. (That one may take this slot for itself: the first screen's
    // frame is then in the one after, and whole if that's a
    // FLASHBACK_KEY_SLOTS-th.)
    bool key = g_count == 0 || g_head % FLASHBACK_KEY_SLOTS == 0 ||
               (haveSec && g_secKeyDue && (g_head + 1) % g_ringSlots % FLASHBACK_KEY_SLOTS == 0);
    if (key)
        g_secKeyDue = true;
    bool secWhole = haveSec && g_secKeyDue;

    if (!g_cmpDone)
    {
        u64 t0 = svcGetSystemTick();
        g_cmpPriUpTo = g_cmpSecUpTo = 0;
        g_cmpPri = refs && g_priRef ? flashbackSameShare(pri, priBefore, g_priBytes, &g_cmpPriUpTo) : 0;
        g_cmpSec = haveSec && !secWhole && secBefore != NULL ? flashbackSameShare(sec, secBefore, g_secBytes, &g_cmpSecUpTo) : 0;
        g_cmpTicks = svcGetSystemTick() - t0;
        g_cmpDone = true;
    }

    bool priSame = !key && g_cmpPri == FLASHBACK_SAME_ALL;
    bool secSame = haveSec && !secWhole && g_cmpSec == FLASHBACK_SAME_ALL;
    // (The newest slot can be shown once more: there is one, and its count isn't at its limit.)
    bool canRepeat = g_count != 0 && (u32)g_repeat[last] + g_capMissedBefore[b] < 0xFFFF;
    // (The first screen's frame is stored even if it's the same, when the
    // newest slot can't be shown again: what changed is a few bytes then.)
    bool storePri = !priSame || !canRepeat;

    // Each screen's frame as stored (0: nothing), how (as the top screen's
    // FLASHBACK_FRAME_ bits), and the second screen's whole frame where it
    // takes a slot for itself.
    u32 priLen = 0, secLen = 0, ownLen = 0, priHow = 0, secHow = 0, off = 0;
    bool secTried = false;      // (the second screen's frame was compressed, stored or not)
    u32 need = (g_slotStride + FLASHBACK_STAGE_ALIGN - 1) & ~(FLASHBACK_STAGE_ALIGN - 1);    // a slot's worth of staging
    u64 packTicks = 0;
    if (storePri || (haveSec && !secSame))
    {
        // Whole frames that wouldn't get smaller are stored as they are
        // (upright). So is the first screen's frame when packing is falling
        // behind (the next frame is already captured and waiting) and the SD
        // card has time to spare (at most half as many frames waiting as the
        // base staging holds uncompressed, which leaves the rest for its
        // pauses): that trades the processor's time for the card's. With the
        // capture queue one frame from full (the next capture would be
        // dropped), up to 3/4 as many may wait. (Counted from the base, at
        // most: staging beyond it is for the card's slow spells, not for
        // this.) The second screen's frame isn't stored then, unless its whole
        // frame is due: there's neither the time nor, mostly, the room.
        //
        // On a New 3DS, a compressed frame is packed tighter with Huffman codes
        // (a fifth less for the card to write: see flashbackHuffman),
        // compressed first into g_packedOffset for that, unless packing is
        // falling behind while the card isn't (at most 3/4 as many frames
        // waiting as the base staging holds): then the processor's time is
        // worth more. An Old 3DS's processor has no time for them: in Ocarina
        // of Time 3D, with the codes, eight times as many frames dropped (3885
        // in 30 minutes, against 456), while its SD card kept up without them.
        u32 consumed = g_stageConsumed, staged = g_stageProduced - consumed;
        u32 queued = g_capProduced - c;
        u32 rawRoom = g_stageSlots < FLASHBACK_STAGE_SLOTS_BASE ? g_stageSlots : FLASHBACK_STAGE_SLOTS_BASE;
        bool storeRaw = queued >= 2 && (staged <= rawRoom / 2 ||
                                        (queued >= flashbackCaptureQueue() && staged <= rawRoom * 3 / 4));
        bool huffman = isN3DS && !storeRaw && (queued < 2 || staged > rawRoom * 3 / 4);
        // (Each screen has codes of its own: the top screen's, then the bottom screen's.)
        FlashbackHuffman *priHuff = huffman ? &g_huff[g_priBottom ? 1 : 0] : NULL;
        FlashbackHuffman *secHuff = huffman ? &g_huff[g_priBottom ? 0 : 1] : NULL;
        // (The second screen's frame may be stored: it changed, and its whole
        // frame is due or packing isn't falling behind. That it is, when the
        // first screen's frame is stored as it is.)
        bool secTry = haveSec && !secSame && (secWhole || !flashbackFallingBehind(queued, staged, consumed));

        // Room for a slot's worth: after the newest staged frame, or at the
        // start of staging if not before its end (the oldest frame, perhaps
        // being written, starts where the free space ends). For two slots'
        // worth where the second is needed to put the frames together: for the
        // second screen's whole frame, which may stay there as a slot of its
        // own, or (the bottom screen first) for the first screen's frame, which
        // goes behind the second screen's in the slot, and so is compressed
        // apart until that one's size is known.
        bool two = storePri && (g_priBottom ? secTry : secWhole);
        u32 want = two ? 2 * need : need;
        off = staged == 0 ? 0 : g_stageHead;        // (nothing waiting: start over at the start)
        if (staged != 0)
        {
            u32 tail = g_stageOff[consumed % FLASHBACK_STAGE_ENTRIES];
            if (staged + 2 > FLASHBACK_STAGE_ENTRIES)
                return false;
            if (off > tail && off + want > g_stageBytes)
                off = 0;                            // (on round to the start)
            if (off <= tail && off + want > tail)   // (off == tail with frames waiting: staging is full)
                return false;
        }
        u8 *stage = g_mem + off, *spare = stage + need;

        u64 t1 = svcGetSystemTick();
        u32 capTicks1 = g_capTicksTotal;
        u32 how, len;

        // The first screen's frame: at the slot's start if it's the top
        // screen's, or if it's all the slot will hold.
        u8 *priAt = g_priBottom && two ? spare : stage;
        if (storePri)
        {
            bool delta = !key && !storeRaw && g_cmpPri >= FLASHBACK_DELTA_MIN_SAME;
            priLen = flashbackPackScreen(priHuff, pri, delta ? priBefore : NULL, g_cmpPriUpTo, g_priW, g_priH,
                                         priAt, g_slotStride, storeRaw, true, &priHow);
            if (priLen == g_priBytes)
                g_winRawFrames++;
        }

        // The second screen's frame, in what's left of the slot: after the
        // first screen's (the top screen first), or before it, at the slot's
        // start (the bottom screen first: then it's what's left with the first
        // screen's frame moved up behind it).
        u32 at = g_priBottom ? 0 : (priLen + 3) & ~3u;
        u32 room = g_priBottom ? (g_slotStride - priLen) & ~3u : g_slotStride - at;
        if (secWhole)
        {
            // Its whole frame is due: compressed where it has a slot's worth
            // to itself (it always fits one). If it fits beside the first
            // screen's frame too, the two go in one slot; if not, it's a slot
            // of its own. (With nothing of the first screen's, it's the
            // slot's frame.)
            u8 *own = two && !g_priBottom ? spare : stage;
            len = flashbackPackScreen(secHuff, sec, NULL, 0, g_secW, g_secH, own, g_slotStride, false, true, &how);
            secTried = true;
            if (len != 0)
                g_secWholeNeed = len + len / 8;
            secHow = how;
            if (!two || len <= room)
            {
                if (two && !g_priBottom)
                    memcpy(stage + at, own, len);
                secLen = len;
            }
            else
                ownLen = len;
        }
        else if (secTry)
        {
            // As what changed, or whole, whichever it's like and is likely to
            // fit; neither if neither is.
            bool delta = secBefore != NULL && g_cmpSec >= FLASHBACK_DELTA_MIN_SAME && room >= g_secDeltaNeed;
            bool whole = room >= g_secWholeNeed;
            if (delta || whole)
            {
                secLen = flashbackPackScreen(secHuff, sec, delta ? secBefore : NULL, g_cmpSecUpTo, g_secW, g_secH,
                                             stage + at, room, false, whole, &how);
                secTried = true;
                if (secLen != 0)
                {
                    secHow = how;
                    if (!(how & FLASHBACK_FRAME_TOP_DELTA))
                        g_secWholeNeed = secLen + secLen / 8;
                }
                else
                {
                    // (It didn't fit: not tried again in no more room than this.)
                    u32 more = room + room / 8 + 1;
                    if (delta)
                        g_secDeltaNeed = more;
                    if (whole)
                        g_secWholeNeed = more;
                }
            }
        }

        // (The bottom screen first: its frame up behind the top screen's, or
        // to the slot's start with none, unless the top screen's whole frame
        // keeps the slot for itself. Then, between the two frames: zeros.)
        if (g_priBottom && two && ownLen == 0)
            memcpy(stage + ((secLen + 3) & ~3u), spare, priLen);
        u32 firstLen = g_priBottom ? secLen : priLen;
        if (ownLen == 0 && priLen != 0 && secLen != 0)
            memset(stage + firstLen, 0, ((firstLen + 3) & ~3u) - firstLen);

        // (Captures interrupt the packer when they share the system core:
        // their time is the capture's, not this.)
        u64 t2 = svcGetSystemTick();
        u32 capTicks = g_packOnGameCore ? 0 : g_capTicksTotal - capTicks1;
        packTicks = t2 - t1 > capTicks ? (t2 - t1) - capTicks : 0;
    }

    bool secStored = secLen != 0 || ownLen != 0;
    bool secKept = haveSec && (secSame || secStored);       // a reader has the second screen's frame
    g_winCaptureTicks += g_capTicks[b];
    g_winCompressTicks += g_cmpTicks + packTicks;
    g_winPackedBytes += priLen + secLen + ownLen;
    g_winFrames++;

    // Grid slots missed just before this frame: copies of the one before it.
    flashbackFillMissed(g_capMissedBefore[b]);

    // In a slot, the top screen's frame comes first, whichever screen does.
    u32 topLen = g_priBottom ? secLen : priLen, botLen = g_priBottom ? priLen : secLen;
    u32 flags = g_priBottom ? secHow | priHow << 2 : priHow | secHow << 2;
    if (priLen == 0 && secLen == 0)
        g_repeat[last]++;   // nothing new to store: the newest slot, once more
    else if (ownLen == 0)
    {
        flashbackStageSlot(off, g_head, topLen, botLen, flags, g_capAudioFrame[b]);
        g_head = (g_head + 1) % g_ringSlots;
        g_count = g_count + 1 < g_ringSlots ? g_count + 1 : g_ringSlots;
    }
    else
    {
        // The second screen's whole frame in a slot of its own: the slot
        // before this frame's in the ring, so a reader has it by then. In
        // staging (handed over in the order it's used) it's in the second
        // slot's worth if it's the bottom screen's, and the first if it's the
        // top screen's.
        u32 next = (g_head + 1) % g_ringSlots;
        if (g_priBottom)
        {
            flashbackStageSlot(off, g_head, ownLen, 0, secHow | FLASHBACK_FRAME_UNSHOWN, g_capAudioFrame[b]);
            flashbackStageSlot(off + need, next, 0, priLen, priHow << 2, g_capAudioFrame[b]);
        }
        else
        {
            flashbackStageSlot(off, next, priLen, 0, priHow, g_capAudioFrame[b]);
            flashbackStageSlot(off + need, g_head, 0, ownLen, secHow << 2 | FLASHBACK_FRAME_UNSHOWN, g_capAudioFrame[b]);
        }
        g_head = (g_head + 2) % g_ringSlots;
        g_count = g_count + 2 < g_ringSlots ? g_count + 2 : g_ringSlots;
    }

    FLASHBACK_PACKED_HOOK(priLen, secLen, ownLen, secKept, secTried);

    // What a reader has now is this frame, which the next one is compared with.
    // The second screen's too if it was stored: its buffer is the one kept
    // from now on, and the one kept so far is free. Otherwise (the same, not
    // stored, or none captured) a reader keeps the one it has, and so does the
    // buffer that one is in.
    g_priRef = refs;
    if (secStored)
    {
        g_secRefBuf = refs ? (u8)secBuf : FLASHBACK_NO_BUFFER;
        g_secDeltaNeed = 0;
        if (secWhole)
            g_secKeyDue = false;
    }
    g_cmpDone = false;
    __sync_synchronize();   // (the buffer kept, before the capture thread sees this frame's as free: flashbackCaptureFrame)
    g_capConsumed++;        // the capture buffer before this one is free again

    if (priLen != 0 || secLen != 0)
    {
        if (g_writerRunning)
            LightEvent_Signal(&g_writeEvent);
        else
            flashbackWritePending();
    }
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

    u32 consumed = g_capConsumed;
    if (g_capProduced - consumed >= flashbackCaptureQueue())
        return false;

    u32 b = g_capProduced % g_capBufs;
    u16 *frame = (u16 *)(g_mem + g_frameOffset + b * g_capStride);

    // The second screen's frame goes into one of its buffers that holds
    // neither a frame waiting to be packed nor the one the packer keeps
    // (g_secRefBuf). That's read after g_capConsumed: the packer sets it
    // before it lets a frame go, so the one it's seen keeping here is kept
    // from before the frames seen waiting (which hold any it keeps after).
    u32 secBuf = FLASHBACK_NO_BUFFER;
    if (g_secBytes != 0)
    {
        __sync_synchronize();
        u32 kept = g_secRefBuf, used = kept != FLASHBACK_NO_BUFFER ? 1u << kept : 0;
        for (u32 i = consumed; i != g_capProduced; i++)
            if (g_capSecBuf[i % g_capBufs] != FLASHBACK_NO_BUFFER)
                used |= 1u << g_capSecBuf[i % g_capBufs];
        for (secBuf = 0; secBuf < g_capBufs && (used >> secBuf & 1) != 0; secBuf++)
            ;
        if (secBuf == g_capBufs)
            secBuf = FLASHBACK_NO_BUFFER;   // (never: fewer frames wait than there are buffers)
    }
    u16 *second = secBuf != FLASHBACK_NO_BUFFER ? (u16 *)(g_mem + g_secOffset + secBuf * g_secStride) : NULL;
    bool secondOk = false;

    // Where the audio is as the frame is captured (after waiting out a buffer
    // swap that would land mid-read, if one's due: see flashbackCaptureWait).
    flashbackCaptureWait();
    u32 audioFrame = g_audioProduced;

    u64 t0 = svcGetSystemTick();
    if (!flashbackCapture(frame, second, &secondOk))
        return false;
    g_capSecBuf[b] = secondOk ? (u8)secBuf : FLASHBACK_NO_BUFFER;
    g_capTicks[b] = svcGetSystemTick() - t0 - g_capWaited;      // (the time it took the processor)
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

    u32 last = (g_head + g_ringSlots - 1) % g_ringSlots;
    u32 repeat = g_repeat[last] + missed, was = g_missed[last] + missed;
    g_repeat[last] = repeat > 0xFFFF ? 0xFFFF : (u16)repeat;
    g_missed[last] = was > 0xFFFF ? 0xFFFF : (u16)was;
}
