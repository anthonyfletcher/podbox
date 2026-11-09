/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade: drawing's inner loops, ARMv4 so one copy serves both players.
 * The C in core_draw.c stays for the simulator and the host, and is the
 * reference these are checked against.
 *
 * Parts, in order:
 *   copy_row_arm     copy_row() forward, a word of clear colour 0 at once
 *   land_slice_arm_* land_slice() with land_fill() inside it, for square
 *                    maps 8 to 256 across, a sample every column (_1) or
 *                    every other (_2)
 *   land_smooth_arm_* land_slice_smooth() with land_paint() inside it, the
 *                    same sizes and steps
 *   turned_row_arm   turned_row() for a source the right way round: a row
 *                    of rspr and rblit, banking's turn of the screen
 *   light_map_arm    light.draw()'s last pass: pixels through their
 *                    levels' tables, four a word
 *   light_dark_arm   light_dark(): pixels through the darkest table
 *   light_span_arm   light_span(): a span of a light's levels
 *   wall_column_arm  wall_column(): a wall's column, a table a dither row
 ****************************************************************************/
    .syntax unified
    .section .text
    .align  2

    .equ    XLAT_CLEAR, 0x100

/* One pixel of the word in r4, its byte at bits lsb up, looked up in the
 * table and stored to [r0, #at] unless clear; r12 counts those drawn */
.macro  pixel at, mask, lsb
    .if \lsb == 24
    mov     r5, r4, lsr #24
    add     r5, r3, r5, lsl #1
    .elseif \lsb == 0
    and     r5, r4, #0xff
    add     r5, r3, r5, lsl #1
    .else
    and     r5, r4, #\mask
    add     r5, r3, r5, lsr #(\lsb - 1)
    .endif
    ldrh    r5, [r5]
    tst     r5, #XLAT_CLEAR
    strbeq  r5, [r0, #\at]
    addeq   r12, r12, #1
.endm

/* uint32_t copy_row_arm(uint8_t *d, const uint8_t *s, int32_t n,
 *                       const uint16_t *xl)
 *
 * copy_row() stepping forward: n pixels from s through xl to d, the clear
 * ones left out. Returns the pixels drawn. When colour 0 is clear, a word
 * of it is passed over whole.
 *
 * r0 d, r1 s, r2 pixels left, r3 xl, r4 the source word or byte, r5 a
 * pixel, r6 0 when colour 0 is clear, r12 the count */
    .global copy_row_arm
    .type   copy_row_arm, %function
copy_row_arm:
    stmfd   sp!, {r4-r6}
    mov     r12, #0
    ldrh    r6, [r3]
    and     r6, r6, #XLAT_CLEAR
    eor     r6, r6, #XLAT_CLEAR
    cmp     r2, #0
    ble     .Ldone
    /* a byte at a time to a word of the source */
.Lhead:
    tst     r1, #3
    beq     .Lwords
    ldrb    r4, [r1], #1
    pixel   0, 0xff, 0
    add     r0, r0, #1
    subs    r2, r2, #1
    bne     .Lhead
    b       .Ldone
.Lwords:
    subs    r2, r2, #4
    blt     .Ltail
.Lword:
    ldr     r4, [r1], #4
    orrs    r5, r4, r6
    beq     .Lskip
    pixel   0, 0xff, 0
    pixel   1, 0xff00, 8
    pixel   2, 0xff0000, 16
    pixel   3, 0, 24
.Lskip:
    add     r0, r0, #4
    subs    r2, r2, #4
    bge     .Lword
.Ltail:
    adds    r2, r2, #4
    beq     .Ldone
1:
    ldrb    r4, [r1], #1
    pixel   0, 0xff, 0
    add     r0, r0, #1
    subs    r2, r2, #1
    bne     1b
.Ldone:
    mov     r0, r12
    ldmfd   sp!, {r4-r6}
    bx      lr
    .size   copy_row_arm, . - copy_row_arm

/* struct slice, core_draw.c, which checks these */
    .equ    SL_HEIGHT, 0
    .equ    SL_COLOUR, 4
    .equ    SL_DPAL, 8
    .equ    SL_PX, 24
    .equ    SL_W, 28
    .equ    SL_X0, 32
    .equ    SL_X1, 36
    .equ    SL_CY0, 40
    .equ    SL_TOP, 48
    .equ    SL_FOGM, 56
    .equ    SL_FOGW, 72

/* int32_t name(const struct slice *s, uint32_t u, uint32_t t, uint32_t su,
 *              uint32_t sv, int32_t base, int32_t rise, uint32_t *filled,
 *              int32_t *lo): land_slice() for a map 2^n square, its index
 * ((t >> tsh) & tmask) | (u << ul >> ur), a sample every step columns.
 *
 * r0 the column's top, r1 the end, r2 u, r3 t, r4 su, r5 sv, r6 the
 * heights, r7 rise, r8 base, r9 the clip's top row, r10 the columns open;
 * r11 the sample's index, r12 the column's top row, lr its new top. */
.macro  landslice name, tsh, tmask, ul, ur, step
    .global \name
    .type   \name, %function
\name:
    stmfd   sp!, {r4-r11, lr}
    sub     sp, sp, #4
    str     r0, [sp]
    /* s at sp; sv, base, rise, filled and lo from sp + 40 */
    ldr     r5, [sp, #40]
    ldr     r8, [sp, #44]
    ldr     r7, [sp, #48]
    mov     r4, r3
    mov     r3, r2
    mov     r2, r1
    ldr     r6, [r0, #SL_HEIGHT]
    ldr     r9, [r0, #SL_CY0]
    ldr     r1, [r0, #SL_TOP]
    ldr     r11, [r0, #SL_X0]
    ldr     r12, [r0, #SL_X1]
    add     r0, r1, r11, lsl #1
    add     r1, r1, r12, lsl #1
    mov     r10, #0
    cmp     r0, r1
    bhs     9f
1:
    ldrsh   r12, [r0]
    cmp     r12, r9
    ble     2f
    add     r10, r10, #1
    mov     r11, r3, lsr #\tsh
    and     r11, r11, #\tmask
    mov     lr, r2, lsl #\ul
    orr     r11, r11, lr, lsr #\ur
    ldrb    lr, [r6, r11]
    mul     lr, r7, lr
    sub     lr, r8, lr
    cmp     r12, lr, asr #16
    bgt     5f
2:
    add     r0, r0, #(2 * \step)
    add     r2, r2, r4
    add     r3, r3, r5
    cmp     r0, r1
    blo     1b
9:
    mov     r0, r10
    add     sp, sp, #4
    ldmfd   sp!, {r4-r11, pc}

5:
    /* land_fill(): r1-r8 kept above s, so filled and lo are at sp + 84 */
    stmfd   sp!, {r1-r8}
    mov     lr, lr, asr #16
    cmp     lr, r9
    movlt   lr, r9
    ldr     r1, [sp, #32]
    ldr     r2, [r1, #SL_COLOUR]
    ldrb    r2, [r2, r11]
    ldr     r3, [r1, #SL_DPAL]
    ldrb    r2, [r3, r2]
    orr     r2, r2, r2, lsl #8
    orr     r2, r2, r2, lsl #16
    ldr     r3, [r1, #SL_TOP]
    sub     r3, r0, r3
    mov     r3, r3, asr #1
    /* r6 column x's colours for four rows, row y's lowest (land_rows()) */
    ldr     r4, [r1, #SL_FOGW]
    and     r7, lr, #3
    mov     r7, r7, lsl #3
    and     r5, r3, #3
    add     r5, r1, r5, lsl #2
    ldr     r5, [r5, #SL_FOGM]
    bic     r6, r2, r5
    and     r5, r4, r5
    orr     r6, r6, r5
    mov     r6, r6, ror r7
    .if \step == 2
    /* and r8 column x + 1's, when it is inside the clip */
    ldr     r5, [r1, #SL_X1]
    add     r8, r3, #1
    cmp     r8, r5
    bge     3f
    and     r8, r8, #3
    add     r8, r1, r8, lsl #2
    ldr     r8, [r8, #SL_FOGM]
    bic     r5, r2, r8
    and     r8, r4, r8
    orr     r8, r5, r8
    mov     r8, r8, ror r7
    ldr     r4, [r1, #SL_W]
    ldr     r5, [r1, #SL_X0]
    sub     r3, r3, r5
    ldr     r5, [r1, #SL_PX]
    mla     r3, lr, r4, r3
    add     r3, r3, r5
    sub     r7, r12, lr
    ldr     r5, [sp, #84]
    ldr     r2, [r5]
    add     r2, r2, r7, lsl #1
    str     r2, [r5]
6:
    strb    r8, [r3, #1]
    strb    r6, [r3], r4
    mov     r8, r8, ror #8
    mov     r6, r6, ror #8
    subs    r7, r7, #1
    bne     6b
    b       4f
3:
    .endif
    ldr     r4, [r1, #SL_W]
    ldr     r5, [r1, #SL_X0]
    sub     r3, r3, r5
    ldr     r5, [r1, #SL_PX]
    mla     r3, lr, r4, r3
    add     r3, r3, r5
    sub     r7, r12, lr
    ldr     r5, [sp, #84]
    ldr     r2, [r5]
    add     r2, r2, r7
    str     r2, [r5]
7:
    strb    r6, [r3], r4
    mov     r6, r6, ror #8
    subs    r7, r7, #1
    bne     7b
4:
    strh    lr, [r0]
    ldr     r5, [sp, #88]
    ldr     r2, [r5]
    cmp     lr, r2
    strlt   lr, [r5]
    ldmfd   sp!, {r1-r8}
    b       2b
    .size   \name, . - \name
.endm

    landslice land_slice_arm_8_1, 13, 0x38, 13, 29, 1
    landslice land_slice_arm_8_2, 13, 0x38, 13, 29, 2
    landslice land_slice_arm_16_1, 12, 0xf0, 12, 28, 1
    landslice land_slice_arm_16_2, 12, 0xf0, 12, 28, 2
    landslice land_slice_arm_32_1, 11, 0x3e0, 11, 27, 1
    landslice land_slice_arm_32_2, 11, 0x3e0, 11, 27, 2
    landslice land_slice_arm_64_1, 10, 0xfc0, 10, 26, 1
    landslice land_slice_arm_64_2, 10, 0xfc0, 10, 26, 2
    landslice land_slice_arm_128_1, 9, 0x3f80, 9, 25, 1
    landslice land_slice_arm_128_2, 9, 0x3f80, 9, 25, 2
    landslice land_slice_arm_256_1, 8, 0xff00, 8, 24, 1
    landslice land_slice_arm_256_2, 8, 0xff00, 8, 24, 2

    .equ    SL_RAMP, 76
    .equ    SL_NRAMP, 80

/* int32_t name(const struct slice *s, uint32_t u, uint32_t t, uint32_t su,
 *              uint32_t sv, int32_t base, int32_t rise, uint32_t *filled,
 *              int32_t *lo): land_slice_smooth() for a map 2^n square, a
 * sample every step columns -- the height blended from the four pixels
 * round each sample, the colour the ramp's at that height.
 *
 * r0 the column's top, r1 the end, r2 u, r3 t, r6 the heights, r7 rise,
 * r8 base, r10 the clip's top row; r4, r5, r9, r11, r12 and lr work. On
 * the stack: s, su, sv and the columns open. */
.macro  landsmooth name, n, step
    .global \name
    .type   \name, %function
\name:
    stmfd   sp!, {r4-r11, lr}
    sub     sp, sp, #16
    str     r0, [sp]
    str     r3, [sp, #4]
    /* sv, base, rise, filled and lo from sp + 52 */
    ldr     r4, [sp, #52]
    str     r4, [sp, #8]
    mov     r4, #0
    str     r4, [sp, #12]
    ldr     r8, [sp, #56]
    ldr     r7, [sp, #60]
    mov     r3, r2
    mov     r2, r1
    ldr     r6, [r0, #SL_HEIGHT]
    ldr     r10, [r0, #SL_CY0]
    ldr     r1, [r0, #SL_TOP]
    ldr     r11, [r0, #SL_X0]
    ldr     r12, [r0, #SL_X1]
    add     r0, r1, r11, lsl #1
    add     r1, r1, r12, lsl #1
    cmp     r0, r1
    bhs     9f
1:
    ldrsh   r9, [r0]
    cmp     r9, r10
    ble     2f
    ldr     r4, [sp, #12]
    add     r4, r4, #1
    str     r4, [sp, #12]
    /* a pixel's height is at its centre, half a pixel in */
    sub     r11, r2, #0x8000
    sub     r12, r3, #0x8000
    mov     r4, r11, lsl #(16 - \n)
    mov     r4, r4, lsr #(32 - \n)          /* x0 */
    mov     r5, r12, lsl #(16 - \n)
    mov     r5, r5, lsr #(32 - \n)
    mov     r5, r5, lsl #\n                 /* row 0 */
    mov     r11, r11, lsr #8
    and     r11, r11, #0xff                 /* fx */
    mov     r12, r12, lsr #8
    and     r12, r12, #0xff                 /* fy */
    add     lr, r6, r5
    ldrb    r9, [lr, r4]                    /* h00 */
    add     r4, r4, #1
    and     r4, r4, #((1 << \n) - 1)        /* x1 */
    ldrb    lr, [lr, r4]                    /* h10 */
    sub     lr, lr, r9
    mul     lr, r11, lr
    add     r9, lr, r9, lsl #8              /* a: row 0 at fx, 8.8 */
    add     r5, r5, #(1 << \n)
    mov     r5, r5, lsl #(32 - 2 * \n)
    mov     r5, r5, lsr #(32 - 2 * \n)      /* row 1 */
    add     r5, r6, r5
    ldrb    lr, [r5, r4]                    /* h11 */
    sub     r4, r4, #1
    and     r4, r4, #((1 << \n) - 1)        /* x0 */
    ldrb    r4, [r5, r4]                    /* h01 */
    sub     lr, lr, r4
    mul     lr, r11, lr
    add     lr, lr, r4, lsl #8              /* b: row 1 at fx */
    sub     lr, lr, r9
    mul     lr, r12, lr
    add     lr, lr, r9, lsl #8              /* the height, 16.16 */
    /* its row: (base - (h * rise >> 16)) >> 16, in 64 bits */
    smull   r4, r5, lr, r7
    mov     r4, r4, lsr #16
    orr     r4, r4, r5, lsl #16
    mov     r5, r5, asr #16
    subs    r4, r8, r4
    mov     r9, r8, asr #31
    sbc     r5, r9, r5
    mov     r4, r4, lsr #16
    orr     r4, r4, r5, lsl #16             /* y */
    ldrsh   r12, [r0]
    cmp     r12, r4
    bgt     5f
2:
    ldr     r4, [sp, #4]
    ldr     r5, [sp, #8]
    add     r0, r0, #(2 * \step)
    add     r2, r2, r4
    add     r3, r3, r5
    cmp     r0, r1
    blo     1b
9:
    ldr     r0, [sp, #12]
    add     sp, sp, #16
    ldmfd   sp!, {r4-r11, pc}

5:
    /* the ramp's colour at the height */
    ldr     r9, [sp]
    mov     r5, lr, asr #16
    ldr     r11, [r9, #SL_NRAMP]
    cmp     r5, r11
    subge   r5, r11, #1
    ldr     r11, [r9, #SL_RAMP]
    ldrb    r5, [r11, r5]
    /* land_paint(): r1-r8 kept above the locals, so s is at sp + 32 and
     * filled and lo at sp + 96 */
    stmfd   sp!, {r1-r8}
    mov     lr, r4
    cmp     lr, r10
    movlt   lr, r10
    ldr     r1, [sp, #32]
    ldr     r3, [r1, #SL_DPAL]
    ldrb    r2, [r3, r5]
    orr     r2, r2, r2, lsl #8
    orr     r2, r2, r2, lsl #16
    ldr     r3, [r1, #SL_TOP]
    sub     r3, r0, r3
    mov     r3, r3, asr #1
    /* r6 column x's colours for four rows, row y's lowest (land_rows()) */
    ldr     r4, [r1, #SL_FOGW]
    and     r7, lr, #3
    mov     r7, r7, lsl #3
    and     r5, r3, #3
    add     r5, r1, r5, lsl #2
    ldr     r5, [r5, #SL_FOGM]
    bic     r6, r2, r5
    and     r5, r4, r5
    orr     r6, r6, r5
    mov     r6, r6, ror r7
    .if \step == 2
    /* and r8 column x + 1's, when it is inside the clip */
    ldr     r5, [r1, #SL_X1]
    add     r8, r3, #1
    cmp     r8, r5
    bge     3f
    and     r8, r8, #3
    add     r8, r1, r8, lsl #2
    ldr     r8, [r8, #SL_FOGM]
    bic     r5, r2, r8
    and     r8, r4, r8
    orr     r8, r5, r8
    mov     r8, r8, ror r7
    ldr     r4, [r1, #SL_W]
    ldr     r5, [r1, #SL_X0]
    sub     r3, r3, r5
    ldr     r5, [r1, #SL_PX]
    mla     r3, lr, r4, r3
    add     r3, r3, r5
    sub     r7, r12, lr
    ldr     r5, [sp, #96]
    ldr     r2, [r5]
    add     r2, r2, r7, lsl #1
    str     r2, [r5]
6:
    strb    r8, [r3, #1]
    strb    r6, [r3], r4
    mov     r8, r8, ror #8
    mov     r6, r6, ror #8
    subs    r7, r7, #1
    bne     6b
    b       4f
3:
    .endif
    ldr     r4, [r1, #SL_W]
    ldr     r5, [r1, #SL_X0]
    sub     r3, r3, r5
    ldr     r5, [r1, #SL_PX]
    mla     r3, lr, r4, r3
    add     r3, r3, r5
    sub     r7, r12, lr
    ldr     r5, [sp, #96]
    ldr     r2, [r5]
    add     r2, r2, r7
    str     r2, [r5]
7:
    strb    r6, [r3], r4
    mov     r6, r6, ror #8
    subs    r7, r7, #1
    bne     7b
4:
    strh    lr, [r0]
    ldr     r5, [sp, #100]
    ldr     r2, [r5]
    cmp     lr, r2
    strlt   lr, [r5]
    ldmfd   sp!, {r1-r8}
    b       2b
    .size   \name, . - \name
.endm

    landsmooth land_smooth_arm_8_1, 3, 1
    landsmooth land_smooth_arm_8_2, 3, 2
    landsmooth land_smooth_arm_16_1, 4, 1
    landsmooth land_smooth_arm_16_2, 4, 2
    landsmooth land_smooth_arm_32_1, 5, 1
    landsmooth land_smooth_arm_32_2, 5, 2
    landsmooth land_smooth_arm_64_1, 6, 1
    landsmooth land_smooth_arm_64_2, 6, 2
    landsmooth land_smooth_arm_128_1, 7, 1
    landsmooth land_smooth_arm_128_2, 7, 2
    landsmooth land_smooth_arm_256_1, 8, 1
    landsmooth land_smooth_arm_256_2, 8, 2

/* turned_row_arm(d, n, u, v, t): turned_row() for a source drawn the right
 * way round. Each of n pixels from d takes the source pixel at u, v
 * (16.16), stepped by du, dv, through the table; points off the source
 * and clear colours leave it.
 *
 * r0 d, r1 pixels left, r2 u, r3 v; from the turn: r4 src, r5 stride,
 * r6 width, r7 height, r8 du, r9 dv, r10 xl; r11 x, r12 the row's address
 * and then the colour. A negative u or v is a large x or y unsigned, so
 * one unsigned compare each finds it off the source. */
    .global turned_row_arm
    .type   turned_row_arm, %function
turned_row_arm:
    ldr     r12, [sp]
    stmfd   sp!, {r4-r11, lr}
    ldmia   r12, {r4-r9}
    ldr     r10, [r12, #28]
    cmp     r1, #0
    ble     3f
1:  mov     r11, r2, asr #16
    cmp     r11, r6
    movlo   r12, r3, asr #16
    cmplo   r12, r7
    bhs     2f
    mla     r12, r5, r12, r4
    ldrb    r12, [r12, r11]
    add     r12, r10, r12, lsl #1
    ldrh    r12, [r12]
    tst     r12, #XLAT_CLEAR
    strbeq  r12, [r0]
2:  add     r0, r0, #1
    add     r2, r2, r8
    add     r3, r3, r9
    subs    r1, r1, #1
    bgt     1b
3:  ldmfd   sp!, {r4-r11, pc}
    .size   turned_row_arm, . - turned_row_arm

/* light_map_arm(px, lit, n, lut, levels): n pixels from px (a multiple of
 * four, px and lit word aligned) each through the table of its level in
 * lit, lut[level * 256 + colour]; a level of levels leaves it as drawn.
 * Four lit together are left whole, four dark together go through the
 * darkest table with one load and one store, and the rest each by its own
 * level.
 *
 * r0 px, r1 lit, r2 pixels left, r3 lut, r4 levels, r6 levels in every
 * byte, r7 four levels, r8 four pixels, r9 and r10 a level and a pixel */
    .global light_map_arm
    .type   light_map_arm, %function
light_map_arm:
    stmfd   sp!, {r4-r10, lr}
    ldr     r4, [sp, #32]
    ldr     r5, =0x01010101
    mul     r6, r5, r4
    cmp     r2, #0
    ble     9f
1:  ldr     r7, [r1], #4
    cmp     r7, r6
    beq     4f
    ldr     r8, [r0]
    cmp     r7, #0
    bne     2f
    and     r9, r8, #0xff
    ldrb    r9, [r3, r9]
    and     r10, r8, #0xff00
    ldrb    r10, [r3, r10, lsr #8]
    orr     r9, r9, r10, lsl #8
    and     r10, r8, #0xff0000
    ldrb    r10, [r3, r10, lsr #16]
    orr     r9, r9, r10, lsl #16
    ldrb    r10, [r3, r8, lsr #24]
    orr     r9, r9, r10, lsl #24
    str     r9, [r0]
    b       4f
2:
    and     r9, r7, #0xff
    cmp     r9, r4
    bhs     50f
    and     r10, r8, #0xff
    add     r10, r10, r9, lsl #8
    ldrb    r10, [r3, r10]
    strb    r10, [r0, #0]
50:
    and     r9, r7, #0xff00
    mov     r9, r9, lsr #8
    cmp     r9, r4
    bhs     51f
    and     r10, r8, #0xff00
    mov     r10, r10, lsr #8
    add     r10, r10, r9, lsl #8
    ldrb    r10, [r3, r10]
    strb    r10, [r0, #1]
51:
    and     r9, r7, #0xff0000
    mov     r9, r9, lsr #16
    cmp     r9, r4
    bhs     52f
    and     r10, r8, #0xff0000
    mov     r10, r10, lsr #16
    add     r10, r10, r9, lsl #8
    ldrb    r10, [r3, r10]
    strb    r10, [r0, #2]
52:
    mov     r9, r7, lsr #24
    cmp     r9, r4
    bhs     53f
    mov     r10, r8, lsr #24
    add     r10, r10, r9, lsl #8
    ldrb    r10, [r3, r10]
    strb    r10, [r0, #3]
53:
4:  add     r0, r0, #4
    subs    r2, r2, #4
    bgt     1b
9:  ldmfd   sp!, {r4-r10, pc}
    .ltorg
    .size   light_map_arm, . - light_map_arm

/* light_dark_arm(px, n, lut): n pixels from px (a multiple of four, px
 * word aligned) through the darkest table, four a word.
 *
 * r0 px, r1 pixels left, r2 lut, r3 four pixels, r4 the four darkened,
 * r12 and lr a pixel each, loaded a step before they are used */
    .global light_dark_arm
    .type   light_dark_arm, %function
light_dark_arm:
    stmfd   sp!, {r4, lr}
1:  ldr     r3, [r0]
    and     r12, r3, #0xff
    and     lr, r3, #0xff00
    ldrb    r4, [r2, r12]
    ldrb    lr, [r2, lr, lsr #8]
    and     r12, r3, #0xff0000
    ldrb    r12, [r2, r12, lsr #16]
    orr     r4, r4, lr, lsl #8
    ldrb    lr, [r2, r3, lsr #24]
    orr     r4, r4, r12, lsl #16
    orr     r4, r4, lr, lsl #24
    str     r4, [r0], #4
    subs    r1, r1, #4
    bgt     1b
    ldmfd   sp!, {r4, pc}
    .size   light_dark_arm, . - light_dark_arm

/* One pixel of light_span_arm: its level from the shade table in tab at
 * q's top nine bits, stored to [r0, #at] where more than it has */
.macro  lspan tab, at
    ldrb    r10, [\tab, r2, lsr #23]
    ldrb    r11, [r0, #\at]
    add     r2, r2, r3
    add     r3, r3, r4
    cmp     r10, r11
    strbhi  r10, [r0, #\at]
.endm

/* light_span_arm(row, n, q, dq, inv, tab): light_span() -- n pixels from
 * row, four a pass, each with the next of the four shade tables in tab.
 *
 * r0 row, r1 pixels left, r2 q, r3 dq, r4 inv, r5-r8 the tables, r10 the
 * level, r11 the one the pixel has */
    .global light_span_arm
    .type   light_span_arm, %function
light_span_arm:
    stmfd   sp!, {r4-r11, lr}
    ldr     r4, [sp, #36]
    ldr     r12, [sp, #40]
    ldmia   r12, {r5-r8}
    subs    r1, r1, #4
    blt     5f
1:  lspan   r5, 0
    lspan   r6, 1
    lspan   r7, 2
    lspan   r8, 3
    add     r0, r0, #4
    subs    r1, r1, #4
    bge     1b
5:  adds    r1, r1, #4
    beq     9f
    lspan   r5, 0
    subs    r1, r1, #1
    beq     9f
    lspan   r6, 1
    subs    r1, r1, #1
    beq     9f
    lspan   r7, 2
9:  ldmfd   sp!, {r4-r11, pc}
    .size   light_span_arm, . - light_span_arm

/* One pixel of a wall's column: the texel row ty's whole part picks from
 * the dither row's table in \tab, stored, and px and ty step on */
.macro  wpix tab
    and     r12, r3, #0x70000
    ldrb    r12, [\tab, r12, lsr #16]
    add     r3, r3, r4
    strb    r12, [r0], r1
.endm

/* wall_column_arm(px, w, n, ty, ts, tab): wall_column(), four rows at a
 * time, a dither row's table each.
 *
 * r0 px, r1 w, r2 rows left, r3 ty, r4 ts, r5-r8 the tables */
    .global wall_column_arm
    .type   wall_column_arm, %function
wall_column_arm:
    stmfd   sp!, {r4-r8, lr}
    ldr     r4, [sp, #24]
    ldr     r12, [sp, #28]
    ldmia   r12, {r5-r8}
    subs    r2, r2, #4
    blt     5f
1:  wpix    r5
    wpix    r6
    wpix    r7
    wpix    r8
    subs    r2, r2, #4
    bge     1b
5:  adds    r2, r2, #4
    beq     9f
    wpix    r5
    subs    r2, r2, #1
    beq     9f
    wpix    r6
    subs    r2, r2, #1
    beq     9f
    wpix    r7
9:  ldmfd   sp!, {r4-r8, pc}
    .size   wall_column_arm, . - wall_column_arm
