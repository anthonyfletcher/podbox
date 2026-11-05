/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade: the cells system's scan of one run of a row, ARMv4 so one copy
 * serves both players. It is run() in sys_cells.c with idle() and stuck()
 * inside it: each cell that is not air and not yet moved is counted, and
 * stepped by cells_step_cell() unless nothing round it lets it do
 * anything. That C stays for the simulator and the host, and is the
 * reference this is checked against.
 *
 * One difference in how, none in what: a cell with only its own material
 * in the six places stuck() looks at is stuck without looking up a
 * density, when its kind says so (K_SAME). That is most of a pool.
 ****************************************************************************/
    .syntax unified
    .section .text
    .align  2

/* struct run, sys_cells.c */
    .equ    R_C, 0
    .equ    R_MOVED, 4
    .equ    R_DENS, 8
    .equ    R_W, 12
    .equ    R_I, 16
    .equ    R_DX, 20
    .equ    R_GROUPS, 24
    .equ    R_SIZE, 28
    .equ    R_WORDS, 32
    .equ    R_LO, 36
    .equ    R_SPAN, 40
    .equ    R_FIRST, 44
    .equ    R_NEXT, 48
    .equ    R_B, 52
    .equ    R_S, 56
    .equ    R_ROWBASE, 60
    .equ    R_Y, 64

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
    ldr     r9, [r0, #R_WORDS]
    cmp     r9, #0
    beq     2f
    /* a word of air passes four cells; going back, the word ends here */
1:
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
.endm

/* uint32_t cells_run_arm(struct run *r): the cells looked at */
    .global cells_run_arm
    .type   cells_run_arm, %function
cells_run_arm:
    stmfd   sp!, {r4-r12, lr}
    ldr     r4, [r0, #R_C]
    ldr     r5, [r0, #R_MOVED]
    ldr     r6, [r0, #R_DENS]
    ldr     r7, [r0, #R_W]
    ldr     r1, [r0, #R_I]
    ldr     r2, [r0, #R_LO]
    ldr     r3, [r0, #R_GROUPS]
    ldr     r9, [r0, #R_SIZE]
    orr     r3, r9, r3, lsl #16
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
