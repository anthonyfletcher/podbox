/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade core: input -- the buttons and the click wheel, as the host hands
 * them over before each tick (arcade_input) -- and quit().
 *
 * btn and btnp use PICO-8's numbers: 0 PREV, 1 NEXT, 2 MENU (only for a
 * cart that claims it; the host never sets it otherwise), 3 PLAY,
 * 4 SELECT, 5 nothing. btnp is true on the tick a button goes down, then
 * on the 16th tick it is held and every 4th after.
 ***************************************************************************/
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)

void arcade_input(struct vm *vm, unsigned held, int wheel, int wheelpos)
{
    uint8_t pressed = 0;
    held &= 0x1f;
    for (int i = 0; i < 5; i++) {
        if (!(held & (1u << i))) {
            vm->held_ticks[i] = 0;
            continue;
        }
        uint16_t t = vm->held_ticks[i];
        if (t < 0xffff)
            t++;
        vm->held_ticks[i] = t;
        if (t == 1 || (t >= 16 && (t - 16) % 4 == 0))
            pressed |= (uint8_t)(1u << i);
    }
    vm->held = (uint8_t)held;
    vm->pressed = pressed;
    vm->wheel = wheel;
    vm->wheelpos = wheelpos < 0 || wheelpos > 95 ? -1 : wheelpos;
}

static int button_arg(struct vm *vm, struct value *a, int argc, uint8_t bits,
                      const char *fn, struct value *out)
{
    if (argc == 0 || a[0].tag == T_NULL) {
        *out = mk(T_INT, bits);
        return 1;
    }
    if (!is_num(a[0]))
        return vm_fail(vm, fn, "() needs a button number, not a ",
                       vm_typename(a[0].tag));
    int32_t i = floor_int(a[0]);
    *out = mk(T_INT, i >= 0 && i < 8 ? (bits >> i) & 1 : 0);
    return 1;
}

NATIVE(btn) { return button_arg(vm, a, argc, vm->held, "btn", out); }
NATIVE(btnp) { return button_arg(vm, a, argc, vm->pressed, "btnp", out); }

NATIVE(wheel)
{
    (void)a; (void)argc;
    *out = mk(T_INT, vm->wheel);
    return 1;
}

NATIVE(wheelpos)
{
    (void)a; (void)argc;
    *out = mk(T_INT, vm->wheelpos);
    return 1;
}

/* menuitem(i, label, fn): item i, 1-5, of the pause menu, calling fn when
 * chosen; without a label, removes it. */
NATIVE(menuitem)
{
    (void)out;
    if (!is_num(a[0]))
        return vm_fail(vm, "menuitem() needs an item number, not a ",
                       vm_typename(a[0].tag), "");
    int32_t i = floor_int(a[0]);
    if (i < 1 || i > 5)
        return vm_fail(vm, "menuitem() takes items 1 to 5", "", "");
    if (argc < 2 || a[1].tag == T_NULL) {
        vm->menu_label[i - 1] = mk(T_NULL, 0);
        vm->menu_fn[i - 1] = mk(T_NULL, 0);
        return 1;
    }
    struct value label;
    if (!vm_tostring(vm, a[1], &label))
        return 0;
    if (argc > 2 && a[2].tag != T_NULL && a[2].tag != T_FN)
        return vm_fail(vm, "menuitem() needs a function, not a ",
                       vm_typename(a[2].tag), "");
    vm->menu_label[i - 1] = label;
    vm->menu_fn[i - 1] = argc > 2 ? a[2] : mk(T_NULL, 0);
    return 1;
}

int arcade_menu_item(const struct vm *vm, int i, char *buf, int cap)
{
    if (i < 1 || i > 5 || vm->menu_label[i - 1].tag != T_STR || cap < 1)
        return 0;
    const struct str *s = MEM(vm, (uint32_t)vm->menu_label[i - 1].v);
    int n = (int)s->len < cap - 1 ? (int)s->len : cap - 1;
    for (int k = 0; k < n; k++)
        buf[k] = (char)s->b[k];
    buf[n] = 0;
    return 1;
}

/* Back to the menu once this tick is over; the host saves on the way. */
NATIVE(quit)
{
    (void)a; (void)argc; (void)out;
    vm->quit = 1;
    return 1;
}

int arcade_rate(const struct vm *vm)
{
    return vm->rate;
}

int arcade_menu_is_game(const struct vm *vm)
{
    return vm->menu_button == 1;
}

const uint8_t *arcade_id(const struct vm *vm)
{
    return vm->id;
}

int arcade_quit_asked(const struct vm *vm)
{
    return vm->quit;
}
