# Flashback user guide

Everything about using Flashback, in detail. To install it, see the [README](../README.md).

## Starting Flashback

If you copied Flashback to `/luma/payloads/y_flashback.firm`, hold <kbd>Y</kbd> while powering on to start it, and power on normally to start your usual Luma3DS. When started this way, Flashback won't chainload any further payloads, so holding <kbd>Y</kbd> too long is fine. To start a different payload, boot your usual Luma3DS.

Your usual Luma3DS needs to be [v13.5](https://github.com/LumaTeam/Luma3DS/releases/tag/v13.5), the version Flashback is built on: with an older one, the configuration menu appears at every boot and Luma's settings keep resetting.

## Recording and saving a clip

1. Open the Rosalina menu with <kbd>L+Down+Select</kbd> and choose **Flashback recorder**.
2. Press <kbd>A</kbd> to turn **Recording** on, then press <kbd>B</kbd> twice to return to your game.
3. Play. Flashback always keeps the most recent part of your game, as long as the clip length you've chosen (up to 10 minutes).
4. When you want to keep a moment, press <kbd>L+R+Down</kbd>. The bottom screen gets a see-through **white** tint while saving, then briefly turns **green** when the clip is saved (or **red** if something went wrong). You can keep seeing and using the bottom screen the whole time.

You can also save from the Flashback screen with <kbd>X</kbd>. Recording pauses while any Rosalina menu is open (the game is paused too), so saving from the menu doesn't add frozen frames to the end of your clip.

**Close the Rosalina menu within about 7 seconds.** Some games, Mario Kart 7 among them, lose their sound for several seconds after the menu has been open for 8 seconds or more, and can freeze for a few seconds too, before both come back on their own. That's a limitation of Luma3DS itself, and happens without Flashback too (see [Limitations](#limitations)). Saving with the hotkey doesn't open the menu at all.

## Flashback screen controls

| Button | Action |
|---|---|
| <kbd>A</kbd> | Turn recording on/off |
| <kbd>X</kbd> | Save a clip now |
| <kbd>Y</kbd> | Resolution: half (200x120) or full (400x240) |
| <kbd>Left</kbd>/<kbd>Right</kbd> | Frame rate: 10, 20 or 30 fps |
| <kbd>L</kbd>/<kbd>R</kbd> | Clip length, from 1 second to 10 minutes: opens a screen where you set the number of seconds a digit at a time (<kbd>Left</kbd>/<kbd>Right</kbd> pick a digit, <kbd>Up</kbd>/<kbd>Down</kbd> change it, <kbd>A</kbd> sets the length, <kbd>B</kbd> goes back without changing it) |
| <kbd>Start</kbd> | Audio: on or off |
| <kbd>Select</kbd> | Change the save hotkey |
| <kbd>B</kbd> | Back |

Turning recording off, or changing the resolution, frame rate, length or audio, clears the buffer. Opening a menu only pauses recording, so the buffer is kept.

Saving a clip takes only a few seconds, however long the clip is: the buffer itself becomes the clip, and recording carries on with a new, empty buffer. That new buffer needs free space on the SD card, as much as a full clip (the Flashback screen shows how much, next to the length: up to about 3.6 GB at full resolution, 30 fps and 10 minutes, with audio); if there isn't enough, recording stops and the Flashback screen says the SD card is full.

The screen also shows live stats. **Rate** should stay close to the frame rate you picked. **Buffered** shows how many frames a save would contain right now, and how many of them are **dropped** frames: frames the recorder missed because it couldn't keep up, filled with a copy of the previous frame so clips still play at the right speed. If many frames are dropped, a lower resolution or frame rate will look smoother. With audio on, **Rate** also shows how many bits of audio were **missed** (see below).

The last two lines show where the time goes, averaged over the last second or so. **Per frame** is how long each frame takes to read from the screen (**capture**), shrink losslessly (**compress**) and write to the SD card (**write**). **Budget** is the time there is per frame at your frame rate (33 ms at 30 fps), and **compressed to** is how big frames are after compressing. Writing happens while the next frame is captured and compressed, so the recorder keeps up as long as capture plus compress, and write on its own, each stay under the budget. (On an Old 3DS/2DS, compressing runs on the game's processor core, in the time the game leaves idle, so there capture and compress each get the whole budget.) Short spells over it are fine: a few captured frames can wait to be compressed, and around a second or more of compressed ones to be written, over ten seconds' worth on a New 3DS/2DS, which has memory to spare (the SD card sometimes slows down for a moment; the better frames compress, the more fit). If write goes over, the SD card is the limit; if capture plus compress does (on an Old 3DS/2DS, either one), the 3DS's processor is (most likely on an Old 3DS). Either way, a lower resolution or frame rate helps.

**Raw** is how many frames are stored as they are, without compressing. When the processor can't keep up compressing every frame (an Old 3DS at full resolution, in a demanding game), the recorder skips compressing some of them whenever the SD card has time to spare (and, when a frame would otherwise be dropped, a little more often), so the two share the load: that keeps the frame rate up, at the cost of writing more. It stays near 0% when the recorder keeps up, as a New 3DS does. **Compress** and **compressed to** include those frames (at their full size).

**Audio.** Flashback records the game's sound with the picture: the final mix the 3DS's sound chip sends to the speakers, read from the memory it shares with the game. It's on by default; <kbd>Start</kbd> turns it off. It adds about 131 KB per second (8 MB for a minute) to the buffer and clips, and exported MP4s get it as an AAC track. If the recorder ever misses a moment of audio (5 ms at a time; it rarely does), that moment is silent in the clip and counted as **missed**.

**Inputs.** Flashback also records what you press, for every frame: the buttons held at any time during it (so a quick tap shows too), where the Circle Pad was, and where the touch screen was touched. Flashback Studio can show them over the clip, and draw them into exported MP4s (see below). Buttons pressed while a Rosalina menu is open are left out, and <kbd>ZL</kbd>, <kbd>ZR</kbd> and the C-Stick aren't recorded. The inputs add under 60 KB to a 2-minute clip.

**Changing the save hotkey:** press <kbd>Select</kbd>, hold the new combination (at least 2 buttons), then let go. If the combination can't be used, press <kbd>A</kbd> to try again. Combos that overlap the Rosalina menu combo aren't allowed.

You can use <kbd>A</kbd>, <kbd>B</kbd>, <kbd>X</kbd>, <kbd>Y</kbd>, <kbd>L</kbd>, <kbd>R</kbd>, <kbd>Start</kbd>, <kbd>Select</kbd> and the D-Pad. **<kbd>ZL</kbd>, <kbd>ZR</kbd> and the C-Stick on New 3DS/2DS can't be used**: the system reports them through a separate input service that Rosalina doesn't read, so it never sees them being pressed. That's the same reason they can't be part of the Rosalina menu combo. The touchscreen and Circle Pad can't be used either.

## Where clips are saved and how to watch them

Clips are saved in `/luma/flashback/` on the SD card as `clip_<date>.flb` files, named after the date and time they were saved.

> **`.flb` clips can only be turned into videos with Flashback Studio** from [Flashback-Studio](https://github.com/LiamTrader43/Flashback-Studio). They use Flashback's own compressed format, so video players, video editors and other tools (plain ffmpeg included) can't open them. If a clip won't play or import, exporting it from Flashback Studio for Windows is the way to fix it.

Flashback Studio comes for the 3DS and for Windows, from the [latest Flashback-Studio release](https://github.com/LiamTrader43/Flashback-Studio/releases/latest) ([Flashback-Studio](https://github.com/LiamTrader43/Flashback-Studio) is the companion project for Flashback clips). Both play clips with their sound, can show the buttons you pressed over the picture, and trim, rename and compact clips (compacting makes a clip about half its size, so it copies to a PC faster).

* **On the 3DS:** a `.3dsx` for the Homebrew Launcher, or a `.cia` to install. It plays clips on the top screen.
* **On Windows 10 or 11:** extract `FlashbackStudio-Windows.zip` anywhere, copy your clips from the SD card to your PC (or put the SD card in it), and open `FlashbackStudio.exe`. It also **exports clips as MP4 videos** (it comes with ffmpeg, which does the encoding): at twice the size with sharp pixels, with the game's sound, and the input display over them if you like, encoded by the CPU or an NVIDIA graphics card. They play in any video player and import into video editors like DaVinci Resolve. Clips saved by older Flashback builds (`.raw` with a `.txt`) export too.

## SD card usage

The buffer lives in `/luma/flashback/ring.bin` on the SD card, and recording writes to it continuously. Frames are compressed (losslessly) before they're written, typically to around half their size, so the amount written depends on the game.

| Setting | Buffer file size (10 s) | Saved clip, `.flb` (10 s) | Written while recording |
|---|---|---|---|
| Half resolution, 10 fps | 4.9 MB | up to 4.9 MB | up to about 1.7 GB per hour |
| Full resolution, 30 fps | 59 MB | up to 59 MB | up to about 21 GB per hour, typically around half that |

A saved clip is the buffer file itself, so it's the same size (smaller if you save before the buffer has filled up). Sizes scale with the length: a 2-minute buffer and clip are 12 times the sizes above, and a 10-minute one 60 times (about 3.6 GB at full resolution and 30 fps). Audio adds 1.6 MB to a 10-second buffer and clip (16 MB to a 2-minute one, 79 MB to a 10-minute one), and about 0.5 GB per hour of writing. A clip over 2 GB (at full resolution and 30 fps, one longer than about 6 minutes) needs an up-to-date Flashback Studio: older versions of its 3DS app can't open one. A good-quality SD card handles this easily, but continuous writing does add wear, especially at full resolution and 30 fps. Turn recording off when you don't need it.

## Limitations

* **Recording is always off after a reboot**: turn it on from the Flashback screen each time. Your other settings (resolution, frame rate, length, audio and hotkey) are remembered in `/luma/flashback/settings.bin` when you leave the Flashback screen. Delete that file to go back to the defaults: full resolution, 30 fps, 30 seconds, audio on, hotkey <kbd>L+R+Down</kbd>.
* **Long visits to the Rosalina menu can stop a game's sound**: Luma3DS pauses the game while the menu is open, and some games don't come back cleanly from a pause of 8 seconds or more. Their sound stops for several seconds after the menu closes, and they can freeze for a few seconds too. This is Luma3DS's own limitation ([Luma3DS issue #1483](https://github.com/LumaTeam/Luma3DS/issues/1483)), not Flashback's. The way around it is to close the menu within about 7 seconds. Saving with the hotkey avoids the menu.
* **Only the top screen** is recorded, and only the left eye when 3D is on.
* **Audio is the game's, at full volume**: it's recorded before the volume slider, so clips have the same level however loud the 3DS is set. Sounds played through the 3DS's other sound hardware (CSND, used by a few games, some system sounds and older homebrew) aren't recorded.
* **Turning clips into videos needs Windows**: Flashback Studio exports them on Windows only. There's no version for macOS or Linux yet.
* **SD card only**: Flashback is disabled when Luma3DS boots from the internal memory (NAND).
* **Old 3DS/2DS are slower**: they have a much slower CPU, so higher settings can make games run slower. They still default to full resolution at 30 fps: in testing, an Old 3DS recorded Mario Kart 7 that way with only a few dropped frames per race, and Ocarina of Time 3D with none dropped until the SD card slowed down (see below). There, Flashback compresses on the game's own processor core, using only the time the game leaves idle. If a game slows down or drops many frames, lower the resolution or frame rate.
* **Some SD cards slow right down for seconds at a time**: frames that come meanwhile wait in memory to be written. A New 3DS/2DS has memory to spare, so Flashback uses up to about 25 MB of it for that while recording (only while plenty is left for the rest of the system), and waits out over ten seconds of it: in testing, a SanDisk Ultra card that had dropped over 100 frames a minute dropped none. An Old 3DS/2DS has little memory to spare, so there a card like that drops frames even though **write** is under the budget on average. A faster card fixes it (a Samsung EVO Plus dropped none in the same game), or a lower frame rate or resolution helps.
* **SD cards can slow down after minutes of nonstop writing**: in testing, an Old 3DS recording Ocarina of Time 3D at full resolution and 30 fps dropped no frames for the first few minutes. Then, as the card slowed down, it dropped up to about 1 frame in 12 (single frames, each filled with a copy of the one before), with **write** just over the budget. A lower frame rate or resolution avoids it.

## Working on Flashback

The recorder's code is in `sysmodules/rosalina/source/flashback*.c` and its two `.s` files. [`flashback_internal.h`](../sysmodules/rosalina/include/flashback_internal.h) lists what each file does. [`docs/flashback-design.md`](flashback-design.md) explains the measurements and reasoning behind the recorder's numbers and rules. [`tests/flashback`](../tests/flashback) has PC tests that run the recorder's own code: `python tests/flashback/run_tests.py` (see its [README](../tests/flashback/README.md) for what you need).
