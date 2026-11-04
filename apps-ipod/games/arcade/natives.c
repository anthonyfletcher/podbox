/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade: the native API's table, from arcade_api.def, and which of its
 * functions this firmware implements. A cart that calls one not in the
 * second table is refused at load as needing a newer PodBox.
 ***************************************************************************/
#include "games/arcade/vm.h"

#define VAL 1
#define VOID 0
#define PARTS 1
#define FLAT 0
#define PIXELS 1
#define CELLS 2
#define LISTEN 0
#define CONTROL 1
#define OWN 2

#define API_ALIAS(name, target)
#define API_CONST(name, value, version)
#define API_CONSTG(group, name, value, version)

enum
{
#define API(name, slot, min, max, ret, flags, cost, level, version) \
    SLOT_##name = slot,
#include "games/arcade/arcade_api.def"
#undef API
};

const struct native_def arcade_api[] = {
#define API(name, slot, min, max, ret, flags, cost, level, version) \
    [slot] = { min, max, ret, flags, level, cost, version },
#include "games/arcade/arcade_api.def"
#undef API
};

const uint32_t arcade_api_count = sizeof(arcade_api) / sizeof(arcade_api[0]);

#define IMPL(name) int n_##name(struct vm *, struct value *, int, \
                                struct value *);
#include "games/arcade/natives_impl.h"
#undef IMPL

const native_fn arcade_impl[sizeof(arcade_api) / sizeof(arcade_api[0])] = {
#define IMPL(name) [SLOT_##name] = n_##name,
#include "games/arcade/natives_impl.h"
#undef IMPL
};
