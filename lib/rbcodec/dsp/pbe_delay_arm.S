/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * pbe_delay_process() in ARM assembly, with pbe_delay.c's output. The
 * samples go in runs that end where the nearest ring wraps, so the inner
 * loop keeps everything in registers and tests no head.
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

/* (buf, count, coefs, ring, head, len). The frame holds [sp] the ring
   starts, [sp, #12] their ends, [sp, #24] the samples left; head and len
   are at [sp, #64] and [sp, #68].

   In the run: r0 buf, r1-r3 the ring positions, r4-r6 the coefs, r7 the
   samples left in the run, r8 x, r9 scratch, r10-r12 f1-f3, then the sum
   and the values written back. */
    .global pbe_delay_process
    .type   pbe_delay_process, %function
pbe_delay_process:
    stmfd   sp!, { r4-r11, lr }
    sub     sp, sp, #28
    str     r1, [sp, #24]
    ldmia   r2, { r4-r6 }
    ldmia   r3, { r7-r9 }           @ starts
    ldr     r10, [sp, #68]
    ldmia   r10, { r10-r12 }        @ lengths
    add     r10, r7, r10, lsl #2
    add     r11, r8, r11, lsl #2
    add     r12, r9, r12, lsl #2
    stmia   sp, { r7-r12 }
    ldr     r10, [sp, #64]
    ldmia   r10, { r1-r3 }          @ heads
    add     r1, r7, r1, lsl #2
    add     r2, r8, r2, lsl #2
    add     r3, r9, r3, lsl #2

    @ A run is as long as the samples left, and no longer than any ring has
    @ before its end; at least one, for a ring whose length dropped under
    @ its head.
.pbe_run:
    ldr     r7, [sp, #24]
    cmp     r7, #0
    ble     .pbe_done
    add     r8, sp, #12
    ldmia   r8, { r10-r12 }         @ ends
    sub     r10, r10, r1
    sub     r11, r11, r2
    sub     r12, r12, r3
    mov     r10, r10, asr #2
    cmp     r10, #1
    movlt   r10, #1
    cmp     r7, r10
    movgt   r7, r10
    mov     r11, r11, asr #2
    cmp     r11, #1
    movlt   r11, #1
    cmp     r7, r11
    movgt   r7, r11
    mov     r12, r12, asr #2
    cmp     r12, #1
    movlt   r12, #1
    cmp     r7, r12
    movgt   r7, r12
    ldr     r8, [sp, #24]
    sub     r8, r8, r7
    str     r8, [sp, #24]

.pbe_loop:
    ldr     r8, [r0]                @ x
    smull   r9, r10, r8, r4
    mov     r10, r10, lsl #1
    orr     r10, r10, r9, lsr #31   @ f1
    smull   r9, r11, r8, r5
    mov     r11, r11, lsl #1
    orr     r11, r11, r9, lsr #31   @ f2
    smull   r9, r12, r8, r6
    mov     r12, r12, lsl #1
    orr     r12, r12, r9, lsr #31   @ f3
    sub     r8, r8, r10             @ x - f1 into ring 0
    sub     r10, r10, r11           @ the sum, from f1 - f2
    sub     r11, r11, r12           @ f2 - f3 into ring 1, f3 into ring 2
    ldr     r9, [r1]
    str     r8, [r1], #4
    add     r10, r10, r9
    ldr     r9, [r2]
    str     r11, [r2], #4
    add     r10, r10, r9
    ldr     r9, [r3]
    str     r12, [r3], #4
    add     r10, r10, r9
    str     r10, [r0], #4
    subs    r7, r7, #1
    bgt     .pbe_loop

    add     r8, sp, #12
    ldmia   r8, { r8-r10 }          @ ends
    cmp     r1, r8
    ldrhs   r1, [sp]
    cmp     r2, r9
    ldrhs   r2, [sp, #4]
    cmp     r3, r10
    ldrhs   r3, [sp, #8]
    b       .pbe_run

.pbe_done:
    ldmia   sp, { r7-r9 }
    sub     r1, r1, r7
    sub     r2, r2, r8
    sub     r3, r3, r9
    mov     r1, r1, asr #2
    mov     r2, r2, asr #2
    mov     r3, r3, asr #2
    ldr     r10, [sp, #64]
    stmia   r10, { r1-r3 }
    add     sp, sp, #28
    ldmpc   regs=r4-r11
    .size   pbe_delay_process, .-pbe_delay_process

#endif /* CPU_ARM_MICRO */
