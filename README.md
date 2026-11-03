# Luma3DS-Flashback

![GitHub Downloads (all assets, all releases)](https://img.shields.io/github/downloads/LiamTrader43/Luma3DS-Flashback/total)
![License](https://img.shields.io/badge/License-GPLv3-blue.svg)

https://github.com/user-attachments/assets/76ac9ae6-6eb3-4fe5-a946-18c9b925d975

**The Switch's capture button, for the 3DS.** Press a button combo and up to the last 10 minutes of your game are saved as a clip, with sound, in any game, on every 3DS and 2DS.

> **This is an unofficial fork of Luma3DS.** It isn't made or supported by the Luma3DS team, so please report problems [here](https://github.com/LiamTrader43/Luma3DS-Flashback/issues) or on the [Flashback Discord](https://discord.gg/N3TTXP5WB2), not to Luma3DS.

## What you need

* A 3DS or 2DS with custom firmware. If you don't have it yet, follow [3ds.hacks.guide](https://3ds.hacks.guide).
* **Luma3DS v13.5** as your usual Luma3DS ([download](https://github.com/LumaTeam/Luma3DS/releases/tag/v13.5)). With an older version, the configuration menu appears at every boot.
* A **good SD card**. Slow cards make Flashback drop frames (see [Recommended settings](#recommended-settings)).
* To make videos: a Windows 10 or 11 PC.

## Installing

1. Download `boot.firm` from the [latest release](https://github.com/LiamTrader43/Luma3DS-Flashback/releases/latest).
2. Rename it to `y_flashback.firm` and copy it to `/luma/payloads/` on your SD card.
3. Get [Flashback Studio](https://github.com/LiamTrader43/Flashback-Studio/releases/latest) for your 3DS, your PC, or both.
4. **Hold <kbd>Y</kbd> while turning on your 3DS** to start Flashback. Turn it on normally and your usual Luma3DS starts instead.

Then, to save a clip:

1. Open the Rosalina menu (<kbd>L+Down+Select</kbd>) and choose **Flashback recorder**.
2. Press <kbd>A</kbd> to turn recording on, then <kbd>B</kbd> twice to go back to your game.
3. When something worth keeping happens, press <kbd>L+R+Down</kbd>. The bottom screen flashes **green** when the clip is saved (**red** if something went wrong).

Clips are saved to `/luma/flashback/` on your SD card. Open them with Flashback Studio: on the 3DS to watch, trim and compact them, or on Windows to also export them as MP4 videos.

Good to know:

* Recording is off after every restart, so turn it on again each time. Your settings are remembered.
* Close the Rosalina menu within about 7 seconds. Some games lose their sound for a while after the menu has been open longer (a Luma3DS issue, not a Flashback one). Saving with the combo never opens the menu.
* Want Flashback every time without holding <kbd>Y</kbd>? Use the file as your `boot.firm` instead, and keep a copy of your old one.

## Recommended settings

* **New 3DS/2DS:** keep the defaults: full resolution, 30 fps, sound on.
* **Old 3DS/2DS:** start with the same defaults. If a game drops frames or slows down, switch to 20 fps first, then to half resolution.
* **Clip length:** up to 10 minutes. Longer clips need more free space on the SD card: about 6 MB per second at full resolution and 30 fps.
* **SD card:** the faster the better. In testing, a Samsung EVO Plus kept up everywhere.

You change these on the Flashback screen. The [user guide](docs/flashback-guide.md) explains every setting.

## FAQ

**Will it wear out my SD card?**
Recording writes about 10 GB an hour at full resolution and 30 fps, about as much as a dashcam recording HD video. Cards made for dashcams are rated for thousands of hours of that, but it's still wear, so turn recording off when you don't need it.

**Does it slow down games?**
Not noticeably. On an Old 3DS it uses the time the game leaves idle; if a game ever slows down, lower the frame rate or resolution.

**How much space do clips take?**
At full resolution and 30 fps, clips take about 6 MB per second, so a full 10-minute clip is about 3.6 GB. Flashback Studio can compact clips to about half that. Recording also needs free space for one full clip, which it uses as its buffer.

**Old or New 3DS?**
Both record at full resolution and 30 fps. A New 3DS has more power and memory, so it keeps up more easily. On an Old 3DS, demanding games can drop a few frames: each one is filled with a copy of the frame before, so the clip still plays at the right speed. A faster SD card or a lower frame rate helps.

## More information

* **[User guide](docs/flashback-guide.md):** every setting, what the stats on the Flashback screen mean, sound and button recording, SD card usage, and all known limitations.
* **[How it works](docs/flashback-design.md):** the measurements and reasoning behind the recorder (technical).
* **For developers:** the recorder's code is in `sysmodules/rosalina/source/flashback*.c`, and [`tests/flashback`](tests/flashback) has PC tests for it.

## Support

Questions, or found a bug? Join the [Flashback Discord](https://discord.gg/N3TTXP5WB2), or [open an issue](https://github.com/LiamTrader43/Luma3DS-Flashback/issues). If you'd like to support Flashback, you can on [Ko-fi](https://ko-fi.com/liamtrader).

## Credits and license

Flashback is built on [Luma3DS](https://github.com/LumaTeam/Luma3DS) by TuxSH, Aurora Wright, PabloMK7 and its many contributors. See its page for everything else Luma3DS does, and for its credits.

Like Luma3DS, this project is licensed under the GPLv3 (see [LICENSE](LICENSE)). Files in the GDB stub are instead triple-licensed as MIT or "GPLv2 or any later version", as stated in their headers, and the PM, SM and PXI reimplementations are also licensed under MIT.
