/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * inflate_fast()'s loop for ARM (ARMv4, both players): literals and whole
 * matches into the window, with the decoder's state in registers. The C in
 * inflate.c stays for the simulator and is the reference this is checked
 * against; it stops where that loop stops, and leaves the same state.
 ****************************************************************************/
#include "config.h"

    .syntax unified
    .section .text
    .align  2

/* struct inflate_fast_regs, in inflate.c */
#define F_LBASE   36
#define F_LEXT    40
#define F_DBASE   44
#define F_DEXT    48

#define BS_ERR_FORMAT (-3)

/* Bits in bb until there are at least 25, a byte at a time: at most 4
 * bytes, and two of these a turn, inside the 8 inflate_fast() leaves past
 * in_end */
.macro refill t
1:
    cmp     r1, #24
    ldrble  \t, [r2], #1
    orrle   r0, r0, \t, lsl r1
    addle   r1, r1, #8
    ble     1b
.endm

/* int inflate_fast_arm(struct inflate_fast_regs *f)
 *
 * r0 bb, r1 bc, r2 in, r3 in_end, r4 the length table, r5 the distance
 * table, r6 win, r7 wp, r8 end, r9 the window mask; r10-r12, lr scratch.
 * The struct's pointer is at [sp]. BS_OK, or BS_ERR_FORMAT. */
    .global inflate_fast_arm
    .type   inflate_fast_arm, %function
inflate_fast_arm:
    stmfd   sp!, {r4-r11, lr}
    stmfd   sp!, {r0}
    mov     r12, r0
    ldmia   r12, {r0-r8}
    ldr     r9, =0x7fff
loop:
    subs    r10, r8, r7
    bmi     done
    cmp     r2, r3
    bhi     done
    refill  r10
    /* the length/literal table, by the next 9 bits */
    mov     r10, r0, lsl #23
    mov     r10, r10, lsr #22
    ldrh    r10, [r4, r10]
    cmp     r10, #0
    beq     done
    and     r11, r10, #15
    mov     r10, r10, lsr #4
    cmp     r10, #256
    bhs     match
    mov     r0, r0, lsr r11
    sub     r1, r1, r11
    and     r12, r7, r9
    strb    r10, [r6, r12]
    add     r7, r7, #1
    b       loop

match:
    /* r10 symbol, r11 its code's length. 256 ends the block: leave it */
    sub     r10, r10, #256
    subs    r10, r10, #1
    bmi     done
    cmp     r10, #29
    bhs     done
    ldr     r12, [sp]
    ldr     r12, [r12, #F_LEXT]
    add     r12, r12, r10, lsl #1
    ldrh    r12, [r12]                  /* le */
    /* the length: its base plus le bits above the code */
    mov     lr, #1
    mov     lr, lr, lsl r12
    sub     lr, lr, #1
    and     lr, lr, r0, lsr r11
    add     r11, r11, r12               /* l + le */
    ldr     r12, [sp]
    ldr     r12, [r12, #F_LBASE]
    add     r12, r12, r10, lsl #1
    ldrh    r12, [r12]
    add     r10, r12, lr                /* len */
    /* the distance code, looked at before anything is taken, so a long
     * one stops the loop with nothing consumed */
    mov     lr, r0, lsr r11
    mov     lr, lr, lsl #23
    mov     lr, lr, lsr #22
    ldrh    lr, [r5, lr]
    cmp     lr, #0
    beq     done
    and     r12, lr, #15                /* dl */
    mov     lr, lr, lsr #4              /* d */
    cmp     lr, #30
    bhs     bad
    add     r11, r11, r12
    mov     r0, r0, lsr r11
    sub     r1, r1, r11
    refill  r11
    ldr     r12, [sp]
    ldr     r11, [r12, #F_DEXT]
    add     r11, r11, lr, lsl #1
    ldrh    r11, [r11]                  /* de */
    ldr     r12, [r12, #F_DBASE]
    add     r12, r12, lr, lsl #1
    ldrh    lr, [r12]
    mov     r12, #1
    mov     r12, r12, lsl r11
    sub     r12, r12, #1
    and     r12, r12, r0
    add     lr, lr, r12                 /* dist */
    mov     r0, r0, lsr r11
    sub     r1, r1, r11
    cmp     lr, r7
    bhi     bad
    /* the copy: r11 from, r12 to, r10 len */
    sub     r11, r7, lr
    and     r11, r11, r9
    and     r12, r7, r9
    add     r7, r7, r10
    add     lr, r12, r10
    cmp     lr, #0x8000
    bhi     wrap
    add     lr, r11, r10
    cmp     lr, #0x8000
    bhi     wrap
    add     r12, r6, r12
    add     r11, r6, r11
    /* A match long enough to repay a call, and behind the bytes it
     * writes, goes to memcpy() or memset(), which move words: whole if it
     * is clear of them; a byte repeated, if it is one back; otherwise in
     * chunks from its start, each as long as the gap so far, which doubles
     * -- the bytes repeat with the distance as their period, so this is
     * the byte loop's result */
    cmp     r10, #16
    blo     bytes
    cmp     r11, r12
    bhs     bytes
    sub     lr, r12, r11
    cmp     lr, r10
    bhs     whole
    cmp     lr, #1
    beq     run
    stmfd   sp!, {r0-r3}
    mov     r9, r12
1:
    sub     r2, r9, r11
    cmp     r2, r10
    movhi   r2, r10
    sub     r10, r10, r2
    mov     r0, r9
    mov     r1, r11
    add     r9, r9, r2
    bl      memcpy
    cmp     r10, #0
    bne     1b
    ldmfd   sp!, {r0-r3}
    ldr     r9, =0x7fff
    b       loop
run:
    stmfd   sp!, {r0-r3}
    mov     r0, r12
    ldrb    r1, [r11]
    mov     r2, r10
    bl      memset
    ldmfd   sp!, {r0-r3}
    b       loop
whole:
    stmfd   sp!, {r0-r3}
    mov     r0, r12
    mov     r1, r11
    mov     r2, r10
    bl      memcpy
    ldmfd   sp!, {r0-r3}
    b       loop
bytes:
    /* forwards a byte at a time, so a match that overlaps the bytes it is
     * writing repeats them */
    ldrb    lr, [r11], #1
    strb    lr, [r12], #1
    subs    r10, r10, #1
    bne     bytes
    b       loop
wrap:
    ldrb    lr, [r6, r11]
    strb    lr, [r6, r12]
    add     r11, r11, #1
    and     r11, r11, r9
    add     r12, r12, #1
    and     r12, r12, r9
    subs    r10, r10, #1
    bne     wrap
    b       loop

bad:
    mvn     r10, #(-BS_ERR_FORMAT - 1)
    b       out
done:
    mov     r10, #0
out:
    ldmfd   sp!, {r11}
    stmia   r11, {r0-r2}
    str     r7, [r11, #28]
    mov     r0, r10
    ldmfd   sp!, {r4-r11, pc}
    .size   inflate_fast_arm, . - inflate_fast_arm
    .ltorg
