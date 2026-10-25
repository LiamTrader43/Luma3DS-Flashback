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

// Flashback: saving clips (see flashback_internal.h).

#include <3ds.h>
#include <string.h>
#include "flashback_internal.h"
#include "fmt.h"
#include "utils.h"

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

FlashbackClipPlan flashbackPlanClip(void)
{
    FlashbackClipPlan plan = { 0, 0, 0 };
    u32 frameTimes = 0;

    // Walk back from the newest slot until the slots (with their repeats)
    // cover the clip length; the oldest one may be partly cut.
    while (plan.slots < g_count && frameTimes < g_frames)
    {
        u32 idx = (g_head + g_frames - 1 - plan.slots) % g_frames;
        frameTimes += 1 + g_repeat[idx];
        plan.slots++;
    }

    plan.trimFirst = frameTimes > g_frames ? frameTimes - g_frames : 0;
    plan.frames = frameTimes - plan.trimFirst;
    return plan;
}

static inline u32 flashbackPlanSlot(const FlashbackClipPlan *plan, u32 i, u32 *copies)
{
    u32 idx = (g_head + g_frames - plan->slots + i) % g_frames;
    *copies = 1 + g_repeat[idx] - (i == 0 ? plan->trimFirst : 0);
    return idx;
}

// The oldest audio frame the ring still holds: all but the oldest chunk, which
// a partly written chunk (see flashbackWriteAudio) may have overwritten.
static u32 flashbackAudioOldest(void)
{
    u32 held = g_audioCapFrames - FLASHBACK_AUDIO_CHUNK_FRAMES;
    return g_audioFlushed > held ? g_audioFlushed - held : 0;
}

// Saved audio positions (in samples) count from this audio frame on: a whole
// number of rings back, so sample i is still at (i % capacity) in the ring,
// and one ring before the oldest frame held, so a frame whose audio is older
// than that still gets a position (where the audio reads as silence). Counted
// from the ring's start, they'd pass 2^32 after ~36 hours of recording.
static u32 flashbackAudioBase(void)
{
    u32 oldest = flashbackAudioOldest();
    return oldest >= g_audioCapFrames ? (oldest / g_audioCapFrames - 1) * g_audioCapFrames : 0;
}

// The saved audio position of audio frame 'frame' (counted since the ring started).
static u32 flashbackAudioSampleOf(u32 frame)
{
    u32 base = flashbackAudioBase();
    return frame > base ? (frame - base) * FLASHBACK_AUDIO_FRAME_SAMPLES : 0;
}

// Clip file format (<clip>.flb): the ring file itself, renamed, with an index
// and a trailer appended. Saving this way copies no frame or audio data, so it
// takes a few seconds however long the clip is; the PC-side converter
// (convert_flashback.bat, in https://github.com/LiamTrader43/Flashback-Studio)
// decodes it into a video, and Flashback Studio plays it on the 3DS. Keep
// those readers in step with any change here.
//
// Version 1, recorded without audio:
//   [ring slots]    slot i at offset i * slotStride: a compressed frame, or the
//                   raw frame if its size is slotBytes (see flashbackCompress)
//   [index]         entryCount x { u32 slot, u32 size, u32 copies }: the clip's
//                   frames oldest first, each slot shown 'copies' times
//   [trailer]       FlashbackClipTrailer, the last 48 bytes of the file
//
// Version 3, recorded with audio:
//   [audio ring]    audio.capacity stereo PCM16 samples: sample i (counting from
//                   when the ring started) at audio.offset + (i % capacity) * 4
//   [ring slots]    as in version 1, after the audio ring (from a 64 KB boundary)
//   [index]         entryCount x { u32 offset, u32 size, u32 copies, u32 audioPos }:
//                   as in version 1, but with each frame's offset in the file,
//                   and the audio sample at which it's first shown (each copy
//                   comes sampleRate / fps samples after the one before)
//   [audio info]    FlashbackClipAudioInfo (32 bytes), including which samples
//                   the ring holds (first .. end - 1)
//   [trailer]       FlashbackClipTrailer, as in version 1
//
// (Version 2 is a clip without audio, compacted by Flashback Studio.)
// All values little-endian. Frames are RGB565, top row first.

#define FLASHBACK_CLIP_MAGIC            0x4B424C46  // "FLBK"
#define FLASHBACK_CLIP_VERSION          1
#define FLASHBACK_CLIP_VERSION_AUDIO    3

typedef struct FlashbackClipTrailer {
    u32 magic;
    u32 version;
    u32 width, height, fps;
    u32 slotStride;     // bytes between ring slots
    u32 slotBytes;      // size of an uncompressed frame
    u32 entryCount;     // index entries
    u32 frameCount;     // frames in the clip, copies included
    u32 indexOffset;    // where the index starts
    u32 reserved[2];
} FlashbackClipTrailer;
_Static_assert(sizeof(FlashbackClipTrailer) == 48, "the clip trailer is 48 bytes");

typedef struct FlashbackClipAudioInfo {
    u32 sampleRate;     // in millihertz
    u16 channels;       // 2
    u16 bitsPerSample;  // 16
    u32 offset;         // where the audio ring starts
    u32 capacity;       // samples the ring has room for: sample i is at offset + (i % capacity) * 4
    u32 first;          // the oldest sample held
    u32 end;            // one past the newest sample held
    u32 missed;         // audio frames (of 160 samples) filled with silence because the recorder missed them...
    u32 full;           // ...of which because its buffer was full (the SD card was busy)
} FlashbackClipAudioInfo;
_Static_assert(sizeof(FlashbackClipAudioInfo) == 32, "the clip audio info is 32 bytes");

// Builds the clip's index, audio info (with audio) and trailer into 'buf';
// returns their size.
static u32 flashbackBuildClipIndex(const FlashbackClipPlan *plan, u32 indexOffset, u8 *buf)
{
    u32 *entry = (u32 *)buf;
    for (u32 i = 0; i < plan->slots; i++)
    {
        u32 copies;
        u32 idx = flashbackPlanSlot(plan, i, &copies);
        if (g_audio)
        {
            *entry++ = g_videoOffset + idx * g_slotStride;
            *entry++ = g_slotLen[idx];
            *entry++ = copies;
            // (The oldest entry may start a few copies in: see flashbackPlanClip.)
            *entry++ = flashbackAudioSampleOf(g_slotAudioFrame[idx]) + flashbackAudioSamplesFor(i == 0 ? plan->trimFirst : 0);
        }
        else
        {
            *entry++ = idx;
            *entry++ = g_slotLen[idx];
            *entry++ = copies;
        }
    }

    if (g_audio)
    {
        FlashbackClipAudioInfo *a = (FlashbackClipAudioInfo *)entry;
        memset(a, 0, sizeof(*a));
        a->sampleRate = FLASHBACK_AUDIO_RATE_MHZ;
        a->channels = 2;
        a->bitsPerSample = 16;
        a->offset = 0;
        a->capacity = g_audioCapFrames * FLASHBACK_AUDIO_FRAME_SAMPLES;
        a->end = flashbackAudioSampleOf(g_audioFlushed);
        a->first = flashbackAudioSampleOf(flashbackAudioOldest());
        a->missed = g_audioMissed;
        a->full = g_audioFull;
        entry = (u32 *)(a + 1);
    }

    FlashbackClipTrailer *t = (FlashbackClipTrailer *)entry;
    memset(t, 0, sizeof(*t));
    t->magic = FLASHBACK_CLIP_MAGIC;
    t->version = g_audio ? FLASHBACK_CLIP_VERSION_AUDIO : FLASHBACK_CLIP_VERSION;
    t->width = g_w;
    t->height = g_h;
    t->fps = g_fps;
    t->slotStride = g_slotStride;
    t->slotBytes = g_slotBytes;
    t->entryCount = plan->slots;
    t->frameCount = plan->frames;
    t->indexOffset = indexOffset;
    return (u32)((u8 *)(t + 1) - buf);
}

// The clip's index is built in staging, at least FLASHBACK_STAGE_SLOTS_MIN
// frames' worth (and empty while saving: see flashbackSave).
_Static_assert(FLASHBACK_MAX_FRAMES * 16 + sizeof(FlashbackClipAudioInfo) + sizeof(FlashbackClipTrailer) <=
                   FLASHBACK_STAGE_SLOTS_MIN * 200 * 120 * 2,
               "a clip's index fits in staging at half resolution");

// Saves the clip by turning the ring file into it, then starts a fresh ring.
// Needs the writer to be idle (flashbackDrainWrites).
static s32 flashbackSaveClipFile(FS_Archive archive, const char *clip, const FlashbackClipPlan *plan)
{
    char path[128];
    u64 total;
    IFile file = {0};
    sprintf(path, "%s.flb", clip);

    // The ring always starts at slot 0, so until it has wrapped once the clip
    // only uses its first g_count slots: the rest can be trimmed off.
    u32 dataEnd = g_videoOffset + (g_count < g_frames ? g_count : g_frames) * g_slotStride;
    u8 *index = g_mem;                  // staging: empty while saving (everything's written)
    u32 indexSize = flashbackBuildClipIndex(plan, dataEnd, index);

    IFile_Close(&g_ring);
    if (R_FAILED(FSUSER_RenameFile(archive, fsMakePath(PATH_ASCII, FLASHBACK_RING_FILE),
                                   archive, fsMakePath(PATH_ASCII, path))))
    {
        // Nothing moved: keep recording into the same ring, buffer intact.
        if (R_FAILED(flashbackOpenRing(archive)))
            g_ringLost = true;
        return -2;
    }

    s32 result = (s32)plan->frames;
    bool ok = R_SUCCEEDED(IFile_OpenFromArchive(&file, archive, fsMakePath(PATH_ASCII, path), FS_OPEN_WRITE)) &&
              R_SUCCEEDED(IFile_SetSize(&file, dataEnd));
    if (ok)
    {
        file.pos = dataEnd;
        ok = R_SUCCEEDED(IFile_Write(&file, &total, index, indexSize, 0)) && total == indexSize;
    }
    IFile_Close(&file);
    if (!ok)
    {
        // Without its index the clip can't be decoded: don't leave it behind.
        FSUSER_DeleteFile(archive, fsMakePath(PATH_ASCII, path));
        result = -3;
    }

    // The old buffer is now the clip: continue with an empty one.
    g_head = 0;
    g_count = 0;
    memset(g_repeat, 0, sizeof(g_repeat));
    flashbackAudioReset();
    if (R_FAILED(flashbackOpenRing(archive)))
        g_ringLost = true;  // no space for a new ring: the capture thread stops recording
    return result;
}

// Returns the number of frames saved, or a negative value:
//   -1 nothing recorded, -2 couldn't create the clip file, -3 couldn't write the clip's index.
s32 flashbackSave(void)
{
    if (g_active)
        flashbackDrainCaptures();   // every captured frame packed, before the ring's state is used
    if (!g_active || g_count == 0)
        return -1;

    // Frames and audio still waiting to be written belong at the end of the
    // clip, and the writer must be done with the ring file before it becomes
    // the clip. (Audio capture resumes along with recording, once the save is done.)
    flashbackAudioPause();
    flashbackDrainWrites();

    FS_Archive archive;
    if (R_FAILED(FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
        return -2;
    if (R_FAILED(flashbackCreateDirectory(archive, FLASHBACK_DIR)))
    {
        FSUSER_CloseArchive(archive);
        return -2;
    }

    char dateTimeStr[32];
    char clipName[48];
    char clip[80];
    dateTimeToString(dateTimeStr, osGetTime(), true);
    sprintf(clipName, "clip_%s", dateTimeStr);
    sprintf(clip, FLASHBACK_DIR "/%s", clipName);

    FlashbackClipPlan plan = flashbackPlanClip();
    s32 result = flashbackSaveClipFile(archive, clip, &plan);

    FSUSER_CloseArchive(archive);
    if (result > 0)
        sprintf(g_lastClip, "%s.flb", clip);
    return result;
}
