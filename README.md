# Luma3DS-Flashback

![GitHub Downloads (all assets, all releases)](https://img.shields.io/github/downloads/LumaTeam/Luma3DS/total)
![License](https://img.shields.io/badge/License-GPLv3-blue.svg)

*Fork of Luma3DS that adds a flashback record option*

![Boot menu screenshot](img/boot_menu_v1321.png)
![Rosalina menu screenshot](img/rosalina_menu_v1321.png)

> **This is an unofficial fork.** It is not made or supported by the Luma3DS team. Please report Flashback issues here, not to the official Luma3DS repository.

## Flashback recorder

Flashback is a "replay buffer" for the 3DS top screen. While recording is on, it keeps the **last 10 seconds** of gameplay. When something worth keeping happens, press a button combo and those 10 seconds are saved to the SD card as a video file.

It is built into Rosalina, so it works in **any game**, including retail games, without plugins. It uses none of the game's memory and has no noticeable effect on game performance.

### Installing

Flashback is a complete Luma3DS build, so it needs [boot9strap](https://github.com/SciresM/boot9strap) like regular Luma3DS. Download `boot.firm` from this fork's releases, then choose one of these:

* **Try it alongside your current Luma3DS (recommended):** rename and copy `boot.firm` to `/luma/payloads/y_flashback.firm` on your SD card. Hold <kbd>Y</kbd> while powering on to boot Flashback; power on normally to boot your usual Luma3DS. (When started this way, Flashback won't chainload any further payloads, so holding <kbd>Y</kbd> too long is fine. To start a different payload, boot your usual Luma3DS.)
* **Use it as your main Luma3DS:** replace `boot.firm` in the root of your SD card. Keep a copy of your old `boot.firm` in case you want to go back.

To check that you're running Flashback, open the Rosalina menu (<kbd>L+Down+Select</kbd>): the bottom line reads **"Luma3DS v13.4 (Flashback)"** and the menu has a **Flashback recorder** entry.

### Recording and saving a clip

1. Open the Rosalina menu with <kbd>L+Down+Select</kbd> and choose **Flashback recorder**.
2. Press <kbd>A</kbd> to turn **Recording** on, then press <kbd>B</kbd> twice to return to your game.
3. Play. Flashback always keeps the most recent 10 seconds.
4. When you want to keep a moment, press <kbd>L+R+Down</kbd>. The bottom screen gets a see-through **white** tint while saving, then briefly turns **green** when the clip is saved (or **red** if something went wrong). You can keep seeing and using the bottom screen the whole time.

You can also save from the Flashback screen with <kbd>X</kbd>. Recording pauses while any Rosalina menu is open (the game is paused too), so saving from the menu doesn't add frozen frames to the end of your clip.

### Flashback screen controls

| Button | Action |
|---|---|
| <kbd>A</kbd> | Turn recording on/off |
| <kbd>X</kbd> | Save a clip now |
| <kbd>Y</kbd> | Resolution: half (200x120) or full (400x240) |
| <kbd>Left</kbd>/<kbd>Right</kbd> | Frame rate: 10, 20 or 30 fps |
| <kbd>Up</kbd>/<kbd>Down</kbd> | Save format: raw video or BMP frames |
| <kbd>Select</kbd> | Change the save hotkey |
| <kbd>B</kbd> | Back |

Changing the resolution or frame rate clears the 10-second buffer. Changing the save format doesn't.

The screen also shows live stats. **Rate** should stay close to the frame rate you picked. If **duplicated** keeps climbing, your SD card can't keep up with the current settings: missed frames are filled with copies of the previous frame so clips still play at the right speed, but a lower resolution or frame rate will look smoother.

**Changing the save hotkey:** press <kbd>Select</kbd>, hold the new combination (at least 2 buttons), then let go. Combos that overlap the Rosalina menu combo aren't allowed.

### Where clips are saved and how to watch them

Clips are saved in `/luma/flashback/` on the SD card, named after the date and time they were saved.

**Raw video (default, fastest to save).** Each clip is a `clip_<date>.raw` file plus a `clip_<date>.txt` that lists its resolution and frame rate, and contains a ready-to-use command to turn it into an MP4. To convert it:

1. Install [ffmpeg](https://ffmpeg.org/download.html) on your PC and make sure the `ffmpeg` command works in a terminal.
2. Copy the `.raw` and `.txt` files to your PC.
3. Open a terminal in that folder and run the command from the `.txt`. It looks like this:

   ```
   ffmpeg -f rawvideo -pixel_format rgb565le -video_size 400x240 -framerate 30 -i "clip_<date>.raw" -vf "scale=iw*2:ih*2:flags=neighbor" -c:v libx264 -crf 12 -pix_fmt yuv420p "clip_<date>.mp4"
   ```

The MP4 plays in any video player and imports into video editors like DaVinci Resolve. The command doubles the size with sharp pixels; for a 1080p editing timeline, use `scale=iw*4.5:ih*4.5:flags=neighbor` at full resolution instead.

**BMP frames.** Each clip is a `clip_<date>/` folder of numbered images (`frame_000.bmp`, `frame_001.bmp`, ...) that you can open directly. To turn them into a video, run this in the clip folder, using the frame rate you recorded at:

```
ffmpeg -framerate 30 -i frame_%03d.bmp -vf "scale=iw*2:ih*2:flags=neighbor" -c:v libx264 -crf 12 -pix_fmt yuv420p clip.mp4
```

### SD card usage

The buffer lives in `/luma/flashback/ring.bin` on the SD card, and recording writes to it continuously.

| Setting | Buffer file size | Saved clip (raw) | Written while recording |
|---|---|---|---|
| Half resolution, 10 fps | 4.8 MB | 4.8 MB | about 1.7 GB per hour |
| Full resolution, 30 fps | 58 MB | 58 MB | about 21 GB per hour |

A good-quality SD card handles this easily, but continuous writing does add wear, especially at full resolution and 30 fps. Turn recording off when you don't need it.

### Limitations

* **Recording is always off after a reboot**: turn it on from the Flashback screen each time. Your other settings (resolution, frame rate, format and hotkey) are remembered in `/luma/flashback/settings.bin` when you leave the Flashback screen. Delete that file to go back to the defaults: full resolution and 30 fps on New 3DS/2DS (half resolution and 20 fps on Old 3DS/2DS), raw format, hotkey <kbd>L+R+Down</kbd>.
* **Only the top screen** is recorded, and only the left eye when 3D is on. There is no audio.
* **SD card only**: Flashback is disabled when Luma3DS boots from the internal memory (NAND).
* **Old 3DS/2DS are slower**: they have a much slower CPU, so higher settings can make games run slower. That's why they default to half resolution and 20 fps. If a game slows down, lower the resolution or frame rate.

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
