/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * ARM inner loops for the WebP reader (webp_load.c), ARMv4 so one copy
 * serves both players. Each is the C beside it with its state in
 * registers; the C stays for the simulator and is the reference these are
 * checked against.
 *
 *   webp_edge_mb_arm/inner_arm - the normal loop filter along one edge
 *   webp_yuv_row_arm           - a row of pixel pairs from YUV to RGB
 ****************************************************************************/
#include "config.h"

    .syntax unified
    .section .text
    .align  2

/* struct edge_limits, in webp_load.c */
#define EL_T     0
#define EL_IT    4
#define EL_HEV   8

/* x to [lo, hi] for the signed clips: [-128, 127] and [-16, 15] */
.macro clamp x, hi, nlo
    cmp     \x, #\hi
    movgt   \x, #\hi
    cmn     \x, #\nlo
    mvnlt   \x, #(\nlo - 1)
.endm

/* x to 0-255 in its low byte, which is all strb stores */
.macro clip8 x
    cmp     \x, #255
    mvnhi   \x, \x, asr #31
.endm

/* |a - b| into d, setting nothing else */
.macro absdiff d, a, b
    subs    \d, \a, \b
    rsbmi   \d, \d, #0
.endm

/* void webp_edge_xxx_arm(uint8_t *p, int hstride, int vstride, int size,
 *                        const struct edge_limits *l)
 *
 * `size` pixels along an edge, `hstride` across it: each where the edge
 * passes the filter's tests, two taps with high edge variance, otherwise
 * six on a macroblock's edge and four inside one.
 *
 * r0 p, r1 hstride, r2 vstride, r3 size, r4 t, r5 interior limit, r6 hev
 * threshold, r8-r11 p1 p0 q0 q1, r7 r12 lr scratch */
.macro edge name, mb
    .global \name
    .type   \name, %function
\name:
    stmfd   sp!, {r4-r11, lr}
    ldr     r12, [sp, #36]
    ldmia   r12, {r4-r6}
    cmp     r3, #0
    ble     9f
1:
    sub     r12, r0, r1, lsl #1
    ldrb    r8, [r12]
    ldrb    r9, [r0, -r1]
    ldrb    r10, [r0]
    ldrb    r11, [r0, r1]
    absdiff r7, r9, r10
    absdiff lr, r8, r11
    add     r7, lr, r7, lsl #2
    cmp     r7, r4
    bgt     8f
    absdiff r7, r8, r9
    cmp     r7, r5
    bgt     8f
    absdiff lr, r11, r10
    cmp     lr, r5
    bgt     8f
    cmp     r7, r6
    cmple   lr, r6
    movgt   r7, #1                      /* high edge variance */
    movle   r7, #0
    ldrb    lr, [r12, -r1]              /* p2 */
    ldrb    r12, [r0, -r1, lsl #2]      /* p3 */
    absdiff r12, r12, lr
    cmp     r12, r5
    bgt     8f
    absdiff r12, lr, r8
    cmp     r12, r5
    bgt     8f
    ldrb    lr, [r0, r1, lsl #1]        /* q2 */
    add     r12, r0, r1, lsl #1
    ldrb    r12, [r12, r1]              /* q3 */
    absdiff r12, r12, lr
    cmp     r12, r5
    bgt     8f
    absdiff r12, lr, r11
    cmp     r12, r5
    bgt     8f
    cmp     r7, #0
    bne     5f
.if \mb
    /* six taps: a from both pairs, shared out 27:18:9 */
    sub     r12, r8, r11
    clamp   r12, 127, 128
    sub     lr, r10, r9
    add     lr, lr, lr, lsl #1
    add     r12, r12, lr
    clamp   r12, 127, 128
    rsb     lr, r12, r12, lsl #5
    sub     lr, lr, r12, lsl #2         /* 27a */
    add     lr, lr, #63
    mov     lr, lr, asr #7
    add     r9, r9, lr
    sub     r10, r10, lr
    clip8   r9
    clip8   r10
    strb    r9, [r0, -r1]
    strb    r10, [r0]
    add     r7, r12, r12, lsl #3        /* 9a */
    add     lr, r7, r7                  /* 18a */
    add     lr, lr, #63
    mov     lr, lr, asr #7
    add     r8, r8, lr
    sub     r11, r11, lr
    clip8   r8
    clip8   r11
    sub     r12, r0, r1, lsl #1
    strb    r8, [r12]
    strb    r11, [r0, r1]
    add     r7, r7, #63
    mov     r7, r7, asr #7
    ldrb    lr, [r12, -r1]
    add     lr, lr, r7
    clip8   lr
    strb    lr, [r12, -r1]
    ldrb    lr, [r0, r1, lsl #1]
    sub     lr, lr, r7
    clip8   lr
    strb    lr, [r0, r1, lsl #1]
.else
    /* four taps: a from the inner pair only */
    sub     lr, r10, r9
    add     lr, lr, lr, lsl #1
    add     r12, lr, #4
    mov     r12, r12, asr #3
    clamp   r12, 15, 16                 /* a1 */
    add     lr, lr, #3
    mov     lr, lr, asr #3
    clamp   lr, 15, 16                  /* a2 */
    add     r9, r9, lr
    sub     r10, r10, r12
    clip8   r9
    clip8   r10
    strb    r9, [r0, -r1]
    strb    r10, [r0]
    add     r7, r12, #1
    mov     r7, r7, asr #1              /* a3 */
    add     r8, r8, r7
    sub     r11, r11, r7
    clip8   r8
    clip8   r11
    sub     r12, r0, r1, lsl #1
    strb    r8, [r12]
    strb    r11, [r0, r1]
.endif
    b       8f
5:  /* two taps */
    sub     r12, r8, r11
    clamp   r12, 127, 128
    sub     lr, r10, r9
    add     lr, lr, lr, lsl #1
    add     r12, r12, lr
    add     lr, r12, #4
    mov     lr, lr, asr #3
    clamp   lr, 15, 16                  /* a1 */
    add     r12, r12, #3
    mov     r12, r12, asr #3
    clamp   r12, 15, 16                 /* a2 */
    add     r9, r9, r12
    sub     r10, r10, lr
    clip8   r9
    clip8   r10
    strb    r9, [r0, -r1]
    strb    r10, [r0]
8:
    add     r0, r0, r2
    subs    r3, r3, #1
    bgt     1b
9:
    ldmfd   sp!, {r4-r11, pc}
    .size   \name, . - \name
.endm

    edge    webp_edge_mb_arm, 1
    edge    webp_edge_inner_arm, 0

/* A term with six fraction bits to 0-0x3fff, its byte above them */
.macro clip6 x
    cmp     \x, #0x4000
    mvnhs   \x, \x, asr #31
    movhs   \x, \x, lsr #18
.endm

/* One pixel: luma r12 to its term, the three channels into a word */
.macro yuv_px
    ldrb    r12, [r1], #1
    add     r12, r5, r12, lsl #1
    ldrsh   r12, [r12]
    mov     r11, #0xff000000
    add     lr, r12, r10
    clip6   lr
    orr     r11, r11, lr, lsr #6
    add     lr, r12, r9
    clip6   lr
    bic     lr, lr, #0x3f
    orr     r11, r11, lr, lsl #2
    add     lr, r12, r8
    clip6   lr
    bic     lr, lr, #0x3f
    orr     r11, r11, lr, lsl #10
    str     r11, [r0], #4
.endm

/* void webp_yuv_row_arm(uint32_t *out, const uint8_t *y, const uint8_t *u,
 *                       const uint8_t *v, int pairs, const void *tabs[3])
 *
 * Two pixels to each chroma sample. tabs: luma's terms (int16_t), U's
 * (green low, blue high) and V's (green low, red high).
 *
 * r0 out, r1 y, r2 u, r3 v, r4 pairs, r5-r7 the tables, r8-r10 the red,
 * green and blue terms, r11 the pixel, r12 lr scratch */
    .global webp_yuv_row_arm
    .type   webp_yuv_row_arm, %function
webp_yuv_row_arm:
    stmfd   sp!, {r4-r11, lr}
    ldr     r4, [sp, #36]
    ldr     r12, [sp, #40]
    ldmia   r12, {r5-r7}
    cmp     r4, #0
    ble     2f
1:
    ldrb    r12, [r2], #1
    ldrb    lr, [r3], #1
    ldr     r12, [r6, r12, lsl #2]
    ldr     lr, [r7, lr, lsl #2]
    mov     r10, r12, asr #16
    mov     r8, lr, asr #16
    mov     r12, r12, lsl #16
    add     r12, r12, lr, lsl #16
    mov     r9, r12, asr #16
    yuv_px
    yuv_px
    subs    r4, r4, #1
    bgt     1b
2:
    ldmfd   sp!, {r4-r11, pc}
    .size   webp_yuv_row_arm, . - webp_yuv_row_arm
