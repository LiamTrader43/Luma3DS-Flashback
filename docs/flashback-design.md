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

Packing first compares each screen's frame with the one before. A frame that's
the same isn't stored, one that's much the same is stored as what changed, and
the bottom screen's frame, if it's recorded, goes in the same ring slot as the
top screen's (see "Frames that haven't changed, or only partly").

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

## Frames that haven't changed, or only partly

*Code: `flashbackPackFrame` (`flashback_record.c`), `flashbackSameShare` and
`flashbackCompressDelta` (`flashback_compress.c`), `flashbackPlanClip`
(`flashback_clip.c`).*

Both models end up limited by the SD card, so what's left to gain is in writing
less. Real clips were measured for how much of that there is (36 of them, read
with Flashback Studio's reader, each frame compared with the one before):

| Game | Frames the same as the one before | Bytes saved by storing only what changed |
| ---- | --------------------------------- | ---------------------------------------- |
| Mario Kart 7 | 0-2% | 2-5% |
| Super Smash Bros. | 0-5% (20% in a clip with a pause) | 3-11% |
| Super Mario 3D Land, Donkey Kong Country Returns 3D | 1-6% | 22-36% |
| New Super Mario Bros. 2 | 2-8% | 42-66% |
| Ocarina of Time 3D | 2-24% | 5% moving, 50-90% with the camera still |

So three things happen to a frame, by how much of it is as it was in the frame
before on the same screen:

- **All of it: nothing is stored.** If that goes for both screens, the ring's
  newest slot is shown for one more grid slot, and nothing is compressed or
  written. These aren't dropped frames: missed captures are counted apart
  (`g_missed`), and a clip's index says how many of a slot's copies they were.
- **Half or more (`FLASHBACK_DELTA_MIN_SAME`): what changed is stored.**
  The same ops as a whole frame, plus one that skips pixels that stay.
- **Less: the whole frame**, as before, by the assembly packer. Mario Kart 7
  and Smash Bros. are mostly here, where storing what changed saves little and
  the C that does it is slower on an Old 3DS than the assembly.

Half, because that's about where storing what changed stops costing an Old 3DS
more time than storing the frame whole. The recorder's own code was timed on a
PC over real clips' frames (the assembly is ~1.45 times as fast as the C it
replaces): a frame as what changed takes under 0.7 of the C's time for a whole
frame once half of it stays. A quarter was tried first. On Smash Bros. clips it
chose that for 78% of the frames, which came out 14% bigger (what stays there
is flat, and a flat area takes next to nothing either way) and took longer.
At a half, every clip measured was the same size or smaller than with whole
frames, and within a few percent of the best choice made with hindsight.

**Telling which** (`flashbackSameShare`) compares one word in every 1024 bytes
first: under 200 words a frame. Only if every one matches are the frames
compared in full, and then storing what changed carries on from where that
comparison stopped. So no frame is read twice.

**In the screen's own order.** A frame stored as what changed is packed column
by column, the way the screen holds it, not upright row by row as whole frames
are. Both frames are then read straight through memory. Row by row, a row
touches a cache line per column in each frame: 800 lines, 25 KB, for a 16 KB
cache on an Old 3DS, which would fetch them again for every row.

**Which screen comes first** (`FlashbackPriority`). With both screens recorded,
one is the first screen and the other the second: the top one first, unless
the bottom one is given priority in the menu (for games played on it). The
first screen is recorded as it is alone: captured with every frame, its frames
always stored. The second gets the time and the room that are left, as the
paragraphs below describe. So when an Old 3DS or a slow card can't keep up
with both, the second screen goes without, and the first is no worse off than
alone, but for what the second does cost: its captures, its compares, and a
whole frame about once a second.

**The frame before** is, for the first screen, the one in the capture buffer
before the one being packed, which the capture thread never captures into: with
4 buffers, 3 frames can wait. For the second screen it's the last of its frames
that was stored (not every one is: see below), in whichever of its 4 buffers
that is. The capture thread leaves that one alone too: it captures into one
that holds neither it nor a frame waiting. A save builds the clip's index over
the buffers, so the frames after a save are stored whole
(`flashbackForgetFrames`).

**Where a clip can start.** A frame stored as what changed needs the frames
before it, back to a whole one. So every 30 ring slots (`FLASHBACK_KEY_SLOTS`)
both screens are stored whole: the first screen's frame in every 30th slot, the
second screen's with its next frame from then on (it isn't captured with every
frame). A clip's index starts with the slots from the last whole frame of each
screen before its first frame, whichever is further back, 38 slots at most
(`FLASHBACK_LEAD_MAX`): read, not shown (0 copies). Slots, not time: a still
picture uses no slots, so it brings no whole frames either.

**Both screens' frames go in one slot**: the top screen's, then the bottom
screen's, whichever comes first. A slot has room for the top screen's frame as
it is (192 KB, the bigger of the two), frames mostly compress to under half,
and the second screen often stays as it is, so there's usually room. That way
the ring is hardly bigger (with a slot for each screen, a 10-minute buffer
would pass the 4 GB a file can be), and the card still gets one write per
frame. With the bottom screen first, its frame is compressed apart, in a
second slot's worth of staging, and moved up behind the top screen's once that
one's size is known: a clip's layout is the same either way, and Flashback
Studio needs to know nothing of which screen came first.

**When there isn't room or time**, the first screen's frames come first:
- The second screen's frame isn't stored: a clip shows the one before it for
  longer. The next one is compared with the last one that was stored, kept in
  its buffer for that, so it can still go in as what changed.
- It isn't compressed at all where it's unlikely to fit: not whole in less room
  than its last whole frame took and an eighth, and not again, either way, in
  no more room than the last try didn't fit in (`g_secWholeNeed`,
  `g_secDeltaNeed`).
- Nor while packing is falling behind (`flashbackFallingBehind`): the next
  frame is already captured and waiting (the processor is behind), or staging
  is over half full (the card is). Half of all of it: a New 3DS's, up to
  ~25 MB, lets frames wait out a slow card for seconds first, and an Old 3DS's,
  ~3 MB, for under one. That came after the measurements below, which showed
  where the remaining drops were.
- Its whole frame, when one is due, is always stored. If it fits beside the
  first screen's frame, the two share the slot. If not, it has a slot of its
  own: the one before the first screen's frame's (`FLASHBACK_FRAME_UNSHOWN`; an
  index entry with 0 copies, which a reader only reads). So however little
  room the first screen leaves, the second has a new picture every 30 slots at
  least, and no clip starts without one. The ring has one more slot in every
  30 for these (`flashbackRingSlots`): 3.5% more, with both screens recorded.

**Measured on an Old 3DS**: Mario Kart 7, a Rainbow Road time trial, full
resolution, 30 fps, both screens, the top one first, read back from the saved
clips' indexes. The bottom screen there is a second 3D scene (a planet, the
rainbow road, stars): 61% of its pixels change from one of its stored frames
to the next, so they're nearly all whole, ~71 KB each (46%).

| | First build | Giving way to room only | This one |
| --- | --- | --- | --- |
| Frames dropped, of 1800 | 361 | 29 | 0 |
| The bottom screen's frames stored | 21.7 a second (1.53 MB/s) | 13.2 a second (0.89 MB/s) | 12.2 a second (0.75 MB/s) |
| Written | 3.80 MB/s | 3.56 MB/s | 3.47 MB/s |
| Compress, write (the Flashback screen, at the end) | 37 ms, 42 ms | 30 ms, 34 ms | 25 ms, 34 ms |

The first build captured the bottom screen with nearly every frame and stored
whatever fitted: a busy bottom screen at up to 30 fps is more than an Old 3DS's
processor and card have to spare. With the top screen alone, it dropped none.
Here, capturing the bottom screen every other frame did the most. The 29 left
came in short bursts (1 to 4 a second), all in the clip's second half, and not
in its busiest seconds: earlier, the card took 4.3 MB a second without a drop.
By the end, writes averaged 34 ms against the 33 a frame has: the card had
slowed (see "Long recordings"). The second screen didn't give way to that, only
to a lack of room: so now it gives way when packing falls behind too (above).
With that, the same run dropped none. The bottom screen gave way mostly on the
moon (41 to 55 s into the clip), where both screens are covered in craters:
twice for a few seconds it was down to its picture a second, while that picture
was changing all the time; its longest gap was 31 frames. In the clip's first
35 seconds it kept nearly all of its 15 a second.

**Capturing the second screen** (`flashbackCapture`) comes after the first
screen's frames. Reading it right after the first screen, and again whenever
the game swaps buffers during the read (as the first screen's is), can take a
whole frame's time: a game like Mario Kart 7 swaps on every refresh, and on an
Old 3DS the two reads, on a system core the recorder gets a third of, don't fit
between two swaps. So:
- its read is never tried again: a frame without one keeps that screen as it
  was, which costs nothing;
- it's only started if it fits before the next swap, and when it doesn't, it
  waits a swap out every other frame instead. The second screen then has half
  the frame rate, taken a refresh after the first screen's frame;
- an Old 3DS only takes it every other frame at all. Each of its frames costs
  the capture a read and the packer at least a compare, out of the little time
  the game leaves them;
- while its whole frame is due, it's taken with the next frame regardless, so
  that a ring's first frame has it.

**No audio setting.** Audio is always recorded: the menu's line and button for
turning it off went to the priority setting. Without audio a clip was 1.6 MB
smaller for every 10 seconds, and the card had three fewer writes a second.

Not measured yet on a 3DS: the bottom screen first, and a New 3DS.

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
