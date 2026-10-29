# Flashback recorder: design notes

This page collects the measurements behind the Flashback recorder's numbers and
rules, and what was tried before. The code's comments say what it does and give
the reason in a clause. Where the story behind a number is longer, they point
to a section here. The code is in `sysmodules/rosalina/source/flashback*.c` and
the two `.s` files. `sysmodules/rosalina/include/flashback_internal.h` lists the
files and what they share.

## How it fits together

Frames and audio go through these stages:

1. **Capture.** On a fixed time grid, the capture thread reads the top screen's
   framebuffer into one of a few capture buffers, converted to RGB565
   (`flashback_framebuffer.c`).
2. **Packing.** The packer thread compresses each captured frame into staging
   memory (`flashback_compress.c`, `flashback_record.c`).
3. **Writing.** The writer thread writes the staged frames to their slots in a
   ring file on the SD card (`flashback_record.c`).
4. **Audio.** The audio thread copies the DSP's output into audio staging, and
   the writer writes it in chunks to an audio ring at the start of the same
   file (`flashback_audio.c`).
5. **Inputs.** The capture thread also keeps a record of the inputs for every
   grid slot, in memory (`flashback_input.c`).
6. **Saving.** A save renames the ring file to the clip, appends the inputs and
   an index, and starts a new ring (`flashback_clip.c`).

Each stage hands work to the next through counters that only go up. So a slow
stage makes the work wait in memory instead of holding up the stages before it.
A grid slot that gets no frame becomes a copy of the frame before it, so clips
always play at the right speed.

## Threads and priorities

*Code: `flashbackCreateThread`.*

The capture, writer and packer threads have the lowest priorities among
Rosalina's threads:

- **The packer** is at the bottom. It does nearly all the work (compressing).
  On an Old 3DS it runs on the game's core, at 63, below everything there (see
  below). On a New 3DS it runs on the system core, at 60.
- **The capture thread** is one step above it, at 59, on the system core. When
  they share that core, it interrupts the packer for the few ms a capture
  takes, so it captures on time.
- **The writer** is level with the capture thread. It hardly uses the CPU
  (it mostly waits on the SD card), and running first means it sends the next
  write the moment the previous one finishes.
- **The audio thread** runs well above them, at 25. It uses very little CPU, but
  it has to catch each audio frame within the ~8 ms it stays put.

Other arrangements were tried:

- **The packer a step lower still, at 61.** Other threads got ahead of it too,
  and an Old 3DS compressed ~30% slower, too slow to keep up.
- **The writer below the packer.** It had to wait out each frame's compression
  with the card idle. An SD speed test on a New 3DS XL put a ~100 KB write at
  ~20 ms, but recording measured 31-36 ms per write.
- **The writer a step above the capture thread, at 58**, so a capture (~10 ms
  in Ocarina of Time 3D on an Old 3DS) can't hold up the next write. It wrote
  no faster there: the time per write is the card's.

**The packer on the game's core** (Old 3DS). An Old 3DS's system core has
little time to spare while a game runs: the recorder got about a third of it in
Mario Kart 7. In Ocarina of Time 3D, compressing there fell behind, so many
frames were stored uncompressed, which then overloaded the SD card too. A test
build could switch the packer between the cores while recording. Over 30 s at
the same spot, at full resolution and 30 fps with sound:

| Packer on                 | System core | Game's core |
| ------------------------- | ----------- | ----------- |
| Frames dropped            | 100         | 0           |
| Stored uncompressed       | 48%         | 0%          |
| The game's own frame rate | 29.3 fps    | 29.9 fps    |

At the lowest priority there, the packer only runs while the game's threads on
that core wait (for the GPU, or the screen's next refresh), so the game didn't
slow down. Mario Kart 7, which runs at 60 fps, recorded fine that way too.

A New 3DS's system core nearly keeps up on its own. The same test on a New 3DS,
in Ocarina of Time 3D:

| Packer on                 | System core | Game's core |
| ------------------------- | ----------- | ----------- |
| Frames dropped            | 38          | 0 in 10 min |
| Time compressing a frame  | 21 ms       | 16 ms       |
| The game's own frame rate | 30.0 fps    | 29.9 fps    |

(Capture got slower that way, reads of 9.9 ms instead of 4.8, with compressing
on the other core competing for memory.) But Super Smash Bros. for Nintendo 3DS
then dropped 28 frames in 60 s, where an earlier session with the packer on the
system core had dropped none, and an Old 3DS dropped none in it on either core.
A game can leave a New 3DS's game core much less idle time (games can use its
extra power), so the packer stays on the system core there.

## SD card writes

**Compression makes full resolution fit.** On a New 3DS XL, the SD path tops out
around 5 MB/s. Full resolution at 30 fps needs 5.8 MB/s uncompressed.

**One request per frame.** Batching several frames per SD request was tried,
and didn't help: the time is in moving the bytes, not in each request.

**FS priority** (`FLASHBACK_FS_PRIORITY`). This was measured on a New 3DS XL
at full resolution and 30 fps. The write time per frame was the same at low and
normal priority (~36 ms), and slightly worse at real-time priority (~41 ms). So
the lowest priority costs nothing, and it keeps the ring file's constant writes
from getting ahead of the game. An Old 3DS in Ocarina of Time 3D measured no
difference between low and normal either. The same goes for the big jobs: sizing a new
ring (up to 3.6 GB to claim) and writing a saved clip's index, all the more as
a save with the hotkey runs while the game plays. Those used to run at
Rosalina's real-time priority, and were suspected in Mario Kart 7's sound
cutting out on saves and length changes. That turned out to be the menu's pause
(see "The Rosalina menu's pause").

**Aligned writes** (`flashbackSlotStride`). A write that starts partway into one
of the card's internal blocks costs the card extra work, and the processor too.
An Old 3DS was tested at full resolution, with frames every 64 KB instead of
every 192,512 bytes:
- ~4 ms less per frame to write;
- ~6 ms less of its time compressing, which writing had been slowing.

Hence ring slots rounded up to a power of two (up to 64 KB), and video slots
starting on a 64 KB boundary.

**Long recordings.** A card can slow down after minutes of nonstop writing. An
Old 3DS recording Ocarina of Time 3D (full resolution, 30 fps, sound: ~100 KB
frames) dropped nothing for the first ~4 minutes, roughly 850 MB in. After
that, the card's time per frame (with the audio's share) kept creeping up: 32.7
ms ten minutes in, 36.4 ms twenty minutes in, against 33.3 ms. Each stretch
dropped what its time predicts (at 36.4 ms, 1 frame in 12). The writer's and the
FS priorities made no difference.

The SD test app's sustained test (`tests/flashback/sdtest`) shows the same card
on its own. Written as Flashback writes, frames took 26.6 ms each in the first
minute, and up to 29.7 ms later. Written back to back (each frame right after
the one before, still about one frame per write), they took 24.3 ms on average,
and got faster (25.5 ms down to 22.4). So on that card, the gaps that fixed
slots leave between frames cost ~13%, and more the longer it writes (likely the
card's own housekeeping). The ring keeps fixed slots anyway: an earlier
back-to-back test dropped about 4 times as many frames on a SanDisk Ultra, and
the change would need testing on many cards. Saved clips give each frame's
place in the file, so it wouldn't need a new clip format.

## Staging

*Code: `FLASHBACK_STAGE_SLOTS_BASE` and the rest in `flashback_internal.h`; the
packing in `flashbackPackFrame`.*

Staging is where frames wait between the packer and the writer. Every frame
captured while the card is busy waits there, and when staging is full, frames
are dropped.

**Short pauses.** On an Old 3DS, where the average write is barely under a
frame's time, the card pauses now and then for single writes of 100-200 ms. In
Mario Kart 7, an Old 3DS dropped ~50 frames in 30 s with room for 4 frames,
waiting on the card, and none with 16. Hence the base, 16 uncompressed frames'
worth (~3 MB at full resolution).

**Long slow spells.** Some cards are slower than a frame's time for many writes
in a row, for seconds at a time. A tester's New 3DS dropped over 100 frames a
minute waiting on a SanDisk Ultra, with room for 16 frames. An SD test app
(`tests/flashback/sdtest`) measured that card on a New 3DS, writing the way
Flashback does. With packed staging, it dropped:

| Staging                          | Frames dropped a minute |
| -------------------------------- | ----------------------- |
| the base (16 frames' worth)      | ~220                    |
| twice the base                   | ~75                     |
| four times the base              | ~30                     |

It would have needed ~6 times the base (~20 MB) to drop none.

**Packed staging.** Staged frames take only what they compressed to: frames
typically compress to a third or a half, so two or three times as many fit,
and a slow spell is waited out for two or three times as long.

**Growing staging.** Staging now grows up to `FLASHBACK_STAGE_SLOTS_MAX` (128
frames' worth, ~25 MB). Recording a game on the tester's console, that card then
dropped none, with up to ~185 frames waiting. Staging only grows while that
leaves `FLASHBACK_MEM_MARGIN` (24 MB) of the system's memory free, though:
- the HOME Menu, applets, and Rosalina's menu and screenshots need some;
- an Old 3DS gets by with ~13 MB free.

How much each model takes:
- **A New 3DS** has ~60 MB of system memory free with a game running, and takes
  the most.
- **An Old 3DS** has ~17 MB free, and stays at the base.

The memory is taken in 4 MB pieces (`FLASHBACK_MEM_CHUNK`). A New 3DS's kernel
turns down single requests of ~14 MB and more ("out of range"), even with that
much free, but gives the same amount in pieces.

## The capture queue

*Code: `FLASHBACK_CAPTURE_BUFS_MAX` in `flashback_internal.h`.*

The capture thread only captures, and the packer compresses. A frame that's
slow to compress (the processor held up for a while) no longer makes the next
capture late, because the packer catches up within a few frames. An Old 3DS was
tested in Mario Kart 7 at full resolution:
- with the capture waiting for each compression: a few frames dropped in 30 s;
- with the capture buffers: none.

## Capture

*Code: `flashbackFlushFramebuffer`.*

The GPU writes the framebuffer behind the caches' back, so the capture flushes
the cache lines of the framebuffer before reading it. It used to flush the
entire data cache instead. On every frame, that also interrupted the game's
core, and threw away its cached data (and all of the New 3DS's L2 cache). It's
the same kernel routine (`svcFlushEntireDataCache` passes it an "everything"
range), just limited to the framebuffer.

**Buffer swaps** (`flashbackCaptureWait`). A read that the game's buffer swap
lands in is thrown away and done again, as the buffer may be getting overwritten
with a later frame. Games swap on the screens' refreshes (59.83 Hz), and the
recorder captures at exactly 30 fps, so against a game at 29.92 fps (every other
refresh) the two drift past each other every 11.85 s. In Ocarina of Time 3D on
an Old 3DS, a read takes 7-10 ms (its frames are BGR8, in main memory), and for
a few seconds of each 11.85 s, nearly every frame was read twice. A 10-minute
clip dropped 1.7 frames a second outside those stretches, and up to 4 inside
them: about 400 of its 1394 drops.

So a frame is now read in 20 pieces, with a check for a swap before each, and
a read a swap spoils stops there. Once two swaps have been seen a whole number
of refreshes apart, a read that the next refresh would land in first waits for
it to pass (asleep). A PC simulation of the recorder's grid against a game
swapping on refreshes went from 1.24 reads per frame to 1.00 for a 30 fps game
(8 ms reads), and from 1.49 to 1.00 for a 60 fps one. On the Old 3DS, Ocarina
of Time 3D then read each frame 1.00-1.04 times, and the same 10-minute clip
dropped 568 frames instead of 1394, none in its first 3¾ minutes. The rest were
the SD card's (see "Long recordings").

## The Rosalina menu's pause

*Code: `menuEnter` (`menu.c`).*

While a Rosalina menu is open, Luma pauses the game, along with the dsp, csnd
and gsp system modules (`menuEnter`). Some games don't come back cleanly from a
pause of more than about 8 seconds. That's Luma3DS's own limitation, with or
without Flashback: [issue #1483](https://github.com/LumaTeam/Luma3DS/issues/1483),
and [#2099](https://github.com/LumaTeam/Luma3DS/issues/2099), which its
maintainers closed as an implementation limitation they can't fix.

In Mario Kart 7 (installed on the SD card, New 3DS), the sound stops as the menu
closes, for 5 to 8 s, and the game can freeze for a few seconds within that.
Regular Luma3DS does it every time with the menu open for 8 s. A test build
that kept measurements in each frame's input record showed, for one such cut:
- The game stopped giving the DSP new audio frames: the frame counter it
  advances stood still for 7.7 s, so it wasn't just playing silence.
- The game ran on for ~3 s, then froze for ~3 s, and came back as the sound did.
- The recorder's own SD writes took their usual 4-85 ms throughout, and HID
  kept its usual 8 readings a frame.

Changing Flashback's settings, and saving with X, are done from the menu, so
they count towards that pause (a save from the menu keeps it open for the few
seconds the save takes). The hotkey saves without opening a menu. The README
explains the limit to users, as Luma3DS's own.
