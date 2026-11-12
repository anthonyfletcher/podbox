/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade: the cells system's step in ARMv4, so one copy serves both
 * players. The C in sys_cells.c stays for the simulator and the host, and
 * is the reference this is checked against: the same cells move the same
 * way, taking the same random bytes in the same order.
 *
 * Parts, in order:
 *   cells_run_arm    run() with idle() and stuck() inside it: each cell
 *                    that is not air and not yet moved is counted, and
 *                    stepped unless nothing round it lets it do anything.
 *                    A cell with only its own material in the six places
 *                    stuck() looks at is stuck without looking up a
 *                    density, when its kind says so (K_SAME): most of a
 *                    pool.
 *   cells_step_cell  one cell's tick, and the swap and set it shares
 ****************************************************************************/
    .syntax unified
    .section .text
    .align  2

/* struct run, sys_cells.c */
    .equ    R_C, 0
    .equ    R_MOVED, 4
    .equ    R_DENS, 8
    .equ    R_W, 12
    .equ    R_H, 16
    .equ    R_ROW, 20
    .equ    R_STOP, 24
    .equ    R_RUNS, 28
    .equ    R_DX, 32
    .equ    R_K, 36
    .equ    R_SPAN, 40
    .equ    R_FIRST, 44
    .equ    R_NEXT, 48
    .equ    R_B, 52
    .equ    R_S, 56
    .equ    R_ROWBASE, 60
    .equ    R_Y, 64
    .equ    R_FROM, 68
    .equ    R_TO, 132

/* K_* in sys_cells.c; a material's kind is 256 bytes before its density */
    .equ    K_MOVES, 1
    .equ    K_FLOWS, 2
    .equ    K_GAS, 4
    .equ    K_REACTS, 8
    .equ    K_DECAYS, 16
    .equ    K_SAME, 32

/* The run one way: dir 1 forward, -1 back.
 *
 * r0 the struct run, r1 the cell's index, r2 the index of x = 1, r3 the
 * groups left above bit 16 and the group's cells left below, r4 the
 * field, r5 the moved bits, r6 the densities, r7 the width, r8 the count.
 * In a cell: r9 its material's density's address, then the row it moves
 * to; r10 its kind; r11 its material, then its density; r12 its address;
 * lr a neighbour. */
.macro  scan dir
    /* a row: its base, x = 1's index (r2) and the span idle may test */
.Lrow\@:
    ldr     r9, [r0, #R_ROW]
    str     r9, [r0, #R_Y]
    mul     r10, r9, r7
    str     r10, [r0, #R_ROWBASE]
    add     r2, r10, #1
    ldr     r11, [r0, #R_H]
    sub     r11, r11, #1
    mov     r12, #0
    cmp     r9, #0
    cmpgt   r11, r9
    cmpgt   r7, #2
    subgt   r12, r7, #2
    str     r12, [r0, #R_SPAN]
    mov     r9, #0
    str     r9, [r0, #R_K]
.Lrun\@:
    /* the row's next run, x0 (r9) to x1 (r11): in groups of a word when
     * it is in whole aligned words, else one group of them all */
    ldr     r9, [r0, #R_K]
    ldr     r10, [r0, #R_RUNS]
    cmp     r9, r10
    bge     .Lnext\@
    add     r11, r0, r9, lsl #2
    add     r9, r9, #1
    str     r9, [r0, #R_K]
    ldr     r9, [r11, #R_FROM]
    ldr     r11, [r11, #R_TO]
    ldr     r10, [r0, #R_ROWBASE]
    sub     r12, r11, r9
    .if \dir < 0
    add     r1, r10, r11
    sub     r1, r1, #1
    .else
    add     r1, r10, r9
    .endif
    add     lr, r4, r10
    add     lr, lr, r9
    orr     lr, lr, r12
    tst     lr, #3
    movne   r3, #0x10000
    orrne   r3, r3, r12
    bne     2f
    mov     r3, r12, lsl #14
    orr     r3, r3, #4
    /* four words of air pass sixteen cells, four groups, while as many
     * are left; then a word passes four; going back, they end here */
1:
    cmp     r3, #(4 << 16)
    blo     6f
    .if \dir < 0
    sub     lr, r1, #15
    .else
    mov     lr, r1
    .endif
    add     lr, r4, lr
    ldmia   lr, {r9-r12}
    orr     r9, r9, r10
    orr     r11, r11, r12
    orrs    r9, r9, r11
    bne     6f
    add     r1, r1, #(16 * \dir)
    sub     r3, r3, #(3 << 16)
    sub     r3, r3, #4
    b       9f
6:
    .if \dir < 0
    sub     r9, r1, #3
    ldr     r9, [r4, r9]
    .else
    ldr     r9, [r4, r1]
    .endif
    cmp     r9, #0
    bne     2f
    add     r1, r1, #(4 * \dir)
    sub     r3, r3, #4
    b       9f
2:
    ldrb    r9, [r4, r1]
    cmp     r9, #0
    beq     8f
    ldrb    r10, [r5, r1, lsr #3]
    and     r11, r1, #7
    mov     r10, r10, lsr r11
    tst     r10, #1
    bne     8f
    add     r8, r8, #1
    add     r9, r6, r9
    ldrb    r10, [r9, #-256]
    /* stone: nothing to do */
    tst     r10, #(K_MOVES | K_REACTS | K_DECAYS)
    beq     8f
    tst     r10, #K_DECAYS
    bne     7f
    /* on the field's edge it is stepped, which looks at nothing off it */
    sub     r11, r1, r2
    ldr     lr, [r0, #R_SPAN]
    cmp     r11, lr
    bhs     7f
    sub     r11, r9, r6
    add     r12, r4, r1
    tst     r10, #K_GAS
    addeq   r9, r12, r7
    subne   r9, r12, r7
    tst     r10, #K_SAME
    beq     3f
    /* its own material all round */
    ldrb    lr, [r9]
    cmp     lr, r11
    bne     3f
    ldrb    lr, [r9, #-1]
    cmp     lr, r11
    bne     3f
    ldrb    lr, [r9, #1]
    cmp     lr, r11
    bne     3f
    ldrb    lr, [r12, #-1]
    cmp     lr, r11
    bne     3f
    ldrb    lr, [r12, #1]
    cmp     lr, r11
    bne     3f
    sub     lr, r12, r9
    ldrb    lr, [r12, lr]
    cmp     lr, r11
    beq     8f
3:
    /* anything lighter below (above, for a gas), or beside if it flows */
    ldrb    r11, [r6, r11]
    tst     r10, #K_MOVES
    beq     4f
    ldrb    lr, [r9]
    ldrb    lr, [r6, lr]
    cmp     lr, r11
    blo     7f
    ldrb    lr, [r9, #-1]
    ldrb    lr, [r6, lr]
    cmp     lr, r11
    blo     7f
    ldrb    lr, [r9, #1]
    ldrb    lr, [r6, lr]
    cmp     lr, r11
    blo     7f
    tst     r10, #K_FLOWS
    beq     4f
    ldrb    lr, [r12, #-1]
    ldrb    lr, [r6, lr]
    cmp     lr, r11
    blo     7f
    ldrb    lr, [r12, #1]
    ldrb    lr, [r6, lr]
    cmp     lr, r11
    blo     7f
4:
    /* a partner beside it, for each of its reactions in turn */
    tst     r10, #K_REACTS
    beq     8f
    ldrb    r9, [r12]
    ldr     r10, [r0, #R_FIRST]
    ldrb    r9, [r10, r9]
5:
    ldr     r10, [r0, #R_B]
    ldrb    r10, [r10, r9]
    ldrb    lr, [r12, #-1]
    cmp     lr, r10
    beq     7f
    ldrb    lr, [r12, #1]
    cmp     lr, r10
    beq     7f
    ldrb    lr, [r12, -r7]
    cmp     lr, r10
    beq     7f
    ldrb    lr, [r12, r7]
    cmp     lr, r10
    beq     7f
    ldr     r10, [r0, #R_NEXT]
    ldrb    r9, [r10, r9]
    cmp     r9, #255
    bne     5b
    b       8f
7:
    stmfd   sp!, {r0-r3}
    ldr     r11, [r0, #R_ROWBASE]
    sub     r1, r1, r11
    ldr     r2, [r0, #R_Y]
    ldr     r0, [r0, #R_S]
    bl      cells_step_cell
    ldmfd   sp!, {r0-r3}
8:
    add     r1, r1, #\dir
    sub     r3, r3, #1
    movs    lr, r3, lsl #16
    bne     2b
9:
    /* the next group, four cells, which come in words */
    subs    r3, r3, #0x10000
    orrne   r3, r3, #4
    bne     1b
    b       .Lrun\@
.Lnext\@:
    /* the row below, down to the band's last */
    ldr     r9, [r0, #R_ROW]
    ldr     r10, [r0, #R_STOP]
    cmp     r9, r10
    sub     r9, r9, #1
    str     r9, [r0, #R_ROW]
    bgt     .Lrow\@
.endm

/* uint32_t cells_run_arm(struct run *r): rows r->row down to r->stop,
 * each its runs; the cells looked at */
    .global cells_run_arm
    .type   cells_run_arm, %function
cells_run_arm:
    stmfd   sp!, {r4-r12, lr}
    ldr     r4, [r0, #R_C]
    ldr     r5, [r0, #R_MOVED]
    ldr     r6, [r0, #R_DENS]
    ldr     r7, [r0, #R_W]
    mov     r8, #0
    ldr     r9, [r0, #R_DX]
    cmp     r9, #0
    blt     .Lback
    scan    1
    b       .Ldone
.Lback:
    scan    -1
.Ldone:
    mov     r0, r8
    ldmfd   sp!, {r4-r12, pc}
    .size   cells_run_arm, . - cells_run_arm

/* struct stepper, sys_cells.c, which checks these */
    .equ    S_C, 0
    .equ    S_W, 4
    .equ    S_H, 8
    .equ    S_MOVE, 16
    .equ    S_SPREAD, 20
    .equ    S_DECAY, 28
    .equ    S_BECOMES, 32
    .equ    S_RB, 44
    .equ    S_RC, 48
    .equ    S_RD, 52
    .equ    S_CHANCE, 56
    .equ    S_MOVED, 60
    .equ    S_DIR, 64
    .equ    S_CHANGED, 68
    .equ    S_CW, 324
    .equ    S_FIRST, 328
    .equ    S_NEXT, 584
    .equ    S_KIND, 648
    .equ    S_DENS, 904
    .equ    S_VM, 1416
    .equ    S_BITS, 1420
    .equ    S_NBITS, 1424
    .equ    S_MOVES, 1428
    .equ    S_STEPS, 1432
    .equ    S_PARTNER, 1440

/* acc op'd with material r0's bit of partner bits r12 (bit 0 of acc is the
 * one that counts); r0 and lr are lost */
.macro  partbit acc, op
    mov     lr, r0, lsr #5
    ldr     lr, [r12, lr, lsl #2]
    and     r0, r0, #31
    .ifc \op, mov
    mov     \acc, lr, lsr r0
    .else
    orr     \acc, \acc, lr, lsr r0
    .endif
.endm

    .equ    MOVE_POWDER, 1
    .equ    MOVE_GAS, 3

/* In cells_step_cell: r4 the stepper, r5 x, r6 y, r7 the width, r8 the
 * cell's index, r9 its material, r10 the random bits left and r11 their
 * count; r12 its density once it moves. */

/* r0 a random byte */
.macro  rand8
    subs    r11, r11, #8
    bllt    .Lrefill
    and     r0, r10, #255
    mov     r10, r10, lsr #8
.endm

/* Cell x, y's chunk is awake next tick */
.macro  busy xr, yr, t1, t2
    ldr     \t1, [r4, #S_CW]
    mov     \t2, \yr, lsr #4
    mul     \t2, \t1, \t2
    add     \t2, \t2, \xr, lsr #4
    add     \t2, \t2, r4
    mov     \t1, #1
    strb    \t1, [\t2, #S_CHANGED]
.endm

/* To no unless the material can move into tx, ty: on the field, lighter
 * and not yet moved. r3 is left its index; r0 and lr are lost. */
.macro  lighter tx, ty, no, checky=1
    cmp     \tx, r7
    bhs     \no
    .if \checky
    ldr     r0, [r4, #S_H]
    cmp     \ty, r0
    bhs     \no
    .endif
    mla     r3, \ty, r7, \tx
    ldr     r0, [r4, #S_C]
    ldrb    r0, [r0, r3]
    add     r0, r0, r4
    ldrb    r0, [r0, #S_DENS]
    cmp     r0, r12
    bhs     \no
    ldr     r0, [r4, #S_MOVED]
    ldrb    r0, [r0, r3, lsr #3]
    and     lr, r3, #7
    mov     r0, r0, lsr lr
    tst     r0, #1
    bne     \no
.endm

/* To no unless cell tx, y is air or a mover: nothing moves diagonally
 * through a corner. r0 and r3 are lost. */
.macro  open tx, no
    cmp     \tx, r7
    bhs     \no
    ldr     r0, [r4, #S_C]
    add     r0, r0, r8
    sub     r3, \tx, r5
    ldrb    r0, [r0, r3]
    add     r3, r0, r4
    ldrb    r3, [r3, #S_KIND]
    cmp     r0, #0
    orreq   r3, r3, #K_MOVES
    tst     r3, #K_MOVES
    beq     \no
.endm

/* Cell ir's way is vr (0 or 1); r1, r2, r12 and lr are lost */
.macro  setway ir, vr
    ldr     r12, [r4, #S_DIR]
    ldrb    r1, [r12, \ir, lsr #3]
    and     r2, \ir, #7
    mov     lr, #1
    bic     r1, r1, lr, lsl r2
    orr     r1, r1, \vr, lsl r2
    strb    r1, [r12, \ir, lsr #3]
.endm

/* void cells_step_cell(struct stepper *s, int32_t x, int32_t y) */
    .global cells_step_cell
    .type   cells_step_cell, %function
cells_step_cell:
    stmfd   sp!, {r4-r11, lr}
    /* 0-12 the four neighbours, 16 and 20 a reaction's partner's place,
     * 24 its reaction; moving, 20 a side's way, 24 the move */
    sub     sp, sp, #28
    mov     r4, r0
    mov     r5, r1
    mov     r6, r2
    ldr     r0, [r4, #S_STEPS]
    add     r0, r0, #1
    str     r0, [r4, #S_STEPS]
    ldr     r7, [r4, #S_W]
    mla     r8, r6, r7, r5
    ldr     r0, [r4, #S_C]
    ldrb    r9, [r0, r8]
    ldr     r10, [r4, #S_BITS]
    ldr     r11, [r4, #S_NBITS]

    /* a life span keeps its chunk awake */
    ldr     r0, [r4, #S_DECAY]
    ldrb    r0, [r0, r9]
    cmp     r0, #0
    beq     1f
    busy    r5, r6, r0, r1
1:
    add     r0, r4, r9
    ldrb    r12, [r0, #S_FIRST]
    cmp     r12, #255
    beq     .Ldecay
    /* the partner's place, one of the four at random, and what is there
     * (r3, -1 off the field) */
    rand8
    and     r0, r0, #3
    mov     r1, r5
    mov     r2, r6
    cmp     r0, #1
    sublt   r1, r1, #1
    addeq   r1, r1, #1
    cmp     r0, #2
    subeq   r2, r2, #1
    addgt   r2, r2, #1
    str     r1, [sp, #16]
    str     r2, [sp, #20]
    mvn     r3, #0
    cmp     r1, r7
    bhs     2f
    ldr     r0, [r4, #S_H]
    cmp     r2, r0
    bhs     2f
    mla     r0, r2, r7, r1
    ldr     lr, [r4, #S_C]
    ldrb    r3, [lr, r0]
2:
    /* r12 its partners' bits: one beside it keeps it awake. r2 gathers
     * each neighbour's bit; off the field is material 256, in word 8,
     * which is 0. */
    add     r12, r4, r12, lsl #6
    add     r12, r12, #S_PARTNER
    ldr     r1, [r4, #S_C]
    add     r1, r1, r8
    cmp     r5, #0
    ldrbgt  r0, [r1, #-1]
    movle   r0, #256
    partbit r2, mov
    sub     lr, r7, #1
    cmp     r5, lr
    ldrblt  r0, [r1, #1]
    movge   r0, #256
    partbit r2, orr
    cmp     r6, #0
    ldrbgt  r0, [r1, -r7]
    movle   r0, #256
    partbit r2, orr
    ldr     lr, [r4, #S_H]
    sub     lr, lr, #1
    cmp     r6, lr
    ldrblt  r0, [r1, r7]
    movge   r0, #256
    partbit r2, orr
    tst     r2, #1
    beq     3f
    busy    r5, r6, r0, r1
3:
    /* the chosen place's material r3, a partner not yet moved, reacts by
     * chance: the first of its reactions with that b */
    cmp     r3, #0
    blt     .Ldecay
    mov     r0, r3
    partbit r2, mov
    tst     r2, #1
    beq     .Ldecay
    ldr     r1, [sp, #16]
    ldr     r2, [sp, #20]
    mla     r0, r2, r7, r1
    ldr     lr, [r4, #S_MOVED]
    ldrb    lr, [lr, r0, lsr #3]
    and     r0, r0, #7
    mov     lr, lr, lsr r0
    tst     lr, #1
    bne     .Ldecay
    add     r0, r4, r9
    ldrb    r12, [r0, #S_FIRST]
    ldr     r0, [r4, #S_RB]
4:
    ldrb    lr, [r0, r12]
    cmp     lr, r3
    addne   lr, r4, r12
    ldrbne  r12, [lr, #S_NEXT]
    bne     4b
    rand8
    ldr     lr, [r4, #S_CHANCE]
    ldrb    lr, [lr, r12]
    cmp     r0, lr
    bhs     .Ldecay
    str     r12, [sp, #24]
    ldr     r0, [r4, #S_RC]
    ldrb    r0, [r0, r12]
    mov     r1, r5
    mov     r2, r6
    mov     r3, r8
    bl      .Lset
    ldr     r12, [sp, #24]
    ldr     r0, [r4, #S_RD]
    ldrb    r0, [r0, r12]
    ldr     r1, [sp, #16]
    ldr     r2, [sp, #20]
    mla     r3, r2, r7, r1
    bl      .Lset
    b       .Lstepped

.Ldecay:
    ldr     r0, [r4, #S_DECAY]
    ldrb    r1, [r0, r9]
    cmp     r1, #0
    beq     .Lmove
    rand8
    cmp     r0, r1
    bhs     .Lmove
    ldr     r0, [r4, #S_BECOMES]
    ldrb    r0, [r0, r9]
    mov     r1, r5
    mov     r2, r6
    mov     r3, r8
    bl      .Lset
    b       .Lstepped

.Lmove:
    ldr     r0, [r4, #S_MOVE]
    ldrb    r1, [r0, r9]
    cmp     r1, #0
    beq     .Lstepped
    str     r1, [sp, #24]
    add     r0, r4, r9
    ldrb    r12, [r0, #S_DENS]
    /* r2 the row it falls to, or rises to */
    cmp     r1, #MOVE_GAS
    subeq   r2, r6, #1
    addne   r2, r6, #1
    /* straight down, taking a new way */
    lighter r5, r2, 1f
    /* a liquid or gas drifts a cell its new way one tick in four */
    rand8
    str     r0, [sp, #20]
    ldr     r1, [sp, #24]
    cmp     r1, #MOVE_POWDER
    beq     9f
    tst     r0, #6
    bne     9f
    tst     r0, #1
    addne   r1, r5, #1
    subeq   r1, r5, #1
    open    r1, 9f
    lighter r1, r2, 9f
    b       .Lside
9:
    mov     r1, r5
    mla     r3, r2, r7, r5
    bl      .Lswap
    ldr     r0, [sp, #20]
    and     r0, r0, #1
    setway  r3, r0
    b       .Lstepped
1:
    /* down to one side at random, then the other */
    rand8
    tst     r0, #1
    addne   r1, r5, #1
    subeq   r1, r5, #1
    open    r1, 2f
    lighter r1, r2, 2f
    b       .Lside
2:
    rsb     r1, r1, r5, lsl #1
    open    r1, 3f
    lighter r1, r2, 3f
    b       .Lside
3:
    ldr     r0, [sp, #24]
    cmp     r0, #MOVE_POWDER
    beq     .Lstepped
    /* liquids and gases, with no more random bytes: r10 the way, 1 or -1,
     * r11 the cells it may run */
    str     r10, [r4, #S_BITS]
    str     r11, [r4, #S_NBITS]
    ldr     r0, [r4, #S_DIR]
    ldrb    r0, [r0, r8, lsr #3]
    and     r1, r8, #7
    mov     r0, r0, lsr r1
    ands    r10, r0, #1
    mvneq   r10, #0
    ldr     r0, [r4, #S_SPREAD]
    ldrb    r11, [r0, r9]
    cmp     r11, #0
    moveq   r11, #2
    add     r1, r5, r10
5:
    /* along while it can, into the first place it can fall */
    lighter r1, r6, 6f, 0
    lighter r1, r2, 7f
    bl      .Lswap
    b       .Lleave
7:
    add     r1, r1, r10
    subs    r11, r11, #1
    bne     5b
6:
    /* resting on what never moves, or the field's edge, it runs only to
     * fall: r2 the row under it */
    ldr     r0, [r4, #S_H]
    cmp     r2, r0
    bhs     .Lleave
    mla     r3, r2, r7, r5
    ldr     r0, [r4, #S_C]
    ldrb    r0, [r0, r3]
    cmp     r0, #0
    beq     4f
    add     r3, r0, r4
    ldrb    r3, [r3, #S_KIND]
    tst     r3, #K_MOVES
    beq     .Lleave
4:
    sub     r1, r1, r10
    cmp     r1, r5
    beq     8f
    mla     r3, r6, r7, r1
    mov     r2, r6
    bl      .Lswap
    b       .Lleave
8:
    /* blocked along: it turns round if it can go the other way */
    sub     r1, r5, r10
    lighter r1, r6, .Lleave, 0
    cmp     r10, #0
    movlt   r0, #1
    movge   r0, #0
    setway  r8, r0
    busy    r5, r6, r0, r1
    b       .Lleave

.Lside:
    /* r1, r2 and r3 the place to one side below; its way is that side */
    cmp     r1, r5
    movgt   r0, #1
    movle   r0, #0
    str     r0, [sp, #20]
    bl      .Lswap
    ldr     r0, [sp, #20]
    setway  r3, r0
.Lstepped:
    str     r10, [r4, #S_BITS]
    str     r11, [r4, #S_NBITS]
.Lleave:
    add     sp, sp, #28
    ldmfd   sp!, {r4-r11, pc}

/* The cell into r1, r2 (index r3), and what was there back, each with its
 * way; the cell moved into is done for the tick. r0-r2 and r12 are lost. */
.Lswap:
    busy    r1, r2, r0, r12
    busy    r5, r6, r0, r12
    ldr     r12, [r4, #S_C]
    ldrb    r0, [r12, r3]
    strb    r0, [r12, r8]
    strb    r9, [r12, r3]
    ldr     r12, [r4, #S_MOVED]
    ldrb    r0, [r12, r3, lsr #3]
    and     r1, r3, #7
    mov     r2, #1
    orr     r0, r0, r2, lsl r1
    strb    r0, [r12, r3, lsr #3]
    ldr     r0, [r4, #S_MOVES]
    add     r0, r0, #1
    str     r0, [r4, #S_MOVES]
    /* the ways change places when they differ */
    ldr     r12, [r4, #S_DIR]
    ldrb    r0, [r12, r8, lsr #3]
    and     r2, r8, #7
    mov     r0, r0, lsr r2
    ldrb    r2, [r12, r3, lsr #3]
    eor     r0, r0, r2, lsr r1
    tst     r0, #1
    moveq   pc, lr
    mov     r0, #1
    eor     r2, r2, r0, lsl r1
    strb    r2, [r12, r3, lsr #3]
    and     r1, r8, #7
    ldrb    r2, [r12, r8, lsr #3]
    eor     r2, r2, r0, lsl r1
    strb    r2, [r12, r8, lsr #3]
    mov     pc, lr

/* Cell r1, r2 (index r3) becomes r0 and is done for the tick. r0-r2 and
 * r12 are lost. */
.Lset:
    busy    r1, r2, r12, r2
    ldr     r12, [r4, #S_C]
    strb    r0, [r12, r3]
    ldr     r12, [r4, #S_MOVED]
    ldrb    r0, [r12, r3, lsr #3]
    and     r2, r3, #7
    mov     r1, #1
    orr     r0, r0, r1, lsl r2
    strb    r0, [r12, r3, lsr #3]
    mov     pc, lr

/* Four more random bytes, keeping r1-r3 and r12 */
.Lrefill:
    stmfd   sp!, {r0-r3, r12, lr}
    ldr     r0, [r4, #S_VM]
    bl      core_random
    mov     r10, r0
    mov     r11, #24
    ldmfd   sp!, {r0-r3, r12, pc}
    .size   cells_step_cell, . - cells_step_cell
