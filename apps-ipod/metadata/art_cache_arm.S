/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * ARM inner loop of the art cache's thumbnail reader, ARMv4 so one copy
 * serves both players. It is the C aat_avg_row() in art_cache.c with its sums
 * in registers; the C stays for the simulator and is the reference this is
 * checked against. RGB565 only, as both players are.
 *
 *   aat_avg_row - one output row of the area-average downscale
 ****************************************************************************/
#include "config.h"

    .syntax unified
    .section .text
    .align  2

/* locals below the saved registers */
#define L_M       0     /* this pixel's reciprocal */
#define L_ROWS    4
#define L_RECIP   8
#define L_OUTEND  12
#define L_SIZE    16

/* void aat_avg_row(fb_data *out, const fb_data *src, int sw, int rows,
 *                  const unsigned short *sx, int dw, const uint32_t *recip)
 *
 * Each of the dw pixels of out averages `rows` rows of src, sw pixels apart,
 * across columns [sx[x], sx[x + 1]): every channel expanded to 8 bits as
 * RGB_UNPACK_* does, summed, times recip[n] >> 31 for n pixels, repacked.
 * The two parts of each expansion occupy separate bits, so adding both
 * parts sums the OR that RGB_UNPACK_* forms.
 *
 * r0 out, r1 src, r2 row stride in bytes, r4 sx, r7 the row being summed,
 * r8 the span in bytes, r9-r11 the red, green and blue sums */
    .global aat_avg_row
    .type   aat_avg_row, %function
aat_avg_row:
    stmfd   sp!, {r4-r11, lr}
    ldr     r4, [sp, #36]           /* sx */
    ldr     r5, [sp, #40]           /* dw */
    ldr     r6, [sp, #44]           /* recip */
    sub     sp, sp, #L_SIZE
    str     r3, [sp, #L_ROWS]
    str     r6, [sp, #L_RECIP]
    add     r5, r0, r5, lsl #1
    str     r5, [sp, #L_OUTEND]
    mov     r2, r2, lsl #1
    cmp     r0, r5
    bhs     4f

1:  /* one output pixel */
    ldrh    r7, [r4], #2            /* sx0 */
    ldrh    r8, [r4]                /* sx1, the next pixel's sx0 */
    sub     r8, r8, r7
    ldr     r3, [sp, #L_ROWS]
    mul     ip, r8, r3              /* n */
    ldr     r6, [sp, #L_RECIP]
    ldr     ip, [r6, ip, lsl #2]
    str     ip, [sp, #L_M]
    add     r7, r1, r7, lsl #1
    mov     r8, r8, lsl #1
    mov     r9, #0
    mov     r10, #0
    mov     r11, #0

2:  /* one source row of it */
    mov     r5, r7
    add     r6, r7, r8
3:
    ldrh    ip, [r5], #2
    and     lr, ip, #0xf800
    add     r9, r9, lr, lsr #8      /* red: (p >> 8) & 0xf8 */
    add     r9, r9, ip, lsr #13     /*      | p >> 13 */
    and     lr, ip, #0x07e0
    add     r10, r10, lr, lsr #3    /* green: (p >> 3) & 0xfc */
    and     lr, ip, #0x0600
    add     r10, r10, lr, lsr #9    /*        | (p >> 9) & 3 */
    and     lr, ip, #0x001f
    add     r11, r11, lr, lsl #3    /* blue: (p << 3) & 0xf8 */
    and     lr, ip, #0x001c
    add     r11, r11, lr, lsr #2    /*       | (p >> 2) & 7 */
    cmp     r5, r6
    blo     3b
    add     r7, r7, r2
    subs    r3, r3, #1
    bne     2b

    /* each sum * m >> 31, and packed as LCD_RGBPACK */
    ldr     ip, [sp, #L_M]
    umull   r5, r6, r9, ip
    mov     r9, r6, lsl #1
    orr     r9, r9, r5, lsr #31
    umull   r5, r6, r10, ip
    mov     r10, r6, lsl #1
    orr     r10, r10, r5, lsr #31
    umull   r5, r6, r11, ip
    mov     r11, r6, lsl #1
    orr     r11, r11, r5, lsr #31
    mov     ip, r11, lsr #3
    mov     r10, r10, lsr #2
    orr     ip, ip, r10, lsl #5
    mov     r9, r9, lsr #3
    orr     ip, ip, r9, lsl #11
    strh    ip, [r0], #2
    ldr     lr, [sp, #L_OUTEND]
    cmp     r0, lr
    blo     1b

4:
    add     sp, sp, #L_SIZE
    ldmfd   sp!, {r4-r11, pc}
    .size   aat_avg_row, .-aat_avg_row
