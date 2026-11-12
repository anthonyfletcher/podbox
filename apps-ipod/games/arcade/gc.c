/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade: the heap. Non-moving mark-sweep over [heap_lo, bump), so a
 * value's offset never changes.
 *
 * Free blocks are kept on exact lists for 16..504 bytes and one first-fit
 * list above that; each sweep rebuilds them all, merging neighbours. The
 * roots are the globals, the value stack, vm->tmp, the drawing target, the
 * sprites' sheet and the pause menu's labels.
 * Marking uses a fixed stack; when it overflows, the heap is rescanned for
 * marked objects until nothing new is marked. A native's working memory
 * (gc_scratch) is taken from the top of the free tail above bump and
 * given back before it returns, so it never leaves a hole among what the
 * cart keeps.
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"

#define HDR(vm, off) ((struct hdr *)MEM(vm, off))
#define NEXT(vm, off) (*(uint32_t *)MEM(vm, (off) + 8))

static void file_free(struct vm *vm, uint32_t off, uint32_t size)
{
    struct hdr *h = HDR(vm, off);
    uint32_t c = size >> 3;
    h->info = GC_FREE;
    h->size = size;
    if (c < FREE_CLASSES) {
        NEXT(vm, off) = vm->freelist[c];
        vm->freelist[c] = off;
    } else {
        NEXT(vm, off) = vm->freebig;
        vm->freebig = off;
    }
}

void gc_init(struct vm *vm)
{
    memset(vm->freelist, 0, sizeof(vm->freelist));
    vm->freebig = 0;
    vm->bump = vm->heap_lo;
    vm->since_gc = 0;
    vm->gc_threshold = (vm->heap_hi - vm->heap_lo) / 4;
}

static uint32_t take(struct vm *vm, uint32_t size)
{
    uint32_t c = size >> 3, off;
    if (c < FREE_CLASSES && vm->freelist[c]) {
        off = vm->freelist[c];
        vm->freelist[c] = NEXT(vm, off);
        return off;
    }
    /* First fit among the big blocks, splitting off the rest. */
    uint32_t *link = &vm->freebig;
    while ((off = *link) != 0) {
        uint32_t have = HDR(vm, off)->size;
        if (have == size || have >= size + 16) {
            *link = NEXT(vm, off);
            if (have > size)
                file_free(vm, off + size, have - size);
            return off;
        }
        link = &NEXT(vm, off);
    }
    if (vm->heap_hi - vm->bump >= size) {
        off = vm->bump;
        vm->bump += size;
        return off;
    }
    return 0;
}

/* A zeroed object of `bytes` after its header, or 0 with the error set.
 * May collect, so every live value must be rooted. */
uint32_t gc_alloc(struct vm *vm, uint32_t type, uint32_t bytes)
{
    if (bytes > vm->heap_hi - vm->heap_lo) {
        vm_fail(vm, "out of memory", "", "");
        return 0;
    }
    uint32_t size = (bytes + sizeof(struct hdr) + 7) & ~7u;
    if (size < 16)
        size = 16;
    uint32_t off = take(vm, size);
    if (!off) {
        gc_collect(vm);
        off = take(vm, size);
        if (!off) {
            vm_fail(vm, "out of memory", "", "");
            return 0;
        }
    }
    memset(MEM(vm, off), 0, size);
    HDR(vm, off)->info = type;
    HDR(vm, off)->size = size;
    vm->since_gc += size;
    return off;
}

/* bytes of zeroed working memory, from the top of the free tail, or 0
 * with the error set; the heap ends below it until gc_scratch_end gives it
 * back, so allocations meanwhile come from under it. May collect. */
uint32_t gc_scratch(struct vm *vm, uint32_t bytes)
{
    bytes = (bytes + 7) & ~7u;
    if (vm->heap_hi - vm->bump < bytes)
        gc_collect(vm);
    if (bytes > vm->heap_hi - vm->heap_lo ||
        vm->heap_hi - vm->bump < bytes) {
        vm_fail(vm, "out of memory", "", "");
        return 0;
    }
    vm->heap_hi -= bytes;
    memset(MEM(vm, vm->heap_hi), 0, bytes);
    return vm->heap_hi;
}

/* working memory of bytes given back: all of gc_scratch's, latest first */
void gc_scratch_end(struct vm *vm, uint32_t bytes)
{
    vm->heap_hi += (bytes + 7) & ~7u;
}

static int in_heap(struct vm *vm, uint32_t off)
{
    return off >= vm->heap_lo && off < vm->bump;
}

/* bytes marked since last cleared: arcade_memory's share of each root */
static uint32_t marked_bytes;

static void mark_off(struct vm *vm, uint32_t off)
{
    if (!in_heap(vm, off))
        return;
    struct hdr *h = HDR(vm, off);
    if (h->info & GC_MARK)
        return;
    h->info |= GC_MARK;
    marked_bytes += h->size;
    uint32_t t = h->info & 0xff;
    if (t == T_STR || t == T_I32 || t == T_U8 || t == T_IMG || t == T_FXA)
        return;
    if (vm->msp < MARK_STACK)
        vm->mstack[vm->msp++] = off;
    else
        vm->moverflow = 1;
}

static void mark_value(struct vm *vm, struct value v)
{
    if ((v.tag >= T_STR && v.tag <= T_OBJ) || v.tag == T_IMG ||
        v.tag == T_FXA)
        mark_off(vm, (uint32_t)v.v);
}

static void scan(struct vm *vm, uint32_t off)
{
    struct hdr *h = HDR(vm, off);
    uint32_t n = 0, b = 0;
    if ((h->info & 0xff) == T_ARR) {
        struct arr *a = MEM(vm, off);
        n = a->len;
        b = a->buf;
    } else if ((h->info & 0xff) == T_OBJ) {
        struct obj *o = MEM(vm, off);
        n = vm->shapes[o->shape].nkeys;
        b = o->buf;
    }
    if (!b)
        return;
    if (in_heap(vm, b) && !(HDR(vm, b)->info & GC_MARK)) {
        HDR(vm, b)->info |= GC_MARK;
        marked_bytes += HDR(vm, b)->size;
    }
    struct buf *bf = BUF(vm, b);
    for (uint32_t i = 0; i < n; i++)
        mark_value(vm, bf->v[i]);
}

static void drain(struct vm *vm)
{
    while (vm->msp)
        scan(vm, vm->mstack[--vm->msp]);
}

/* everything marked's children marked, rescanning the heap for any the
 * mark stack overflowed past */
static void finish_marking(struct vm *vm)
{
    drain(vm);
    while (vm->moverflow) {
        vm->moverflow = 0;
        for (uint32_t off = vm->heap_lo; off < vm->bump;
             off += HDR(vm, off)->size) {
            if (HDR(vm, off)->info & GC_MARK) {
                scan(vm, off);
                drain(vm);
            }
        }
    }
}

/* the engine's roots beside the globals */
static void mark_engine_roots(struct vm *vm)
{
    uint32_t i;
    for (struct value *v = vm->stack; v < vm->sp; v++) {
        mark_value(vm, *v);
        drain(vm);
    }
    for (i = 0; i < vm->ntmp; i++)
        mark_value(vm, vm->tmp[i]);
    mark_value(vm, vm->d.target);
    mark_value(vm, vm->d.sheet);
    for (i = 0; i < 5; i++)
        mark_value(vm, vm->menu_label[i]);
    finish_marking(vm);
}

void arcade_memory(struct vm *vm, struct arcade_memory *m,
                   uint32_t *by_global, uint32_t n)
{
    uint32_t i, off;
    memset(m, 0, sizeof(*m));
    m->arena = vm->size;
    m->tables = vm->heap_lo;
    m->heap = vm->heap_hi - vm->heap_lo;
    vm->msp = 0;
    vm->moverflow = 0;
    for (i = 0; i < vm->nglobals; i++) {
        marked_bytes = 0;
        mark_value(vm, vm->globals[i]);
        finish_marking(vm);
        if (i < n)
            by_global[i] = marked_bytes;
    }
    marked_bytes = 0;
    mark_engine_roots(vm);
    m->engine = marked_bytes;
    for (off = vm->heap_lo; off < vm->bump;) {
        struct hdr *h = HDR(vm, off);
        uint32_t size = h->size, kind = h->info & 0xff;
        if (h->info & GC_FREE) {
            m->free += size;
            if (size > m->largest)
                m->largest = size;
        } else if (h->info & (GC_MARK | GC_PERM)) {
            m->held += size;
            if (h->info & GC_PERM)
                m->constants += size;
            if (kind < ARCADE_KINDS) {
                m->kind_bytes[kind] += size;
                m->kind_count[kind]++;
            }
        } else
            m->garbage += size;
        h->info &= ~GC_MARK;
        off += size;
    }
    /* the untouched tail above the last object */
    m->free += vm->heap_hi - vm->bump;
    if (vm->heap_hi - vm->bump > m->largest)
        m->largest = vm->heap_hi - vm->bump;
}

void gc_collect(struct vm *vm)
{
    uint32_t i, off;
    vm->msp = 0;
    vm->moverflow = 0;
    for (i = 0; i < vm->nglobals; i++)
        mark_value(vm, vm->globals[i]);
    drain(vm);
    mark_engine_roots(vm);

    /* Sweep: merge each run of dead and free blocks into one. */
    memset(vm->freelist, 0, sizeof(vm->freelist));
    vm->freebig = 0;
    uint32_t run = 0;
    for (off = vm->heap_lo; off < vm->bump;) {
        struct hdr *h = HDR(vm, off);
        uint32_t size = h->size;
        if (h->info & (GC_MARK | GC_PERM)) {
            h->info &= ~GC_MARK;
            if (run) {
                file_free(vm, run, off - run);
                run = 0;
            }
        } else if (!run)
            run = off;
        off += size;
    }
    if (run)
        vm->bump = run;
    vm->since_gc = 0;
}
