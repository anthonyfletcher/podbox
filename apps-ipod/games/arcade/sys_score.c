/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade services: high scores and achievements. A cart declares its
 * boards and achievements in cart.json (the loader reads them from BORD
 * and ACHV); each board keeps its best ten, with the date and an optional
 * name, and each achievement the date it was earned. The host keeps them
 * in a file, through the record packed here, shows them in the pause
 * menu, and announces an achievement as it is earned.
 *
 * Parts, in order:
 *   the table     ranking a value, inserting it
 *   the record    packing the scores for the host's file, and back
 *   the text      a value as its board shows it
 *   natives       score.submit, best, get, name, count, show; ach.earn,
 *                 ach.has
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)
#define RET_INT(v) (*out = mk(T_INT, (int32_t)(v)), 1)

/* ---- the table ----------------------------------------------------------- */

/* Whether a beats b on this board; a tie keeps the earlier entry first */
static int beats(const struct arcade_board *b, int32_t a, int32_t than)
{
    return b->low ? a < than : a > than;
}

/* Records value on board b: its rank 1-10, or 0 when it is not in the ten */
static int insert(struct vm *vm, struct arcade_board *b, int32_t value,
                  const char *name)
{
    int at = 0;
    while (at < b->count && !beats(b, value, b->e[at].value))
        at++;
    if (at >= ARCADE_RANKS)
        return 0;
    int last = b->count < ARCADE_RANKS ? b->count : ARCADE_RANKS - 1;
    memmove(&b->e[at + 1], &b->e[at], (size_t)(last - at) * sizeof(b->e[0]));
    struct arcade_entry *e = &b->e[at];
    memset(e, 0, sizeof(*e));
    e->value = value;
    if (vm->host->date) {
        int32_t d[7];
        vm->host->date(vm->host->ctx, d);
        e->date = d[0] * 10000 + d[1] * 100 + d[2];
    }
    memcpy(e->name, name, strlen(name));
    if (b->count < ARCADE_RANKS)
        b->count++;
    vm->scores_dirty = 1;
    return at + 1;
}

struct arcade_scores *arcade_scores(struct vm *vm, int *dirty)
{
    if (dirty) {
        *dirty = vm->scores_dirty;
        vm->scores_dirty = 0;
    }
    return &vm->scores;
}

void arcade_scores_restore(struct vm *vm, const struct arcade_scores *saved)
{
    struct arcade_scores *s = &vm->scores;
    s->plays = saved->plays;
    s->seconds = saved->seconds;
    for (int i = 0; i < s->count; i++)
        for (int j = 0; j < saved->count; j++) {
            const struct arcade_board *o = &saved->b[j];
            if (o->key != s->b[i].key || o->version != s->b[i].version)
                continue;
            s->b[i].count = o->count;
            memcpy(s->b[i].e, o->e, sizeof(o->e));
        }
    for (int i = 0; i < s->nach; i++)
        for (int j = 0; j < saved->nach; j++)
            if (saved->ach[j].key == s->ach[i].key)
                s->ach[i].date = saved->ach[j].date;
}

int arcade_score_shown(struct vm *vm, int *rank)
{
    int b = vm->show_board - 1;
    *rank = vm->show_rank;
    vm->show_board = 0;
    return b;
}

/* ---- the record ---------------------------------------------------------- *
 * "PBSC", a version byte, the cart's id, rate, plays, seconds and name,
 * then each board: key, version, low, format, title, count and entries;
 * then (from version 2) the achievements: a count, and each one's key,
 * date, title and text. Little-endian; text is a length byte and its
 * bytes. */

struct out
{
    uint8_t *p, *end;
};

static void put(struct out *o, const void *b, int n)
{
    if (o->p && o->end - o->p >= n) {
        memcpy(o->p, b, (size_t)n);
        o->p += n;
    } else {
        o->p = NULL;
    }
}

static void put32(struct out *o, uint32_t v)
{
    uint8_t b[4];
    wr_le32(b, v);
    put(o, b, 4);
}

static void put_text(struct out *o, const char *s, int cap)
{
    uint8_t n = 0;
    while (n < cap - 1 && s[n])
        n++;
    put(o, &n, 1);
    put(o, s, n);
}

int arcade_scores_pack(const struct arcade_scores *s, uint8_t *buf, int cap)
{
    struct out o = { buf, buf + cap };
    uint8_t v[4] = { 2, s->rate, (uint8_t)s->count, 0 };
    put(&o, "PBSC", 4);
    put(&o, v, 4);
    put(&o, s->id, 8);
    put32(&o, s->plays);
    put32(&o, s->seconds);
    put_text(&o, s->cart, sizeof(s->cart));
    for (int i = 0; i < s->count; i++) {
        const struct arcade_board *b = &s->b[i];
        uint8_t f[4] = { (uint8_t)b->version, (uint8_t)(b->version >> 8),
                         b->low, b->format };
        put32(&o, b->key);
        put(&o, f, 4);
        put_text(&o, b->title, sizeof(b->title));
        put(&o, &b->count, 1);
        for (int k = 0; k < b->count; k++) {
            put32(&o, (uint32_t)b->e[k].value);
            put32(&o, (uint32_t)b->e[k].date);
            put_text(&o, b->e[k].name, sizeof(b->e[k].name));
        }
    }
    uint8_t na = (uint8_t)s->nach;
    put(&o, &na, 1);
    for (int i = 0; i < s->nach; i++) {
        put32(&o, s->ach[i].key);
        put32(&o, (uint32_t)s->ach[i].date);
        put_text(&o, s->ach[i].title, sizeof(s->ach[i].title));
        put_text(&o, s->ach[i].text, sizeof(s->ach[i].text));
    }
    return o.p ? (int)(o.p - buf) : 0;
}

struct in
{
    const uint8_t *p, *end;
    int bad;
};

static const uint8_t *get(struct in *c, int n)
{
    if (c->bad || c->end - c->p < n) {
        c->bad = 1;
        return NULL;
    }
    const uint8_t *r = c->p;
    c->p += n;
    return r;
}

static uint32_t get32(struct in *c)
{
    const uint8_t *b = get(c, 4);
    return b ? rd_le32(b) : 0;
}

static void get_text(struct in *c, char *dst, int cap)
{
    const uint8_t *n = get(c, 1);
    const uint8_t *b = n ? get(c, n[0]) : NULL;
    int k = b ? (n[0] < cap - 1 ? n[0] : cap - 1) : 0;
    if (b)
        memcpy(dst, b, (size_t)k);
    dst[k] = 0;
}

int arcade_scores_unpack(struct arcade_scores *s, const uint8_t *buf, int n)
{
    struct in c = { buf, buf + n, 0 };
    memset(s, 0, sizeof(*s));
    const uint8_t *m = get(&c, 4), *v = get(&c, 4), *id = get(&c, 8);
    if (c.bad || memcmp(m, "PBSC", 4) || v[0] < 1 || v[0] > 2 ||
        v[2] > ARCADE_BOARDS)
        return 0;
    s->rate = v[1];
    s->count = v[2];
    memcpy(s->id, id, 8);
    s->plays = get32(&c);
    s->seconds = get32(&c);
    get_text(&c, s->cart, sizeof(s->cart));
    for (int i = 0; i < s->count && !c.bad; i++) {
        struct arcade_board *b = &s->b[i];
        b->key = get32(&c);
        const uint8_t *f = get(&c, 4);
        if (!f)
            break;
        b->version = (uint16_t)(f[0] | f[1] << 8);
        b->low = f[2];
        b->format = f[3];
        get_text(&c, b->title, sizeof(b->title));
        const uint8_t *k = get(&c, 1);
        if (!k || k[0] > ARCADE_RANKS)
            return 0;
        b->count = k[0];
        for (int j = 0; j < b->count; j++) {
            b->e[j].value = (int32_t)get32(&c);
            b->e[j].date = (int32_t)get32(&c);
            get_text(&c, b->e[j].name, sizeof(b->e[j].name));
        }
    }
    if (v[0] >= 2) {
        const uint8_t *na = get(&c, 1);
        if (!na || na[0] > ARCADE_ACHIEVEMENTS)
            return 0;
        s->nach = na[0];
        for (int i = 0; i < s->nach && !c.bad; i++) {
            s->ach[i].key = get32(&c);
            s->ach[i].date = (int32_t)get32(&c);
            get_text(&c, s->ach[i].title, sizeof(s->ach[i].title));
            get_text(&c, s->ach[i].text, sizeof(s->ach[i].text));
        }
    }
    return !c.bad;
}

/* ---- the text ------------------------------------------------------------ */

/* Writes backwards from at: sep (when not 0), then n in decimal before it,
 * at least width digits; returns where the digits start */
static char *digits(char *at, uint64_t n, int width, char sep)
{
    if (sep)
        *--at = sep;
    do {
        *--at = (char)('0' + n % 10);
        n /= 10;
        width--;
    } while (n || width > 0);
    return at;
}

void arcade_score_text(const struct arcade_scores *s, int board,
                       int32_t value, char *buf, int cap)
{
    char t[32], *at = t + sizeof(t);
    int64_t v = value;
    int neg = v < 0;
    if (neg)
        v = -v;
    *--at = 0;
    if (s->b[board].format == ARCADE_SCORE_NUMBER) {
        at = digits(at, (uint64_t)v, 1, 0);
    } else {
        /* a time: hundredths, from ms or from ticks at the cart's rate */
        uint64_t cs = (uint64_t)(s->b[board].format == ARCADE_SCORE_TIME
                                 ? v / 10 : v * 100 / (s->rate ? s->rate : 33));
        uint64_t h = cs / 360000;
        at = digits(at, cs % 100, 2, 0);
        at = digits(at, cs / 100 % 60, 2, '.');
        at = digits(at, h ? cs / 6000 % 60 : cs / 6000, h ? 2 : 1, ':');
        if (h)
            at = digits(at, h, 1, ':');
    }
    if (neg)
        *--at = '-';
    int n = (int)(t + sizeof(t) - at);
    if (n > cap) {
        n = cap;
        at[n - 1] = 0;
    }
    memcpy(buf, at, (size_t)n);
}

/* ---- natives ------------------------------------------------------------- */

/* a[0] as one of the cart's boards, or the cart stopped */
static struct arcade_board *score_board(struct vm *vm, struct value v,
                                        const char *fn)
{
    if (v.tag != T_INT || v.v < 0 || v.v >= vm->scores.count) {
        vm_fail(vm, fn, "() needs one of the cart's boards, BOARD.<name>", "");
        return NULL;
    }
    return &vm->scores.b[v.v];
}

/* a[i] as a rank of b, 1 to its count, or -1 when there is none */
static int rank_of(const struct arcade_board *b, struct value v)
{
    if (!is_num(v))
        return -1;
    int32_t r = floor_int(v);
    return r >= 1 && r <= b->count ? r - 1 : -1;
}

/* score.submit(board, value, [name]): records it; its rank 1-10, or 0 */
NATIVE(score_submit)
{
    struct arcade_board *b = score_board(vm, a[0], "score.submit");
    if (!b)
        return 0;
    if (!is_num(a[1]))
        return vm_fail(vm, "score.submit() needs a number to record", "", "");
    char name[16];
    uint32_t n = 0;
    if (argc > 2 && a[2].tag != T_NULL) {
        n = vm_text(vm, a[2], name, sizeof(name) - 1);
        n = utf8_whole(name, n);
    }
    name[n] = 0;
    int rank = insert(vm, b, floor_int(a[1]), name);
    vm->last_rank[a[0].v] = (int8_t)rank;
    vm_charge(vm, 50);
    return RET_INT(rank);
}

/* score.best(board): the top value, or null when the board is empty */
NATIVE(score_best)
{
    (void)argc;
    struct arcade_board *b = score_board(vm, a[0], "score.best");
    if (!b)
        return 0;
    vm_charge(vm, 10);
    *out = b->count ? mk(T_INT, b->e[0].value) : mk(T_NULL, 0);
    return 1;
}

/* score.get(board, rank): the value at rank 1-10, or null */
NATIVE(score_get)
{
    (void)argc;
    struct arcade_board *b = score_board(vm, a[0], "score.get");
    if (!b)
        return 0;
    int r = rank_of(b, a[1]);
    vm_charge(vm, 10);
    *out = r >= 0 ? mk(T_INT, b->e[r].value) : mk(T_NULL, 0);
    return 1;
}

/* score.name(board, rank): the name given with it, "" for none, or null
 * when there is no such rank */
NATIVE(score_name)
{
    (void)argc;
    struct arcade_board *b = score_board(vm, a[0], "score.name");
    if (!b)
        return 0;
    int r = rank_of(b, a[1]);
    if (r < 0) {
        *out = mk(T_NULL, 0);
        vm_charge(vm, 10);
        return 1;
    }
    const char *n = b->e[r].name;
    vm_charge(vm, 10);
    return vm_new_string(vm, (const uint8_t *)n, (uint32_t)strlen(n), out);
}

/* score.count(board): how many it holds, 0-10 */
NATIVE(score_count)
{
    (void)argc;
    struct arcade_board *b = score_board(vm, a[0], "score.count");
    if (!b)
        return 0;
    vm_charge(vm, 10);
    return RET_INT(b->count);
}

/* score.show(board): the engine's table, after this tick (the last asked
 * for, when several are), with the board's latest score marked when it
 * made the ten; the cart's next tick follows its closing */
NATIVE(score_show)
{
    (void)argc; (void)out;
    if (!score_board(vm, a[0], "score.show"))
        return 0;
    vm->show_board = (int8_t)(a[0].v + 1);
    vm->show_rank = vm->last_rank[a[0].v];
    vm_charge(vm, 10);
    return 1;
}

/* ach.* takes one of the cart's achievements, ACH.<name> */
static struct arcade_achievement *ach_of(struct vm *vm, struct value v,
                                         const char *fn)
{
    if (v.tag != T_INT || v.v < 0 || v.v >= vm->scores.nach) {
        vm_fail(vm, fn, "() needs one of the cart's achievements, ACH.<name>",
                "");
        return NULL;
    }
    return &vm->scores.ach[v.v];
}

int arcade_achieved(struct vm *vm)
{
    if (!vm->nachieved)
        return -1;
    int i = vm->achieved[0];
    memmove(vm->achieved, vm->achieved + 1, --vm->nachieved);
    return i;
}

/* ach.earn(ACH.name): true when it is earned now, for the first time --
 * the player then announces it; false when it was already */
NATIVE(ach_earn)
{
    (void)argc;
    struct arcade_achievement *e = ach_of(vm, a[0], "ach.earn");
    if (!e)
        return 0;
    vm_charge(vm, 20);
    if (e->date)
        return RET_INT(0);
    e->date = 1;
    if (vm->host->date) {
        int32_t d[7];
        vm->host->date(vm->host->ctx, d);
        e->date = d[0] * 10000 + d[1] * 100 + d[2];
    }
    vm->scores_dirty = 1;
    if (vm->nachieved < sizeof(vm->achieved))
        vm->achieved[vm->nachieved++] = (int8_t)a[0].v;
    return RET_INT(1);
}

/* ach.has(ACH.name): whether it has been earned */
NATIVE(ach_has)
{
    (void)argc;
    struct arcade_achievement *e = ach_of(vm, a[0], "ach.has");
    if (!e)
        return 0;
    vm_charge(vm, 10);
    return RET_INT(e->date != 0);
}
