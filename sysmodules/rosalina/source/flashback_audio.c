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

// Flashback: recording the game's audio (see flashback_internal.h).

#include <3ds.h>
#include <string.h>
#include "flashback_internal.h"
#include "menu.h"
#include "utils.h"

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
//
// The DSP mixes everything the game plays 160 samples at a time (an "audio
// frame", ~4.9 ms at its 32728 Hz rate, SYSCLOCK_ARM11 / 8192) and leaves each
// mix ("final mix samples": 160 stereo PCM16 samples, what the speakers get)
// in one of the two shared memory regions it uses with the game, at physical
// 0x1FF50000 and 0x1FF70000. The two take turns: once the DSP has written a
// region's frame, the game advances that region's frame counter (its last
// u16), so the region with the newer counter holds the newest complete frame,
// and the DSP doesn't write that region again for two frames (~9.8 ms).
// (Layout from Citra/Azahar's notes on the DSP firmware. Checked on a New 3DS
// with a homebrew app and in Ocarina of Time 3D: polling about once a
// millisecond read every frame back complete and in order.)
//
// The audio thread copies each new frame into staging memory, and the writer
// thread writes staged frames to an audio ring at the start of the ring file,
// so a save is still just a rename. Each video frame records how many audio
// samples had been captured when it was, which places it in the audio. Frames
// the audio thread misses, and stretches where the DSP produces none (no game
// audio running), are filled with silence, so the audio keeps real time like
// the video.

#define FLASHBACK_DSP_REGION0           0x1FF50000
#define FLASHBACK_DSP_REGION1           0x1FF70000
#define FLASHBACK_DSP_FINAL_OFFSET      0xA80       // DSP address 0x8540 (word) in region 0
#define FLASHBACK_DSP_COUNTER_OFFSET    0x7FFE      // DSP address 0xBFFF: the frame counter

volatile bool g_wantAudio = true;           // chosen in the menu
volatile bool g_audio;                      // in effect (part of the recording format)
u32 g_audioCapFrames;                       // audio ring capacity in frames (0 without audio)
u32 g_audioBytes;                           // the audio ring's size
u32 g_videoOffset;                          // where the video slots start in the ring file (flashbackVideoOffset)
u32 g_audioStageOffset;                     // audio staging memory, in g_mem...
u32 g_audioStageFrames;                     // ...and its size in frames (a whole number of chunks)

// Audio frames since the ring started: captured (audio thread), written in
// whole chunks (writer; frees their staging), and on the SD card (writer; ahead
// of g_audioConsumed after an unfinished chunk was written for a save).
volatile u32 g_audioProduced, g_audioConsumed, g_audioFlushed;
volatile u32 g_audioMissed;                 // frames filled with silence because the thread missed them
volatile u32 g_audioFull;                   // ...of which because the staging memory was full (the SD card was busy)
static volatile bool g_audioCapture;        // capture thread -> audio thread: capture now...
static volatile u64 g_audioCaptureSince;    // ...since this tick...
static volatile u64 g_audioCapturedTicks;   // ...after this much time captured before (since the ring started)
static volatile bool g_audioIdle = true;    // audio thread -> capture thread: not capturing
volatile bool g_audioFlushRequest;          // writer: write unfinished chunks too (for a save)

// Audio position (audio frames since the ring started) of each ring slot's frame.
u32   g_slotAudioFrame[FLASHBACK_MAX_FRAMES];

// Turns audio capture on or off. The audio thread keeps the audio in step with
// the time spent capturing since the ring started, which is the time the video
// covers too: both skip the pauses in between.
void flashbackAudioSetCapture(bool on)
{
    if (on == g_audioCapture)
        return;
    u64 now = svcGetSystemTick();
    if (on)
        g_audioCaptureSince = now;
    else
        g_audioCapturedTicks += now - g_audioCaptureSince;
    __sync_synchronize();   // (the audio thread reads these once it sees the change)
    g_audioCapture = on;
}

// Stops audio capture, and waits until the audio thread is no longer touching
// the staging memory or the audio counters.
void flashbackAudioPause(void)
{
    flashbackAudioSetCapture(false);
    __sync_synchronize();
    while (g_audioRunning && !g_audioIdle)
        svcSleepThread(1 * 1000 * 1000LL);
    __sync_synchronize();
}

// An empty audio buffer (with capture paused).
void flashbackAudioReset(void)
{
    g_audioProduced = g_audioConsumed = g_audioFlushed = 0;
    g_audioMissed = 0;
    g_audioFull = 0;
    g_audioCapturedTicks = 0;
}

// Audio frames in 'ticks' of time, to the nearest frame.
static inline u32 flashbackAudioFramesIn(u64 ticks)
{
    return (u32)((ticks + FLASHBACK_AUDIO_FRAME_TICKS / 2) / FLASHBACK_AUDIO_FRAME_TICKS);
}

static inline bool flashbackCounterNewer(u16 a, u16 b)
{
    return a != b && (u16)(a - b) < 0x8000;
}

// Appends one audio frame to the staging memory: a copy of 'src', or silence
// if it's NULL. Returns false if the staging memory is full (the writer is
// behind): the frame is lost, and the time it covered is filled in later.
static bool flashbackAppendAudio(const u32 *src)
{
    u32 produced = g_audioProduced;
    if (produced - g_audioConsumed >= g_audioStageFrames)
    {
        g_audioFull++;
        return false;
    }

    u32 *dst = (u32 *)(g_mem + g_audioStageOffset + (produced % g_audioStageFrames) * FLASHBACK_AUDIO_FRAME_BYTES);
    if (src != NULL)
        memcpy(dst, src, FLASHBACK_AUDIO_FRAME_BYTES);
    else
        memset(dst, 0, FLASHBACK_AUDIO_FRAME_BYTES);
    __sync_synchronize();   // the frame must be visible before it's counted
    g_audioProduced = produced + 1;

    if ((produced + 1) % FLASHBACK_AUDIO_CHUNK_FRAMES == 0 && g_writerRunning)
        LightEvent_Signal(&g_writeEvent);
    return true;
}

// Captures the DSP's output while the capture thread asks for it. Reads go
// through Luma's uncached mapping of physical memory (PA_PTR), so there's no
// cache to manage. A "run" is a stretch of capturing between pauses (recording
// off, a Rosalina menu open, a save): the audio, like the video, skips pauses.
// It's kept in step with the time captured since the ring started ('ticks'),
// so that it skips exactly as much as the video does each time.
void flashbackAudioThreadMain(void)
{
    const u8 *region[2] = { (const u8 *)PA_PTR(FLASHBACK_DSP_REGION0), (const u8 *)PA_PTR(FLASHBACK_DSP_REGION1) };
    u32 block[FLASHBACK_AUDIO_FRAME_BYTES / 4];
    bool capturing = false, align = false;
    u16 last = 0;
    u64 lastTick = 0, ticks = 0, behindSince = 0;

    while (!preTerminationRequested)
    {
        // (The capture thread clears g_audioCapture and then waits for
        // g_audioIdle; checking g_audioCapture again after clearing g_audioIdle
        // means it can never miss an iteration that's under way.)
        if (!g_audioCapture)
        {
            // A run ended. The video covers it to the end, but the audio can
            // stop short of that: the next frame was on its way, or the game
            // had stopped playing audio first (it's frozen as soon as a
            // Rosalina menu opens, and recording stops at the next video
            // frame). Make that up with silence, so the next run starts in step.
            if (capturing)
            {
                u32 end = flashbackAudioFramesIn(g_audioCapturedTicks);
                while (g_audioProduced < end && flashbackAppendAudio(NULL))
                    ;
                capturing = false;
            }
            g_audioIdle = true;
            svcSleepThread(10 * 1000 * 1000LL);     // (not recording: nothing to keep up with)
            continue;
        }
        g_audioIdle = false;
        __sync_synchronize();
        if (!g_audioCapture)
            continue;

        u64 now = svcGetSystemTick();
        u16 c0 = *(const vu16 *)(region[0] + FLASHBACK_DSP_COUNTER_OFFSET);
        u16 c1 = *(const vu16 *)(region[1] + FLASHBACK_DSP_COUNTER_OFFSET);
        u32 r = flashbackCounterNewer(c0, c1) ? 0 : 1;
        u16 top = r == 0 ? c0 : c1;

        if (!capturing)
        {
            // A new run: take frames from the next one on. The video started
            // when recording did (or resumed), and this thread can notice up
            // to ~10 ms later; the first frame lines the audio up with the
            // time captured (below).
            capturing = true;
            align = true;
            last = top;
            lastTick = now;
            ticks = g_audioCapturedTicks + (now - g_audioCaptureSince);
            behindSince = 0;
            svcSleepThread(1 * 1000 * 1000LL);
            continue;
        }

        ticks += now - lastTick;
        lastTick = now;
        u32 wanted = (u32)(ticks / FLASHBACK_AUDIO_FRAME_TICKS);   // frames there should be by now
        bool got = false;

        if (flashbackCounterNewer(top, last))
        {
            u16 step = (u16)(top - last);
            const vu32 *src = (const vu32 *)(region[r] + FLASHBACK_DSP_FINAL_OFFSET);
            for (u32 i = 0; i < FLASHBACK_AUDIO_FRAME_BYTES / 4; i++)
                block[i] = src[i];

            // The frame stays put until the counters have moved on twice more;
            // if they have (this thread was held up mid-copy), it may be torn.
            u16 d0 = *(const vu16 *)(region[0] + FLASHBACK_DSP_COUNTER_OFFSET);
            u16 d1 = *(const vu16 *)(region[1] + FLASHBACK_DSP_COUNTER_OFFSET);
            u16 top2 = flashbackCounterNewer(d0, d1) ? d0 : d1;
            bool torn = flashbackCounterNewer(top2, top) && (u16)(top2 - top) >= 2;

            // Frames missed since the last one become silence; a big jump
            // (the DSP restarted, ...) is left to the time check below. Never
            // more than real time allows, in case the DSP had stopped for a
            // while and the time check already filled that in.
            u32 have = g_audioProduced;
            u32 room = wanted + 1 > have + 1 ? wanted - have : 0;
            u32 fill = step > 1 && step <= 16 ? step - 1u : 0;
            for (u32 i = 0; i < fill && i < room && flashbackAppendAudio(NULL); i++)
                g_audioMissed++;

            // Lining up (the run's first frame, or the audio fell behind):
            // with this frame, the audio should reach the time captured, to
            // the nearest frame (this one has only just arrived). Silence
            // makes up the difference.
            if (align)
            {
                u32 target = flashbackAudioFramesIn(ticks);
                while (g_audioProduced + 1 < target && flashbackAppendAudio(NULL))
                    ;
                align = false;
                behindSince = 0;
            }
            if (!flashbackAppendAudio(torn ? NULL : block) || torn)
                g_audioMissed++;
            last = top;
            got = true;
        }
        else if (top != last)
        {
            // The counters went backwards: the game restarted its audio (Ocarina
            // of Time 3D seems to, when loading), and they start over from a low
            // count. Carry on from there; waiting for them to pass the old count
            // again would miss up to 160 s of audio.
            last = top;
        }

        // No frames from the DSP (the game isn't playing audio, or isn't
        // running): keep real time with silence.
        while (g_audioProduced + 3 < wanted && flashbackAppendAudio(NULL))
            ;

        // That leaves the audio up to 3 frames behind real time, for as long as
        // the run lasts (frames come at exactly real time after that), which
        // would put the sound up to 15 ms early in the clip. Normally it's at
        // most a frame behind (the next one is on its way); 2 or more for over
        // 20 ms means frames went missing (the DSP paused, or restarted), so
        // line up again. (A late wake-up doesn't count: the frames it skipped
        // are filled in by 'step' above.)
        if (g_audioProduced + 1 < wanted)
        {
            if (behindSince == 0)
                behindSince = now;
            else if (now - behindSince > SYSCLOCK_ARM11 / 50)
                align = true;
        }
        else
            behindSince = 0;

        // Until the next frame lines it up, silence keeps the audio at the
        // time captured (less the frame on its way), so that a silent stretch
        // (the HOME menu, a loading screen) is in step too.
        if (align)
        {
            u32 target = flashbackAudioFramesIn(ticks);
            while (g_audioProduced + 1 < target && flashbackAppendAudio(NULL))
                ;
        }

        // The next frame is due ~4.9 ms after this one: sleep most of that.
        svcSleepThread(got ? 3500 * 1000LL : 1000 * 1000LL);
    }
    g_audioIdle = true;
}
