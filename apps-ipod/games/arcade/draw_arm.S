/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade: drawing's inner loops, ARMv4 so one copy serves both players.
 * Each is the C function of the same name in core_draw.c without the
 * _arm, which stays for the simulator and the host and is the reference
 * this is checked against.
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
