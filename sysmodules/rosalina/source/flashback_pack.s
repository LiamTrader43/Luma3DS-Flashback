@ Flashback's packer (flashbackCompress, flashback_compress.c) in ARM assembly,
@ arranged for the ARM11 (in order, one instruction at a time): the same output
@ as flashbackPackPixels, byte for byte, without its room check.
@
@   void flashbackPackRowAsm(const u16 *src, const u16 *end, u32 stepBytes, FlashbackPackState *st);
@   void flashbackPackRowAsmPf(...)     the same, prefetching the cache line 8 pixels on
@
@ Packs the pixels at src, src + stepBytes, ... up to (not including) 'end'.
@ The caller has made sure there's room for 4 bytes per pixel. Reads one pixel
@ past the last (at 'end'), which must be readable. FlashbackPackState is
@ { u32 *index; u8 *out; u32 prev; u32 run; } (in and out; the table is 64
@ words). An Old 3DS packs a full resolution frame in ~10 ms this way (~14 in
@ C), most of it waiting for the frame's cache lines.
@
@ The waits it's arranged around: a load's result is ready 3 cycles on, a
@ multiply's ~4, and a mispredicted branch costs ~5. So the next pixel is
@ loaded while this one is packed, the hash multiply starts before the run
@ flush (conditional instructions, not a branch), the table lookup's wait
@ starts on the channel differences, and two copies of the loop take turns
@ with the "this pixel" and "next pixel" registers, so neither needs a move.
@
@ Registers: r0 src (the pixel after the one loaded last), r1 end + step,
@ r2 step, r4 table, r5 out, r6 prev, r7 run, r8 the hash constant, r9/r10
@ this and the next pixel (taking turns); r3, r11, r12, lr scratch.

.arm
.syntax unified

@ One pixel: 'cur' is it, 'nxt' gets the next; 'self' names this copy's labels,
@ 'other' the copy that carries on with the registers swapped.
.macro PIXEL cur, nxt, self, other, pf, done
\self\()_top:
.if \pf
    pld     [r0, r2, lsl #3]
.endif
    ldrh    \nxt, [r0], r2
    cmp     \cur, r6
    bne     \self\()_new
    @ RUN: the previous pixel again
    add     r7, r7, #1
    cmp     r7, #62
    beq     \self\()_runmax
\self\()_next:
    cmp     r0, r1
    bne     \other\()_top
    b       \done
\self\()_runmax:
    mov     r3, #0xFD               @ 0xC0 | (62 - 1)
    strb    r3, [r5], #1
    mov     r7, #0
    b       \self\()_next

\self\()_new:
    mul     r3, \cur, r8            @ hash: (px * 0x9E3779B1) >> 26
    cmp     r7, #0                  @ a pending run: 0xC0 | (run - 1)
    addne   r11, r7, #0xBF
    strbne  r11, [r5], #1
    movne   r7, #0
    lsr     r3, r3, #26
    ldr     r11, [r4, r3, lsl #2]
    lsr     r12, r6, #11            @ (while the load is on its way: prev's
    lsr     lr, r6, #5              @  channels, for DIFF/LUMA)
    cmp     r11, \cur
    bne     \self\()_miss
    @ INDEX
    strb    r3, [r5], #1
    mov     r6, \cur
    cmp     r0, r1
    bne     \other\()_top
    b       \done

\self\()_miss:
    str     \cur, [r4, r3, lsl #2]
    rsb     r12, r12, \cur, lsr #11 @ red difference (red's 5 bits)
    rsb     lr, lr, \cur, lsr #5    @ green (6 bits; red's bits are above them)
    sub     r11, \cur, r6           @ blue (5 bits; the others are above them)
    lsl     r12, r12, #27           @ each wrapped by moving it to the top:
    lsl     lr, lr, #26             @ dr << 27, dg << 26, db << 27
    lsl     r11, r11, #27
    mov     r6, \cur
    @ DIFF: dr, dg, db all in -2..1 (each + 2, at the top, below 4 there)
    add     r3, r12, #0x10000000
    cmp     r3, #0x20000000
    addlo   r3, lr, #0x08000000
    cmplo   r3, #0x10000000
    addlo   r3, r11, #0x10000000
    cmplo   r3, #0x20000000
    bhs     \self\()_luma
    @ 0x40 | (dr+2)<<4 | (dg+2)<<2 | (db+2) = 0x6A + dr*16 + dg*4 + db
    asr     r3, r11, #27
    add     r3, r3, r12, asr #23
    add     r3, r3, lr, asr #24
    add     r3, r3, #0x6A
    strb    r3, [r5], #1
    cmp     r0, r1
    bne     \other\()_top
    b       \done

\self\()_luma:
    @ tr = dr - (dg >> 1), tb = db - (dg >> 1), at << 26 (they need 6 bits)
    asr     r3, lr, #1
    bic     r3, r3, #0x02000000     @ (dg >> 1) << 26
    rsb     r12, r3, r12, asr #1
    rsb     r11, r3, r11, asr #1
    add     r12, r12, #0x20000000   @ (tr + 8) << 26
    add     r11, r11, #0x20000000   @ (tb + 8) << 26
    orr     r3, r12, r11            @ both in 0..15?
    cmp     r3, #0x40000000
    bhs     \self\()_raw
    mov     r3, #0xA0               @ 0x80 | (dg + 32)
    add     r3, r3, lr, asr #26
    strb    r3, [r5], #1
    lsr     r3, r11, #26            @ (tr + 8) << 4 | (tb + 8)
    add     r3, r3, r12, lsr #22
    strb    r3, [r5], #1
    cmp     r0, r1
    bne     \other\()_top
    b       \done

\self\()_raw:
    mov     r3, #0xFE
    strb    r3, [r5], #1
    strb    r6, [r5], #1
    lsr     r3, r6, #8
    strb    r3, [r5], #1
    cmp     r0, r1
    bne     \other\()_top
    b       \done
.endm

.macro PACKROW name, pf
    .section .text.\name, "ax", %progbits
    .global \name
    .type   \name, %function
    .align  4
\name:
    push    {r3-r11, lr}            @ (r3: the state, for the end)
    ldr     r4, [r3, #0]
    ldr     r5, [r3, #4]
    ldr     r6, [r3, #8]
    ldr     r7, [r3, #12]
    ldr     r8, =0x9E3779B1
    add     r1, r1, r2              @ done when the pixel after the next one is due
    ldrh    r9, [r0], r2            @ the first pixel
    PIXEL   r9, r10, A\pf, B\pf, \pf, .Ldone\pf
    PIXEL   r10, r9, B\pf, A\pf, \pf, .Ldone\pf
.Ldone\pf:
    ldr     r3, [sp]
    str     r5, [r3, #4]
    str     r6, [r3, #8]
    str     r7, [r3, #12]
    pop     {r3-r11, pc}
    .ltorg
    .size   \name, . - \name
.endm

    PACKROW flashbackPackRowAsm, 0
    PACKROW flashbackPackRowAsmPf, 1
