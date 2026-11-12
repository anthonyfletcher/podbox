/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade: the verifier. A cart is untrusted; before anything runs, every
 * function is decoded and checked, so the interpreter need only check
 * value tags and call depth:
 *
 *   - every opcode is known and every operand is in range;
 *   - every jump lands on an instruction inside its function;
 *   - the stack height is the same on every path into an instruction,
 *     never negative, within the declared maximum, empty at each return,
 *     and no path runs off the end;
 *   - every native exists in this firmware and suits the music level.
 *
 * arcade/sdk/src/bytecode.js verify() is the reference: the same checks,
 * and the compiler's tests run every cart they make through it.
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"

static const char *const operands[OP_COUNT] = {
#define OP(name, ops, pops, pushes) ops,
#define MTH(name, min, max)
#define INTRINSIC(name, min, max)
#include "games/arcade/opcodes.def"
#undef OP
#undef MTH
#undef INTRINSIC
};

static const int8_t pops_of[OP_COUNT] = {
#define OP(name, ops, pops, pushes) pops,
#define MTH(name, min, max)
#define INTRINSIC(name, min, max)
#include "games/arcade/opcodes.def"
#undef OP
#undef MTH
#undef INTRINSIC
};

static const int8_t pushes_of[OP_COUNT] = {
#define OP(name, ops, pops, pushes) pushes,
#define MTH(name, min, max)
#define INTRINSIC(name, min, max)
#include "games/arcade/opcodes.def"
#undef OP
#undef MTH
#undef INTRINSIC
};

/* Each method's and intrinsic's argument counts; the implementations
 * read their arguments without checking how many came. */
static const uint8_t method_args[M_COUNT][2] = {
#define OP(name, ops, pops, pushes)
#define MTH(name, min, max) { min, max },
#define INTRINSIC(name, min, max)
#include "games/arcade/opcodes.def"
#undef OP
#undef MTH
#undef INTRINSIC
};

static const uint8_t intrinsic_args[I_COUNT][2] = {
#define OP(name, ops, pops, pushes)
#define MTH(name, min, max)
#define INTRINSIC(name, min, max) { min, max },
#include "games/arcade/opcodes.def"
#undef OP
#undef MTH
#undef INTRINSIC
};

static int size_of(int op)
{
    int n = 1;
    for (const char *k = operands[op]; *k; k++)
        n += (*k == 'b' || *k == 'B') ? 1 : (*k == 'i') ? 4 : 2;
    return n;
}

static uint32_t u16at(const uint8_t *p) { return p[0] | (p[1] << 8); }

static int is_jump(int op)
{
    return op == OP_JMP || op == OP_JF || op == OP_JT ||
        op == OP_ANDJ || op == OP_ORJ || (op >= OP_JLT && op <= OP_JNE) ||
        (op >= OP_JLTLI && op <= OP_JGELI);
}

/* A jump's offset, its last operand, from the end of the instruction */
static int32_t offset_of(const uint8_t *p)
{
    return (int16_t)u16at(p + size_of(p[0]) - 2);
}

/* Stack effect of the instruction at p; 0 when an operand is invalid. */
static int effect(struct vm *vm, const uint8_t *p, int *pops, int *pushes)
{
    int op = p[0];
    *pops = pops_of[op];
    *pushes = pushes_of[op];
    switch (op) {
    case OP_CALL: {
        uint32_t f = u16at(p + 1);
        if (f >= vm->nfns)
            return vm_fail(vm, "this game file is damaged: ", "call", "");
        *pops = vm->fns[f].arity;
        break;
    }
    case OP_CALLV:
        *pops = p[1] + 1;
        break;
    case OP_NCALL: {
        uint32_t slot = u16at(p + 1), argc = p[3];
        if (slot >= arcade_api_count || !arcade_impl[slot])
            return vm_fail(vm, "This game needs a newer PodBox", "", "");
        const struct native_def *d = &arcade_api[slot];
        if (d->level > vm->music)
            return vm_fail(vm, "this game uses the music without "
                           "declaring it", "", "");
        if (d->parts ? argc < 1 : (argc < d->min || argc > d->max))
            return vm_fail(vm, "this game file is damaged: ", "arguments", "");
        *pops = (int)argc;
        *pushes = d->ret;
        break;
    }
    case OP_NEWARR:
        *pops = (int)u16at(p + 1);
        break;
    case OP_NEWOBJ: {
        uint32_t s = u16at(p + 1);
        if (s >= vm->ncshapes)
            return vm_fail(vm, "this game file is damaged: ", "shape", "");
        *pops = vm->shapes[vm->cshape[s]].nkeys;
        break;
    }
    case OP_METHOD:
        *pops = p[2] + 1;
        break;
    case OP_INTR:
        *pops = p[2];
        break;
    }
    return 1;
}

static int operands_ok(struct vm *vm, const struct fninfo *f,
                       const uint8_t *p)
{
    switch (p[0]) {
    case OP_LDL: case OP_STL: case OP_INCL:
        return p[1] < f->nlocals;
    case OP_LDG: case OP_STG:
        return u16at(p + 1) < vm->nglobals;
    case OP_GETGL:
        return u16at(p + 1) < vm->nglobals && p[3] < f->nlocals;
    case OP_GETGP:
        return u16at(p + 1) < vm->nglobals && u16at(p + 3) < vm->nstrings &&
            u16at(p + 5) < vm->nic;
    case OP_JLTLI: case OP_JLELI: case OP_JGTLI: case OP_JGELI:
        return p[1] < f->nlocals;
    case OP_PUSHS:
        return u16at(p + 1) < vm->nstrings;
    case OP_PUSHFN:
        return u16at(p + 1) < vm->nfns;
    case OP_GETPROP: case OP_SETPROP:
        return u16at(p + 1) < vm->nstrings && u16at(p + 3) < vm->nic;
    case OP_METHOD:
        return p[1] < M_COUNT && p[2] >= method_args[p[1]][0] &&
            p[2] <= method_args[p[1]][1];
    case OP_INTR:
        return p[1] < I_COUNT && p[2] >= intrinsic_args[p[1]][0] &&
            p[2] <= intrinsic_args[p[1]][1];
    }
    return 1;
}

static int verify_fn(struct vm *vm, uint32_t fi, uint8_t *scratch,
                     uint32_t room)
{
    const struct fninfo *f = &vm->fns[fi];
    const uint8_t *code = vm->code + f->off;
    uint32_t len = f->len, pc;
    /* height, a word aligned, then work and start, as laid out below */
    if ((((uint64_t)len * 2 + 3) & ~(uint64_t)3) + (uint64_t)len * 5 > room)
        return vm_fail(vm, "not enough memory for this game", "", "");
    if (f->arity > f->nlocals)
        return vm_fail(vm, "this game file is damaged: ", "locals", "");
    uint16_t *height = (uint16_t *)scratch;
    uint32_t *work = (uint32_t *)(scratch + ((len * 2 + 3) & ~3u));
    uint8_t *start = scratch + ((len * 2 + 3) & ~3u) + len * 4;
    memset(start, 0, len);
    memset(height, 0xff, len * 2);

    for (pc = 0; pc < len;) {
        int op = code[pc];
        if (op >= OP_COUNT)
            return vm_fail(vm, "this game file is damaged: ", "opcode", "");
        uint32_t n = (uint32_t)size_of(op);
        if (n > len - pc)
            return vm_fail(vm, "this game file is damaged: ", "truncated", "");
        if (!operands_ok(vm, f, code + pc))
            return vm_fail(vm, "this game file is damaged: ", "operand", "");
        start[pc] = 1;
        pc += n;
    }
    for (pc = 0; pc < len; pc++) {
        if (!start[pc] || !is_jump(code[pc]))
            continue;
        int64_t t = (int64_t)pc + size_of(code[pc]) + offset_of(code + pc);
        if (t < 0 || t >= len || !start[t])
            return vm_fail(vm, "this game file is damaged: ", "jump", "");
    }

    uint32_t nwork = 0, max = 0;
    height[0] = 0;
    work[nwork++] = 0;
    while (nwork) {
        pc = work[--nwork];
        const uint8_t *p = code + pc;
        int h = height[pc], pops, pushes;
        if (!effect(vm, p, &pops, &pushes))
            return 0;
        if (h < pops)
            return vm_fail(vm, "this game file is damaged: ", "stack", "");
        if (p[0] == OP_RET || p[0] == OP_RETNULL) {
            if (h != pops)
                return vm_fail(vm, "this game file is damaged: ", "return", "");
            continue;
        }
        int after = h - pops + pushes;
        if ((uint32_t)after > max)
            max = (uint32_t)after;
        uint32_t next = pc + (uint32_t)size_of(p[0]);
        uint32_t to[2];
        int hs[2], nto = 0;
        if (is_jump(p[0])) {
            to[nto] = (uint32_t)((int32_t)next + offset_of(p));
            hs[nto++] = (p[0] == OP_ANDJ || p[0] == OP_ORJ) ? h : after;
        }
        if (p[0] != OP_JMP) {
            to[nto] = next;
            hs[nto++] = after;
        }
        for (int i = 0; i < nto; i++) {
            if (to[i] >= len)
                return vm_fail(vm, "this game file is damaged: ", "end", "");
            if (height[to[i]] == 0xffff) {
                height[to[i]] = (uint16_t)hs[i];
                work[nwork++] = to[i];
            } else if (height[to[i]] != hs[i])
                return vm_fail(vm, "this game file is damaged: ", "paths", "");
        }
    }
    if (max > f->maxstack)
        return vm_fail(vm, "this game file is damaged: ", "stack size", "");
    if ((uint32_t)f->nlocals + f->maxstack + STACK_SLACK > STACK_VALUES)
        return vm_fail(vm, "a function needs more stack than a game has",
                       "", "");
    return 1;
}

int verify_cart(struct vm *vm)
{
    uint32_t base = (vm->perm + 7) & ~7u;
    uint8_t *scratch = MEM(vm, base);
    uint32_t room = vm->size - base;
    if (vm->entry[1] == NONE || vm->entry[2] == NONE)
        return vm_fail(vm, "this game file is damaged: ", "entries", "");
    for (int i = 0; i < 5; i++)
        if (vm->entry[i] != NONE && vm->entry[i] >= vm->nfns)
            return vm_fail(vm, "this game file is damaged: ", "entries", "");
    for (uint32_t i = 0; i < vm->nfns; i++)
        if (!verify_fn(vm, i, scratch, room))
            return 0;
    return 1;
}
