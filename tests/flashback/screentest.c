/* PC test of the Flashback screen's in-place redraw (flashback_menu.c's
   flashbackDrawLines / flashbackBlankRows, extracted). Random screens of
   random lines are drawn
     - the old way: clear, then Draw_DrawFormattedString (the reference), and
     - the new way: over whatever the previous screen (or another screen) left,
   and must come out pixel for pixel the same. Every write to the screen is
   watched: a pixel that is the same before and after a redraw must never be
   given another value in between (that would show as a flicker).
   Draw_DrawCharacter / Draw_DrawString are copied from Rosalina's draw.c, with
   a made-up font. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint16_t u16;
typedef uint32_t u32;

#define SCREEN_BOT_WIDTH  320
#define SCREEN_BOT_HEIGHT 240
#define SPACING_Y 11
#define SPACING_X 6
#define COLOR_BLACK 0
#define DRAW_MAX_FORMATTED_STRING_SIZE 512

static u16 g_fb[SCREEN_BOT_WIDTH * SCREEN_BOT_HEIGHT];
#define FB_BOTTOM_VRAM_ADDR ((void *)g_fb)

/* watching writes: the screen before a redraw, and whether a pixel that should
   stay the same was given another value */
static u16 g_before[SCREEN_BOT_WIDTH * SCREEN_BOT_HEIGHT];
static int g_watch;
static long g_flicker, g_errors;
static u16 g_after[SCREEN_BOT_WIDTH * SCREEN_BOT_HEIGHT];
static void watchWrite(u32 i, u16 v)
{
    if (g_watch && g_before[i] == g_after[i] && v != g_before[i])
        g_flicker++;
}

static unsigned char font[256 * 10];

static void Draw_DrawCharacter(u32 posX, u32 posY, u32 color, char character)
{
    u16 *const fb = (u16 *)FB_BOTTOM_VRAM_ADDR;
    int y;
    for (y = 0; y < 10; y++)
    {
        char charPos = font[(unsigned char)character * 10 + y];
        int x;
        for (x = 6; x >= 1; x--)
        {
            u32 screenPos = (posX * SCREEN_BOT_HEIGHT * 2 + (SCREEN_BOT_HEIGHT - y - posY - 1) * 2) + (5 - x) * 2 * SCREEN_BOT_HEIGHT;
            u32 pixelColor = ((charPos >> x) & 1) ? color : COLOR_BLACK;
            watchWrite(screenPos / 2, (u16)pixelColor);
            fb[screenPos / 2] = (u16)pixelColor;
        }
    }
}

static u32 Draw_DrawString(u32 posX, u32 posY, u32 color, const char *string)
{
    for (u32 i = 0, line_i = 0; i < strlen(string); i++)
        switch (string[i])
        {
            case '\n':
                posY += SPACING_Y;
                line_i = 0;
                break;
            case '\t':
                line_i += 2;
                break;
            default:
                if (line_i >= ((SCREEN_BOT_WIDTH) - posX) / SPACING_X)
                {
                    posY += SPACING_Y;
                    line_i = 1;
                    if (string[i] == ' ') break;
                }
                Draw_DrawCharacter(posX + line_i * SPACING_X, posY, color, string[i]);
                line_i++;
                break;
        }
    return posY;
}

static u32 Draw_DrawFormattedString(u32 posX, u32 posY, u32 color, const char *fmt, ...)
{
    char buf[DRAW_MAX_FORMATTED_STRING_SIZE + 1];
    va_list args;
    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    va_end(args);
    return Draw_DrawString(posX, posY, color, buf);
}

/* the blanking's memset, watched */
static void *watchedMemset(void *p, int v, size_t n)
{
    u16 *q = p;
    if (q >= g_fb && q < g_fb + SCREEN_BOT_WIDTH * SCREEN_BOT_HEIGHT)     /* (the screen, not a line of text) */
        for (size_t k = 0; k < n / 2; k++)
            watchWrite((u32)(q + k - g_fb), (u16)v);
    return memset(p, v, n);
}
#define memset watchedMemset
#include "drawlines_extracted.c"     /* (made by run_tests.py) */
#undef memset

/* a screen: lines of text (each "...\n", some "\n\n"), some gaps of 10 rows */
#define MAXL 24
typedef struct { char text[MAXL][80]; u32 color[MAXL]; int gapBefore[MAXL]; int n; } Screen;

static void randomScreen(Screen *s)
{
    s->n = 3 + rand() % 14;
    for (int i = 0; i < s->n; i++)
    {
        int len = rand() % 4 == 0 ? 0 : rand() % (FLASHBACK_SCREEN_COLS + 1);   /* never past the width */
        for (int k = 0; k < len; k++)
            s->text[i][k] = (char)(' ' + rand() % 95);
        strcpy(s->text[i] + len, rand() % 5 == 0 ? "\n\n" : "\n");
        if (i == s->n - 1 && rand() % 2)
            s->text[i][len] = '\0';     /* the last line without '\n' */
        s->color[i] = 1 + rand() % 0xFFFE;
        s->gapBefore[i] = rand() % 6 == 0;
    }
}

static void drawOld(const Screen *s)
{
    memset(g_fb, 0, sizeof(g_fb));
    Draw_DrawString(10, 10, 0x1234, "Flashback recorder");
    u32 posY = 30;
    for (int i = 0; i < s->n && posY < 220; i++)
    {
        if (s->gapBefore[i]) posY += 10;
        posY = Draw_DrawFormattedString(10, posY, s->color[i], "%s", s->text[i]);
    }
}

static void drawNew(const Screen *s)
{
    g_screenBlankFrom = 0;
    flashbackDrawLines(10, 0x1234, "Flashback recorder");
    u32 posY = 30;
    for (int i = 0; i < s->n && posY < 220; i++)
    {
        if (s->gapBefore[i]) posY += 10;
        posY = flashbackDrawLines(posY, s->color[i], "%s", s->text[i]);
    }
    flashbackBlankRows(g_screenBlankFrom, SCREEN_BOT_HEIGHT);
}

int main(void)
{
    srand(7);
    for (int i = 0; i < 256 * 10; i++)
        font[i] = (unsigned char)(i * 37 + (i >> 3) * 11);
    for (int y = 0; y < 10; y++)
        font[' ' * 10 + y] = 0;     /* a space is blank */

    Screen prev, cur;
    randomScreen(&prev);
    drawOld(&prev);     /* what's there when the screen opens (or another screen) */
    long redraws = 0, same = 0;
    for (int n = 0; n < 20000; n++)
    {
        if (rand() % 4 == 0) randomScreen(&cur); else { cur = prev; same++; }   /* (often nothing changes) */
        if (rand() % 50 == 0)
        {
            /* another screen was up (the hotkey screen): whatever it drew */
            Screen other;
            randomScreen(&other);
            drawOld(&other);
        }
        drawOld(&cur);
        memcpy(g_after, g_fb, sizeof(g_fb));    /* the reference */
        /* back to what was on screen before, then the new way over it */
        drawOld(&prev);
        if (n % 50 == 0) { Screen other; randomScreen(&other); drawOld(&other); }
        memcpy(g_before, g_fb, sizeof(g_fb));
        g_watch = 1;
        drawNew(&cur);
        g_watch = 0;
        if (memcmp(g_fb, g_after, sizeof(g_fb)) != 0)
        {
            long diff = 0;
            for (int i = 0; i < SCREEN_BOT_WIDTH * SCREEN_BOT_HEIGHT; i++) diff += g_fb[i] != g_after[i];
            if (g_errors++ < 5) fprintf(stderr, "ERROR: redraw %d differs from clear+draw in %ld pixels\n", n, diff);
        }
        prev = cur;
        redraws++;
    }
    printf("%ld redraws (%ld unchanged screens); pixels flickered: %ld\n", redraws, same, g_flicker);
    if (g_flicker) g_errors++;
    printf("%s (%ld errors)\n", g_errors ? "FAILED" : "all good", g_errors);
    return g_errors != 0;
}
