/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade services: speech -- a cart's text said by the player's
 * synthesiser through struct arcade_speak. A cart narrating its screens
 * follows speak.on(), the owner's choice of spoken menus; one whose game
 * is speech need not, though nothing sounds while the synthesiser is off.
 *
 * Parts, in order:
 *   natives       speak.say, queue, busy, stop, on
 ***************************************************************************/
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)
#define RET_INT(v) (*out = mk(T_INT, (int32_t)(v)), 1)

#define SPEAK_BYTES 512

static const struct arcade_speak *speak_of(struct vm *vm)
{
    vm_charge(vm, 20);
    return vm->host->speak;
}

/* The values as one line of text, a space between each, ended; cut short
 * at a whole character when there is too much */
static void phrase(struct vm *vm, const struct value *a, int argc, char *buf)
{
    uint32_t n = 0;
    for (int i = 0; i < argc && n < SPEAK_BYTES - 2; i++) {
        if (n)
            buf[n++] = ' ';
        n += vm_text(vm, a[i], buf + n, SPEAK_BYTES - 1 - n);
    }
    buf[utf8_whole(buf, n)] = 0;
}

static int say(struct vm *vm, const struct value *a, int argc, int queue)
{
    static char text[SPEAK_BYTES];
    const struct arcade_speak *s = speak_of(vm);
    phrase(vm, a, argc, text);
    if (s && s->say && text[0])
        s->say(vm->host->ctx, text, queue);
    return 1;
}

/* speak.say(...values): said as one phrase, cutting off what is being
 * said */
NATIVE(speak_say) { (void)out; return say(vm, a, argc, 0); }

/* speak.queue(...values): said after what is being said */
NATIVE(speak_queue) { (void)out; return say(vm, a, argc, 1); }

/* speak.busy(): whether anything is still being said */
NATIVE(speak_busy)
{
    (void)a; (void)argc;
    const struct arcade_speak *s = speak_of(vm);
    return RET_INT(s && s->busy ? s->busy(vm->host->ctx) != 0 : 0);
}

NATIVE(speak_stop)
{
    (void)a; (void)argc; (void)out;
    const struct arcade_speak *s = speak_of(vm);
    if (s && s->stop)
        s->stop(vm->host->ctx);
    return 1;
}

/* speak.on(): whether the owner has the player's menus spoken */
NATIVE(speak_on)
{
    (void)a; (void)argc;
    const struct arcade_speak *s = speak_of(vm);
    return RET_INT(s && s->on ? s->on(vm->host->ctx) != 0 : 0);
}
