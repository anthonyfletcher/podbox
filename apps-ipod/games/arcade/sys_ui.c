/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade services: dialogs -- the player's own lists, questions and text
 * entry, opened over the game through struct arcade_ui. Each runs to its
 * end inside the call, so a cart asks and has its answer on the next
 * line; the game waits meanwhile, and the time is not charged to it.
 *
 * Parts, in order:
 *   text in       a cart's value as ended text, of a bounded length
 *   natives       ui.choose, confirm, message, number, text
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)
#define RET_INT(v) (*out = mk(T_INT, (int32_t)(v)), 1)

#define MAX_ITEMS  32
#define ITEM_BYTES 48
#define TEXT_BYTES 1024
#define LINE_BYTES 65       /* ui.text: 64 bytes and its end */

/* ---- text in ------------------------------------------------------------- */

/* v as text into dst, ended, cut before a character rather than inside */
static void text_of(struct vm *vm, struct value v, char *dst, uint32_t cap)
{
    uint32_t n = v.tag == T_NULL ? 0 : vm_text(vm, v, dst, cap - 1);
    dst[utf8_whole(dst, n)] = 0;
}

static const struct arcade_ui *ui_of(struct vm *vm)
{
    vm_charge(vm, 50);
    return vm->host->ui;
}

/* ---- natives ------------------------------------------------------------- */

/* ui.choose(title, items, [at]): the index chosen, or -1 */
NATIVE(ui_choose)
{
    static char title[ITEM_BYTES], items[MAX_ITEMS][ITEM_BYTES];
    const char *list[MAX_ITEMS];
    if (a[1].tag != T_ARR)
        return vm_fail(vm, "ui.choose() needs a list of items", "", "");
    struct arr *r = ARR(vm, a[1]);
    if (r->len < 1 || r->len > MAX_ITEMS)
        return vm_fail(vm, "ui.choose() takes 1 to 32 items", "", "");
    text_of(vm, a[0], title, sizeof(title));
    for (uint32_t i = 0; i < r->len; i++) {
        text_of(vm, BUF(vm, r->buf)->v[i], items[i], ITEM_BYTES);
        list[i] = items[i];
    }
    int at = argc > 2 && is_num(a[2]) ? floor_int(a[2]) : 0;
    if (at < 0 || at >= (int)r->len)
        at = 0;
    const struct arcade_ui *u = ui_of(vm);
    return RET_INT(u && u->choose ? u->choose(vm->host->ctx, title, list,
                                              (int)r->len, at) : -1);
}

/* The title a cart gave in a[i], or its own name */
static void title_of(struct vm *vm, struct value *a, int argc, int i,
                     char *dst, uint32_t cap)
{
    if (argc > i && a[i].tag != T_NULL)
        text_of(vm, a[i], dst, cap);
    else
        strcpy(dst, vm->scores.cart);
}

/* ui.confirm(text, [title]): true for yes; the cart's name above the
 * question when no title is given */
NATIVE(ui_confirm)
{
    static char text[TEXT_BYTES], title[ITEM_BYTES];
    text_of(vm, a[0], text, sizeof(text));
    title_of(vm, a, argc, 1, title, sizeof(title));
    const struct arcade_ui *u = ui_of(vm);
    return RET_INT(u && u->confirm
                   ? u->confirm(vm->host->ctx, title, text) != 0 : 0);
}

/* ui.message(text, [title]): text to read; the cart's name above it when
 * no title is given */
NATIVE(ui_message)
{
    (void)out;
    static char text[TEXT_BYTES], title[ITEM_BYTES];
    text_of(vm, a[0], text, sizeof(text));
    title_of(vm, a, argc, 1, title, sizeof(title));
    const struct arcade_ui *u = ui_of(vm);
    if (u && u->message)
        u->message(vm->host->ctx, title, text);
    return 1;
}

/* ui.number(title, min, max, value, [step]): the number chosen, value
 * when backed out of */
NATIVE(ui_number)
{
    static char title[ITEM_BYTES];
    for (int i = 1; i < 4; i++)
        if (!is_num(a[i]))
            return vm_fail(vm, "ui.number() needs a title, then the least, "
                           "the most and the starting number", "", "");
    int32_t min = floor_int(a[1]), max = floor_int(a[2]);
    int32_t value = floor_int(a[3]), step = 1;
    if (argc > 4 && is_num(a[4]))
        step = floor_int(a[4]);
    if (min >= max || step < 1)
        return vm_fail(vm, "ui.number() needs the least below the most, ",
                       "and a step of at least 1", "");
    if (value < min)
        value = min;
    if (value > max)
        value = max;
    text_of(vm, a[0], title, sizeof(title));
    const struct arcade_ui *u = ui_of(vm);
    if (u && u->number)
        value = u->number(vm->host->ctx, title, min, max, step, value);
    return RET_INT(value < min ? min : value > max ? max : value);
}

/* ui.text(title, max, [start]): a line of at most max bytes (1-64), typed
 * with the wheel, or null when backed out of */
NATIVE(ui_text)
{
    static char title[ITEM_BYTES], line[LINE_BYTES];
    if (!is_num(a[1]) || floor_int(a[1]) < 1 || floor_int(a[1]) > 64)
        return vm_fail(vm, "ui.text() needs the most it may hold, 1 to 64 "
                       "bytes", "", "");
    int cap = floor_int(a[1]) + 1;
    text_of(vm, a[0], title, sizeof(title));
    if (argc > 2)
        text_of(vm, a[2], line, (uint32_t)cap);
    else
        line[0] = 0;
    const struct arcade_ui *u = ui_of(vm);
    if (!u || !u->text || !u->text(vm->host->ctx, title, line, cap)) {
        *out = mk(T_NULL, 0);
        return 1;
    }
    line[cap - 1] = 0;
    return vm_new_string(vm, (const uint8_t *)line, (uint32_t)strlen(line),
                         out);
}
