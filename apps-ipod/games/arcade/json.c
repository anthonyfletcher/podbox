/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade: JSON to a cart's values and back, for data files (data.read) and
 * the cart's storage (store.read, store.write). Arrays and records become
 * the cart's arrays and records, strings its strings, true and false 1 and
 * 0. A whole number is an integer; any other a fraction (16.16), written
 * with five decimals, which reads back to the same bits. No floating point.
 *
 * Parts, in order:
 *   reading   numbers, strings, and the values, nested through a holder
 *             array the collector sees
 *   writing   a value as text, counted first so its string can be made
 *             to size
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"
#include "games/arcade/json.h"

#define DEPTH 64            /* containers inside one another */
#define SPEND_BYTES 64      /* a unit charged for each this many bytes */

/* ---- reading ------------------------------------------------------------- */

struct jin
{
    const uint8_t *p, *start, *end;
    const char *what;           /* the file, for the message */
};

static void ws(struct jin *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' ||
                             *j->p == '\n' || *j->p == '\r'))
        j->p++;
}

/* n in decimal into buf, ended */
static void decimal(uint32_t n, char *buf)
{
    char t[12];
    int k = 0;
    do
        t[k++] = (char)('0' + n % 10);
    while (n /= 10);
    while (k)
        *buf++ = t[--k];
    *buf = 0;
}

static int bad(struct vm *vm, struct jin *j, const char *why)
{
    static char at[48];
    memcpy(at, " at byte ", 9);
    decimal((uint32_t)(j->p - j->start), at + 9);
    return vm_fail(vm, j->what, why, at);
}

static int lit(struct jin *j, const char *w)
{
    size_t n = strlen(w);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, w, n))
        return 0;
    j->p += n;
    return 1;
}

/* A number: a fraction when written with digits after its point that its
 * exponent does not cancel, as in a cart's code (2.0 is a fraction, 1.5e1
 * the integer 15), within the cart's range and rounded to the nearest
 * 1/65536; otherwise an integer, within 32 bits */
static int number(struct vm *vm, struct jin *j, struct value *out)
{
    int neg = 0, digits = 0;
    int64_t mant = 0;
    int32_t exp10 = 0;
    if (j->p < j->end && *j->p == '-') {
        neg = 1;
        j->p++;
    }
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        if (mant < 100000000000000000LL)
            mant = mant * 10 + (*j->p - '0');
        else
            exp10++;
        j->p++;
        digits++;
    }
    if (j->p < j->end && *j->p == '.') {
        j->p++;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
            if (mant < 100000000000000000LL) {
                mant = mant * 10 + (*j->p - '0');
                exp10--;
            }
            j->p++;
            digits++;
        }
    }
    if (!digits)
        return bad(vm, j, ": a number with no digits");
    if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
        int eneg = 0, e = 0;
        j->p++;
        if (j->p < j->end && (*j->p == '+' || *j->p == '-'))
            eneg = *j->p++ == '-';
        if (j->p >= j->end || *j->p < '0' || *j->p > '9')
            return bad(vm, j, ": an exponent with no digits");
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
            if (e < 1000)
                e = e * 10 + (*j->p - '0');
            j->p++;
        }
        exp10 += eneg ? -e : e;
    }
    if (mant == 0) {
        *out = mk(exp10 < 0 ? T_FIX : T_INT, 0);
        return 1;
    }
    /* whole: scaled up while it stays within 32 bits */
    while (exp10 > 0 && mant <= 0x80000000LL) {
        mant *= 10;
        exp10--;
    }
    if (exp10 >= 0) {
        if (exp10 > 0 || mant > 0x80000000LL || (!neg && mant > 0x7fffffffLL))
            return bad(vm, j, ": a number beyond a cart's");
        *out = mk(T_INT, (int32_t)(neg ? -mant : mant));
        return 1;
    }
    /* a fraction: mant / 10^-exp10 in 16.16, kept within 64 bits */
    while (mant > 10000000000000LL && exp10 < 0) {
        mant = (mant + 5) / 10;
        exp10++;
    }
    while (exp10 < -18) {
        mant = (mant + 5) / 10;
        exp10++;
    }
    int64_t den = 1;
    for (int32_t k = exp10; k < 0; k++)
        den *= 10;
    /* the whole part checked first: mant * 65536 must not overflow */
    if (mant / den > 32767)
        return bad(vm, j, ": a fraction beyond a cart's (-32768 to 32767)");
    int64_t raw = (mant * 65536 + den / 2) / den;
    if (raw >= 0x80000000LL)
        return bad(vm, j, ": a fraction beyond a cart's (-32768 to 32767)");
    *out = mk(T_FIX, (int32_t)(neg ? -raw : raw));
    return 1;
}

static int hex4(struct jin *j, uint32_t *u)
{
    *u = 0;
    if (j->end - j->p < 4)
        return 0;
    for (int i = 0; i < 4; i++) {
        uint8_t c = *j->p++;
        uint32_t d = c >= '0' && c <= '9' ? c - '0'
                   : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10
                   : 99;
        if (d > 15)
            return 0;
        *u = *u << 4 | d;
    }
    return 1;
}

/* The string from j->p, just past its opening quote, decoded into o, or
 * only measured when o is NULL; its length, or -1 when it is not one */
static int32_t string_pass(struct jin *j, uint8_t *o)
{
    uint32_t n = 0;
    while (j->p < j->end) {
        uint8_t c = *j->p++;
        if (c == '"')
            return (int32_t)n;
        if (c < 0x20)
            return -1;
        if (c != '\\') {
            if (o) o[n] = c;
            n++;
            continue;
        }
        if (j->p >= j->end)
            return -1;
        c = *j->p++;
        uint32_t u;
        switch (c) {
        case '"': case '\\': case '/': u = c; break;
        case 'b': u = 8; break;
        case 'f': u = 12; break;
        case 'n': u = 10; break;
        case 'r': u = 13; break;
        case 't': u = 9; break;
        case 'u':
            if (!hex4(j, &u))
                return -1;
            if (u >= 0xd800 && u < 0xdc00) {
                /* a pair, or a lone half: the replacement character */
                const uint8_t *back = j->p;
                uint32_t lo;
                if (j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                    j->p += 2;
                    if (hex4(j, &lo) && lo >= 0xdc00 && lo < 0xe000) {
                        u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
                        break;
                    }
                }
                j->p = back;
                u = 0xfffd;
            } else if (u >= 0xdc00 && u < 0xe000) {
                u = 0xfffd;
            }
            break;
        default:
            return -1;
        }
        n += (uint32_t)(o ? utf8_put(o + n, u) : utf8_bytes(u));
    }
    return -1;
}

static int string(struct vm *vm, struct jin *j, struct value *out)
{
    const uint8_t *from = j->p;
    int32_t n = string_pass(j, NULL);
    if (n < 0)
        return bad(vm, j, ": a string not closed, or with a bad escape");
    if (!vm_spend(vm, (uint32_t)n / SPEND_BYTES) ||
        !vm_new_text(vm, (uint32_t)n, out))
        return 0;
    j->p = from;
    string_pass(j, STR(vm, *out)->b);
    return 1;
}

/* The holder's top, n down from it (0 the top) */
static struct value held(struct vm *vm, struct value h, uint32_t down)
{
    struct arr *a = ARR(vm, h);
    return BUF(vm, a->buf)->v[a->len - 1 - down];
}

/* v pushed onto the holder: kept from the collector while the holder
 * grows, since nothing else holds it yet */
static int hold(struct vm *vm, struct value h, struct value v)
{
    if (!vm_root(vm, v))
        return 0;
    int ok = vm_push(vm, h, v);
    vm_unroot(vm);
    return ok;
}

/* v, just completed and the holder's top, given to what holds it: the
 * record under its key, or the array; 1 with *done when it was the whole */
static int fold(struct vm *vm, struct value h, int *done)
{
    uint32_t n = ARR(vm, h)->len;
    if (n == 1) {
        *done = 1;
        return 1;
    }
    struct value v = held(vm, h, 0), under = held(vm, h, 1);
    if (under.tag == T_STR) {
        if (!vm_set(vm, held(vm, h, 2), under, v))
            return 0;
        ARR(vm, h)->len = n - 2;
    } else {
        if (!vm_push(vm, under, v))
            return 0;
        ARR(vm, h)->len = n - 1;
    }
    return 1;
}

int json_read(struct vm *vm, const uint8_t *text, uint32_t len,
              const char *what, struct value *out)
{
    struct jin j = { text, text, text + len, what };
    struct value h, v;
    int done = 0, depth = 0;
    if (!vm_new_array(vm, 0, &h) || !vm_root(vm, h))
        return 0;
    for (;;) {
        /* a value: opened, or read whole and folded in */
        ws(&j);
        if (!vm_spend(vm, 1))
            goto fail;
        if (j.p >= j.end) {
            bad(vm, &j, ": the text ends early");
            goto fail;
        }
        uint8_t c = *j.p;
        int open = 0;
        if (c == '[' || c == '{') {
            if (++depth > DEPTH) {
                bad(vm, &j, ": more than 64 lists inside one another");
                goto fail;
            }
            j.p++;
            if (!(c == '[' ? vm_new_array(vm, 0, &v)
                           : vm_new_record(vm, NULL, 0, &v)) ||
                !hold(vm, h, v))
                goto fail;
            open = 1;
        } else if (c == '"') {
            j.p++;
            if (!string(vm, &j, &v) || !hold(vm, h, v))
                goto fail;
        } else if (lit(&j, "true")) {
            v = mk(T_INT, 1);
        } else if (lit(&j, "false") || lit(&j, "null")) {
            v = j.p[-1] == 'e' ? mk(T_INT, 0) : mk(T_NULL, 0);
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            if (!number(vm, &j, &v))
                goto fail;
        } else {
            bad(vm, &j, ": not a value");
            goto fail;
        }
        if (v.tag != T_STR && !open && !hold(vm, h, v))
            goto fail;
        /* what follows: a container's first key or element, a comma, or
         * closers that complete values, each folded in */
        int close = !open;
        for (;;) {
            ws(&j);
            if (open) {
                struct value top = held(vm, h, 0);
                uint8_t want = top.tag == T_ARR ? ']' : '}';
                if (j.p < j.end && *j.p == want) {
                    j.p++;
                    depth--;
                    open = 0;
                    close = 1;
                    continue;
                }
                if (top.tag == T_ARR)
                    break;              /* its first element */
                goto key;
            }
            if (close) {
                if (!fold(vm, h, &done))
                    goto fail;
                if (done) {
                    ws(&j);
                    if (j.p != j.end) {
                        bad(vm, &j, ": text after the end");
                        goto fail;
                    }
                    *out = held(vm, h, 0);
                    vm_unroot(vm);
                    return 1;
                }
                close = 0;
            }
            /* inside the container now on top */
            struct value top = held(vm, h, 0);
            uint8_t want = top.tag == T_ARR ? ']' : '}';
            if (j.p < j.end && *j.p == want) {
                j.p++;
                depth--;
                close = 1;
                continue;
            }
            if (j.p >= j.end || *j.p != ',') {
                bad(vm, &j, j.p >= j.end ? ": the text ends early"
                            : top.tag == T_ARR ? ": a comma or ] wanted"
                                               : ": a comma or } wanted");
                goto fail;
            }
            j.p++;
            if (top.tag == T_ARR)
                break;                  /* its next element */
        key:
            ws(&j);
            if (j.p >= j.end || *j.p != '"') {
                bad(vm, &j, ": a name in quotes wanted");
                goto fail;
            }
            j.p++;
            if (!string(vm, &j, &v) || !hold(vm, h, v))
                goto fail;
            ws(&j);
            if (j.p >= j.end || *j.p != ':') {
                bad(vm, &j, ": a colon wanted");
                goto fail;
            }
            j.p++;
            break;                      /* its value */
        }
    }
fail:
    vm_unroot(vm);
    return 0;
}

/* ---- writing ------------------------------------------------------------- */

struct jout
{
    uint8_t *p;                 /* NULL: only counting */
    uint32_t n;
    uint32_t most;              /* bytes it may take */
    int full;                   /* more were wanted: the rest is skipped */
};

static void put(struct jout *o, const void *b, uint32_t n)
{
    if (o->full || n > o->most - o->n) {
        o->full = 1;
        return;
    }
    if (o->p)
        memcpy(o->p + o->n, b, n);
    o->n += n;
}

static void put_c(struct jout *o, char c)
{
    put(o, &c, 1);
}

static void put_int(struct jout *o, int64_t v)
{
    char t[24];
    if (v < 0) {
        put_c(o, '-');
        v = -v;
    }
    decimal((uint32_t)v, t);    /* within 32 bits: an int's magnitude */
    put(o, t, (uint32_t)strlen(t));
}

/* A fraction, five decimals or fewer: enough that reading it gives the
 * same 16.16 back; a whole one as 2.0, so it reads back a fraction */
static void put_fix(struct jout *o, int32_t raw)
{
    int64_t a = raw;
    if (a < 0) {
        put_c(o, '-');
        a = -a;
    }
    int64_t whole = a >> 16, d = ((a & 0xffff) * 100000 + 32768) >> 16;
    if (d == 100000) {
        whole++;
        d = 0;
    }
    put_int(o, whole);
    if (!d) {
        put(o, ".0", 2);
        return;
    }
    char t[6];
    for (int i = 4; i >= 0; i--, d /= 10)
        t[i] = (char)('0' + d % 10);
    int k = 5;
    while (t[k - 1] == '0')
        k--;
    put_c(o, '.');
    put(o, t, (uint32_t)k);
}

static void put_string(struct jout *o, const uint8_t *b, uint32_t n)
{
    static const char hex[] = "0123456789abcdef";
    put_c(o, '"');
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c == '"' || c == '\\') {
            put_c(o, '\\');
            put_c(o, (char)c);
        } else if (c == '\n') {
            put(o, "\\n", 2);
        } else if (c == '\t') {
            put(o, "\\t", 2);
        } else if (c < 0x20) {
            char u[6] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };
            put(o, u, 6);
        } else {
            put_c(o, (char)c);
        }
    }
    put_c(o, '"');
}

static int write_value(struct vm *vm, struct jout *o, struct value v,
                       int depth, const char *what)
{
    if (depth > DEPTH)
        return vm_fail(vm, what, ": more than 64 lists inside one another, "
                       "or a list inside itself", "");
    if (o->full)
        return 1;
    if (!vm_spend(vm, 1))
        return 0;
    switch (v.tag) {
    case T_NULL:
        put(o, "null", 4);
        return 1;
    case T_INT:
        put_int(o, v.v);
        return 1;
    case T_FIX:
        put_fix(o, v.v);
        return 1;
    case T_STR:
        put_string(o, STR(vm, v)->b, STR(vm, v)->len);
        return vm_spend(vm, STR(vm, v)->len / SPEND_BYTES);
    case T_I32:
    case T_U8:
    case T_FXA: {
        uint32_t n = v.tag == T_U8 ? U8A(vm, v)->len : I32(vm, v)->len;
        put_c(o, '[');
        for (uint32_t i = 0; i < n; i++) {
            if (i)
                put_c(o, ',');
            if (v.tag == T_FXA)
                put_fix(o, I32(vm, v)->d[i]);
            else
                put_int(o, v.tag == T_I32 ? I32(vm, v)->d[i]
                                          : U8A(vm, v)->d[i]);
        }
        put_c(o, ']');
        return vm_spend(vm, n / 16);
    }
    case T_ARR: {
        put_c(o, '[');
        for (uint32_t i = 0; i < ARR(vm, v)->len && !o->full; i++) {
            if (i)
                put_c(o, ',');
            struct value e = BUF(vm, ARR(vm, v)->buf)->v[i];
            if (!write_value(vm, o, e, depth + 1, what))
                return 0;
        }
        put_c(o, ']');
        return 1;
    }
    case T_OBJ: {
        struct value name, e;
        /* vm_field() walks from the last key back to the i'th */
        uint32_t keys = vm->shapes[OBJ(vm, v)->shape].nkeys;
        put_c(o, '{');
        for (uint32_t i = 0; !o->full && vm_field(vm, v, i, &name, &e); i++) {
            if (!vm_spend(vm, (keys - i) / 8))
                return 0;
            if (i)
                put_c(o, ',');
            put_string(o, STR(vm, name)->b, STR(vm, name)->len);
            put_c(o, ':');
            if (!write_value(vm, o, e, depth + 1, what))
                return 0;
        }
        put_c(o, '}');
        return 1;
    }
    default:
        return vm_fail(vm, what, ": cannot keep a ", vm_typename(v.tag));
    }
}

int json_write(struct vm *vm, struct value v, const char *what,
               uint32_t most, struct value *out)
{
    struct jout count = { NULL, 0, most, 0 };
    if (!write_value(vm, &count, v, 0, what))
        return 0;
    if (count.full)
        return 2;
    if (!vm_root(vm, v))
        return 0;
    int ok = vm_new_text(vm, count.n, out);
    vm_unroot(vm);
    if (!ok)
        return 0;
    struct jout text = { STR(vm, *out)->b, 0, count.n, 0 };
    return write_value(vm, &text, v, 0, what);
}
