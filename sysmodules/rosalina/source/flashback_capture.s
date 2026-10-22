@ Flashback's capture conversion (flashbackConvert in flashback.c) in ARM
@ assembly, for full resolution (every framebuffer row) in the common formats:
@ the same RGB565 output as flashbackConvertFrame, byte for byte, at a fraction
@ of the instructions.
@
@   void flashbackConvertRgba8(const u8 *src, u16 *dst, u32 cols, u32 skip);
@   void flashbackConvertBgr8(const u8 *src, u16 *dst, u32 cols, u32 skip);
@   void flashbackConvertRgb565(const u8 *src, u16 *dst, u32 cols, u32 skip);
@
@ Each converts 'cols' framebuffer columns of 240 pixels, one after the other
@ ('skip' bytes from the end of one to the start of the next: 0, or a
@ column's worth in 800px mode), into 'dst' in the same order. 'src', 'skip'
@ and 'dst' are multiples of 4 (whole words are loaded and stored). Four
@ pixels are loaded at a time and two written per word; each pixel takes 5
@ shift-and-mask instructions (masks in registers), where the C loop takes ~14
@ and stalls on every load. An Old 3DS converts a full resolution RGBA8 frame
@ in 2.8 ms this way (5.1 in C).

.arm
.syntax unified

@ r8 = 0xF800, r9 = 0x07E0, r10 = 0x001F; 'out' = the RGB565 pixel of the
@ RGBA8 pixel in 'px' (bytes A, B, G, R): R>>3 << 11 | G>>2 << 5 | B>>3.
.macro RGBA8 out, px
    and     r12, r8, \px, lsr #16
    and     lr, r9, \px, lsr #13
    orr     r12, r12, lr
    and     lr, r10, \px, lsr #11
    orr     \out, r12, lr
.endm

    .section .text.flashbackConvertRgba8, "ax", %progbits
    .global flashbackConvertRgba8
    .type   flashbackConvertRgba8, %function
    .align  4
flashbackConvertRgba8:
    push    {r4-r11, lr}
    mov     r8, #0xF800
    mov     r9, #0x07E0
    mov     r10, #0x001F
1:  mov     r11, #30                @ 30 x 8 pixels a column
2:  pld     [r0, #256]
    ldmia   r0!, {r4-r7}
    RGBA8   r4, r4
    RGBA8   r5, r5
    orr     r4, r4, r5, lsl #16
    RGBA8   r6, r6
    RGBA8   r7, r7
    orr     r5, r6, r7, lsl #16
    stmia   r1!, {r4, r5}
    ldmia   r0!, {r4-r7}
    RGBA8   r4, r4
    RGBA8   r5, r5
    orr     r4, r4, r5, lsl #16
    RGBA8   r6, r6
    RGBA8   r7, r7
    orr     r5, r6, r7, lsl #16
    stmia   r1!, {r4, r5}
    subs    r11, r11, #1
    bne     2b
    add     r0, r0, r3
    subs    r2, r2, #1
    bne     1b
    pop     {r4-r11, pc}
    .size   flashbackConvertRgba8, . - flashbackConvertRgba8

@ Four BGR8 pixels (bytes B, G, R each) from three words w0 = r4, w1 = r5,
@ w2 = r6, as two words of RGB565 in r4, r5 (r7, r12, lr scratch):
@   pixel 0: w0 bits 0-23      pixel 1: w0 bits 24-31, w1 bits 0-15
@   pixel 2: w1 16-31, w2 0-7  pixel 3: w2 bits 8-31
.macro BGR8x4
    and     r7, r8, r5              @ pixel 1: R in w1 bits 8-15 (top 5 already in place)
    and     lr, r9, r5, lsl #3      @          G in w1 bits 0-7
    orr     r7, r7, lr
    orr     r7, r7, r4, lsr #27     @          B in w0 bits 24-31
    and     r12, r8, r4, lsr #8     @ pixel 0
    and     lr, r9, r4, lsr #5
    orr     r12, r12, lr
    and     lr, r10, r4, lsr #3
    orr     r4, r12, lr
    orr     r4, r4, r7, lsl #16
    and     r7, r8, r6, lsl #8      @ pixel 2: R in w2 bits 0-7
    and     lr, r9, r5, lsr #21     @          G in w1 bits 24-31
    orr     r7, r7, lr
    and     lr, r10, r5, lsr #19    @          B in w1 bits 16-23
    orr     r7, r7, lr
    and     r12, r8, r6, lsr #16    @ pixel 3
    and     lr, r9, r6, lsr #13
    orr     r12, r12, lr
    and     lr, r10, r6, lsr #11
    orr     r5, r12, lr
    orr     r5, r7, r5, lsl #16
.endm

    .section .text.flashbackConvertBgr8, "ax", %progbits
    .global flashbackConvertBgr8
    .type   flashbackConvertBgr8, %function
    .align  4
flashbackConvertBgr8:
    push    {r4-r11, lr}
    mov     r8, #0xF800
    mov     r9, #0x07E0
    mov     r10, #0x001F
1:  mov     r11, #30                @ 30 x 8 pixels a column
2:  pld     [r0, #256]
    ldmia   r0!, {r4-r6}
    BGR8x4
    stmia   r1!, {r4, r5}
    ldmia   r0!, {r4-r6}
    BGR8x4
    stmia   r1!, {r4, r5}
    subs    r11, r11, #1
    bne     2b
    add     r0, r0, r3
    subs    r2, r2, #1
    bne     1b
    pop     {r4-r11, pc}
    .size   flashbackConvertBgr8, . - flashbackConvertBgr8

@ RGB565 is copied: 32 bytes (16 pixels) at a time.
    .section .text.flashbackConvertRgb565, "ax", %progbits
    .global flashbackConvertRgb565
    .type   flashbackConvertRgb565, %function
    .align  4
flashbackConvertRgb565:
    push    {r4-r11, lr}
1:  mov     r12, #15                @ 15 x 16 pixels a column
2:  pld     [r0, #256]
    ldmia   r0!, {r4-r11}
    stmia   r1!, {r4-r11}
    subs    r12, r12, #1
    bne     2b
    add     r0, r0, r3
    subs    r2, r2, #1
    bne     1b
    pop     {r4-r11, pc}
    .size   flashbackConvertRgb565, . - flashbackConvertRgb565
