/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * ARM inner loops for the image decoders, ARMv4 so one copy serves both
 * players. Each is the C beside it with its state in registers; the C stays
 * for the simulator and is the reference these are checked against.
 *
 *   area_run_rgb24/32 - the area scaler over one part (resize.c)
 *   unfilter_*        - PNG's Up, Average and Paeth filters (png_load.c)
 ****************************************************************************/
#include "config.h"

    .syntax unified
    .section .text
    .align  2

/* struct area_run, in resize.c */
#define AR_R      0
#define AR_TR     12
#define AR_OXE    24
#define AR_MUL    28
#define AR_OUT    32
#define AR_HI     36
#define AR_HO     40
#define AR_ACCUM  44

/* void area_run_xxx(struct area_run *s, const uint8_t *px, int n)
 *
 * n source pixels, `size` bytes apart with red, green and blue at the given
 * offsets, into the area sums of s; each area they complete is scaled,
 * added to what the output pixel holds if s->accum, and stored.
 *
 * r0 s, r1 px, r2 n, r3-r5 the sums, r6 oxe, r7 h_o_val, r8 h_i_val */
.macro area_run name, size, ro, go, bo
    .global \name
    .type   \name, %function
\name:
    cmp     r2, #0
    bxle    lr
    stmfd   sp!, {r4-r11, lr}
    ldmia   r0, {r3-r5}
    ldr     r6, [r0, #AR_OXE]
    ldr     r7, [r0, #AR_HO]
    ldr     r8, [r0, #AR_HI]
1:  /* inside an area. Taking four pixels a turn while all four are is
     * slower at fit size, where an area is a few pixels wide. */
    add     r6, r6, r7
    cmp     r6, r8
    bhs     2f
    ldrb    r9, [r1, #\ro]
    ldrb    r10, [r1, #\go]
    ldrb    r11, [r1, #\bo]
    add     r1, r1, #\size
    add     r3, r3, r9
    add     r4, r4, r10
    add     r5, r5, r11
    subs    r2, r2, #1
    bne     1b
    b       3f
2:  /* the end of one: the saved partial pixel from its start, then this
     * pixel's share of it */
    sub     r6, r6, r8
    ldr     r9, [r0, #AR_MUL]
    ldr     r10, [r0, #AR_TR]
    mul     r3, r7, r3
    mla     r3, r10, r9, r3
    ldr     r10, [r0, #AR_TR + 4]
    mul     r4, r7, r4
    mla     r4, r10, r9, r4
    ldr     r10, [r0, #AR_TR + 8]
    mul     r5, r7, r5
    mla     r5, r10, r9, r5
    ldrb    r9, [r1, #\ro]
    ldrb    r10, [r1, #\go]
    ldrb    r11, [r1, #\bo]
    add     r1, r1, #\size
    add     r12, r0, #AR_TR
    stmia   r12, {r9-r11}
    sub     r12, r7, r6
    mla     r3, r9, r12, r3
    mla     r4, r10, r12, r4
    mla     r5, r11, r12, r5
    add     r3, r3, #0x200000
    mov     r3, r3, lsr #22
    add     r4, r4, #0x200000
    mov     r4, r4, lsr #22
    add     r5, r5, #0x200000
    mov     r5, r5, lsr #22
    ldr     lr, [r0, #AR_OUT]
    ldr     r12, [r0, #AR_ACCUM]
    cmp     r12, #0
    ldmne   lr, {r9-r11}
    addne   r3, r3, r9
    addne   r4, r4, r10
    addne   r5, r5, r11
    stmia   lr, {r3-r5}
    add     lr, lr, #16
    str     lr, [r0, #AR_OUT]
    str     r6, [r0, #AR_MUL]
    mov     r3, #0
    mov     r4, #0
    mov     r5, #0
    subs    r2, r2, #1
    bne     1b
3:
    stmia   r0, {r3-r5}
    str     r6, [r0, #AR_OXE]
    ldmfd   sp!, {r4-r11, pc}
    .size   \name, . - \name
.endm

    /* packed R, G, B bytes (RESIZE_RGB24) */
    area_run area_run_rgb24, 3, 0, 1, 2
    /* struct uint8_rgb: blue, green, red, alpha */
    area_run area_run_rgb32, 4, 2, 1, 0

/* PNG's Average and Paeth filters from a row's (bpp+1)th byte on: r is that
 * byte of the row, u the same byte of the row above, n the bytes left. The
 * 3-byte versions keep each channel's left and up-left bytes in registers;
 * n is then a multiple of 3. The general ones load them, bpp bytes back. */

/* void unfilter_up(uint8_t *r, const uint8_t *u, size_t n)
 *
 * Up over a whole row, 16 bytes a turn: four bytes added to four, each lane
 * on its own, as (a & 7f..) + (b & 7f..) with the top bits put back by
 * (a ^ b) & 80... r and u are word-aligned. */
.macro add_lanes a, b
    eor     r11, \a, \b
    and     r11, r11, lr
    and     \a, \a, r12
    and     \b, \b, r12
    add     \a, \a, \b
    eor     \a, \a, r11
.endm

    .global unfilter_up
    .type   unfilter_up, %function
unfilter_up:
    stmfd   sp!, {r4-r11, lr}
    ldr     r12, =0x7f7f7f7f
    mvn     lr, r12
    subs    r2, r2, #16
    blo     2f
1:
    ldmia   r0, {r3-r6}
    ldmia   r1!, {r7-r10}
    add_lanes r3, r7
    add_lanes r4, r8
    add_lanes r5, r9
    add_lanes r6, r10
    stmia   r0!, {r3-r6}
    subs    r2, r2, #16
    bhs     1b
2:
    adds    r2, r2, #16
    beq     4f
3:
    ldrb    r3, [r0]
    ldrb    r4, [r1], #1
    add     r3, r3, r4
    strb    r3, [r0], #1
    subs    r2, r2, #1
    bne     3b
4:
    ldmfd   sp!, {r4-r11, pc}
    .size   unfilter_up, . - unfilter_up
    .ltorg

/* r0 r, r1 u, r2 end of r */
.macro avg_byte a
    ldrb    r9, [r1], #1
    ldrb    r10, [r0]
    add     r9, r9, \a
    add     r10, r10, r9, lsr #1
    and     \a, r10, #255
    strb    \a, [r0], #1
.endm

/* void unfilter_avg3(uint8_t *r, const uint8_t *u, size_t n) */
    .global unfilter_avg3
    .type   unfilter_avg3, %function
unfilter_avg3:
    cmp     r2, #0
    bxeq    lr
    stmfd   sp!, {r4-r5, r9-r10, lr}
    add     r2, r0, r2
    ldrb    r3, [r0, #-3]
    ldrb    r4, [r0, #-2]
    ldrb    r5, [r0, #-1]
1:
    avg_byte r3
    avg_byte r4
    avg_byte r5
    cmp     r0, r2
    blo     1b
    ldmfd   sp!, {r4-r5, r9-r10, pc}
    .size   unfilter_avg3, . - unfilter_avg3

/* The predictor for one byte: a left, c up-left, both registers kept per
 * channel; r9 up, r10 the choice, r11 pa, r12 pb, lr pc. pa <= pb and pc
 * picks a, then pb <= pc picks b, else c -- the order the standard gives. */
.macro paeth_byte a, c
    ldrb    r9, [r1], #1
    subs    r11, r9, \c
    rsbmi   r11, r11, #0
    subs    r12, \a, \c
    rsbmi   r12, r12, #0
    add     lr, \a, r9
    subs    lr, lr, \c, lsl #1
    rsbmi   lr, lr, #0
    cmp     r12, lr
    movls   r10, r9
    movhi   r10, \c
    movhi   r12, lr
    cmp     r11, r12
    movls   r10, \a
    ldrb    r11, [r0]
    mov     \c, r9
    add     r11, r11, r10
    and     \a, r11, #255
    strb    \a, [r0], #1
.endm

/* void unfilter_paeth3(uint8_t *r, const uint8_t *u, size_t n) */
    .global unfilter_paeth3
    .type   unfilter_paeth3, %function
unfilter_paeth3:
    cmp     r2, #0
    bxeq    lr
    stmfd   sp!, {r4-r11, lr}
    add     r2, r0, r2
    ldrb    r3, [r0, #-3]
    ldrb    r4, [r0, #-2]
    ldrb    r5, [r0, #-1]
    ldrb    r6, [r1, #-3]
    ldrb    r7, [r1, #-2]
    ldrb    r8, [r1, #-1]
1:
    paeth_byte r3, r6
    paeth_byte r4, r7
    paeth_byte r5, r8
    cmp     r0, r2
    blo     1b
    ldmfd   sp!, {r4-r11, pc}
    .size   unfilter_paeth3, . - unfilter_paeth3

/* void unfilter_avg(uint8_t *r, const uint8_t *u, size_t n, int bpp) */
    .global unfilter_avg
    .type   unfilter_avg, %function
unfilter_avg:
    cmp     r2, #0
    bxeq    lr
    add     r2, r0, r2
    stmfd   sp!, {r4, lr}
1:
    ldrb    r12, [r0, -r3]
    ldrb    lr, [r1], #1
    ldrb    r4, [r0]
    add     r12, r12, lr
    add     r4, r4, r12, lsr #1
    strb    r4, [r0], #1
    cmp     r0, r2
    blo     1b
    ldmfd   sp!, {r4, pc}
    .size   unfilter_avg, . - unfilter_avg

/* void unfilter_paeth(uint8_t *r, const uint8_t *u, size_t n, int bpp)
 * r4 a, r5 c, then paeth_byte's registers */
    .global unfilter_paeth
    .type   unfilter_paeth, %function
unfilter_paeth:
    cmp     r2, #0
    bxeq    lr
    stmfd   sp!, {r4-r11, lr}
    add     r2, r0, r2
1:
    ldrb    r4, [r0, -r3]
    ldrb    r5, [r1, -r3]
    paeth_byte r4, r5
    cmp     r0, r2
    blo     1b
    ldmfd   sp!, {r4-r11, pc}
    .size   unfilter_paeth, . - unfilter_paeth
