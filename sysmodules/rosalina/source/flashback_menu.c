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

// Flashback: the Flashback screen and the settings file (see
// flashback_internal.h).

#include <3ds.h>
#include <string.h>
#include "flashback_internal.h"
#include "draw.h"
#include "fmt.h"
#include "luma_config.h"
#include "menu.h"

// ---------------------------------------------------------------------------
// Settings file
// ---------------------------------------------------------------------------

#define FLASHBACK_SETTINGS_FILE     FLASHBACK_DIR "/settings.bin"
#define FLASHBACK_SETTINGS_MAGIC    0x54534246  // "FBST"
// Version 2 added the clip length, as an index into the 5 to 30 second list of
// that time; version 3 stores it in seconds, so the list can change; version 4
// added audio. Older files still load.
#define FLASHBACK_SETTINGS_VERSION  4

typedef struct FlashbackSettings {
    u32 magic;
    u16 version;
    u16 size;           // sizeof(FlashbackSettings), so newer versions can append fields
    u8  enabled;        // unused (always 0): recording is always off at boot
    u8  res;            // FlashbackResolution
    u8  fpsIdx;         // index into g_fpsChoices
    u8  format;         // unused (always 0): was the save format, when BMP frames were one
    u32 saveCombo;
    // Version 2 and later:
    u8  length;         // clip length in seconds (version 2: index into { 5, 10, ..., 30 })
    // Version 4 and later:
    u8  audio;          // record audio: 1 or 0
    u8  reserved[2];
} FlashbackSettings;

// The shortest available length that's at least 'seconds' (or the longest).
static u32 flashbackLengthIdxFor(u32 seconds)
{
    for (u32 i = 0; i < FLASHBACK_LENGTH_CHOICES; i++)
        if (g_lengthChoices[i] >= seconds)
            return i;
    return FLASHBACK_LENGTH_CHOICES - 1;
}

#define FLASHBACK_SETTINGS_V1_SIZE  16  // everything up to and including saveCombo

static const char *flashbackCheckCombo(u32 combo);

static FlashbackSettings flashbackCurrentSettings(void)
{
    FlashbackSettings s;
    memset(&s, 0, sizeof(s));
    s.magic = FLASHBACK_SETTINGS_MAGIC;
    s.version = FLASHBACK_SETTINGS_VERSION;
    s.size = sizeof(s);
    s.enabled = 0;      // not remembered, so toggling recording doesn't rewrite the file
    s.res = (u8)g_wantRes;
    s.fpsIdx = (u8)g_wantFpsIdx;
    s.saveCombo = g_saveCombo;
    s.length = (u8)g_lengthChoices[g_wantLenIdx];
    s.audio = g_wantAudio ? 1 : 0;
    return s;
}

// Loads saved settings, keeping the defaults for anything missing or invalid.
void flashbackLoadSettings(void)
{
    FlashbackSettings s;
    IFile file = {0};
    u64 total = 0;

    if (R_FAILED(IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                            fsMakePath(PATH_ASCII, FLASHBACK_SETTINGS_FILE), FS_OPEN_READ)))
        return;

    memset(&s, 0, sizeof(s));
    Result res = IFile_Read(&file, &total, &s, sizeof(s));
    IFile_Close(&file);

    if (R_FAILED(res) || total < FLASHBACK_SETTINGS_V1_SIZE || s.magic != FLASHBACK_SETTINGS_MAGIC ||
        s.version < 1 || s.version > FLASHBACK_SETTINGS_VERSION)
        return;

    // Version 1 files end before the length: it keeps its default.
    if (s.version == 2 && total >= sizeof(s) && s.length < 6)
        g_wantLenIdx = flashbackLengthIdxFor((s.length + 1) * 5);   // 5..30 s, rounded up
    else if (s.version >= 3 && total >= sizeof(s) && s.length != 0)
        g_wantLenIdx = flashbackLengthIdxFor(s.length);
    if (s.res < FLASHBACK_RES_COUNT)
        g_wantRes = s.res;
    if (s.fpsIdx < FLASHBACK_FPS_CHOICES)
        g_wantFpsIdx = s.fpsIdx;
    if (s.version >= 4 && total >= sizeof(s))
        g_wantAudio = s.audio != 0;     // (older files: the default, on)
    // Re-checked in case the Rosalina menu combo was changed since.
    if (flashbackCheckCombo(s.saveCombo) == NULL)
        g_saveCombo = s.saveCombo;
    // Recording itself always starts off at boot, whatever was saved.
}

static void flashbackSaveSettings(void)
{
    FlashbackSettings s = flashbackCurrentSettings();
    FS_Archive archive;
    IFile file = {0};
    u64 total;

    if (!flashbackIsSdMode() || R_FAILED(FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
        return;

    if (R_SUCCEEDED(flashbackCreateDirectory(archive, FLASHBACK_DIR)) &&
        R_SUCCEEDED(IFile_OpenFromArchive(&file, archive, fsMakePath(PATH_ASCII, FLASHBACK_SETTINGS_FILE),
                                          FS_OPEN_CREATE | FS_OPEN_WRITE)))
    {
        IFile_SetSize(&file, sizeof(s));
        IFile_Write(&file, &total, &s, sizeof(s), 0);
        IFile_Close(&file);
    }

    FSUSER_CloseArchive(archive);
}

// ---------------------------------------------------------------------------
// Hotkey and menu
// ---------------------------------------------------------------------------

// Buttons allowed in the save hotkey: face buttons, D-Pad, L/R, ZL/ZR,
// Start/Select (not the touchscreen, Circle Pad or C-Stick).
#define FLASHBACK_COMBO_KEYS    (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R | KEY_ZL | KEY_ZR | \
                                 KEY_START | KEY_SELECT | KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)

// Returns why 'combo' can't be the save hotkey, or NULL if it can.
static const char *flashbackCheckCombo(u32 combo)
{
    u32 keys = combo & FLASHBACK_COMBO_KEYS;

    if (keys != combo)
        return "Only buttons can be used:\nnot the touchscreen, Circle Pad or C-Stick.";
    if (__builtin_popcount(keys) < 2)
        return "Use at least 2 buttons, so it can't be\npressed by accident during play.";
    // Inside the menu combo: opening the menu would also save. Containing it:
    // the menu would open instead of saving.
    if ((keys & menuCombo) == keys || (keys & menuCombo) == menuCombo)
        return "It overlaps the Rosalina menu combo.";
    return NULL;
}

static void FlashbackMenu_ChangeHotkey(void)
{
    char current[128], entered[128];
    LumaConfig_ConvertComboToString(current, g_saveCombo);

    // Ask for a combo until a valid one is entered, or the user backs out.
    for (;;)
    {
        Draw_Lock();
        Draw_ClearFramebuffer();
        Draw_DrawString(10, 10, COLOR_TITLE, "Flashback recorder: save hotkey");
        u32 posY = Draw_DrawFormattedString(10, 30, COLOR_WHITE, "Current hotkey: %s\n\n", current);
        Draw_DrawString(10, posY, COLOR_WHITE, "Hold the new hotkey (2 or more buttons),\nthen let go of all buttons.");
        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 combo = waitCombo();    // waits for the previous buttons to be released first
        if (combo == 0)             // menu closing (sleep, shell closed...)
            return;

        const char *error = flashbackCheckCombo(combo);
        if (error == NULL)
            g_saveCombo = combo;
        LumaConfig_ConvertComboToString(entered, combo);

        u32 pressed;
        do
        {
            Draw_Lock();
            Draw_ClearFramebuffer();
            Draw_DrawString(10, 10, COLOR_TITLE, "Flashback recorder: save hotkey");
            posY = Draw_DrawFormattedString(10, 30, COLOR_WHITE, "You pressed: %s\n\n", entered);
            if (error == NULL)
            {
                posY = Draw_DrawFormattedString(10, posY, COLOR_GREEN, "Save hotkey changed to %s.\n", entered);
                Draw_DrawString(10, posY + 10, COLOR_WHITE, "Press B to go back.");
            }
            else
            {
                posY = Draw_DrawFormattedString(10, posY, COLOR_RED, "%s\nThe hotkey is still %s.\n", error, current);
                Draw_DrawString(10, posY + 10, COLOR_WHITE, "Press A to try again, or B to go back.");
            }
            Draw_FlushFramebuffer();
            Draw_Unlock();

            pressed = waitInput();
            if (menuShouldExit)
                return;
        }
        while (!(pressed & KEY_B) && !(error != NULL && (pressed & KEY_A)));

        if (pressed & KEY_B)
            return;
        // A after an invalid combo: ask again.
    }
}

// The Flashback screen is redrawn a few times a second, in place: Rosalina
// draws straight onto the screen, so clearing it first would show as a
// flicker. Instead each line is drawn the whole width of the screen (its text,
// then spaces), and the rows between and below the lines are blanked, so
// nothing from before is left over and nothing goes blank in between.
#define FLASHBACK_SCREEN_COLS   ((SCREEN_BOT_WIDTH - 10) / SPACING_X)  // (Draw_DrawString wraps after this many)

static u32 g_screenBlankFrom;   // the first row not drawn yet this time

// Blanks rows y0 .. y1 - 1 of the bottom screen (pixel (x, y) is at
// fb[x * 240 + 239 - y]).
static void flashbackBlankRows(u32 y0, u32 y1)
{
    u16 *fb = (u16 *)FB_BOTTOM_VRAM_ADDR;
    if (y1 > SCREEN_BOT_HEIGHT)
        y1 = SCREEN_BOT_HEIGHT;
    if (y0 >= y1)
        return;
    for (u32 x = 0; x < SCREEN_BOT_WIDTH; x++)
        memset(fb + x * SCREEN_BOT_HEIGHT + (SCREEN_BOT_HEIGHT - y1), 0, (y1 - y0) * 2);
}

// Draw_DrawFormattedString at x = 10, the Flashback screen's way: each line
// padded to the screen's width (a longer one goes on below, in pieces), and
// the rows above it that nothing drew blanked. Returns the y after the last
// '\n', as Draw_DrawFormattedString does.
static u32 flashbackDrawLines(u32 posY, u32 color, const char *fmt, ...)
{
    char text[DRAW_MAX_FORMATTED_STRING_SIZE + 1], line[FLASHBACK_SCREEN_COLS + 1];
    va_list args;
    va_start(args, fmt);
    vsprintf(text, fmt, args);
    va_end(args);

    for (const char *p = text; *p != '\0'; )
    {
        u32 len = 0;
        while (p[len] != '\0' && p[len] != '\n')
            len++;
        for (;;)
        {
            u32 n = len < FLASHBACK_SCREEN_COLS ? len : FLASHBACK_SCREEN_COLS;
            memset(line, ' ', FLASHBACK_SCREEN_COLS);
            memcpy(line, p, n);
            line[FLASHBACK_SCREEN_COLS] = '\0';
            flashbackBlankRows(g_screenBlankFrom, posY);
            Draw_DrawString(10, posY, color, line);
            g_screenBlankFrom = posY + 10;      // (characters are 10 rows high)
            p += n;
            len -= n;
            if (len == 0)
                break;
            posY += SPACING_Y;
        }
        if (*p == '\n')
        {
            posY += SPACING_Y;
            p++;
        }
    }
    return posY;
}

void FlashbackMenu_Show(void)
{
    bool sdMode = flashbackIsSdMode();
    char comboStr[128];
    FlashbackSettings before = flashbackCurrentSettings();

    do
    {
        // What the settings will be once applied (they apply within one frame).
        u32 res = g_wantRes;
        u32 fps = g_fpsChoices[g_wantFpsIdx];
        u32 seconds = g_lengthChoices[g_wantLenIdx];
        u32 frames = seconds * fps;
        bool audio = flashbackAudioWanted();
        bool applied = res == g_res && g_wantFpsIdx == g_fpsIdx && g_wantLenIdx == g_lenIdx && audio == g_audio;

        Draw_Lock();
        g_screenBlankFrom = 0;
        flashbackDrawLines(10, COLOR_TITLE, "Flashback recorder");

        u32 posY = 30;
        if (!g_threadRunning)
            posY = flashbackDrawLines(posY, COLOR_RED, "Recorder thread failed to start.\n\n");
        else if (!sdMode)
            posY = flashbackDrawLines(posY, COLOR_RED, "Only available when booting from the SD card.\n\n");

        posY = flashbackDrawLines(posY, COLOR_WHITE, "Recording:  %s\n", g_enabled ? "on" : "off");
        posY = flashbackDrawLines(posY, COLOR_WHITE, "Resolution: %s\n",
                                  res == FLASHBACK_RES_FULL ? "full (400x240)" : "half (200x120)");
        // Size of a saved clip of the full length: the ring file (the audio
        // ring, and the frames' slots: see flashbackSlotStride).
        u32 frameBytes = (res == FLASHBACK_RES_FULL ? 400 * 240 : 200 * 120) * 2;
        u32 audioBytes = audio ? flashbackAudioFramesFor(seconds) * FLASHBACK_AUDIO_FRAME_BYTES : 0;
        u64 clipBytes = flashbackVideoOffset(audioBytes) + (u64)frames * flashbackSlotStride(frameBytes);
        u32 clipTenthsMb = (u32)((clipBytes + 50000) / 100000);     // in tenths of a MB (1 MB = 1,000,000 bytes)

        if (clipTenthsMb < 100)
            posY = flashbackDrawLines(posY, COLOR_WHITE, "Frame rate: %lu fps   Length: %lu s (%lu.%lu MB)\n",
                                      fps, seconds, clipTenthsMb / 10, clipTenthsMb % 10);
        else
            posY = flashbackDrawLines(posY, COLOR_WHITE, "Frame rate: %lu fps   Length: %lu s (%lu MB)\n",
                                      fps, seconds, (clipTenthsMb + 5) / 10);
        posY = flashbackDrawLines(posY, COLOR_WHITE, "Audio:      %s\n\n",
                                  !g_audioRunning ? "failed" : g_wantAudio ? "on" : "off");

        // Counted the same way a save would be: frame times in the clip, and
        // how many of those are dropped frames filled with a copy.
        u32 buffered = 0, dropped = 0;
        if (applied && g_active)
        {
            FlashbackClipPlan plan = flashbackPlanClip();
            buffered = plan.frames;
            dropped = plan.frames - plan.slots;
        }

        posY = flashbackDrawLines(posY, COLOR_WHITE, "Buffered:  %lu / %lu frames, %lu dropped\n",
                                  buffered, frames, dropped);
        if (applied && g_active && g_audio)
            posY = flashbackDrawLines(posY, COLOR_WHITE, "Rate:      %lu.%lu / %lu fps   audio: %lu missed\n",
                                      g_statFpsX10 / 10, g_statFpsX10 % 10, fps, g_audioMissed);
        else
            posY = flashbackDrawLines(posY, COLOR_WHITE, "Rate:      %lu.%lu / %lu fps\n",
                                      g_statFpsX10 / 10, g_statFpsX10 % 10, fps);
        posY = flashbackDrawLines(posY, COLOR_WHITE, "Per frame: capture %lu.%lu, compress %lu.%lu, write %lu.%lums\n",
                                  g_statCaptureUs / 1000, (g_statCaptureUs / 100) % 10,
                                  g_statCompressUs / 1000, (g_statCompressUs / 100) % 10,
                                  g_statWriteUs / 1000, (g_statWriteUs / 100) % 10);
        posY = flashbackDrawLines(posY, COLOR_WHITE, "Budget:    %lums, compressed to %lu%%, raw %lu%%\n\n",
                                  1000 / fps, g_statSizePct, g_statRawPct);

        const char *recordError = g_recordError;
        if (g_saving || g_saveRequested)
            posY = flashbackDrawLines(posY, COLOR_WHITE, "Saving...\n");
        else if (recordError != NULL)
            posY = flashbackDrawLines(posY, COLOR_RED, "%s\n", recordError);
        else if (g_lastSaveResult > 0)
            posY = flashbackDrawLines(posY, COLOR_GREEN, "Saved %ld frames in %lu.%lus to\n%s\n", g_lastSaveResult,
                                      g_lastSaveMs / 1000, (g_lastSaveMs / 100) % 10, g_lastClip);
        else if (g_lastSaveResult < 0)
            posY = flashbackDrawLines(posY, COLOR_RED, "Save failed (%ld).\n", g_lastSaveResult);

        posY += 10;
        posY = flashbackDrawLines(posY, COLOR_WHITE, "A: recording on/off   X: save clip now   B: back\n");
        posY = flashbackDrawLines(posY, COLOR_WHITE, "Y: resolution   Left/Right: fps   L/R: length\n");
        posY = flashbackDrawLines(posY, COLOR_WHITE, "Start: audio   Select: hotkey\n");
        posY = flashbackDrawLines(posY, COLOR_WHITE, "Off or res/fps/length/audio clear the buffer.\n\n");
        LumaConfig_ConvertComboToString(comboStr, g_saveCombo);
        flashbackDrawLines(posY, COLOR_WHITE, "In game: %s saves a clip.", comboStr);
        flashbackBlankRows(g_screenBlankFrom, SCREEN_BOT_HEIGHT);

        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 pressed = waitInputWithTimeout(250);
        bool busy = g_saving || g_saveRequested;

        if ((pressed & KEY_A) && sdMode && g_threadRunning)
        {
            g_enabled = !g_enabled;
            g_recordError = NULL;
        }
        else if ((pressed & KEY_X) && !busy)
        {
            g_saveFromHotkey = false;
            g_saveRequested = true;
        }
        else if ((pressed & KEY_Y) && !busy)
            g_wantRes = (g_wantRes + 1) % FLASHBACK_RES_COUNT;
        else if ((pressed & KEY_DRIGHT) && !busy)
            g_wantFpsIdx = (g_wantFpsIdx + 1) % FLASHBACK_FPS_CHOICES;
        else if ((pressed & KEY_DLEFT) && !busy)
            g_wantFpsIdx = (g_wantFpsIdx + FLASHBACK_FPS_CHOICES - 1) % FLASHBACK_FPS_CHOICES;
        else if ((pressed & KEY_R) && !busy && g_wantLenIdx + 1 < FLASHBACK_LENGTH_CHOICES)
            g_wantLenIdx = g_wantLenIdx + 1;    // longer (stops at the longest)
        else if ((pressed & KEY_L) && !busy && g_wantLenIdx > 0)
            g_wantLenIdx = g_wantLenIdx - 1;    // shorter (stops at the shortest)
        else if ((pressed & KEY_START) && !busy && g_audioRunning)
            g_wantAudio = !g_wantAudio;
        else if (pressed & KEY_SELECT)
            FlashbackMenu_ChangeHotkey();
        else if (pressed & KEY_B)
            break;
    }
    while (!menuShouldExit);

    // Remember the settings for the next boot (only if something changed).
    FlashbackSettings after = flashbackCurrentSettings();
    if (memcmp(&before, &after, sizeof(before)) != 0)
        flashbackSaveSettings();
}
