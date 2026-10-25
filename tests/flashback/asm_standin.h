/* Stands in for flashback_pack.s on the PC: the same contract (no room check,
   reads one pixel past the row) through flashbackPackPixels with a limit it
   can't reach. The assembly itself is checked by asmcheck.py.
   Include after the extracted recorder code. */
#pragma once
#include <stdint.h>

static void standinPackRow(const u16 *src, const u16 *end, u32 stepBytes, FlashbackPackState *st)
{
    volatile u16 past = *end;   /* (the read past the end of the row) */
    (void)past;
    u32 step = stepBytes / 2;
    flashbackPackPixels(src, (u32)(end - src) / step, step, st, (const u8 *)UINTPTR_MAX);
}
void flashbackPackRowAsm(const u16 *src, const u16 *end, u32 stepBytes, FlashbackPackState *st)
{
    standinPackRow(src, end, stepBytes, st);
}
void flashbackPackRowAsmPf(const u16 *src, const u16 *end, u32 stepBytes, FlashbackPackState *st)
{
    standinPackRow(src, end, stepBytes, st);
}
