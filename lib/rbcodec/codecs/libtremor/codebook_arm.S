/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * ARM assembly for the Vorbis residue: vorbis_book_decodevv_add_2ch_even()
 * in codebook.c for books of dimension 2 and 4, with decode_packed_block()'s
 * fast loop adding each vector into the channels as its codeword is read.
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
   long vorbis_vv2_<dim>_<lsl|asr>_arm(codebook *book, oggpack_buffer *b,
                                       struct vv2_arm *s, unsigned int n);

   The C function codeword for codeword: the same chunks of at most 32, and
   in each the same choice between the fast loop and, within 8 bytes of the
   end of the packet, vorbis_vv2_slow().  Each vector goes t[0] to *p0++,
   t[1] to *p1++, then t[2] and t[3] likewise for dimension 4, shifted left
   (lsl) by -s->shift or arithmetically right (asr) by s->shift.  Returns 0,
   or -1 where the C returns -1; s->p0 and s->p1 follow the writes.

   Registers through the fast loop:

     r0  the bit cache
     r1  cachesize - maxlength: a refill is due when it goes negative
     r2, r3, r12, lr  scratch
     r4  firsttable   r5  cachemask   r6  codelengths   r7  valuelist
     r8  p0           r9  p1          r10 codewords left r11 shift
 */

/* codebook, oggpack_buffer and struct vv2_arm, checked in codebook.c */
#define B_USED          8
#define B_VALUELIST     16
#define B_CODELIST      20
#define B_CODELENGTHS   28
#define B_FIRSTTABLE    32
#define B_FIRSTTABLEN   36
#define B_MAXLENGTH     40
#define O_ENDBYTE       0
#define O_ENDBIT        4
#define O_PTR           12
#define O_STORAGE       16
#define S_SHIFT         8

/* The stack frame */
#define F_FT            0
#define F_MASK          4
#define F_CL            8
#define F_VL            12
#define F_MAXL          16
#define F_BIT           20
#define F_BITEND        24
#define F_PTR           28
#define F_USED          32
#define F_CODELIST      36
#define F_SHIFT         40
#define F_BOOK          44
#define F_B             48
#define F_S             52
#define F_N             56
#define F_LEFT          60
#define F_CHUNK         64
#define FRAME           68

/* Two values of a vector into the channels: from r2 to *p0++, from r3 to
   *p1++. */
.macro ADDPAIR op
    ldr     lr, [r8]
    add     lr, lr, r2, \op r11
    str     lr, [r8], #4
    ldr     lr, [r9]
    add     lr, lr, r3, \op r11
    str     lr, [r9], #4
.endm

.macro VV2 name, dim, op
#ifdef USE_IRAM
    .section .icode,"ax",%progbits
#else
    .section .text.\name,"ax",%progbits
#endif
    .align  2
    .global \name
\name:
    stmfd   sp!, { r4-r11, lr }
    sub     sp, sp, #FRAME
    add     r12, sp, #F_BOOK
    stmia   r12, { r0-r3 }            @ book, b, s, n
    ldr     r4, [r0, #B_FIRSTTABLE]
    ldr     r5, [r0, #B_FIRSTTABLEN]
    mvn     r12, #0
    mvn     r5, r12, lsl r5           @ (1 << dec_firsttablen) - 1
    ldr     r6, [r0, #B_CODELENGTHS]
    ldr     r7, [r0, #B_VALUELIST]
    ldr     r8, [r0, #B_MAXLENGTH]
    stmia   sp, { r4-r8 }
    ldr     r4, [r0, #B_USED]
    ldr     r5, [r0, #B_CODELIST]
    ldr     r6, [r2, #S_SHIFT]
.ifc \op, lsl
    rsb     r6, r6, #0
.endif
    add     r12, sp, #F_USED
    stmia   r12, { r4-r6 }

10: @ next chunk: r3 the values left per channel
    cmp     r3, #0
    beq     19f
.if \dim == 2
    cmp     r3, #32                   @ chunk = 16*dim > n ? (n*2-1)/dim + 1
    movhs   r12, #32                  @                    : 32
    movlo   r12, r3
.else
    cmp     r3, #64
    movhs   r12, #32
    movlo   r12, r3, lsl #1
    sublo   r12, r12, #1
    movlo   r12, r12, lsr #2
    addlo   r12, r12, #1
.endif
    str     r12, [sp, #F_CHUNK]
    str     r12, [sp, #F_LEFT]

11: @ next run: at least one codeword left in the chunk
    ldr     r3, [sp, #F_B]
    ldr     r0, [r3, #O_ENDBYTE]
    ldr     r1, [r3, #O_STORAGE]
    sub     r1, r1, #8
    cmp     r0, r1
    blt     12f
    ldr     r0, [sp, #F_BOOK]         @ the last bytes: one codeword in C
    mov     r1, r3
    ldr     r2, [sp, #F_S]
    bl      vorbis_vv2_slow
    cmp     r0, #0
    blt     18f
    ldr     r12, [sp, #F_LEFT]
    subs    r12, r12, #1
    str     r12, [sp, #F_LEFT]
    bne     11b
    b       17f

12: @ a fast run: r3 b, r0 b->endbyte
    ldr     r2, [r3, #O_PTR]
    ldr     r1, [r3, #O_ENDBIT]
    ldr     r12, [r3, #O_STORAGE]
    and     lr, r2, #3
    add     r1, r1, lr, lsl #3        @ bit
    bic     r2, r2, #3                @ the word holding it
    sub     r12, r12, r0
    add     r12, r12, lr
    mov     r12, r12, lsl #3          @ bitend
    str     r1, [sp, #F_BIT]
    str     r12, [sp, #F_BITEND]
    str     r2, [sp, #F_PTR]
    ldmia   sp, { r4-r7 }
    ldr     r12, [sp, #F_S]
    ldmia   r12, { r8, r9 }
    ldr     r11, [sp, #F_SHIFT]
    ldr     r10, [sp, #F_LEFT]
    ldr     r1, [sp, #F_MAXL]
    rsb     r1, r1, #0                @ an empty cache: refill first
    mov     r0, #0

1:  @ loop
    cmp     r1, #0
    blt     5f
2:  @ look up
    and     r12, r0, r5
    ldr     r12, [r4, r12, lsl #2]
    subs    r12, r12, #1              @ every slot holds entry + 1, or a
    blt     7f                        @ hint with the top bit set
3:  @ found: r12 the entry
    ldrb    r2, [r6, r12]
    sub     r1, r1, r2
    mov     r0, r0, lsr r2
.if \dim == 2
    add     r12, r7, r12, lsl #3
    ldmia   r12, { r2, r3 }
    ADDPAIR \op
.else
    add     r12, r7, r12, lsl #4
    ldmia   r12!, { r2, r3 }
    ADDPAIR \op
    ldmia   r12, { r2, r3 }
    ADDPAIR \op
.endif
    subs    r10, r10, #1
    bne     1b

    ldr     r12, [sp, #F_MAXL]
    add     r1, r1, r12               @ cachesize
4:  @ the run is over: r1 cachesize, r10 codewords left
    ldr     r12, [sp, #F_S]
    stmia   r12, { r8, r9 }
    str     r10, [sp, #F_LEFT]
    ldr     r3, [sp, #F_B]
    ldr     r2, [r3, #O_PTR]
    ldr     r0, [sp, #F_BIT]
    and     lr, r2, #3
    add     r1, r1, lr, lsl #3
    sub     r0, r0, r1                @ the bits consumed
    ldr     r1, [r3, #O_ENDBYTE]
    add     r1, r1, r0, lsr #3
    str     r1, [r3, #O_ENDBYTE]
    add     r2, r2, r0, lsr #3
    str     r2, [r3, #O_PTR]
    and     r0, r0, #7
    str     r0, [r3, #O_ENDBIT]
    cmp     r10, #0
    bne     11b

17: @ the chunk is done
    ldr     r3, [sp, #F_N]
    ldr     r12, [sp, #F_CHUNK]
    sub     r3, r3, r12, lsl #(\dim / 4)  @ n -= chunk * dim / 2
    str     r3, [sp, #F_N]
    b       10b

18: mvn     r0, #0
    b       20f
19: mov     r0, #0
20: add     sp, sp, #FRAME
    ldmpc   regs=r4-r11

5:  @ refill
    ldr     r12, [sp, #F_MAXL]
    ldr     r2, [sp, #F_BIT]
    ldr     r3, [sp, #F_BITEND]
    add     r1, r1, r12               @ cachesize
    sub     r2, r2, r1                @ the first bit not yet consumed
    sub     r3, r3, #32
    cmp     r2, r3
    bhs     4b                        @ too near the end; F_BIT unchanged
    ldr     lr, [sp, #F_PTR]
    mov     r3, r2, lsr #5
    add     lr, lr, r3, lsl #2
    ldr     r0, [lr]
    ands    r3, r2, #31
    movne   r0, r0, lsr r3
    rsbne   r3, r3, #32
    ldrne   lr, [lr, #4]
    orrne   r0, r0, lr, lsl r3
    add     r2, r2, #32
    str     r2, [sp, #F_BIT]
    rsb     r1, r12, #32              @ a full cache
    b       2b

7:  @ the first table misses: bisect the codelist, as bisect_codelist()
    stmfd   sp!, { r4-r7 }
    add     r12, r12, #1              @ the hint itself
    ldr     r4, [sp, #16 + F_USED]
    ldr     r5, [sp, #16 + F_CODELIST]
    mov     r2, r12, lsl #2
    mov     r2, r2, lsr #17           @ lo = (hint >> 15) & 0x7fff
    mov     r3, r12, lsl #17
    sub     r3, r4, r3, lsr #17       @ hi = used_entries - (hint & 0x7fff)
    mov     r6, r0, ror #16           @ the cache bit-reversed, into r6
    mov     r7, #0xff
    orr     r7, r7, #0xff0000
    and     r4, r7, r6, lsr #8
    eor     r6, r6, r4, lsl #8
    orr     r6, r4, r6, lsl #8
    eor     r7, r7, r7, lsl #4
    and     r4, r7, r6, lsr #4
    eor     r6, r6, r4, lsl #4
    orr     r6, r4, r6, lsl #4
    eor     r7, r7, r7, lsl #2
    and     r4, r7, r6, lsr #2
    eor     r6, r6, r4, lsl #2
    orr     r6, r4, r6, lsl #2
    eor     r7, r7, r7, lsl #1
    and     r4, r7, r6, lsr #1
    eor     r6, r6, r4, lsl #1
    orr     r6, r4, r6, lsl #1
8:  sub     r4, r3, r2
    mov     r4, r4, asr #1
    cmp     r4, #0
    ble     9f
    add     r7, r2, r4
    ldr     r7, [r5, r7, lsl #2]
    cmp     r7, r6
    subhi   r3, r3, r4
    addls   r2, r2, r4
    b       8b
9:  mov     r12, r2
    ldmfd   sp!, { r4-r7 }
    b       3b
.endm

    VV2 vorbis_vv2_d2_lsl_arm, 2, lsl
    VV2 vorbis_vv2_d2_asr_arm, 2, asr
    VV2 vorbis_vv2_d4_lsl_arm, 4, lsl
    VV2 vorbis_vv2_d4_asr_arm, 4, asr
