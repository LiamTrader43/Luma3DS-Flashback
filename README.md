# Luma3DS-Flashback

![GitHub Downloads (all assets, all releases)](https://img.shields.io/github/downloads/LiamTrader43/Luma3DS-Flashback/total)
![License](https://img.shields.io/badge/License-GPLv3-blue.svg)

*Fork of Luma3DS that adds a flashback record option*

![Boot menu screenshot](img/boot_menu_v1321.png)
![Rosalina menu screenshot](img/rosalina_menu_v1321.png)

> **This is an unofficial fork.** It is not made or supported by the Luma3DS team. Please report Flashback issues here, not to the official Luma3DS repository.

## Flashback recorder

Flashback is a "replay buffer" for the 3DS top screen. While recording is on, it keeps the **last 30 seconds** of gameplay (adjustable from 1 second to 10 minutes), with sound and the buttons you press. When something worth keeping happens, press a button combo and those seconds are saved to the SD card as a clip, which [Flashback Studio](#where-clips-are-saved-and-how-to-watch-them) plays and turns into a video.

It is built into Rosalina, so it works in **any game**, including retail games, without plugins. It uses none of the game's memory and has no noticeable effect on game performance.

It records at **full resolution and 30 fps on both the Old and New 3DS/2DS**. What it needs most is a **good SD card**: some cards slow down for seconds at a time, which makes it drop frames, especially on an Old 3DS/2DS (see [Limitations](#limitations)).

### Installing

Flashback is a complete Luma3DS build, so it needs [boot9strap](https://github.com/SciresM/boot9strap) like regular Luma3DS. Download `boot.firm` from [this fork's latest release](https://github.com/LiamTrader43/Luma3DS-Flashback/releases/latest), then choose one of these:

* **Try it alongside your current Luma3DS (recommended):** rename and copy `boot.firm` to `/luma/payloads/y_flashback.firm` on your SD card. Hold <kbd>Y</kbd> while powering on to boot Flashback; power on normally to boot your usual Luma3DS. (When started this way, Flashback won't chainload any further payloads, so holding <kbd>Y</kbd> too long is fine. To start a different payload, boot your usual Luma3DS.)
* **Use it as your main Luma3DS:** replace `boot.firm` in the root of your SD card. Keep a copy of your old `boot.firm` in case you want to go back.

To check that you're running Flashback, open the Rosalina menu (<kbd>L+Down+Select</kbd>): the bottom line reads **"Luma3DS v13.4 (Flashback)"** and the menu has a **Flashback recorder** entry.

### Recording and saving a clip

1. Open the Rosalina menu with <kbd>L+Down+Select</kbd> and choose **Flashback recorder**.
2. Press <kbd>A</kbd> to turn **Recording** on, then press <kbd>B</kbd> twice to return to your game.
3. Play. Flashback always keeps the most recent 30 seconds (or the length you've chosen).
4. When you want to keep a moment, press <kbd>L+R+Down</kbd>. The bottom screen gets a see-through **white** tint while saving, then briefly turns **green** when the clip is saved (or **red** if something went wrong). You can keep seeing and using the bottom screen the whole time.

You can also save from the Flashback screen with <kbd>X</kbd>. Recording pauses while any Rosalina menu is open (the game is paused too), so saving from the menu doesn't add frozen frames to the end of your clip.

**Close the Rosalina menu within about 7 seconds.** Some games, Mario Kart 7 among them, lose their sound for several seconds after the menu has been open for 8 seconds or more, and can freeze for a few seconds too, before both come back on their own. That's a limitation of Luma3DS itself, and happens without Flashback too (see [Limitations](#limitations)). Saving with the hotkey doesn't open the menu at all.

### Flashback screen controls

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

The last two lines show where the time goes, averaged over the last second or so. **Per frame** is how long each frame takes to read from the screen (**capture**), shrink losslessly (**compress**) and write to the SD card (**write**). **Budget** is the time there is per frame at your frame rate (33 ms at 30 fps), and **compressed to** is how big frames are after compressing. Writing happens while the next frame is captured and compressed, so the recorder keeps up as long as capture plus compress, and write on its own, each stay under the budget. Short spells over it are fine: a few captured frames can wait to be compressed, and around a second or more of compressed ones to be written, over ten seconds' worth on a New 3DS/2DS, which has memory to spare (the SD card sometimes slows down for a moment; the better frames compress, the more fit). If write goes over, the SD card is the limit; if capture plus compress does, the 3DS's processor is (most likely on an Old 3DS). Either way, a lower resolution or frame rate helps.

**Raw** is how many frames are stored as they are, without compressing. When the processor can't keep up compressing every frame (an Old 3DS at full resolution), the recorder skips compressing some of them whenever the SD card has time to spare (and, when a frame would otherwise be dropped, a little more often), so the two share the load: that keeps the frame rate up, at the cost of writing more. It stays near 0% when the recorder keeps up, as a New 3DS does. **Compress** and **compressed to** include those frames (at their full size).

**Audio.** Flashback records the game's sound with the picture: the final mix the 3DS's sound chip sends to the speakers, read from the memory it shares with the game. It's on by default; <kbd>Start</kbd> turns it off. It adds about 131 KB per second (8 MB for a minute) to the buffer and clips, and exported MP4s get it as an AAC track. If the recorder ever misses a moment of audio (5 ms at a time; it rarely does), that moment is silent in the clip and counted as **missed**.

**Inputs.** Flashback also records what you press, for every frame: the buttons held at any time during it (so a quick tap shows too), where the Circle Pad was, and where the touch screen was touched. Flashback Studio can show them over the clip, and draw them into exported MP4s (see below). Buttons pressed while a Rosalina menu is open are left out, and <kbd>ZL</kbd>, <kbd>ZR</kbd> and the C-Stick aren't recorded. The inputs add under 60 KB to a 2-minute clip.

**Changing the save hotkey:** press <kbd>Select</kbd>, hold the new combination (at least 2 buttons), then let go. If the combination can't be used, press <kbd>A</kbd> to try again. Combos that overlap the Rosalina menu combo aren't allowed.

You can use <kbd>A</kbd>, <kbd>B</kbd>, <kbd>X</kbd>, <kbd>Y</kbd>, <kbd>L</kbd>, <kbd>R</kbd>, <kbd>Start</kbd>, <kbd>Select</kbd> and the D-Pad. **<kbd>ZL</kbd>, <kbd>ZR</kbd> and the C-Stick on New 3DS/2DS can't be used**: the system reports them through a separate input service that Rosalina doesn't read, so it never sees them being pressed. That's the same reason they can't be part of the Rosalina menu combo. The touchscreen and Circle Pad can't be used either.

### Where clips are saved and how to watch them

Clips are saved in `/luma/flashback/` on the SD card as `clip_<date>.flb` files, named after the date and time they were saved.

> **`.flb` clips can only be turned into videos with Flashback Studio** from [Flashback-Studio](https://github.com/LiamTrader43/Flashback-Studio). They use Flashback's own compressed format, so video players, video editors and other tools (plain ffmpeg included) can't open them. If a clip won't play or import, exporting it from Flashback Studio for Windows is the way to fix it.

Flashback Studio comes for the 3DS and for Windows, from the [latest Flashback-Studio release](https://github.com/LiamTrader43/Flashback-Studio/releases/latest) ([Flashback-Studio](https://github.com/LiamTrader43/Flashback-Studio) is the companion project for Flashback clips). Both play clips with their sound, can show the buttons you pressed over the picture, and trim, rename and compact clips (compacting makes a clip about half its size, so it copies to a PC faster).

* **On the 3DS:** a `.3dsx` for the Homebrew Launcher, or a `.cia` to install. It plays clips on the top screen.
* **On Windows 10 or 11:** extract `FlashbackStudio-Windows.zip` anywhere, copy your clips from the SD card to your PC (or put the SD card in it), and open `FlashbackStudio.exe`. It also **exports clips as MP4 videos** (it comes with ffmpeg, which does the encoding): at twice the size with sharp pixels, with the game's sound, and the input display over them if you like, encoded by the CPU or an NVIDIA graphics card. They play in any video player and import into video editors like DaVinci Resolve. Clips saved by older Flashback builds (`.raw` with a `.txt`) export too.

### SD card usage

The buffer lives in `/luma/flashback/ring.bin` on the SD card, and recording writes to it continuously. Frames are compressed (losslessly) before they're written, typically to around half their size, so the amount written depends on the game.

| Setting | Buffer file size (10 s) | Saved clip, `.flb` (10 s) | Written while recording |
|---|---|---|---|
| Half resolution, 10 fps | 4.9 MB | up to 4.9 MB | up to about 1.7 GB per hour |
| Full resolution, 30 fps | 59 MB | up to 59 MB | up to about 21 GB per hour, typically around half that |

A saved clip is the buffer file itself, so it's the same size (smaller if you save before the buffer has filled up). Sizes scale with the length: a 2-minute buffer and clip are 12 times the sizes above, and a 10-minute one 60 times (about 3.6 GB at full resolution and 30 fps). Audio adds 1.6 MB to a 10-second buffer and clip (16 MB to a 2-minute one, 79 MB to a 10-minute one), and about 0.5 GB per hour of writing. A clip over 2 GB (at full resolution and 30 fps, one longer than about 6 minutes) needs an up-to-date Flashback Studio: older versions of its 3DS app can't open one. A good-quality SD card handles this easily, but continuous writing does add wear, especially at full resolution and 30 fps. Turn recording off when you don't need it.

### Limitations

* **Recording is always off after a reboot**: turn it on from the Flashback screen each time. Your other settings (resolution, frame rate, length, audio and hotkey) are remembered in `/luma/flashback/settings.bin` when you leave the Flashback screen. Delete that file to go back to the defaults: full resolution, 30 fps, 30 seconds, audio on, hotkey <kbd>L+R+Down</kbd>.
* **Long visits to the Rosalina menu can stop a game's sound**: Luma3DS pauses the game while the menu is open, and some games don't come back cleanly from a pause of 8 seconds or more. Their sound stops for several seconds after the menu closes, and they can freeze for a few seconds too. This is Luma3DS's own limitation ([Luma3DS issue #1483](https://github.com/LumaTeam/Luma3DS/issues/1483)), not Flashback's. The way around it is to close the menu within about 7 seconds. Saving with the hotkey avoids the menu.
* **Only the top screen** is recorded, and only the left eye when 3D is on.
* **Audio is the game's, at full volume**: it's recorded before the volume slider, so clips have the same level however loud the 3DS is set. Sounds played through the 3DS's other sound hardware (CSND, used by a few games, some system sounds and older homebrew) aren't recorded.
* **Turning clips into videos needs Windows**: Flashback Studio exports them on Windows only. There's no version for macOS or Linux yet.
* **SD card only**: Flashback is disabled when Luma3DS boots from the internal memory (NAND).
* **Old 3DS/2DS are slower**: they have a much slower CPU, so higher settings can make games run slower. They still default to full resolution at 30 fps: in testing, an Old 3DS recorded Mario Kart 7 that way with only a few dropped frames per race. If a game slows down or drops many frames, lower the resolution or frame rate.
* **Some SD cards slow right down for seconds at a time**: frames that come meanwhile wait in memory to be written. A New 3DS/2DS has memory to spare, so Flashback uses up to about 25 MB of it for that while recording (only while plenty is left for the rest of the system), and waits out over ten seconds of it: in testing, a SanDisk Ultra card that had dropped over 100 frames a minute dropped none. An Old 3DS/2DS has little memory to spare, so there a card like that drops frames even though **write** is under the budget on average. A faster card fixes it (a Samsung EVO Plus dropped none in the same game), or a lower frame rate or resolution helps.

### Working on Flashback

The recorder's code is in `sysmodules/rosalina/source/flashback*.c` and its two `.s` files. [`flashback_internal.h`](sysmodules/rosalina/include/flashback_internal.h) lists what each file does. [`docs/flashback-design.md`](docs/flashback-design.md) explains the measurements and reasoning behind the recorder's numbers and rules. [`tests/flashback`](tests/flashback) has PC tests that run the recorder's own code: `python tests/flashback/run_tests.py` (see its [README](tests/flashback/README.md) for what you need).

## Description
**Luma3DS** patches and reimplements significant parts of the system software running on all models of the Nintendo 3DS family of consoles. It aims to greatly improve the user experience and support the 3DS far beyond its end-of-life. Features include:

* **First-class support for homebrew applications**
* **Rosalina**, an overlay menu (triggered by <kbd>L+Down+Select</kbd> by default), allowing things like:
    * Taking screenshots while in game
    * Blue light filters and other screen filters
    * Input redirection to play with external devices, such as controllers
    * Using cheat codes
    * Setting time and date accurately from the network (NTP)
    * ... and much more!
* **Many game modding features**, such as, but not limited to:
    * Game plugins (in 3GX format)
    * Per-game language overrides ("locale emulation")
    * Asset content path redirection ("LayeredFS")
* **Support for user-provided patches and/or full "system modules" replacements**, an essential feature for Nintendo Network replacements (amongst other projects)
* A **fully-fledged GDB stub**, allowing homebrew developers and reverse-engineers alike to work much more efficiently
* Ability to chainload other firmware files, including other versions of itself
* ... and much more!

## Installation and upgrade
Luma3DS requires [boot9strap](https://github.com/SciresM/boot9strap) to run.

Once boot9strap has been installed, simply download the [latest release archive](https://github.com/LumaTeam/Luma3DS/releases/latest) and extract the archive onto the root of your SD card to "install" or to upgrade Luma3DS alongside the [homebrew menu and certs bundle](https://github.com/devkitPro/3ds-hbmenu) shipped with it. Replace existing files and merge existing folders if necessary.

## Basic usage
**The main Luma3DS configuration menu** can be accessed by pressing <kbd>Select</kbd> at boot. The configuration file is stored in `/luma/config.ini` on the SD card (or `/rw/luma/config.ini` on the CTRNAND partition if Luma3DS has been launched from the CTRNAND partition, which happens when SD card is missing).

**The chainloader menu** is accessed by pressing <kbd>Start</kbd> at boot, or from the configuration menu. Payloads are expected to be located in `/luma/payloads` with the `.firm` extension; if there is only one such payload, the aforementioned selection menu will be skipped. Hotkeys can be assigned to payload, for example `x_test.firm` will be chainloaded when <kbd>X</kbd> is pressed at boot.

**The overlay menu, Rosalina**, has a default button combination: <kbd>L+Down+Select</kbd>. For greater flexibility, most Rosalina menu settings aren't saved automatically, hence the "Save settings" option.

**GDB ports**, when enabled, are `4000-4002` for the normal ports. Use of `attach` in "extended-remote" mode, alongside `info os processes` is supported and encouraged (for reverse-engineering, also check out `monitor getmemregions`). The port for the break-on-start feature is `4003` without "extended-remote". Both devkitARM-patched GDB and IDA Pro (without "stepping support" enabled) are actively supported.

We have a wiki, however it is currently very outdated.

## Components

Luma3DS consists of multiple components. While the code style within each component is mostly consistent, these components have been written over many years and may not reflect how maintainers would write new code in new components/projects:

* **arm9**, **arm11**: baremetal main settings menu, chainloader and firmware loader. Aside from showing settings and chainloading to other homebrew firmware files on demand, it is responsible for patching the official firmware to modify `Process9` code and to inject all other custom components. This was the first component ever written for this project, in 2015
* **k11_extension**: code extending the Arm11 `NATIVE_FIRM` kernel (`Kernel11`). It is injected by the above mentioned baremetal loader into the kernel by hooking its startup code, then hooks itself into the rest of the kernel. Its features include hooking system calls (SVCs), introducing new SVCs and hooking into interprocess communications, to bypass limitations in Nintendo's system design. This is the component that allows Rosalina to pause other processes on overlay menu entry, for example. This was written at a time when we didn't fully reverse-engineer the kernel, and originally released in 2017 alongside Rosalina. Further hooks for "game plugin" support have been merged in 2023
* **sysmodules**: reimplementation of "system modules" (processes) of the 3DS's OS (except for Rosalina being custom), currently only initial processes loaded directly in-memory by the kernel ("kernel initial process", or KIP in short)
    * **loader**: process that loads non-KIP processes from storage. Because this is the perfect place to patch/replace executable code, this is where all process patches are done, enabling in particular "game modding" features. This is also the sysmodule handling 3DSX homebrew loading. Introduced in 2016
    * _**rosalina**_: the most important component of Luma3DS and custom KIP: overlay menu, GDB server, `err:f` (fatal error screen) reimplementation, and much more. Introduced in mid-2017, and has continuously undergone changes and received many external contributions ever since
    * **pxi**: Arm11<>Arm9 communication KIP, reimplemented just for the sake of it. Introduced late 2017
    * **sm**: service manager KIP, reimplemented to remove service access control restrictions. Introduced late 2017
    * **pm**: process manager KIP responsible of starting/terminating processes and instructing `loader` to load them. The reimplementation allows for break-on-start GDB feature in Rosalina, as well as lifting FS access control restrictions the proper way. Introduced in 2019

## Maintainers

* **[@TuxSH](https://github.com/TuxSH)**: lead developer, created and maintains most features of the project. Joined in 2016
* **[@AuroraWright](https://github.com/AuroraWright)**: author of the project, implemented the core features (most of the baremetal boot settings menu and firmware loading code) with successful design decisions that made the project popular. Created the project in 2015, currently inactive
* **[@PabloMK7](https://github.com/PabloMK7)**: maintainer of the plugin loader feature merged for the v13.0 release. Joined in 2023

## Roadmap

There are still a lot more features and consolidation planned for Luma3DS! Here is a list of what is currently in store:

* Full reimplementation of `TwlBg` and `AgbBg`. This will allow much better, and more configurable, upscaling for top screen in DS and GBA games (except on Old 2DS). This is currently being developed privately in C++23 (no ETA). While this is quite a difficult endeavor as this requires rewriting the entire driver stack in semi-bare-metal (limited kernel with no IPC), this is the most critical feature for Luma3DS to have and will make driver sysmodule reimplementation trivial
* Reimplementation of `Process9` for `TWL_FIRM` and `AGB_FIRM` to allow for more features in DS and GBA compatibility mode (ones that require file access)
* Eventually, a full `Kernel11` reimplementation

## Known issues

* **Cheat engine crashes with some applications, in particular Pokémon games**: there is a race condition in Nintendo's `Kernel11` pertaining to attaching a new `KDebugThread` to a `KThread` on thread creation, and another thread null-dereferencing `thread->debugThread`. This causes the cheat engine to crashes games that create and destroy many threads all the time (like Pokémon).
    * For these games, having a **dedicated "game plugin"** is the only alternative until `Kernel11` is reimplemented.
* **Applications reacting to Rosalina menu button combo**: Rosalina merely polls button input at an interval to know when to show the menu. This means that the Rosalina menu combo can sometimes be processed by the game/process that is going to be paused.
    * You can **change the menu combo** in the "Miscellaneous options" submenu (then save it with "Save settings" in the main menu) to work around this.

## Building from source

To build Luma3DS, the following is needed:
* git
* [makerom](https://github.com/jakcron/Project_CTR) in `$PATH`
* [firmtool](https://github.com/TuxSH/firmtool) installed
* up-to-date devkitARM and libctru:
    * install `dkp-pacman` (or, for distributions that already provide pacman, add repositories): https://devkitpro.org/wiki/devkitPro_pacman
    * install packages from `3ds-dev` metapackage: `sudo dkp-pacman -S 3ds-dev --needed`
    * while libctru and Luma3DS releases are kept in sync, you may have to build libctru from source for non-release Luma3DS commits

While Luma3DS releases are bundled with `3ds-hbmenu`, Luma3DS actually compiles into one single file: `boot.firm`. Just copy it over to the root of your SD card ([ftpd](https://github.com/mtheall/ftpd) is the easiest way to do so), and you're done.

## Licensing
This software is licensed under the terms of the GPLv3. You can find a copy of the license in the LICENSE.txt file.

Files in the GDB stub are instead triple-licensed as MIT or "GPLv2 or any later version", in which case it's specified in the file header. PM, SM, PXI reimplementations are also licensed under MIT.

## Credits

Luma3DS would not be what it is without the contributions and constructive feedback of many. We would like to thanks in particular:

* **[@devkitPro](https://github.com/devkitPro)** (especially **[@fincs](https://github.com/fincs)**, **[@WinterMute](https://github.com/WinterMute)** and **[@mtheall](https://github.com/mtheall)**) for providing quality and easy-to-use toolchains with bleeding-edge GCC, and for their continued technical advice
* **[@Nanquitas](https://github.com/Nanquitas)** for the initial version of the game plugin loader code as well as very useful contributions to the GDB stub
* **[@piepie62](https://github.com/piepie62)** for the current implementation of the Rosalina cheat engine, **Duckbill** for its original implementation
* **[@panicbit](https://github.com/panicbit)** for the original implementation of screen filters in Rosalina
* **[@jasondellaluce](https://github.com/jasondellaluce)** for LayeredFS
* **[@LiquidFenrir](https://github.com/LiquidFenrir)** for the memory viewer inside Rosalina's "Process List"
* **ChaN** for [FatFs](http://elm-chan.org/fsw/ff/00index_e.html)
* Everyone who has contributed to the Luma3DS repository
* Everyone who has assisted with troubleshooting end-users
* Everyone who has provided constructive feedback to Luma3DS
