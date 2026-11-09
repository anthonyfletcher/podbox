/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade: the links' passes in ARMv4, so one copy serves both players.
 * The C in sys_links.c stays for the simulator and the host, and is the
 * reference this is checked against: the same links move their points to
 * the same 1/65536 of a pixel.
 *
 * Parts, in order:
 *   link_run_arm     link_run(): links pulled toward their lengths the
 *                    short way, until one that needs the exact way
 ****************************************************************************/
    .syntax unified
    .section .text
    .align  2

/* struct qlink, sys_links.c */
    .equ    Q_IJ, 0
    .equ    Q_L2, 4
    .equ    Q_INV, 8
    .equ    Q_SIZE, 12

/* One end's place, r7 or r11, moved by the move in r8-r10 shifted right
 * by sh: added to it (op add) or taken from it (op sub) */
.macro  nudge end, op, sh
    ldmia   \end, {r4-r6}
    \op     r4, r4, r8, asr #\sh
    \op     r5, r5, r9, asr #\sh
    \op     r6, r6, r10, asr #\sh
    stmia   \end, {r4-r6}
.endm

/* const struct qlink *link_run_arm(int32_t *P, const struct qlink *q,
 *                                  const struct qlink *end, int slack)
 *
 * r0 P, r1 the link, r2 end, r3 its ij, r4-r6 scratch, r7 and r11 its
 * ends' places, r8-r10 its difference in 1/256 pixels and then the move,
 * r12 scratch, lr bit 31 set when the pool is slack (a squashed link of
 * a slack pool is let be) */
    .global link_run_arm
    .type   link_run_arm, %function
link_run_arm:
    stmfd   sp!, {r4-r11, lr}
    movs    lr, r3
    movne   lr, #0x80000000
    cmp     r1, r2
    bhs     9f
1:  ldr     r3, [r1, #Q_IJ]
    cmn     r3, #1                  @ QLINK_NONE
    beq     8f
    ldr     r6, [r1, #Q_INV]
    cmp     r6, #0
    beq     7f
    mov     r7, r3, lsl #16
    mov     r7, r7, lsr #16
    bic     r7, r7, #3
    add     r7, r0, r7              @ Pi
    mov     r11, r3, lsr #16
    bic     r11, r11, #3
    add     r11, r0, r11            @ Pj
    ldmia   r7, {r8-r10}
    ldmia   r11, {r4, r5, r12}
    sub     r8, r4, r8
    mov     r8, r8, asr #8          @ ax
    sub     r9, r5, r9
    mov     r9, r9, asr #8          @ ay
    sub     r10, r12, r10
    mov     r10, r10, asr #8        @ az
    add     r4, r8, #16384          @ each within 64 pixels
    cmp     r4, #32768
    bhs     7f
    add     r4, r9, #16384
    cmp     r4, #32768
    bhs     7f
    add     r4, r10, #16384
    cmp     r4, #32768
    bhs     7f
    mul     r4, r8, r8
    mla     r4, r9, r9, r4
    mla     r4, r10, r10, r4        @ d^2
    ldr     r5, [r1, #Q_L2]
    sub     r4, r4, r5
    smull   r5, r12, r4, r6
    mov     r12, r12, lsl #1
    orr     r6, r12, r5, lsr #31    @ e, the stretch of the square
    add     r4, r6, #16384          @ within a quarter
    cmp     r4, #32768
    bhs     7f
    tst     r6, lr
    bne     8f
    mul     r4, r6, r6
    mov     r4, r4, asr #16         @ e^2
    mul     r5, r4, r6
    mov     r5, r5, asr #16         @ e^3
    add     r6, r6, r6, lsr #31
    mov     r6, r6, asr #1          @ e / 2, toward 0
    add     r4, r4, r4, lsl #1
    sub     r6, r6, r4, asr #3
    add     r5, r5, r5, lsl #2
    add     r6, r6, r5, asr #4      @ the share
    mul     r8, r6, r8
    mul     r9, r6, r9
    mul     r10, r6, r10
    tst     r3, #1
    bne     2f
    tst     r3, #0x10000
    bne     3f
    nudge   r7, add, 9
    nudge   r11, sub, 9
    b       8f
2:  nudge   r11, sub, 8             @ i pinned: all of it to j
    b       8f
3:  nudge   r7, add, 8              @ j pinned: all of it to i
8:  add     r1, r1, #Q_SIZE
    cmp     r1, r2
    blo     1b
9:  mov     r0, r2
    ldmfd   sp!, {r4-r11, pc}
7:  mov     r0, r1                  @ the exact way for this one
    ldmfd   sp!, {r4-r11, pc}
    .size   link_run_arm, . - link_run_arm
