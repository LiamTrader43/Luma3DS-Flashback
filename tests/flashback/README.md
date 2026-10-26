# Flashback's PC tests

These tests run the Flashback recorder's own code on a PC. The recorder's code is
`sysmodules/rosalina/source/flashback*.c`, its two `.s` files, and
`sysmodules/rosalina/include/flashback_internal.h`. Run them after changing the
recorder, before trying a build on a 3DS:

```
python run_tests.py --cc "C:/Program Files/JetBrains/CLion 2025.2.4/bin/mingw/bin/gcc.exe"
```

It builds and runs every test, then lists which passed. If any failed, it exits
with 1. A full run takes about 3 minutes, most of it the end-to-end test.

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

`decode.c` is a copy of Flashback Studio's frame decoder (`flbDecompress` in its
`3ds-app/source/flb.c`). That way the tests decode frames the way the readers
do, without that repository. Keep the copy in step with the original.

## The tests

**Fuzz test** (`fuzz.c`). A deterministic random test of everything recording
does: the capture queue, packed staging at every size it can have, audio
staging, the writer, and the inputs' records. It also covers saves (including
ones that fail and carry on), write failures, staging running full, every clip
length and frame rate, and audio on and off. Frames are really compressed, at a
tiny resolution. The "SD card" lets the other threads' work happen in the middle
of each write, so touching data that's being written would show up. A fake HID
takes readings between and during grid slots, sometimes more than the 8 it
holds, and the recorder sometimes wakes up late. As on the 3DS, HID's
timestamps only change when a reading goes into index 0, once a round of 8.
After every drain, it checks exactly that:
- the audio ring holds every audio frame it should;
- every buffered slot holds the right frame, with the right audio position;
- a save would write the right clip index, and the right inputs for every frame
  (against a model of what each record should hold).

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
from while a menu was open (a fake HID holds SELECT then). It writes the clips to `build/out/clip_test1..4.flb` (clip 4 has a beep and a
white frame once a second, for checking sync by eye in Flashback Studio). It
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
Flashback never drops a frame. Build it with devkitPro (`make` in `sdtest/`),
then run `FlashbackSDTest.3dsx` from the Homebrew Launcher. It shows the results
and saves them to `sd:/flashback_sdtest.txt`. The runner checks the app's logic
on a PC, against a simulated card with regular slow stretches
(`sdtest/pc/`).
