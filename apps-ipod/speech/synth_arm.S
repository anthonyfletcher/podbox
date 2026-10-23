/***************************************************************************
 * GNU General Public License (version 2 only)
 * Port of NV Speech Player, Copyright (C) 2014 NV Access Limited
 *
 * synth.c's inner loops in ARM assembly, with the C versions' output sample
 * for sample. ARMv4 and up; one version serves both players.
 *
 * The block filters run one resonator over n samples (n >= 1) with its
 * coefficients and state in registers, two samples per pass so that the
 * state registers swap roles instead of being moved. The accumulator starts
 * at the rounding constant, and the signal goes in smlal's Rs, where the
 * ARM7's multiplier finishes early on the smaller value.
 *
 * struct resonator begins a, b, c, p1, p2: offsets 0 to 16. struct
 * source_args is laid out at the SA_ offsets below.
 ****************************************************************************/

    .syntax unified
    .section .text
    .align  2

/* rd = (rx * ry) >> 16, as synth.c's mul16(). t1, t2 and rx must differ. */
.macro MUL16 rd, rx, ry, t1, t2
    smull   \t1, \t2, \rx, \ry
    mov     \t2, \t2, lsl #16
    orr     \rd, \t2, \t1, lsr #16
.endm

/* void synth_source_block(struct source_args *a, int n,
 *                         int32_t *cascade, int32_t *parallel)
 * synth.c's source_block() without vibrato.
 * r0 a, r1 samples left, r2 r3 the outputs; r4 pitch, r5 the pitch phase,
 * r6 the noise seed, r7 the aspiration noise, r8 the frication noise;
 * r9-r12 and lr scratch. */
    .equ    SA_PITCH, 0
    .equ    SA_PITCH_STEP, 4
    .equ    SA_PHASE_PER_HZ, 8
    .equ    SA_OPEN, 12
    .equ    SA_TURB_AMP, 16
    .equ    SA_VOICE_AMP, 20
    .equ    SA_ASP_AMP, 24
    .equ    SA_FRIC_AMP, 28
    .equ    SA_GAIN, 32
    .equ    SA_PHASE, 36
    .equ    SA_ASP, 40
    .equ    SA_SEED, 44

    .global synth_source_block
    .type   synth_source_block, %function
synth_source_block:
    stmfd   sp!, { r4-r11, lr }
    ldr     r4, [r0, #SA_PITCH]
    ldr     r5, [r0, #SA_PHASE]
    ldr     r6, [r0, #SA_SEED]
    ldr     r7, [r0, #SA_ASP]
.Lsrc_loop:
    ldr     r9, [r0, #SA_PHASE_PER_HZ]
    MUL16   r9, r4, r9, r10, r11
    add     r5, r5, r9                  @ the pitch phase steps

    ldr     r9, .Lsrc_lcg_a
    ldr     r10, .Lsrc_lcg_c
    mla     r6, r9, r6, r10
    add     r11, r7, r7, lsl #1
    mov     r7, r6, lsr #14
    add     r7, r7, r11, asr #2         @ aspiration noise
    ldr     r9, .Lsrc_k_0_2
    MUL16   r12, r7, r9, r10, r11       @ r12 aspiration
    @ An amplitude of 0 makes its product 0: skip the multiply. The noise
    @ above has already stepped, as it must.
    ldr     r9, [r0, #SA_TURB_AMP]
    movs    lr, r9                      @ lr turbulence
    beq     1f
    MUL16   lr, r12, r9, r10, r11
    ldr     r10, [r0, #SA_OPEN]
    mov     r9, r5, lsr #14
    cmp     r9, r10
    bge     1f
    ldr     r9, .Lsrc_k_0_01
    MUL16   lr, lr, r9, r10, r11        @ glottis closed
1:
    mov     r9, r5, lsr #14
    add     r9, r9, r9
    sub     r9, r9, #0x40000
    add     r9, r9, lr                  @ voice
    ldr     r10, [r0, #SA_VOICE_AMP]
    MUL16   r9, r9, r10, r11, lr
    ldr     r10, [r0, #SA_ASP_AMP]
    cmp     r10, #0
    beq     2f
    MUL16   r12, r12, r10, r11, lr
    add     r9, r9, r12
2:
    ldr     r10, [r0, #SA_GAIN]
    MUL16   r9, r9, r10, r11, lr
    mov     r9, r9, asr #1
    str     r9, [r2], #4

    ldr     r9, .Lsrc_lcg_a
    ldr     r10, .Lsrc_lcg_c
    mla     r6, r9, r6, r10
    mov     r8, r6, lsr #14
    sub     r8, r8, #0x20000            @ frication noise, white
    ldr     r9, [r0, #SA_FRIC_AMP]
    movs    r12, r9
    beq     3f
    ldr     r12, .Lsrc_k_0_75
    MUL16   r12, r8, r12, r10, r11
    MUL16   r12, r12, r9, r10, r11
    ldr     r9, [r0, #SA_GAIN]
    MUL16   r12, r12, r9, r10, r11
    mov     r12, r12, asr #1
3:
    str     r12, [r3], #4

    ldr     r9, [r0, #SA_PITCH_STEP]
    add     r4, r4, r9
    subs    r1, r1, #1
    bgt     .Lsrc_loop

    str     r5, [r0, #SA_PHASE]
    str     r6, [r0, #SA_SEED]
    str     r7, [r0, #SA_ASP]
    ldmfd   sp!, { r4-r11, pc }

.Lsrc_lcg_a:    .word   1103515245
.Lsrc_lcg_c:    .word   12345
.Lsrc_k_0_2:    .word   13107
.Lsrc_k_0_01:   .word   655
.Lsrc_k_0_75:   .word   49152
    .size   synth_source_block, .-synth_source_block

/* void synth_resonate_block(struct resonator *r, const int32_t *in,
 *                           int32_t *out, int n)
 * out = (a.in + b.p1 + c.p2 + 2^27) >> 28; p2 = p1; p1 = out.
 * r0 r, r1 in, r2 out, r3 samples left; r4-r6 a b c; r7 r8 the last two
 * outputs; r9 r10 the accumulator; r11 x; r12 the rounding constant. */
    .global synth_resonate_block
    .type   synth_resonate_block, %function
synth_resonate_block:
    stmfd   sp!, { r4-r11, lr }
    ldmia   r0, { r4-r8 }
    mov     r12, #0x08000000
    subs    r3, r3, #2
    blt     .Lres_tail
.Lres_pair:
    ldr     r11, [r1], #4
    mov     r9, r12
    mov     r10, #0
    smlal   r9, r10, r4, r11
    smlal   r9, r10, r5, r7
    smlal   r9, r10, r6, r8
    mov     r9, r9, lsr #28
    orr     r8, r9, r10, lsl #4     @ p1 is now r8, p2 r7
    str     r8, [r2], #4
    ldr     r11, [r1], #4
    mov     r9, r12
    mov     r10, #0
    smlal   r9, r10, r4, r11
    smlal   r9, r10, r5, r8
    smlal   r9, r10, r6, r7
    mov     r9, r9, lsr #28
    orr     r7, r9, r10, lsl #4     @ and back: p1 r7, p2 r8
    str     r7, [r2], #4
    subs    r3, r3, #2
    bge     .Lres_pair
.Lres_tail:
    adds    r3, r3, #2
    beq     .Lres_done
    ldr     r11, [r1]
    mov     r9, r12
    mov     r10, #0
    smlal   r9, r10, r4, r11
    smlal   r9, r10, r5, r7
    smlal   r9, r10, r6, r8
    mov     r9, r9, lsr #28
    orr     r8, r9, r10, lsl #4
    str     r8, [r2]
    str     r8, [r0, #12]
    str     r7, [r0, #16]
    ldmfd   sp!, { r4-r11, pc }
.Lres_done:
    str     r7, [r0, #12]
    str     r8, [r0, #16]
    ldmfd   sp!, { r4-r11, pc }
    .size   synth_resonate_block, .-synth_resonate_block

/* void synth_antiresonate_block(struct resonator *r, const int32_t *in,
 *                               int32_t *out, int n)
 * out = (a.in + b.p1 + c.p2 + 2^19) >> 20; p2 = p1; p1 = in. Q20
 * coefficients: the antiresonator with a frequency other than 0.
 * Registers as above, but r7 r8 hold the last two inputs. */
    .global synth_antiresonate_block
    .type   synth_antiresonate_block, %function
synth_antiresonate_block:
    stmfd   sp!, { r4-r11, lr }
    ldmia   r0, { r4-r8 }
    mov     r12, #0x00080000
    subs    r3, r3, #2
    blt     .Lanti_tail
.Lanti_pair:
    ldr     r11, [r1], #4
    mov     r9, r12
    mov     r10, #0
    smlal   r9, r10, r4, r11
    smlal   r9, r10, r5, r7
    smlal   r9, r10, r6, r8
    mov     r9, r9, lsr #20
    orr     r9, r9, r10, lsl #12
    str     r9, [r2], #4
    mov     r8, r11                 @ p1 is now r8, p2 r7
    ldr     r11, [r1], #4
    mov     r9, r12
    mov     r10, #0
    smlal   r9, r10, r4, r11
    smlal   r9, r10, r5, r8
    smlal   r9, r10, r6, r7
    mov     r9, r9, lsr #20
    orr     r9, r9, r10, lsl #12
    str     r9, [r2], #4
    mov     r7, r11                 @ and back: p1 r7, p2 r8
    subs    r3, r3, #2
    bge     .Lanti_pair
.Lanti_tail:
    adds    r3, r3, #2
    beq     .Lanti_done
    ldr     r11, [r1]
    mov     r9, r12
    mov     r10, #0
    smlal   r9, r10, r4, r11
    smlal   r9, r10, r5, r7
    smlal   r9, r10, r6, r8
    mov     r9, r9, lsr #20
    orr     r9, r9, r10, lsl #12
    str     r9, [r2]
    str     r11, [r0, #12]
    str     r7, [r0, #16]
    ldmfd   sp!, { r4-r11, pc }
.Lanti_done:
    str     r7, [r0, #12]
    str     r8, [r0, #16]
    ldmfd   sp!, { r4-r11, pc }
    .size   synth_antiresonate_block, .-synth_antiresonate_block

/* One resonator sample: y = (a.x + b.p1 + c.p2 + 2^27) >> 28 into p2's
 * register, which becomes p1 (the caller swaps the names). x may be lo. */
.macro RES1 a, b, c, x, p1, p2, lo, hi
    smull   \lo, \hi, \a, \x
    smlal   \lo, \hi, \b, \p1
    smlal   \lo, \hi, \c, \p2
    adds    \lo, \lo, #0x08000000
    adc     \hi, \hi, #0
    mov     \lo, \lo, lsr #28
    orr     \p2, \lo, \hi, lsl #4
.endm

/* void synth_resonate_pair_block(struct resonator *r1,
 *                                struct resonator *r2, int32_t *buf, int n)
 * buf through r1 then r2, in place: two of the cascade's resonators in one
 * pass, every register in use.
 * r0 buf, r1 samples left; r2-r6 r1's a b c p1 p2, r7-r11 r2's; r12 lr the
 * accumulator, r12 also the input. [sp] [sp, #4] the two structs. */
    .global synth_resonate_pair_block
    .type   synth_resonate_pair_block, %function
synth_resonate_pair_block:
    stmfd   sp!, { r4-r11, lr }
    stmfd   sp!, { r0, r1 }
    ldmia   r1, { r7-r11 }
    ldmia   r0, { r4-r6, r12, lr }
    mov     r0, r2
    mov     r1, r3
    mov     r2, r4                      @ r1's a b c p1 p2 into r2-r6
    mov     r3, r5
    mov     r4, r6
    mov     r5, r12
    mov     r6, lr
    subs    r1, r1, #2
    blt     .Lpair_tail
.Lpair_loop:
    ldr     r12, [r0]
    RES1    r2, r3, r4, r12, r5, r6, r12, lr    @ r1: p1 now r6, p2 r5
    RES1    r7, r8, r9, r6, r10, r11, r12, lr   @ r2: p1 now r11, p2 r10
    str     r11, [r0], #4
    ldr     r12, [r0]
    RES1    r2, r3, r4, r12, r6, r5, r12, lr    @ back: p1 r5, p2 r6
    RES1    r7, r8, r9, r5, r11, r10, r12, lr   @ back: p1 r10, p2 r11
    str     r10, [r0], #4
    subs    r1, r1, #2
    bge     .Lpair_loop
.Lpair_tail:
    adds    r1, r1, #2
    beq     .Lpair_done
    ldr     r12, [r0]
    RES1    r2, r3, r4, r12, r5, r6, r12, lr
    RES1    r7, r8, r9, r6, r10, r11, r12, lr
    str     r11, [r0]
    ldmfd   sp!, { r0, r1 }
    str     r6, [r0, #12]               @ r1: p1 p2
    str     r5, [r0, #16]
    str     r11, [r1, #12]              @ r2: p1 p2
    str     r10, [r1, #16]
    ldmfd   sp!, { r4-r11, pc }
.Lpair_done:
    ldmfd   sp!, { r0, r1 }
    add     r0, r0, #12
    stmia   r0, { r5, r6 }
    add     r1, r1, #12
    stmia   r1, { r10, r11 }
    ldmfd   sp!, { r4-r11, pc }
    .size   synth_resonate_pair_block, .-synth_resonate_pair_block

/* void synth_parallel_formant_block(struct resonator *r, const int32_t *in,
 *                                   int32_t *sum, int n, int32_t pa)
 * One parallel resonator over in, adding (y - in) * pa >> 16 to sum: the
 * filter and its share of the bank's output in one pass.
 * r0 r, r1 in, r2 sum, r3 samples left; r4-r8 a b c p1 p2; r9 pa;
 * r10 r11 the accumulator; r12 x; lr scratch. */
    .global synth_parallel_formant_block
    .type   synth_parallel_formant_block, %function
synth_parallel_formant_block:
    stmfd   sp!, { r4-r11, lr }
    ldr     r9, [sp, #36]
    ldmia   r0, { r4-r8 }
    subs    r3, r3, #2
    blt     .Lpf_tail
.Lpf_loop:
    ldr     r12, [r1], #4
    RES1    r4, r5, r6, r12, r7, r8, r10, r11   @ y in r8: p1 r8, p2 r7
    sub     lr, r8, r12
    MUL16   r10, lr, r9, r10, r11
    ldr     lr, [r2]
    add     lr, lr, r10
    str     lr, [r2], #4
    ldr     r12, [r1], #4
    RES1    r4, r5, r6, r12, r8, r7, r10, r11   @ y in r7: p1 r7, p2 r8
    sub     lr, r7, r12
    MUL16   r10, lr, r9, r10, r11
    ldr     lr, [r2]
    add     lr, lr, r10
    str     lr, [r2], #4
    subs    r3, r3, #2
    bge     .Lpf_loop
.Lpf_tail:
    adds    r3, r3, #2
    beq     .Lpf_done
    ldr     r12, [r1]
    RES1    r4, r5, r6, r12, r7, r8, r10, r11
    sub     lr, r8, r12
    MUL16   r10, lr, r9, r10, r11
    ldr     lr, [r2]
    add     lr, lr, r10
    str     lr, [r2]
    str     r8, [r0, #12]
    str     r7, [r0, #16]
    ldmfd   sp!, { r4-r11, pc }
.Lpf_done:
    str     r7, [r0, #12]
    str     r8, [r0, #16]
    ldmfd   sp!, { r4-r11, pc }
    .size   synth_parallel_formant_block, .-synth_parallel_formant_block
