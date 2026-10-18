/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * ARM assembly for the ALAC decoder: the adaptive FIR at orders 1 to 8,
 * and the Rice decoder.
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

#include "config.h"

    .syntax unified

/* ---------------------------------------------------------------------------
   void alac_fir_adapt_arm(int32_t *buffer_out, int count, int16_t *coefs,
                           int order, int quant, int readsamplesize);

   The loop of fir_adapt_n() in alac.c, sample for sample.  It differs in
   one respect: the coefficients are held in 32-bit registers where the C
   wraps them at 16 bits, so the caller passes only a frame in which none
   can reach the limit -- each moves at most one step per sample.  The
   caller also guarantees order 1 to 8, quant 1 to 15, readsamplesize 1 to
   32 and count at least 1.

   Tap j is the sample j+1 behind the window's base: its difference
   d = base - b[j+1] is multiplied by coefs[order-1-j], held in r4+j.  The
   sum runs as the sum of d * c, which is the negated sum of the C; the
   adaptation reloads only the taps it reaches, two on average, rather than
   keeping every difference live.

   Registers through the loop:

     r0  quant in bits 0-7, 32 - readsamplesize in bits 8-15: a shift by
         register reads only the low byte
     r1  base, the window's first sample
     r2, r3  scratch
     r4-r11  the coefficients, tap j in r4+j
     r12 the sum, then the residual as the adaptation spends it
     lr  walks the window; at the output, the slot holding its residual
     [sp] the end of the window's walk
 */

/* r12 -= r2 * k, for the tap's weight k; r2 is spent */
.macro MULSUB k
.if \k == 1
    sub     r12, r12, r2
.elseif \k == 2
    sub     r12, r12, r2, lsl #1
.elseif \k == 3
    add     r2, r2, r2, lsl #1
    sub     r12, r12, r2
.elseif \k == 4
    sub     r12, r12, r2, lsl #2
.elseif \k == 5
    add     r2, r2, r2, lsl #2
    sub     r12, r12, r2
.elseif \k == 6
    add     r2, r2, r2, lsl #1
    sub     r12, r12, r2, lsl #1
.elseif \k == 7
    rsb     r2, r2, r2, lsl #3
    sub     r12, r12, r2
.else
    sub     r12, r12, r2, lsl #3
.endif
.endm

/* One tap of the sum, the sample already in \reg */
.macro TAP_SUM reg, coef
    sub     \reg, r1, \reg
    mla     r12, \reg, \coef, r12
.endm

/* One step of the adaptation for a positive residual: the coefficient moves
   against the difference's sign, and the residual drops by |d| >> quant
   times the tap's weight.  Stops at \done once the residual is no longer
   positive. */
.macro ADAPT_POS order, j, coef, done
    ldr     r2, [lr, #-4 * (\order - \j)]
    subs    r2, r1, r2
    addlt   \coef, \coef, #1
    subgt   \coef, \coef, #1
    rsblt   r2, r2, #0
    mov     r2, r2, asr r0
    MULSUB  \j + 1
.if \j < \order - 1
    cmp     r12, #0
    ble     \done
.endif
.endm

/* The same for a negative residual: the coefficient moves with the
   difference's sign, and -|d| >> quant rounds down as the C's does */
.macro ADAPT_NEG order, j, coef, done
    ldr     r2, [lr, #-4 * (\order - \j)]
    subs    r2, r1, r2
    addgt   \coef, \coef, #1
    sublt   \coef, \coef, #1
    rsbgt   r2, r2, #0
    mov     r2, r2, asr r0
    MULSUB  \j + 1
.if \j < \order - 1
    cmp     r12, #0
    bge     \done
.endif
.endm

/* Coefficient j's table entry is coefs[order-1-j], from r2 */
.macro COEFS op, order
    \op     r4, [r2, #2 * (\order - 1)]
.if \order > 1
    \op     r5, [r2, #2 * (\order - 2)]
.endif
.if \order > 2
    \op     r6, [r2, #2 * (\order - 3)]
.endif
.if \order > 3
    \op     r7, [r2, #2 * (\order - 4)]
.endif
.if \order > 4
    \op     r8, [r2, #2 * (\order - 5)]
.endif
.if \order > 5
    \op     r9, [r2, #2 * (\order - 6)]
.endif
.if \order > 6
    \op     r10, [r2, #2 * (\order - 7)]
.endif
.if \order > 7
    \op     r11, [r2, #2 * (\order - 8)]
.endif
.endm

.macro FIR_ORDER order
.Lfir\order:
    COEFS   ldrsh, \order
1:
    /* the sum: base and b[1] first, then the rest in pairs */
    ldmia   lr!, { r1, r2 }
    sub     r2, r1, r2
    mul     r12, r2, r4
.if \order >= 3
    ldmia   lr!, { r2, r3 }
    TAP_SUM r2, r5
    TAP_SUM r3, r6
.elseif \order == 2
    ldr     r2, [lr], #4
    TAP_SUM r2, r5
.endif
.if \order >= 5
    ldmia   lr!, { r2, r3 }
    TAP_SUM r2, r7
    TAP_SUM r3, r8
.elseif \order == 4
    ldr     r2, [lr], #4
    TAP_SUM r2, r7
.endif
.if \order >= 7
    ldmia   lr!, { r2, r3 }
    TAP_SUM r2, r9
    TAP_SUM r3, r10
.elseif \order == 6
    ldr     r2, [lr], #4
    TAP_SUM r2, r9
.endif
.if \order == 8
    ldr     r2, [lr], #4
    TAP_SUM r2, r11
.endif

    /* the output, over its residual: the rounded sum, base and residual,
       sign-extended from readsamplesize bits */
    mov     r2, #1
    mov     r2, r2, lsl r0
    rsb     r2, r12, r2, lsr #1       @ 1 << (quant-1), plus the C's sum
    mov     r2, r2, asr r0
    add     r2, r2, r1
    ldr     r12, [lr]
    add     r2, r2, r12
    mov     r3, r0, lsr #8
    mov     r2, r2, lsl r3
    mov     r2, r2, asr r3
    str     r2, [lr]

    cmp     r12, #0
    bgt     2f
    blt     3f
    b       4f
2:
    ADAPT_POS \order, 0, r4, 4f
.if \order > 1
    ADAPT_POS \order, 1, r5, 4f
.endif
.if \order > 2
    ADAPT_POS \order, 2, r6, 4f
.endif
.if \order > 3
    ADAPT_POS \order, 3, r7, 4f
.endif
.if \order > 4
    ADAPT_POS \order, 4, r8, 4f
.endif
.if \order > 5
    ADAPT_POS \order, 5, r9, 4f
.endif
.if \order > 6
    ADAPT_POS \order, 6, r10, 4f
.endif
.if \order > 7
    ADAPT_POS \order, 7, r11, 4f
.endif
    b       4f
3:
    ADAPT_NEG \order, 0, r4, 4f
.if \order > 1
    ADAPT_NEG \order, 1, r5, 4f
.endif
.if \order > 2
    ADAPT_NEG \order, 2, r6, 4f
.endif
.if \order > 3
    ADAPT_NEG \order, 3, r7, 4f
.endif
.if \order > 4
    ADAPT_NEG \order, 4, r8, 4f
.endif
.if \order > 5
    ADAPT_NEG \order, 5, r9, 4f
.endif
.if \order > 6
    ADAPT_NEG \order, 6, r10, 4f
.endif
.if \order > 7
    ADAPT_NEG \order, 7, r11, 4f
.endif
4:
    sub     lr, lr, #4 * \order       @ the next window, one sample on
    ldr     r3, [sp]
    cmp     lr, r3
    bne     1b

    ldr     r2, [sp, #4]
    COEFS   strh, \order
    b       .Lfir_ret
.endm

#ifdef USE_IRAM
    .section .icode,"ax",%progbits
#else
    .section .text.alac_fir_adapt,"ax",%progbits
#endif
    .align  2
    .global alac_fir_adapt_arm
alac_fir_adapt_arm:
    stmfd   sp!, { r4-r11, lr }
    add     r1, r0, r1, lsl #2        @ where the window's walk ends
    stmfd   sp!, { r1, r2 }           @ end, coefs
    mov     lr, r0
    ldr     r0, [sp, #44]             @ quant
    ldr     r1, [sp, #48]             @ readsamplesize
    rsb     r1, r1, #32
    orr     r0, r0, r1, lsl #8
    sub     r3, r3, #1
    add     pc, pc, r3, lsl #2        @ reads as two instructions on
    nop
    b       .Lfir1
    b       .Lfir2
    b       .Lfir3
    b       .Lfir4
    b       .Lfir5
    b       .Lfir6
    b       .Lfir7
    b       .Lfir8

    FIR_ORDER 1
    FIR_ORDER 2
    FIR_ORDER 3
    FIR_ORDER 4
    FIR_ORDER 5
    FIR_ORDER 6
    FIR_ORDER 7
    FIR_ORDER 8

.Lfir_ret:
    add     sp, sp, #8
    ldmpc   regs=r4-r11

/* ---------------------------------------------------------------------------
   void alac_deinterlace_24_arm(int32_t *buffer0, int32_t *buffer1,
                                const unsigned char *lows, int count,
                                int lows_shift, int interlacing_shift,
                                int interlacing_leftweight);

   deinterlace_24_bytes() in alac.c, sample for sample.

   The low bytes are read through a window, r4, that takes one byte of the
   stream a channel-sample: its last two bytes hold the sample's low byte
   at bit lows_shift.  The output is shifted up by SCALE24, which is 5.

   Registers through the loop:

     r0 buffer0   r1 buffer1   r2 lows   r3 pairs left
     r4 window    r5 lows_shift          r6 interlacing_shift
     r7 interlacing_leftweight           r8-r11 the samples   r12 scratch
 */

/* One channel-sample: \s, its upper bits, becomes the output */
.macro D24_LOW s
    ldrb    r12, [r2], #1
    orr     r4, r12, r4, lsl #8
    mov     r12, r4, lsr r5
    and     r12, r12, #0xff
    orr     \s, r12, \s, lsl #8
    mov     \s, \s, lsl #5
.endm

/* Mid and difference in \l and \r to right in \l and left in \r */
.macro D24_WEIGHT l, r
    mul     r12, \r, r7
    sub     \l, \l, r12, asr r6
    add     \r, \r, \l
.endm

/* The loop for a frame weighted or not; left and right end up in the
   registers named first and second of each pair */
.macro D24_LOOP weighted, lt0, rt0, lt1, rt1
    tst     r3, #1                    @ an odd sample first, alone
    beq     2f
    ldr     r8, [r0]
    ldr     r10, [r1]
.if \weighted
    D24_WEIGHT r8, r10
.endif
    D24_LOW \lt0
    D24_LOW \rt0
    str     \lt0, [r0], #4
    str     \rt0, [r1], #4
2:
    movs    r3, r3, lsr #1
    beq     .Ld24_ret
1:
    ldmia   r0, { r8, r9 }
    ldmia   r1, { r10, r11 }
.if \weighted
    D24_WEIGHT r8, r10
    D24_WEIGHT r9, r11
.endif
    D24_LOW \lt0
    D24_LOW \rt0
    D24_LOW \lt1
    D24_LOW \rt1
    stmia   r0!, { \lt0, \lt1 }
    stmia   r1!, { \rt0, \rt1 }
    subs    r3, r3, #1
    bne     1b
    b       .Ld24_ret
.endm

#ifdef USE_IRAM
    .section .icode,"ax",%progbits
#else
    .section .text.alac_deinterlace_24,"ax",%progbits
#endif
    .align  2
    .global alac_deinterlace_24_arm
alac_deinterlace_24_arm:
    stmfd   sp!, { r4-r11, lr }
    add     r12, sp, #36
    ldmia   r12, { r5-r7 }
    ldrb    r4, [r2], #1
    cmp     r7, #0
    beq     .Ld24_plain
    D24_LOOP 1, r10, r8, r11, r9
.Ld24_plain:
    D24_LOOP 0, r8, r10, r9, r11
.Ld24_ret:
    ldmpc   regs=r4-r11

/* ---------------------------------------------------------------------------
   int alac_rice_decode_arm(int32_t *out, int n, struct alac_rice *st);

   The loop of entropy_rice_decode() in alac.c for the samples that take
   its common path, up to n of them; returns how many it decoded and leaves
   st's position and history where the C would have.  It stops before a
   sample whose unary run is 8 or more (an escape is 9), or whose history
   puts the parameter estimate past 16 bits, and leaves that sample to the
   C.  It stops after a sample that leaves the history below 128 with more
   to follow, setting zero_run: the C decodes the run of zeros that comes
   next.  The caller passes only a kmodifier of 1 to 17 and a zero
   sign_modifier, which with a run of at most 7 bound a code at 25 bits.

   The cache is two whole words of the stream and a bit position in the
   first, as in the FLAC reader: each sample reads a 32-bit window at the
   position, and once the position passes 32 the second word becomes the
   first and the next is loaded.

   Registers through the loop:

     r0 out     r1 samples left    r2 history    r3 scratch
     r4 first word   r5 second word   r6 bit position in r4
     r7 next word to load, aligned   r8 leading-zero table (ARMv4)
     r9 kmodifier    r10 historymult   r11, r12, lr scratch
 */

/* A word as loaded, little-endian, into stream order. */
.macro ALAC_BSWAP reg, tmp
    eor     \tmp, \reg, \reg, ror #16
    bic     \tmp, \tmp, #0xff0000
    mov     \reg, \reg, ror #8
    eor     \reg, \reg, \tmp, lsr #8
.endm

#ifdef USE_IRAM
    .section .icode,"ax",%progbits
#else
    .section .text.alac_rice_decode,"ax",%progbits
#endif
    .align  2
    .global alac_rice_decode_arm
alac_rice_decode_arm:
    stmfd   sp!, { r4-r11, lr }
    stmfd   sp!, { r1, r2 }           @ n, st
    ldr     lr, [r2]                  @ st->buffer
    ldr     r12, [r2, #4]             @ st->index, in bits
    ldr     r9, [r2, #16]             @ st->kmodifier
    ldr     r10, [r2, #20]            @ st->historymult
    ldr     r2, [r2, #8]              @ st->history
    cmp     r1, #0
    ble     .Lrc_done

    add     r7, lr, r12, lsr #3       @ byte holding the next bit
    and     r3, r7, #3                @ ... and how far into its word it is
    bic     r7, r7, #3
    and     r6, r12, #7
    add     r6, r6, r3, lsl #3        @ bit position within that word
    ldr     r4, [r7], #4
    ldr     r5, [r7], #4
    ALAC_BSWAP r4, r3
    ALAC_BSWAP r5, r3
#if ARM_ARCH < 5
    adr     r8, .Lrc_lztab
#endif

.Lrc_loop:
    /* k = min(log2((history >> 9) + 3), kmodifier), as the C's
       31 - kmodifier - clz() with its 16-bit table */
    mov     r3, r2, asr #9
    add     r3, r3, #3
    cmp     r3, #0x10000
    bhs     .Lrc_done                 @ past the table: C
#if ARM_ARCH >= 5
    clz     r12, r3
    rsb     r12, r12, #31
#else
    movs    r12, r3, lsr #8
    ldrbne  r12, [r8, r12]
    rsbne   r12, r12, #15
    ldrbeq  r12, [r8, r3]
    rsbeq   r12, r12, #7
#endif
    cmp     r12, r9
    movhi   r12, r9

    /* the window, and its run of 1s */
    rsb     r3, r6, #32
    mov     lr, r4, lsl r6
    orr     lr, lr, r5, lsr r3        @ a shift by 32 gives 0, so r6 = 0 works
    mvn     r3, lr
#if ARM_ARCH >= 5
    clz     r3, r3
#else
    mov     r3, r3, lsr #24
    ldrb    r3, [r8, r3]              @ 8 means "8 or more"
#endif
    cmp     r3, #7
    bhi     .Lrc_done                 @ a long run, or an escape: C

    /* x 1s, a 0, then k bits v: x * (2^k - 1) + v - 1, or for a v of 0
       or 1 just the first term and one bit fewer */
    add     r11, r3, #1
    mov     lr, lr, lsl r11
    rsb     r11, r12, #32
    mov     lr, lr, lsr r11           @ v
    rsb     r11, r3, r3, lsl r12      @ x * (2^k - 1)
    add     r3, r3, r12               @ the code's length if v <= 1
    cmp     lr, #1
    addhi   r11, r11, lr
    subhi   r11, r11, #1
    addhi   r3, r3, #1
    add     r6, r6, r3

    /* the sign is the value's low bit */
    add     r3, r11, #1
    mov     r3, r3, lsr #1
    tst     r11, #1
    rsbne   r3, r3, #0
    str     r3, [r0], #4

    /* history += value * mult - ((history * mult) >> 9), and 0xffff for
       a value past 16 bits */
    mul     r3, r2, r10
    mla     r2, r11, r10, r2
    sub     r2, r2, r3, asr #9
    cmp     r11, #0x10000
    mvnhs   r2, #0
    movhs   r2, r2, lsr #16

    cmp     r6, #32
    blo     1f
    mov     r4, r5
    ldr     r5, [r7], #4
    sub     r6, r6, #32
    ALAC_BSWAP r5, r3
1:
    cmp     r2, #128
    blt     .Lrc_low
.Lrc_next:
    subs    r1, r1, #1
    bne     .Lrc_loop
    b       .Lrc_done

.Lrc_low:                             @ a run of zeros follows, unless this
    cmp     r1, #1                    @ was the last sample
    beq     .Lrc_next
    sub     r1, r1, #1
    mov     r11, #1
    b       .Lrc_out

.Lrc_done:
    mov     r11, #0
.Lrc_out:                             @ r1 left undecoded, r11 zero_run
    ldmfd   sp!, { r3, r12 }          @ n, st
    str     r2, [r12, #8]             @ st->history
    str     r11, [r12, #12]           @ st->zero_run
    ldr     lr, [r12]
    sub     r0, r7, lr                @ r4 was loaded from r7 - 8, so the
    mov     r0, r0, lsl #3            @ index is 8*(r7-buffer) - 64 + r6
    sub     r0, r0, #64
    add     r0, r0, r6
    str     r0, [r12, #4]             @ st->index
    sub     r0, r3, r1                @ samples decoded
    ldmpc   regs=r4-r11

#if ARM_ARCH < 5
.Lrc_lztab:
    .byte  8, 7, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4
    .byte  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3
    .byte  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2
    .byte  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2
    .byte  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
    .byte  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
    .byte  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
    .byte  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
    .byte  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    .byte  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    .byte  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    .byte  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    .byte  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    .byte  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    .byte  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    .byte  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
#endif
