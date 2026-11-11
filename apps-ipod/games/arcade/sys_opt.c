/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade services: options -- the settings a cart declares in cart.json
 * (the loader reads them from OPTN), shown in its pause menu and kept by
 * the host. The cart only reads them; the owner changes them.
 *
 * Parts, in order:
 *   the values    setting one, the host's record of them
 *   the help      the cart's help text, from META
 *   natives       opt.get, opt.set, opt.changed
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)
#define RET_INT(v) (*out = mk(T_INT, (int32_t)(v)), 1)

/* ---- the values ---------------------------------------------------------- */

const struct arcade_options *arcade_options(const struct vm *vm)
{
    return &vm->opts;
}

/* value within o's range, on one of its steps from min */
static int32_t fit(const struct arcade_option *o, int32_t value)
{
    if (value < o->min)
        value = o->min;
    if (value > o->max)
        value = o->max;
    return (int32_t)(o->min + ((int64_t)value - o->min) / o->step * o->step);
}

void arcade_option_set(struct vm *vm, int i, int32_t value)
{
    if (i < 0 || i >= vm->opts.count)
        return;
    struct arcade_option *o = &vm->opts.o[i];
    value = fit(o, value);
    if (value != o->value)
        vm->opt_pending = 1;
    o->value = value;
}

/* "PBOP", a version byte, the count, two spare bytes, then each option's
 * key and value; little-endian */
int arcade_options_pack(const struct vm *vm, uint8_t *buf, int cap)
{
    const struct arcade_options *s = &vm->opts;
    int n = 8 + s->count * 8;
    if (cap < n)
        return 0;
    memcpy(buf, "PBOP", 4);
    buf[4] = 1;
    buf[5] = (uint8_t)s->count;
    buf[6] = vm->muted;             /* the owner's mute */
    buf[7] = 0;
    for (int i = 0; i < s->count; i++) {
        wr_le32(buf + 8 + i * 8, s->o[i].key);
        wr_le32(buf + 12 + i * 8, (uint32_t)s->o[i].value);
    }
    return n;
}

int arcade_options_unpack(struct vm *vm, const uint8_t *buf, int n)
{
    if (n < 8 || memcmp(buf, "PBOP", 4) || buf[4] != 1 ||
        n < 8 + buf[5] * 8)
        return 0;
    vm->muted = buf[6] & 1;
    for (int k = 0; k < buf[5]; k++) {
        uint32_t key = rd_le32(buf + 8 + k * 8);
        int32_t value = (int32_t)rd_le32(buf + 12 + k * 8);
        for (int i = 0; i < vm->opts.count; i++)
            if (vm->opts.o[i].key == key)
                vm->opts.o[i].value = fit(&vm->opts.o[i], value);
    }
    return 1;
}

/* ---- the help ------------------------------------------------------------ */

int arcade_help(const struct vm *vm, const char **text)
{
    const uint8_t *p = vm->meta, *end = vm->meta + vm->meta_len;
    while (p && end - p >= 3) {
        int tag = p[0], len = p[1] | p[2] << 8;
        if (end - p - 3 < len)
            break;
        if (tag == 4) {
            *text = (const char *)p + 3;
            return len;
        }
        p += 3 + len;
    }
    *text = NULL;
    return 0;
}

/* ---- natives ------------------------------------------------------------- */

/* opt.get(OPT.name): the choice's index, 0 or 1, or the number */
NATIVE(opt_get)
{
    (void)argc;
    if (a[0].tag != T_INT || a[0].v < 0 || a[0].v >= vm->opts.count)
        return vm_fail(vm, "opt.get() needs one of the cart's options, "
                       "OPT.<name>", "", "");
    vm_charge(vm, 10);
    return RET_INT(vm->opts.o[a[0].v].value);
}

/* opt.set(OPT.name, value): an option set from the game -- a title
 * screen's own settings page -- fitted to its range and kept as the pause
 * menu keeps it; not a change opt.changed() reports */
NATIVE(opt_set)
{
    (void)argc; (void)out;
    if (a[0].tag != T_INT || a[0].v < 0 || a[0].v >= vm->opts.count ||
        !is_num(a[1]))
        return vm_fail(vm, "opt.set() needs one of the cart's options, "
                       "OPT.<name>, and a value", "", "");
    struct arcade_option *o = &vm->opts.o[a[0].v];
    int32_t v = fit(o, floor_int(a[1]));
    if (v != o->value)
        vm->opt_set = 1;
    o->value = v;
    vm_charge(vm, 10);
    return 1;
}

int arcade_options_set(struct vm *vm)
{
    int was = vm->opt_set;
    vm->opt_set = 0;
    return was;
}

/* opt.changed(): whether the owner changed any since the last tick */
NATIVE(opt_changed)
{
    (void)a; (void)argc;
    vm_charge(vm, 5);
    return RET_INT(vm->opt_changed);
}
