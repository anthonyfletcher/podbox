/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade: loading a cart into the arena.
 *
 * Parts: the perm allocator; string keys (ids, the content hash); reading
 * the chunks, each CRC-checked, SRC_ and unknown chunks skipped unread;
 * building the tables from them; the error's source position from LINE;
 * arcade_load() itself.
 *
 * The byte layouts are the ones arcade/sdk/src/compiler.js writes; its
 * bytecode.js load() is the reference this follows.
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"

/* The LZ4 block format: n bytes from src make exactly size bytes in dst,
 * or 0 for data that is malformed or makes any other number. */
int lz4_decode(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t size)
{
    const uint8_t *s = src, *end = src + n;
    uint32_t o = 0;
    while (s < end) {
        uint32_t t = *s++, lit = t >> 4, b;
        if (lit == 15)
            do {
                if (s >= end)
                    return 0;
                b = *s++;
                lit += b;
            } while (b == 255);
        if (lit > (uint32_t)(end - s) || lit > size - o)
            return 0;
        memcpy(dst + o, s, lit);
        s += lit;
        o += lit;
        if (s >= end)
            break;                  /* the last sequence has no match */
        if (end - s < 2)
            return 0;
        uint32_t off = s[0] | (uint32_t)s[1] << 8, m = (t & 15) + 4;
        s += 2;
        if (!off || off > o)
            return 0;
        if ((t & 15) == 15)
            do {
                if (s >= end)
                    return 0;
                b = *s++;
                m += b;
            } while (b == 255);
        if (m > size - o)
            return 0;
        for (; m; m--, o++)
            dst[o] = dst[o - off];
    }
    return o == size;
}

uint32_t perm_alloc(struct vm *vm, uint32_t bytes)
{
    uint32_t off = (vm->perm + 7) & ~7u;
    if (bytes > vm->size || off > vm->size - bytes)
        return 0;
    vm->perm = off + bytes;
    memset(MEM(vm, off), 0, bytes);
    return off;
}

/* ---- keys ---------------------------------------------------------------- *
 * Every string a cart uses as a record key has an id. A constant string's
 * id is its CNST index; strings made at run time and used as keys are
 * copied and added after them. The id is cached in the string's header. */

static uint32_t hash_bytes(const uint8_t *b, uint32_t n)
{
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}

static void hash_insert(struct vm *vm, uint32_t id)
{
    struct str *s = MEM(vm, vm->keyoff[id]);
    uint32_t i = hash_bytes(s->b, s->len) & vm->hashmask;
    while (vm->keyhash[i])
        i = (i + 1) & vm->hashmask;
    vm->keyhash[i] = (uint16_t)(id + 1);
    s->h.info = (s->h.info & 0xffff) | ((id + 1) << 16);
}

/* The key id of the len bytes at b, or NONE when no record has that key */
uint32_t key_find(struct vm *vm, const char *b, uint32_t len)
{
    uint32_t i = hash_bytes((const uint8_t *)b, len) & vm->hashmask, id;
    while ((id = vm->keyhash[i]) != 0) {
        struct str *k = MEM(vm, vm->keyoff[id - 1]);
        if (k->len == len && !memcmp(k->b, b, len))
            return id - 1;
        i = (i + 1) & vm->hashmask;
    }
    return NONE;
}

/* The key id of string s, or NONE when it has none and add is 0. With add,
 * a new key is made; failing that the error is set and NONE returned. */
uint32_t key_id(struct vm *vm, struct value s, int add)
{
    struct str *p = STR(vm, s);
    uint32_t id = p->h.info >> 16;
    if (id)
        return id - 1;
    uint32_t i = hash_bytes(p->b, p->len) & vm->hashmask;
    while ((id = vm->keyhash[i]) != 0) {
        struct str *k = MEM(vm, vm->keyoff[id - 1]);
        if (k->len == p->len && !memcmp(k->b, p->b, p->len))
            return id - 1;
        i = (i + 1) & vm->hashmask;
    }
    if (!add)
        return NONE;
    if (vm->nkeys >= vm->nstrings + MAX_RTKEYS) {
        vm_fail(vm, "too many different record keys", "", "");
        return NONE;
    }
    /* A copy that is never collected, so the id stays valid. */
    uint32_t len = p->len;
    uint32_t off = gc_alloc(vm, T_STR | GC_PERM, sizeof(struct str) + len);
    if (!off)
        return NONE;
    p = STR(vm, s);
    struct str *k = MEM(vm, off);
    k->len = len;
    memcpy(k->b, p->b, len);
    id = vm->nkeys++;
    vm->keyoff[id] = off;
    hash_insert(vm, id);
    return id;
}

/* ---- reading ---------------------------------------------------------- */

struct in
{
    struct vm *vm;
    arcade_read_fn read;
    void *ctx;
};

static uint32_t crc_table[256];

static void crc_init(void)
{
    if (crc_table[1])
        return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc32(const uint8_t *b, uint32_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--)
        c = crc_table[(c ^ *b++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

/* Lazy chunk c, read from the cart file through the host and checked,
 * then unpacked into size bytes at dst, or only checked when dst is NULL:
 * 1, or 0 with the error set. Its bytes are working memory, from the
 * heap's top, or the arena's while the cart loads. */
int lazy_unpack(struct vm *vm, const struct lazy *c, uint8_t *dst,
                uint32_t size)
{
    const struct arcade_host *h = vm->host;
    if (!h || !h->cart_read)
        return vm_fail(vm, "this player cannot read the game's maps and "
                       "images as it plays", "", "");
    uint32_t held = 0;
    uint8_t *buf;
    if (vm->heap_hi) {
        uint32_t off = gc_scratch(vm, c->len);
        if (!off)
            return 0;
        buf = MEM(vm, off);
        held = c->len;
    } else {
        uint32_t top = (vm->size - c->len) & ~7u;
        if (c->len > vm->size || top < vm->perm)
            return vm_fail(vm, "not enough memory for this game", "", "");
        buf = MEM(vm, top);
    }
    crc_init();
    int ok = h->cart_read(h->ctx, c->at, buf, c->len) == c->len &&
             crc32(buf, c->len) == c->crc &&
             (!dst || lz4_decode(buf, c->len, dst, size));
    if (held)
        gc_scratch_end(vm, held);
    /* measured on the ARM rig (lzcost): about 41 cycles a packed byte,
     * read, checked and unpacked; the unpacked bytes are the caller's */
    vm_charge(vm, c->len);
    return ok ? 1 : vm_fail(vm, "the game file has changed or is damaged",
                            "", "");
}

static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

/* A bounded cursor over a chunk. */
struct cur
{
    const uint8_t *p, *end;
    int bad;
};

static const uint8_t *take(struct cur *c, uint32_t n)
{
    if (c->bad || (uint32_t)(c->end - c->p) < n) {
        c->bad = 1;
        return NULL;
    }
    const uint8_t *r = c->p;
    c->p += n;
    return r;
}

static uint32_t c8(struct cur *c)
{
    const uint8_t *p = take(c, 1);
    return p ? p[0] : 0;
}

static uint32_t c16(struct cur *c)
{
    const uint8_t *p = take(c, 2);
    return p ? rd16(p) : 0;
}

static uint32_t c32(struct cur *c)
{
    const uint8_t *p = take(c, 4);
    return p ? rd_le32(p) : 0;
}

/* ---- the tables ------------------------------------------------------- */

static int bad(struct vm *vm, const char *what)
{
    return vm_fail(vm, "this game file is damaged: ", what, "");
}

static int load_header(struct vm *vm, const uint8_t *p, uint32_t n)
{
    if (n < 20)
        return bad(vm, "header");
    /* format 2 adds ending() to the entries; 3 keeps each map's cells, and
     * each lazy image's pixels, in a chunk of its own */
    vm->format = (uint8_t)rd16(p);
    if (vm->format < 1 || vm->format > 3)
        return vm_fail(vm, "This game needs a newer PodBox", "", "");
    vm->api = (uint16_t)rd16(p + 2);
    if (vm->api > ARCADE_API_VERSION)
        return vm_fail(vm, "This game needs a newer PodBox", "", "");
    vm->rate = p[4];
    vm->menu_button = p[5];
    vm->music = p[6];
    vm->no_suspend = p[7] & 1;
    if (vm->rate != 20 && vm->rate != 25 && vm->rate != 33 && vm->rate != 50)
        return bad(vm, "rate");
    memcpy(vm->id, p + 12, 8);
    return 1;
}

/* Text from the chunk into a fixed buffer, cut to fit and ended */
static void copy_text(struct cur *c, char *dst, uint32_t cap)
{
    uint32_t len = c16(c);
    const uint8_t *t = take(c, len);
    if (!t)
        len = 0;
    if (len > cap - 1)
        len = cap - 1;
    if (len)
        memcpy(dst, t, len);
    dst[len] = 0;
}

/* BORD, the cart's high-score boards: a count, then each one's key,
 * version, whether low wins, its format and its title. With them, the
 * cart's id, rate and name, so the host's file says whose they are. */
static int load_boards(struct vm *vm, const uint8_t *p, uint32_t n)
{
    struct arcade_scores *s = &vm->scores;
    memcpy(s->id, vm->id, 8);
    s->rate = vm->rate;
    struct cur m = { vm->meta, vm->meta + vm->meta_len, 0 };
    while (vm->meta && !m.bad && m.p < m.end) {
        uint32_t tag = c8(&m), len = c16(&m);
        const uint8_t *b = take(&m, len);
        if (b && tag == 1) {
            uint32_t k = len < sizeof(s->cart) - 1 ? len : sizeof(s->cart) - 1;
            /* cut before a character, not inside one */
            while (k < len && k > 0 && (b[k] & 0xc0) == 0x80)
                k--;
            memcpy(s->cart, b, k);
            s->cart[k] = 0;
        }
    }
    if (!p)
        return 1;
    struct cur c = { p, p + n, 0 };
    uint32_t count = c8(&c);
    if (count > ARCADE_BOARDS)
        return bad(vm, "boards");
    for (uint32_t i = 0; i < count; i++) {
        struct arcade_board *b = &s->b[i];
        b->key = c32(&c);
        b->version = (uint16_t)c16(&c);
        b->low = c8(&c) != 0;
        b->format = (uint8_t)c8(&c);
        copy_text(&c, b->title, sizeof(b->title));
        if (c.bad || b->format > ARCADE_SCORE_TICKS)
            return bad(vm, "boards");
    }
    s->count = (int)count;
    return 1;
}

/* OPTN, the cart's options: a count, then each one's key, type, default,
 * range and step, title, choices and help. Each starts at its default. */
static int load_options(struct vm *vm, const uint8_t *p, uint32_t n)
{
    if (!p)
        return 1;
    struct arcade_options *s = &vm->opts;
    struct cur c = { p, p + n, 0 };
    uint32_t count = c8(&c);
    if (count > ARCADE_OPTIONS)
        return bad(vm, "options");
    for (uint32_t i = 0; i < count; i++) {
        struct arcade_option *o = &s->o[i];
        o->key = c32(&c);
        o->type = (uint8_t)c8(&c);
        o->def = (int32_t)c32(&c);
        o->min = (int32_t)c32(&c);
        o->max = (int32_t)c32(&c);
        o->step = (int32_t)c32(&c);
        copy_text(&c, o->title, sizeof(o->title));
        o->nchoices = (uint8_t)c8(&c);
        if (o->nchoices > ARCADE_CHOICES)
            return bad(vm, "options");
        for (int k = 0; k < o->nchoices; k++)
            copy_text(&c, o->choice[k], sizeof(o->choice[k]));
        o->help_len = (int)c16(&c);
        o->help = (const char *)take(&c, (uint32_t)o->help_len);
        /* the shapes the compiler writes, and no other: the pause menu
         * indexes a choice's texts by value and divides by a number's
         * range */
        if (o->type == ARCADE_OPT_CHOICE ? o->nchoices < 2 || o->min != 0 ||
                                           o->max != o->nchoices - 1
            : o->type == ARCADE_OPT_ONOFF ? o->nchoices || o->min != 0 ||
                                            o->max != 1
            : o->type != ARCADE_OPT_NUMBER || o->nchoices || o->min >= o->max)
            return bad(vm, "options");
        if (c.bad || o->step < 1 || o->def < o->min || o->def > o->max)
            return bad(vm, "options");
        o->value = o->def;
    }
    s->count = (int)count;
    return 1;
}

/* ACHV, the cart's achievements: a count, then each one's key, title and
 * text; none is earned until the host says */
static int load_achievements(struct vm *vm, const uint8_t *p, uint32_t n)
{
    if (!p)
        return 1;
    struct arcade_scores *s = &vm->scores;
    struct cur c = { p, p + n, 0 };
    uint32_t count = c8(&c);
    if (count > ARCADE_ACHIEVEMENTS)
        return bad(vm, "achievements");
    for (uint32_t i = 0; i < count; i++) {
        struct arcade_achievement *a = &s->ach[i];
        a->key = c32(&c);
        copy_text(&c, a->title, sizeof(a->title));
        copy_text(&c, a->text, sizeof(a->text));
        a->date = 0;
    }
    if (c.bad)
        return bad(vm, "achievements");
    s->nach = (int)count;
    return 1;
}

/* The fonts, checked whole: each glyph inside the chunk and no bigger
 * than 32 a side, the codepoints rising */
static int load_fonts(struct vm *vm, const uint8_t *p, uint32_t n)
{
    if (!p)
        return 1;
    struct cur c = { p, p + n, 0 };
    vm->nfonts = c8(&c);
    if (!vm->nfonts || vm->nfonts > MAX_CART_FONTS)
        return bad(vm, "fonts");
    for (uint32_t i = 0; i < vm->nfonts; i++) {
        struct cartfont *f = &vm->fonts[i];
        f->line = (uint8_t)c8(&c);
        f->n = c16(&c);
        f->index = take(&c, f->n * 8);
        if (c.bad || !f->n || !f->line || f->line > 32)
            return bad(vm, "fonts");
        f->glyphs = c.p;
        uint32_t room = (uint32_t)(c.end - c.p), end = 0, last = 0;
        for (uint32_t g = 0; g < f->n; g++) {
            uint32_t cp = rd_le32(f->index + g * 8);
            uint32_t off = rd_le32(f->index + g * 8 + 4);
            if ((g && cp <= last) || off > room || room - off < 5)
                return bad(vm, "fonts");
            const uint8_t *q = f->glyphs + off;
            uint32_t size = 5 + q[4] * ((q[3] + 7u) >> 3);
            if (q[3] > 32 || q[4] > 32 || size > room - off)
                return bad(vm, "fonts");
            if (off + size > end)
                end = off + size;
            last = cp;
        }
        c.p += end;
    }
    return c.p == c.end ? 1 : bad(vm, "fonts");
}

static int load_code(struct vm *vm, const uint8_t *p, uint32_t n)
{
    struct cur c = { p, p + n, 0 };
    vm->nfns = c16(&c);
    vm->nic = c16(&c);
    for (int i = 0; i < 5; i++)
        vm->entry[i] = i < 4 || vm->format >= 2 ? c16(&c) : NONE;
    uint32_t off = perm_alloc(vm, vm->nfns * sizeof(struct fninfo) + 8);
    if (!off)
        return vm_fail(vm, "not enough memory for this game", "", "");
    vm->fns = MEM(vm, off);
    for (uint32_t i = 0; i < vm->nfns; i++) {
        struct fninfo *f = &vm->fns[i];
        f->arity = (uint8_t)c8(&c);
        f->nlocals = (uint8_t)c8(&c);
        f->maxstack = (uint16_t)c16(&c);
        f->off = c32(&c);
        f->len = c32(&c);
    }
    if (c.bad)
        return bad(vm, "function table");
    vm->code = c.p;
    uint32_t avail = (uint32_t)(c.end - c.p);
    for (uint32_t i = 0; i < vm->nfns; i++) {
        struct fninfo *f = &vm->fns[i];
        if (f->off > avail || f->len > avail - f->off || f->len == 0)
            return bad(vm, "function bounds");
    }
    return 1;
}

static uint32_t new_shape(struct vm *vm, uint32_t parent, uint32_t key)
{
    for (uint32_t s = vm->shapes[parent].child; s; s = vm->shapes[s].sibling)
        if (vm->shapes[s].key == key)
            return s;
    if (vm->nshapes >= MAX_SHAPES) {
        vm_fail(vm, "too many different kinds of record", "", "");
        return NONE;
    }
    uint32_t s = vm->nshapes++;
    struct shape *sh = &vm->shapes[s];
    sh->parent = (uint16_t)parent;
    sh->key = (uint16_t)key;
    sh->nkeys = (uint16_t)(vm->shapes[parent].nkeys + 1);
    sh->child = 0;
    sh->sibling = vm->shapes[parent].child;
    vm->shapes[parent].child = (uint16_t)s;
    return s;
}

/* The shape after adding key to parent, made if new; NONE on failure. */
uint32_t vm_shape_add(struct vm *vm, uint32_t parent, uint32_t key)
{
    return new_shape(vm, parent, key);
}

static int load_constants(struct vm *vm, const uint8_t *p, uint32_t n)
{
    struct cur c = { p, p + n, 0 };
    vm->nstrings = c16(&c);
    /* Key ids, plus one, must fit the 16 bits a string header holds. */
    if (vm->nstrings + MAX_RTKEYS >= 0xffff)
        return vm_fail(vm, "this game has too many strings", "", "");
    uint32_t off = perm_alloc(vm, (vm->nstrings + MAX_RTKEYS) * 4 + 8);
    uint32_t koff = perm_alloc(vm, (vm->nstrings + MAX_RTKEYS) * 4 + 8);
    uint32_t hsize = 1;
    while (hsize < 2 * (vm->nstrings + MAX_RTKEYS + 8))
        hsize <<= 1;
    uint32_t hoff = perm_alloc(vm, hsize * 2);
    if (!off || !koff || !hoff)
        return vm_fail(vm, "not enough memory for this game", "", "");
    vm->strings = MEM(vm, off);
    vm->keyoff = MEM(vm, koff);
    vm->keyhash = MEM(vm, hoff);
    vm->hashmask = hsize - 1;
    for (uint32_t i = 0; i < vm->nstrings; i++) {
        uint32_t len = c16(&c);
        const uint8_t *b = take(&c, len);
        if (!b)
            return bad(vm, "strings");
        uint32_t s = perm_alloc(vm, sizeof(struct str) + len + 1);
        if (!s)
            return vm_fail(vm, "not enough memory for this game", "", "");
        struct str *st = MEM(vm, s);
        st->h.info = T_STR;
        st->h.size = (uint32_t)((sizeof(struct str) + len + 7) & ~7u);
        st->len = len;
        memcpy(st->b, b, len);
        vm->strings[i] = s;
        vm->keyoff[i] = s;
        hash_insert(vm, i);
    }
    vm->nkeys = vm->nstrings;

    off = perm_alloc(vm, MAX_SHAPES * sizeof(struct shape));
    if (!off)
        return vm_fail(vm, "not enough memory for this game", "", "");
    vm->shapes = MEM(vm, off);
    vm->nshapes = 1;
    vm->ncshapes = c16(&c);
    off = perm_alloc(vm, vm->ncshapes * 2 + 8);
    if (!off)
        return vm_fail(vm, "not enough memory for this game", "", "");
    vm->cshape = MEM(vm, off);
    for (uint32_t i = 0; i < vm->ncshapes; i++) {
        uint32_t nk = c8(&c), s = 0;
        for (uint32_t k = 0; k < nk; k++) {
            uint32_t key = c16(&c);
            if (key >= vm->nstrings)
                return bad(vm, "shape key");
            for (uint32_t t = s; t; t = vm->shapes[t].parent)
                if (vm->shapes[t].key == key)
                    return bad(vm, "shape repeats a key");
            s = new_shape(vm, s, key);
            if (s == NONE)
                return 0;
        }
        vm->cshape[i] = (uint16_t)s;
    }
    if (c.bad)
        return bad(vm, "constants");
    return 1;
}

static int load_globals(struct vm *vm, const uint8_t *p, uint32_t n)
{
    struct cur c = { p, p + n, 0 };
    vm->nglobals = c16(&c);
    uint32_t off = perm_alloc(vm, vm->nglobals * sizeof(struct value) + 8);
    if (!off)
        return vm_fail(vm, "not enough memory for this game", "", "");
    vm->globals = MEM(vm, off);
    for (uint32_t i = 0; i < vm->nglobals; i++) {
        struct value *g = &vm->globals[i];
        switch (c8(&c)) {
        case 0: *g = mk(T_NULL, 0); break;
        case 1: *g = mk(T_INT, (int32_t)c32(&c)); break;
        case 2: {
            uint32_t s = c16(&c);
            if (s >= vm->nstrings)
                return bad(vm, "global string");
            *g = mk(T_STR, (int32_t)vm->strings[s]);
            break;
        }
        case 4: *g = mk(T_FIX, (int32_t)c32(&c)); break;
        case 5: {
            /* an image: core_draw.c sets it once the images are made */
            uint32_t img = c16(&c);
            if (!vm->img_globals) {
                uint32_t at = perm_alloc(vm, vm->nglobals * 4 + 8);
                if (!at)
                    return vm_fail(vm, "not enough memory for this game", "",
                                   "");
                vm->img_globals = MEM(vm, at);
            }
            vm->img_globals[vm->nimg_globals * 2] = (uint16_t)i;
            vm->img_globals[vm->nimg_globals * 2 + 1] = (uint16_t)img;
            vm->nimg_globals++;
            *g = mk(T_NULL, 0);
            break;
        }
        case 3: {
            uint32_t f = c16(&c);
            if (f >= vm->nfns)
                return bad(vm, "global function");
            *g = mk(T_FN, (int32_t)f);
            break;
        }
        default:
            return bad(vm, "global");
        }
    }
    if (c.bad)
        return bad(vm, "globals");
    return 1;
}

static int load_runtime(struct vm *vm)
{
    static const char *const names[4] =
        { "number", "string", "object", "function" };
    uint32_t off;
    for (int i = 0; i < 4; i++) {
        uint32_t len = (uint32_t)strlen(names[i]);
        off = perm_alloc(vm, sizeof(struct str) + len + 1);
        if (!off)
            return 0;
        struct str *s = MEM(vm, off);
        s->h.info = T_STR;
        s->h.size = (uint32_t)((sizeof(struct str) + len + 7) & ~7u);
        s->len = len;
        memcpy(s->b, names[i], len);
        vm->s_typeof[i] = off;
    }
    off = perm_alloc(vm, vm->nic * sizeof(struct ic) + 8);
    if (!off)
        return 0;
    vm->ic = MEM(vm, off);
    memset(vm->ic, 0xff, vm->nic * sizeof(struct ic));
    off = perm_alloc(vm, MAX_FRAMES * sizeof(struct frame));
    if (!off)
        return 0;
    vm->frames = MEM(vm, off);
    off = perm_alloc(vm, MARK_STACK * 4);
    if (!off)
        return 0;
    vm->mstack = MEM(vm, off);
    off = perm_alloc(vm, STACK_VALUES * sizeof(struct value));
    if (!off)
        return 0;
    vm->stack = MEM(vm, off);
    vm->sp = vm->stack;
    return 1;
}

/* ---- arcade_load() ----------------------------------------------------- */

static int read_chunks(struct vm *vm, arcade_read_fn read, void *ctx)
{
    uint8_t hd[8];
    static const char tags[][5] = { "PBG1", "CODE", "CNST", "GLOB", "LINE",
                                    "PALS", "SHET", "MAPS", "IMGS", "SFX_",
                                    "BORD", "OPTN", "DATA", "ACHV", "FONT" };
    enum { CHUNKS = sizeof(tags) / sizeof(tags[0]) };
    const uint8_t *chunk[CHUNKS] = { 0 };
    uint32_t clen[CHUNKS] = { 0 };
    int first = 1;
    uint32_t pos = 0;               /* where the chunk starts in the file */
    crc_init();
    for (;; pos += 12 + rd_le32(hd + 4)) {
        uint32_t got = read(ctx, hd, 8);
        if (got == 0)
            break;
        if (got != 8)
            return bad(vm, "chunk header");
        uint32_t len = rd_le32(hd + 4);
        /* no chunk is bigger than the memory, and len + 4 must not wrap */
        if (len > vm->size - 4)
            return bad(vm, "chunk length");
        if (first && memcmp(hd, "PBG1", 4))
            return vm_fail(vm, "not a game file", "", "");
        first = 0;
        int want = -1;
        for (int i = 0; i < CHUNKS; i++)
            if (!memcmp(hd, tags[i], 4))
                want = i;
        /* a map's cells or an image's pixels, left in the file: where
         * they are and their checksum, read when they are used */
        int mapc = !memcmp(hd, "MAPC", 4), imgc = !memcmp(hd, "IMGC", 4);
        if (mapc || imgc) {
            uint32_t *nc = mapc ? &vm->nmapc : &vm->nimgc;
            struct lazy **table = mapc ? &vm->mapc : &vm->imgc;
            uint8_t sum[4];
            if (*nc >= LAZY_MAX)
                return vm_fail(vm, "this game has more than 64 maps or "
                               "lazy images", "", "");
            if (!*table) {
                uint32_t at = perm_alloc(vm, LAZY_MAX * sizeof(struct lazy));
                if (!at)
                    return vm_fail(vm, "not enough memory for this game", "",
                                   "");
                *table = MEM(vm, at);
            }
            struct lazy *c = *table + *nc;
            if (read(ctx, NULL, len) != len || read(ctx, sum, 4) != 4)
                return bad(vm, "truncated");
            c->at = pos + 8;
            c->len = len;
            c->crc = rd_le32(sum);
            (*nc)++;
            vm->cart_hash = (vm->cart_hash << 5 | vm->cart_hash >> 27) ^ c->crc;
            continue;
        }
        int is_meta = !memcmp(hd, "META", 4);
        if (want < 0 && !is_meta) {
            if (read(ctx, NULL, len + 4) != len + 4)
                return bad(vm, "truncated");
            continue;
        }
        uint32_t off = perm_alloc(vm, len + 4);
        if (!off)
            return vm_fail(vm, "not enough memory for this game", "", "");
        uint8_t *b = MEM(vm, off);
        if (read(ctx, b, len + 4) != len + 4)
            return bad(vm, "truncated");
        if (crc32(b, len) != rd_le32(b + len))
            return bad(vm, "a checksum fails");
        vm->cart_hash = (vm->cart_hash << 5 | vm->cart_hash >> 27) ^
                        rd_le32(b + len);
        if (is_meta) {
            vm->meta = b;
            vm->meta_len = len;
        } else {
            chunk[want] = b;
            clen[want] = len;
        }
    }
    for (int i = 0; i < 4; i++)
        if (!chunk[i])
            return bad(vm, "a chunk is missing");
    vm->lines = chunk[4];
    vm->lines_len = clen[4];
    vm->pals = chunk[5];
    vm->pals_len = clen[5];
    vm->shet = chunk[6];
    vm->shet_len = clen[6];
    vm->maps = chunk[7];
    vm->maps_len = clen[7];
    vm->imgs = chunk[8];
    vm->imgs_len = clen[8];
    vm->sfx = chunk[9];
    vm->sfx_len = clen[9];
    vm->data = chunk[12];
    vm->data_len = clen[12];
    return load_header(vm, chunk[0], clen[0]) &&
        load_boards(vm, chunk[10], clen[10]) &&
        load_options(vm, chunk[11], clen[11]) &&
        load_achievements(vm, chunk[13], clen[13]) &&
        load_fonts(vm, chunk[14], clen[14]) &&
        load_code(vm, chunk[1], clen[1]) &&
        load_constants(vm, chunk[2], clen[2]) &&
        load_globals(vm, chunk[3], clen[3]);
}

struct vm *arcade_load(void *mem, uint32_t size, arcade_read_fn read,
                       void *rctx, const struct arcade_host *host,
                       uint64_t seed)
{
    if (size < sizeof(struct vm) + 64 || ((uintptr_t)mem & 7))
        return NULL;
    struct vm *vm = mem;
    memset(vm, 0, sizeof(*vm));
    vm->mem = mem;
    vm->size = size;
    vm->host = host;
    vm->perm = (uint32_t)sizeof(struct vm);
    vm->ceiling = 0xffffffffu;
    vm->native = -1;
    vm->wheelpos = -1;          /* untouched, until the host says */
    core_seed(vm, seed);
    if (!read_chunks(vm, read, rctx))
        return vm;
    if (!load_runtime(vm)) {
        vm_fail(vm, "not enough memory for this game", "", "");
        return vm;
    }
    if (!verify_cart(vm))
        return vm;
    if (!core_draw_init(vm) || !sound_load(vm)) {
        vm_fail(vm, "not enough memory for this game", "", "");
        return vm;
    }
    vm->heap_lo = (vm->perm + 7) & ~7u;
    vm->heap_hi = vm->size & ~7u;
    if (vm->heap_hi < vm->heap_lo + 16384) {
        vm_fail(vm, "not enough memory for this game", "", "");
        return vm;
    }
    gc_init(vm);
    return vm;
}

/* ---- where an error happened ---------------------------------------- */

struct where
{
    char file[40];
    char fn[48];
    int line;
};

static void copy_name(char *dst, uint32_t cap, const uint8_t *b, uint32_t n)
{
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, b, n);
    dst[n] = 0;
}

/* Fills w from LINE for err_fn and err_pc; returns 0 when unknown. */
static int find_where(const struct vm *vm, struct where *w)
{
    if (!vm->lines || !vm->err_where)
        return 0;
    struct cur c = { vm->lines, vm->lines + vm->lines_len, 0 };
    const uint8_t *files[64];
    uint32_t flen[64];
    uint32_t nfiles = c16(&c), i, n;
    for (i = 0; i < nfiles; i++) {
        n = c16(&c);
        const uint8_t *b = take(&c, n);
        if (i < 64) {
            files[i] = b;
            flen[i] = n;
        }
    }
    for (i = c16(&c); i > 0; i--)
        take(&c, c16(&c));
    for (uint32_t f = 0; f < vm->nfns && !c.bad; f++) {
        n = c16(&c);
        const uint8_t *name = take(&c, n);
        uint32_t file = c8(&c);
        uint32_t pairs = c16(&c);
        if (f != vm->err_fn) {
            take(&c, pairs * 4);
            continue;
        }
        if (c.bad)
            return 0;
        copy_name(w->fn, sizeof(w->fn), name, n);
        w->file[0] = 0;
        if (file < nfiles && file < 64)
            copy_name(w->file, sizeof(w->file), files[file], flen[file]);
        w->line = 0;
        for (i = 0; i < pairs; i++) {
            uint32_t pc = c16(&c), line = c16(&c);
            if (pc <= vm->err_pc)
                w->line = (int)line;
        }
        return !c.bad;
    }
    return 0;
}

const char *arcade_error(const struct vm *vm, const char **file, int *line,
                         const char **fn)
{
    static struct where w;
    if (!vm->err)
        return NULL;
    if (find_where(vm, &w)) {
        if (file) *file = w.file;
        if (line) *line = w.line;
        if (fn) *fn = w.fn;
    } else {
        if (file) *file = NULL;
        if (line) *line = 0;
        if (fn) *fn = NULL;
    }
    return vm->msg;
}
