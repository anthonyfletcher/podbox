/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * The equalizer's band filters in ARM assembly: the C in eq_filter.c,
 * sample for sample, with the same output. Each loop runs several samples a
 * pass and lets the history move between registers rather than copying it:
 * four a pass for the peak and the first-order shelves, six for the
 * shelves.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "rbcodecconfig.h"

    .syntax unified

#ifndef CPU_ARM_MICRO
#if defined(CPU_PP)
    .section    .icode,"ax",%progbits
#else
    .section    .text, "ax",%progbits
#endif

/* struct dsp_filter: coefs at 00h, history[2][4] at 14h, frac[2] at 38h.
   Both functions take (f, buf, count, channels) and keep on the stack
   [sp] &history[ch], [sp, #4] &buf[ch], [sp, #8] count, [sp, #12] the
   channels left after this one, [sp, #16] &frac[ch]. */

    @ Ends a channel: frac and the history in \regs back, and on to the
    @ next channel if any.
.macro NEXT_CHANNEL loop, regs
    ldr     r1, [sp, #16]
    ldr     r3, [sp, #12]
    str     r14, [r1], #4
    ldr     r0, [sp]
    str     r1, [sp, #16]
    subs    r3, r3, #1
    stmia   r0, { \regs }
    add     r0, r0, #16
    ldmibhs sp, { r1-r2 }
    addhs   r1, r1, #4
    strhs   r1, [sp, #4]
    strhs   r3, [sp, #12]
    strhs   r0, [sp]
    bhs     \loop
    add     sp, sp, #20
.endm

/****************************************************************************
 * eq_peak_process: e = g (x - x2) + c1 e1 + c2 e2, y = x + e, shift 4.
 *
 * r4-r6 g, c1, c2; r7, r8, r11, r12 the inputs, rotating; r9, r10 e1 and
 * e2, swapping; r0, r1 the sum; r2 samples left; r3 the buffer; r14 frac.
 */
    .global eq_peak_process
    .type   eq_peak_process, %function
eq_peak_process:
    stmfd   sp!, { r4-r11, lr }
    cmp     r2, #0
    ble     .pk_exit
    ldmia   r0, { r4-r6 }
    add     r0, r0, #0x14           @ r0 = history
    sub     r3, r3, #1              @ channels left after the first
    add     r12, r0, #0x24          @ r12 = frac
    stmfd   sp!, { r0-r3, r12 }
.pk_channel:
    ldmia   r0, { r7-r10 }          @ x2, x1, e1, e2
    ldr     r3, [r1]                @ r3 = buf[ch]
    ldr     r14, [sp, #16]
    ldr     r14, [r14]              @ frac
    subs    r2, r2, #4
    blt     .pk_tail
.pk_loop:
    ldmia   r3, { r11, r12 }        @ x2 r7, x1 r8
    sub     r7, r11, r7
    smull   r0, r1, r4, r7
    smlal   r0, r1, r5, r9
    smlal   r0, r1, r6, r10
    mov     r10, r1, lsl #4
    orr     r10, r10, r0, lsr #28
    adds    r14, r14, r0, lsl #4
    adc     r10, r10, #0
    add     r7, r11, r10
    str     r7, [r3], #4
    sub     r8, r12, r8             @ x2 r8, x1 r11
    smull   r0, r1, r4, r8
    smlal   r0, r1, r5, r10
    smlal   r0, r1, r6, r9
    mov     r9, r1, lsl #4
    orr     r9, r9, r0, lsr #28
    adds    r14, r14, r0, lsl #4
    adc     r9, r9, #0
    add     r8, r12, r9
    str     r8, [r3], #4
    ldmia   r3, { r7, r8 }          @ x2 r11, x1 r12
    sub     r11, r7, r11
    smull   r0, r1, r4, r11
    smlal   r0, r1, r5, r9
    smlal   r0, r1, r6, r10
    mov     r10, r1, lsl #4
    orr     r10, r10, r0, lsr #28
    adds    r14, r14, r0, lsl #4
    adc     r10, r10, #0
    add     r11, r7, r10
    str     r11, [r3], #4
    sub     r12, r8, r12            @ x2 r12, x1 r7
    smull   r0, r1, r4, r12
    smlal   r0, r1, r5, r10
    smlal   r0, r1, r6, r9
    mov     r9, r1, lsl #4
    orr     r9, r9, r0, lsr #28
    adds    r14, r14, r0, lsl #4
    adc     r9, r9, #0
    add     r12, r8, r9
    str     r12, [r3], #4           @ x2 r7, x1 r8 again
    subs    r2, r2, #4
    bge     .pk_loop
.pk_tail:
    adds    r2, r2, #4
    beq     .pk_done
.pk_one:
    ldr     r11, [r3]
    sub     r7, r11, r7
    smull   r0, r1, r4, r7
    smlal   r0, r1, r5, r9
    smlal   r0, r1, r6, r10
    mov     r7, r1, lsl #4
    orr     r7, r7, r0, lsr #28
    adds    r14, r14, r0, lsl #4
    adc     r7, r7, #0
    mov     r10, r9
    mov     r9, r7
    add     r7, r11, r7
    str     r7, [r3], #4
    mov     r7, r8
    mov     r8, r11
    subs    r2, r2, #1
    bgt     .pk_one
.pk_done:
    NEXT_CHANNEL .pk_channel, r7-r10
.pk_exit:
    ldmpc   regs=r4-r11
    .size   eq_peak_process, .-eq_peak_process

/****************************************************************************
 * eq_low_shelf_process, eq_high_shelf_process:
 * e = n0 u + n2 u1 + c1 e1 + c2 e2, u = x +- x1, y = x + e, shift 5.
 *
 * r4-r7 n0, n2, c1, c2; r8-r10 x, x1 and u1, rotating, as u is formed in
 * x1's register; r11, r12 e1 and e2, swapping. The rest as for the peak.
 */

    @ One sample. x into \x; u = x \op x1 into \x1, which becomes u1;
    @ e into \e2, which becomes e1; y out through \u1.
.macro SHELF_SAMPLE op, x, x1, u1, e1, e2
    ldr     \x, [r3]
    \op     \x1, \x, \x1
    smull   r0, r1, r4, \x1
    smlal   r0, r1, r5, \u1
    smlal   r0, r1, r6, \e1
    smlal   r0, r1, r7, \e2
    mov     \e2, r1, lsl #5
    orr     \e2, \e2, r0, lsr #27
    adds    r14, r14, r0, lsl #5
    adc     \e2, \e2, #0
    add     \u1, \x, \e2
    str     \u1, [r3], #4
.endm

.macro SHELF_PROCESS name, op
    .global \name
    .type   \name, %function
\name:
    stmfd   sp!, { r4-r11, lr }
    cmp     r2, #0
    ble     .\name\()_exit
    ldmia   r0, { r4-r7 }
    add     r0, r0, #0x14
    sub     r3, r3, #1
    add     r12, r0, #0x24
    stmfd   sp!, { r0-r3, r12 }
.\name\()_channel:
    ldmia   r0, { r9-r12 }          @ x1, u1, e1, e2
    ldr     r3, [r1]
    ldr     r14, [sp, #16]
    ldr     r14, [r14]
    subs    r2, r2, #6
    blt     .\name\()_tail
.\name\()_loop:
    SHELF_SAMPLE \op, r8,  r9,  r10, r11, r12
    SHELF_SAMPLE \op, r10, r8,  r9,  r12, r11
    SHELF_SAMPLE \op, r9,  r10, r8,  r11, r12
    SHELF_SAMPLE \op, r8,  r9,  r10, r12, r11
    SHELF_SAMPLE \op, r10, r8,  r9,  r11, r12
    SHELF_SAMPLE \op, r9,  r10, r8,  r12, r11
    subs    r2, r2, #6
    bge     .\name\()_loop
.\name\()_tail:
    adds    r2, r2, #6
    beq     .\name\()_done
.\name\()_one:
    SHELF_SAMPLE \op, r8,  r9,  r10, r11, r12
    mov     r10, r9
    mov     r9, r8
    mov     r0, r11
    mov     r11, r12
    mov     r12, r0
    subs    r2, r2, #1
    bgt     .\name\()_one
.\name\()_done:
    NEXT_CHANNEL .\name\()_channel, r9-r12
.\name\()_exit:
    ldmpc   regs=r4-r11
    .size   \name, .-\name
.endm

    @ IRAM has no room for the shelves as well; they run from cached SDRAM,
    @ the peak from IRAM, since a band is a peak more often than a shelf.
    .section    .text, "ax",%progbits
    SHELF_PROCESS eq_low_shelf_process, add
    SHELF_PROCESS eq_high_shelf_process, sub

/****************************************************************************
 * eq_low_shelf1_process, eq_high_shelf1_process, eq_high_shelf1_gain_process:
 * e = k u + p e1, u = x +- x1, y = x + e, shift 4; with the gain,
 * y = (G x >> 28) + e and k already scaled by G.
 *
 * r4, r5, r9 k, p, G; r6 and r8 x and x1, swapping, as u is formed in x1's
 * register; r7 e1; r10 G x. The rest as for the peak.
 */

    @ One sample. x into \x; u = x \op x1 into \x1; e into r7; y out
    @ through \x1, leaving \x as x1.
.macro SHELF1_SAMPLE op, gain, x, x1
    ldr     \x, [r3]
    \op     \x1, \x, \x1
    smull   r0, r1, \x1, r4
    smlal   r0, r1, r5, r7
    mov     r7, r1, lsl #4
    orr     r7, r7, r0, lsr #28
    adds    r14, r14, r0, lsl #4
    adc     r7, r7, #0
.if \gain
    smull   r0, r1, \x, r9
    mov     r10, r1, lsl #4
    orr     r10, r10, r0, lsr #28
    add     \x1, r10, r7
.else
    add     \x1, \x, r7
.endif
    str     \x1, [r3], #4
.endm

.macro SHELF1_PROCESS name, op, gain
    .global \name
    .type   \name, %function
\name:
    stmfd   sp!, { r4-r11, lr }
    cmp     r2, #0
    ble     .\name\()_exit
    ldmia   r0, { r4, r5, r9 }
    add     r0, r0, #0x14
    sub     r3, r3, #1
    add     r12, r0, #0x24
    stmfd   sp!, { r0-r3, r12 }
.\name\()_channel:
    ldmia   r0, { r6-r7 }           @ x1, e1
    ldr     r3, [r1]
    ldr     r14, [sp, #16]
    ldr     r14, [r14]
    subs    r2, r2, #4
    blt     .\name\()_tail
.\name\()_loop:
    SHELF1_SAMPLE \op, \gain, r8, r6
    SHELF1_SAMPLE \op, \gain, r6, r8
    SHELF1_SAMPLE \op, \gain, r8, r6
    SHELF1_SAMPLE \op, \gain, r6, r8
    subs    r2, r2, #4
    bge     .\name\()_loop
.\name\()_tail:
    adds    r2, r2, #4
    beq     .\name\()_done
.\name\()_one:
    SHELF1_SAMPLE \op, \gain, r8, r6
    mov     r6, r8
    subs    r2, r2, #1
    bgt     .\name\()_one
.\name\()_done:
    NEXT_CHANNEL .\name\()_channel, r6-r7
.\name\()_exit:
    ldmpc   regs=r4-r11
    .size   \name, .-\name
.endm

    SHELF1_PROCESS eq_low_shelf1_process, add, 0
    SHELF1_PROCESS eq_high_shelf1_process, sub, 0
    SHELF1_PROCESS eq_high_shelf1_gain_process, sub, 1

#endif /* CPU_ARM_MICRO */
