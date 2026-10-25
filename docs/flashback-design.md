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
5. **Saving.** A save renames the ring file to the clip, appends an index, and
   starts a new ring (`flashback_clip.c`).

Each stage hands work to the next through counters that only go up. So a slow
stage makes the work wait in memory instead of holding up the stages before it.
A grid slot that gets no frame becomes a copy of the frame before it, so clips
always play at the right speed.

## Threads and priorities

*Code: `flashbackCreateThread`.*

The capture, writer and packer threads have the lowest priorities among
Rosalina's threads, and run on the system core:

- **The packer** is at the bottom, 60. It does nearly all the work
  (compressing).
- **The capture thread** is one step above it, at 59. It interrupts the packer
  for the few ms a capture takes, so it captures on time.
- **The writer** is level with the capture thread. It hardly uses the CPU
  (it mostly waits on the SD card), and running first means it sends the next
  write the moment the previous one finishes.
- **The audio thread** runs well above them, at 25. It uses very little CPU, but
  it has to catch each audio frame within the ~8 ms it stays put.

Two other arrangements were tried first:

- **The packer a step lower still, at 61.** Other threads got ahead of it too,
  and an Old 3DS compressed ~30% slower, too slow to keep up.
- **The writer below the packer.** It had to wait out each frame's compression
  with the card idle. An SD speed test on a New 3DS XL put a ~100 KB write at
  ~20 ms, but recording measured 31-36 ms per write.

## SD card writes

**Compression makes full resolution fit.** On a New 3DS XL, the SD path tops out
around 5 MB/s. Full resolution at 30 fps needs 5.8 MB/s uncompressed.

**One request per frame.** Batching several frames per SD request was tried,
and didn't help: the time is in moving the bytes, not in each request.

**FS priority** (`FLASHBACK_RING_FS_PRIORITY`). This was measured on a New 3DS XL
at full resolution and 30 fps. The write time per frame was the same at low and
normal priority (~36 ms), and slightly worse at real-time priority (~41 ms). So
the lowest priority costs nothing, and it keeps the ring file's constant writes
from getting ahead of the game.

**Aligned writes** (`flashbackSlotStride`). A write that starts partway into one
of the card's internal blocks costs the card extra work, and the processor too.
An Old 3DS was tested at full resolution, with frames every 64 KB instead of
every 192,512 bytes:
- ~4 ms less per frame to write;
- ~6 ms less of its time compressing, which writing had been slowing.

Hence ring slots rounded up to a power of two (up to 64 KB), and video slots
starting on a 64 KB boundary.

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
