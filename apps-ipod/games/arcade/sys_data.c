/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade services: a cart's data. Its data files (the DATA chunk, from the
 * .json and .txt files in the cart folder's data folder) read as values,
 * and its own stored files, written and read back as JSON through struct
 * arcade_store.
 *
 * Parts, in order:
 *   data files    finding one in DATA
 *   storage       names, and the natives store.read, write, list, remove
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"
#include "games/arcade/json.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)
#define RET_INT(v) (*out = mk(T_INT, (int32_t)(v)), 1)

static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* ---- data files ---------------------------------------------------------- *
 * DATA: a count, then each file's kind (0 JSON, 1 text), its name and its
 * bytes. The compiler has checked each JSON file reads. */

/* data.read(DATA.name): the file's value, made afresh at each call -- a
 * cart keeps what it reads rather than reading every tick */
NATIVE(data_read)
{
    (void)argc;
    const uint8_t *p = vm->data, *end = vm->data + vm->data_len;
    uint32_t count = p && vm->data_len >= 2 ? rd16(p) : 0;
    if (a[0].tag != T_INT || a[0].v < 0 || (uint32_t)a[0].v >= count)
        return vm_fail(vm, "data.read() needs one of the cart's data files, "
                       "DATA.<name>", "", "");
    p += 2;
    for (int32_t i = 0;; i++) {
        /* each length checked against what is left before it is used */
        if (end - p < 3 || (uint32_t)(end - p - 3) < rd16(p + 1) + 4u)
            return vm_fail(vm, "this game file is damaged: data", "", "");
        uint32_t kind = p[0], nlen = rd16(p + 1);
        const uint8_t *name = p + 3, *body = name + nlen + 4;
        uint32_t len = rd_le32(name + nlen);
        if (len > (uint32_t)(end - body))
            return vm_fail(vm, "this game file is damaged: data", "", "");
        if (i == a[0].v) {
            static char what[48];
            uint32_t k = nlen < 40 ? nlen : 40;
            memcpy(what, "data/", 5);
            memcpy(what + 5, name, k);
            what[5 + k] = 0;
            if (kind == 1) {
                vm_charge(vm, 10 + len / 64);
                return vm_new_string(vm, body, len, out);
            }
            return json_read(vm, body, len, what, out);
        }
        p = body + len;
    }
}

/* ---- storage ------------------------------------------------------------- */

/* A store call is a file's work on the disk, which costs far more than
 * its bytes; a write the more, being a write and a rename */
#define STORE_CALL  2000
#define STORE_WRITE 8000

static const struct arcade_store *store_of(struct vm *vm)
{
    return vm->host->store;
}

/* a[0] as a stored file's name, ended, into name: 1 to 32 letters, digits,
 * - and _; else the cart stopped */
static int name_of(struct vm *vm, struct value v, const char *fn, char *name)
{
    uint32_t n = v.tag == T_STR ? STR(vm, v)->len : 0;
    const uint8_t *b = n ? STR(vm, v)->b : NULL;
    int ok = n >= 1 && n <= 32;
    for (uint32_t i = 0; ok && i < n; i++)
        ok = (b[i] >= 'a' && b[i] <= 'z') || (b[i] >= 'A' && b[i] <= 'Z') ||
             (b[i] >= '0' && b[i] <= '9') || b[i] == '-' || b[i] == '_';
    if (!ok)
        return vm_fail(vm, fn, "() needs a name of 1 to 32 letters, digits, "
                       "- and _", "");
    memcpy(name, b, n);
    name[n] = 0;
    return 1;
}

/* store.read(name): the value written as name, or null when there is none */
NATIVE(store_read)
{
    (void)argc;
    char name[33];
    static char what[48];
    if (!name_of(vm, a[0], "store.read", name))
        return 0;
    if (!vm_spend(vm, STORE_CALL))
        return 0;
    const struct arcade_store *s = store_of(vm);
    int32_t n = s && s->read ? s->read(vm->host->ctx, name, NULL, 0) : -1;
    if (n < 0 || n > ARCADE_STORE_FILE) {
        *out = mk(T_NULL, 0);
        return 1;
    }
    struct value text;
    if (!vm_new_text(vm, (uint32_t)n, &text) || !vm_root(vm, text))
        return 0;
    int32_t got = s->read(vm->host->ctx, name, STR(vm, text)->b, n);
    memcpy(what, "store ", 6);
    strcpy(what + 6, name);
    int ok = got == n ? json_read(vm, STR(vm, text)->b, (uint32_t)n, what, out)
                      : (*out = mk(T_NULL, 0), 1);
    vm_unroot(vm);
    return ok;
}

/* store.write(name, value): true when kept; false when it is over 64 KB as
 * JSON, or the cart's area is full, or the disk refuses */
NATIVE(store_write)
{
    (void)argc;
    char name[33];
    static char what[48];
    if (!name_of(vm, a[0], "store.write", name))
        return 0;
    memcpy(what, "store.write(", 12);
    strcpy(what + 12, name);
    strcat(what, ")");
    struct value text;
    int r = json_write(vm, a[1], what, ARCADE_STORE_FILE, &text);
    if (r == 0)
        return 0;
    if (!vm_spend(vm, STORE_CALL))
        return 0;
    const struct arcade_store *s = store_of(vm);
    if (r == 2 || !s || !s->write)
        return RET_INT(0);
    if (!vm_spend(vm, STORE_WRITE + STR(vm, text)->len / 16))
        return 0;
    return RET_INT(s->write(vm->host->ctx, name, STR(vm, text)->b,
                            (int32_t)STR(vm, text)->len) != 0);
}

/* store.remove(name): true when there was one */
NATIVE(store_remove)
{
    (void)argc;
    char name[33];
    if (!name_of(vm, a[0], "store.remove", name))
        return 0;
    if (!vm_spend(vm, STORE_CALL))
        return 0;
    const struct arcade_store *s = store_of(vm);
    return RET_INT(s && s->remove ? s->remove(vm->host->ctx, name) != 0 : 0);
}

/* store.list(): the names kept, sorted */
NATIVE(store_list)
{
    (void)a; (void)argc;
    static char names[ARCADE_STORE_FILES][33];
    if (!vm_spend(vm, STORE_CALL))
        return 0;
    const struct arcade_store *s = store_of(vm);
    int n = s && s->list ? s->list(vm->host->ctx, names, ARCADE_STORE_FILES)
                         : 0;
    struct value list;
    if (!vm_new_array(vm, 0, &list) || !vm_root(vm, list))
        return 0;
    for (int i = 0; i < n; i++) {
        struct value str;
        names[i][32] = 0;
        if (!vm_new_string(vm, (const uint8_t *)names[i],
                           (uint32_t)strlen(names[i]), &str) ||
            !vm_root(vm, str)) {
            vm_unroot(vm);
            return 0;
        }
        int ok = vm_push(vm, list, str);
        vm_unroot(vm);
        if (!ok) {
            vm_unroot(vm);
            return 0;
        }
    }
    vm_unroot(vm);
    *out = list;
    return 1;
}
