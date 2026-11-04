/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade: present's conversion of one screen row, ARMv4 so one copy serves
 * both players. It is arcade_present_rows()'s inner loop in present.c,
 * eight source pixels a pass, each of the two panel rows written with one
 * store-multiple; that C stays for the simulator and the host, and is the
 * reference this is checked against.
 ****************************************************************************/
    .syntax unified
    .section .text
    .align  2

/* void present_row_arm(const uint8_t *src, uint32_t *d0, uint32_t *d1,
 *                      const uint32_t lut2[256], int n)
 *
 * n groups of eight pixels from src, word-aligned, each looked up in lut2
 * and stored to both d0 and d1.
 *
 * r0 src, r1 d0, r2 d1, r3 lut2, ip n, lr four source pixels, r4-r11 the
 * eight converted pixels */
    .global present_row_arm
    .type   present_row_arm, %function
present_row_arm:
    stmfd   sp!, {r4-r11, lr}
    ldr     ip, [sp, #36]
1:
    ldr     lr, [r0], #4
    and     r4, lr, #0xff
    and     r5, lr, #0xff00
    and     r6, lr, #0xff0000
    mov     r7, lr, lsr #24
    ldr     r4, [r3, r4, lsl #2]
    ldr     r5, [r3, r5, lsr #6]
    ldr     r6, [r3, r6, lsr #14]
    ldr     r7, [r3, r7, lsl #2]
    ldr     lr, [r0], #4
    and     r8, lr, #0xff
    and     r9, lr, #0xff00
    and     r10, lr, #0xff0000
    mov     r11, lr, lsr #24
    ldr     r8, [r3, r8, lsl #2]
    ldr     r9, [r3, r9, lsr #6]
    ldr     r10, [r3, r10, lsr #14]
    ldr     r11, [r3, r11, lsl #2]
    stmia   r1!, {r4-r11}
    stmia   r2!, {r4-r11}
    subs    ip, ip, #1
    bne     1b
    ldmfd   sp!, {r4-r11, pc}
    .size   present_row_arm, . - present_row_arm
