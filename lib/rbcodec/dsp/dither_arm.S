/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * dither_channel() in ARM assembly, with dither.c's output. The history
 * stays in registers for the whole block.
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
    .section    .text, "ax",%progbits

/* One sample. e0 holds error[0] on the way in and the new random value on
   the way out; rnd holds the last random value in and the new error[0]
   out. So the loop runs two of these with the pair swapped, and moves
   nothing between samples. */
.macro dither_one e0, rnd
    ldr     r12, [r0], #4
    add     r12, r12, \e0
    sub     r12, r12, r5
    add     r12, r12, r6            @ sample, noise shaped
    mov     r6, r5                  @ error[2] = error[1]
    add     r5, \e0, \e0, lsr #31
    mov     r5, r5, asr #1          @ error[1] = error[0] / 2, toward zero
    mla     \e0, \rnd, r10, r11     @ the next random value
    and     \rnd, \rnd, r8
    add     lr, r12, r9
    sub     lr, lr, \rnd
    and     \rnd, \e0, r8
    add     lr, lr, \rnd            @ biased, with high-pass triangle noise
    mov     lr, lr, asr r3
    sub     \rnd, r12, lr, lsl r3   @ error[0]: what the quantising lost
    mov     r12, lr, asr #15
    teq     r12, r12, asr #31
    mvnne   r12, #0x8000
    eorne   lr, r12, lr, asr #31    @ clip to -32768..32767
    strh    lr, [r1], #4
.endm

/* (st, src, dst, count, scale). In the loop: r0 src, r1 dst, r2 count,
   r3 scale, r4 and r7 the swapping pair, r5 error[1], r6 error[2], r8 the
   mask of the bits dropped, r9 half the output's LSB, r10 and r11 the
   random generator's constants. The state pointer is kept at [sp]. */
    .global dither_channel
    .type   dither_channel, %function
dither_channel:
    cmp     r3, #0
    bxle    lr
    stmfd   sp!, { r0, r4-r11, lr }
    ldr     r12, [sp, #40]          @ scale
    ldmia   r0, { r4-r7 }           @ error[0..2], random
    mov     r0, r1
    mov     r1, r2
    mov     r2, r3
    mov     r3, r12
    mov     r8, #1
    mov     r8, r8, lsl r3
    sub     r8, r8, #1
    add     r9, r8, #1
    mov     r9, r9, lsr #1
    ldr     r10, =0x0019660d
    ldr     r11, =0x3c6ef35f

    @ An odd count starts at the second half, with the pair swapped to suit
    tst     r2, #1
    beq     .dither_loop
    add     r2, r2, #1
    mov     r12, r4
    mov     r4, r7
    mov     r7, r12
    b       .dither_odd

.dither_loop:
    dither_one r4, r7
.dither_odd:
    dither_one r7, r4
    subs    r2, r2, #2
    bgt     .dither_loop

    ldmfd   sp!, { r0 }
    stmia   r0, { r4-r7 }
    ldmpc   regs=r4-r11
    .ltorg
    .size   dither_channel, .-dither_channel

#endif /* CPU_ARM_MICRO */
