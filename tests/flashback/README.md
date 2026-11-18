# Flashback's PC tests

These tests run the Flashback recorder's own code on a PC. The recorder's code is
`sysmodules/rosalina/source/flashback*.c`, its two `.s` files, and
`sysmodules/rosalina/include/flashback_internal.h`. Run them after changing the
recorder, before trying a build on a 3DS:

```
python run_tests.py --cc "C:/Program Files/JetBrains/CLion 2025.2.4/bin/mingw/bin/gcc.exe"
```

It builds and runs every test, then lists which passed. If any failed, it exits
with 1. A full run takes about 4 minutes, most of it the fuzz and end-to-end tests.

- `--cc`: the C compiler. The default is `gcc`. On Windows, use a MinGW gcc, such
  as CLion's or MSYS2's. The runner puts the compiler's folder on the PATH.
- `--skip-e2e`: leaves out the end-to-end test.
- `--quick`: checks fewer rows and conversions in the assembly checks.

You need Python 3 and a C compiler for the PC. The end-to-end test needs Windows
(it uses winmm's timer), so it's left out elsewhere. Everything is built in
`build/`, which git ignores.

## How the recorder's code gets into the tests

`extract.py` copies `flashback_internal.h` and most of the recorder's files,
unchanged, into `build/recorder_extracted.c`. It leaves out their `#include`s,
and every function that isn't on its list of the ones the tests use. The state,
constants and types are all kept. The tests include that file, with stubs for
the 3DS's libraries in front of it (`stubs_fuzz.h`, `stubs_e2e.h`).
`asm_standin.h` stands in for the packer's assembly, which asmcheck.py checks on
its own.

Some changes to the recorder need matching changes here:
- a function the tests' code now calls, or a new name for one: add it to
  `extract.py`'s list;
- a new recorder file with code the tests use: add it to `extract.py`'s files;
- a new libctru or Rosalina call in that code: add it to the stubs.

The tests fail loudly if any of these is out of step.

`decode.c` is a copy of Flashback Studio's frame decoder (`flbDecodeFrame` in its
`3ds-app/source/flb.c`). That way the tests decode frames the way the readers
do, without that repository. Keep the copy in step with the original.

The fuzz test and the end-to-end test define `FLASHBACK_PACKED_HOOK`, which the
recorder's packer calls (it does nothing otherwise) with what became of each
frame: how much it stored for each screen, whether the bottom screen's frame
got a slot of its own, and whether it was compressed at all. That's how the
fuzz test's model follows the recorder frame by frame.

## The tests

**Fuzz test** (`fuzz.c`). A deterministic random test of everything recording
does: the capture queue, packed staging at every size it can have, audio
staging, the writer, and the inputs' records. It also covers saves (including
ones that fail and carry on), write failures, staging running full, clip
lengths from 1 second to 10 minutes, every frame rate, audio on and off, and the
top screen alone or both screens, with either one first (the recorder's
priority setting). Frames are really compressed, at a tiny
resolution. The screens change the way real ones do: often not at all, often a
little, sometimes altogether, and the bottom one now and then can't be captured.
So frames go unstored for being the same as the one before, or are stored as
what changed, whole, packed with Huffman codes or as they are, and the bottom
screen's sometimes find no room in their slot: then they aren't stored, or
when a whole one is due, get a slot of their own. The "SD card" lets the other threads' work happen in the middle
of each write, so touching data that's being written would show up. In a
third of the runs, frames are packed as by a packer thread of their own, which
sometimes lags behind the captures (as a busy processor does). A fake HID
takes readings between and during grid slots, sometimes more than the 8 it
holds, and the recorder sometimes wakes up late. As on the 3DS, HID's
timestamps only change when a reading goes into index 0, once a round of 8.
As each frame is packed, the test's model checks what the recorder did with it
against the rules: nothing goes unstored unless a reader already has it, and
the first screen's frame never does if it changed; whole frames are stored
when they're due (the first screen's with every 30th slot, the second screen's
with its next frame captured, in a slot of its own when it doesn't fit); the
top screen's frame comes first in a slot whichever screen is first; a frame of
the second screen's that isn't stored is seldom compressed first; and while
packing is falling behind (the next frame waiting, or staging over half full),
the second screen's frame isn't even compressed unless its whole frame is due,
while otherwise a changed one is stored whenever there's room for it.
After every drain, it checks exactly that:
- the audio ring holds every audio frame it should;
- the ring's slots, read in order the way a reader reads a clip (whole frames,
  then what changed, on each screen), show the right picture on both screens
  at every slot, for as many grid slots as they should (and as many of those
  missed captures), with the right audio position;
- a save would write the right clip index, and the right inputs for every frame
  (against a model of what each record should hold). They're built where a save
  builds them, at the start of the recorder's memory, and must fit in the room
  kept for them there;
- the clip's own slots, read from the first one in its index, show the same
  pictures: the top screen has its picture by the first slot shown, and the
  bottom screen too whenever the ring has its whole frame near enough.

A save builds the clip's index over the capture buffers, the frame before with
them. The test then leaves every buffer holding exactly what the next capture
will find, so a recorder that still compared with them would store nothing, and
be caught. `-DFUZZ_RUNS=40` makes a quick run of it.

**Compression test** (`huffman.c`). Frames stored as what changed since the one
before (`flashbackCompressDelta`, and `flashbackSameShare`, which tells how much
has), and the Huffman codes the recorder packs compressed frames with
(`flashbackHuffman`). For the codes, frames of several kinds (game-like, texture,
flat, dithered, noise, one color, and ones whose bytes are almost all the same), at both
resolutions, are packed one after another, as recording does: each frame's
codes come from the counts of the frame before. Every frame packed must:
- decode with `decode.c` to exactly the frame;
- come out smaller than its compressed bytes, and start with 0xFF;
- leave the counts of its own bytes for the next frame.

A frame that wouldn't come out smaller must give 0. Either way, nothing may be
written past the room the frame had. The code lengths, from thousands of sets of
counts (some far more skewed than 12-bit codes can follow), must be 1 to 12
bits, fill the code space exactly, and be no longer for a more common byte.

For frames as what changed, a screen's frames follow one another the way they do
on a 3DS (the same, one pixel changed, a box, a few pixels, a scroll, a new
picture, half of one), at both screens' sizes and both resolutions, in rooms of
every size. Each must decode over the frame before it to exactly the frame,
plain and packed with codes, fit its room or give 0, and write nothing past it.

**End-to-end test** (`e2e.c`). The recorder's threads run for real, in real
time:
- a fake DSP, with every sample numbered;
- the audio and writer threads;
- a 30 fps capture loop;
- an SD card that takes 0-2 ms per write.

It goes through Rosalina menu pauses, a DSP stall, the audio thread held up, a
save in the middle, and recording on afterwards. Then it reads the saved clips
the way a reader would, and checks the audio sample by sample against the
video. It also checks the inputs: a record for every frame, in order, and none
from while a menu was open (a fake HID holds SELECT then). Both screens are
recorded: the top one all new at every capture, or mostly as it was, and the
bottom one mostly as it was. So the packer thread compares each frame with the
one before while the capture loop fills the other buffers, and every picture a
clip shows, on both screens, must be one that was captured, in order. One clip
has a bottom screen that's new at every capture and takes a lot to store,
beside a top screen that leaves it no room: its whole frames must get slots of
their own, and its other frames must not be compressed for nothing. Another is
recorded with the bottom screen first, busy, and a top screen whose frames
never fit beside it: then it's the top screen's frames that go unstored, but
for the whole ones in slots of their own. It writes the clips to `build/out/clip_test1..7.flb` (clip 4 has a beep and a
white frame once a second, for checking sync by eye in Flashback Studio; clip 5
has screens that mostly stay as they are; clips 6 and 7 are the two with a
screen that has no room), and each one's pictures' fingerprints in a `.txt` next to it,
which Flashback Studio's readers can be checked against. It
runs in real time, so a busy PC can make it fail on timing. If it fails, run it
again on a quiet PC before looking further.

**Screen redraw test** (`screentest.c`). The Flashback screen redraws in place
(`flashbackDrawLines` / `flashbackBlankRows`), so it doesn't flicker. This test
draws random screens both that way and the old way (clear, then draw), and they
must match pixel for pixel. A pixel that is the same before and after a redraw
must never change in between. The runner takes the code from the marker
`#define FLASHBACK_SCREEN_COLS` up to `void FlashbackMenu_Show(void)`.

**Packer and capture assembly checks** (`asmcheck.py`). These run
`flashback_pack.s` and `flashback_capture.s` on a small interpreter of the ARM
instructions they use, and compare them with the C they replace:
- for the packer, random pixel streams must give the same bytes and leave the
  same state;
- for the capture, every format must give the same pixels, write nothing past
  the end, and leave the pointers where the C does.

You can also run it on its own:

```
python asmcheck.py pack ../../sysmodules/rosalina/source/flashback_pack.s 3000
python asmcheck.py capture ../../sysmodules/rosalina/source/flashback_capture.s 600
```

A new instruction in the assembly needs adding to the interpreter. Until then,
the check stops and names the instruction.

**SD test app** (`sdtest/`). Flashback SD test is a 3DS app that writes to the
SD card like Flashback recording at full resolution, 30 fps and 60 s, with
sound. It shows how much room for waiting frames the card needs so that
Flashback never drops a frame. Its sustained test (X) writes like an Old 3DS
recording Ocarina of Time 3D with a 10-minute buffer, for 12 minutes, once as
Flashback writes and once with frames back to back, several in one write, and
shows the write time per frame and the frames dropped, minute by minute. Build
it with devkitPro (`make` in `sdtest/`), then run `FlashbackSDTest.3dsx` from
the Homebrew Launcher. It shows the results and saves them to
`sd:/flashback_sdtest.txt` (or `flashback_sdtest_sustained.txt`). The runner
checks the app's logic on a PC, against a simulated card with regular slow
stretches (`sdtest/pc/`).
