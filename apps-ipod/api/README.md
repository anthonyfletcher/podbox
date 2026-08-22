# `apps-ipod/api/` — the boundary between `apps-ipod/` and the rest of the firmware

Code outside `apps-ipod/` includes a small number of its headers by bare name —
`firmware/powermgmt.c` has `#include "misc.h"`. Those includes resolve purely by
luck of the `-I` search path, which means any reorganisation inside `apps-ipod/`
can break a file in `firmware/` or `lib/` — and this fork's cleanup work is
confined to `apps-ipod/`, so those files cannot be edited to compensate.

This directory makes that contract explicit. Each header here is a one-line
forwarding stub. `apps-ipod/apps.make` puts `api/` **first** on the include path:

```make
INCLUDES += -I$(APPSDIR)/api -I$(APPSDIR)
```

so these stubs are what external code actually resolves to. When a real header
moves inside `apps-ipod/`, update the stub here and nothing outside notices.

## The contract, as measured

Every `#include "*.h"` under `firmware/`, `lib/`, `bootloader/` and
`uisimulator/`, matched against the `apps-ipod/` header set. `uisimulator/` is
on that list because the simulator builds and is upstream's code as-is, so it
reaches in by bare name exactly the way `firmware/` does:

| Stub | Included by |
|---|---|
| `settings.h` | `firmware/backlight.c`, `firmware/scroll_engine.c`, `firmware/sound.c`, `firmware/usb.c`, `lib/rbcodec/dsp/{afr,pbe,surround,tdspeed}.c` |
| `misc.h` | `firmware/powermgmt.c`, `firmware/scroll_engine.c`, `firmware/usb.c` |
| `action.h` | `firmware/backlight.c` |
| `splash.h` | `firmware/powermgmt.c` |
| `buffering.h` | `lib/rbcodec/metadata/metadata.c` |
| `fracmul.h` | `lib/rbcodec/dsp/*.c` (10 files) |
| `rbcodecconfig.h` | `lib/rbcodec/codecs/codecs.h`, `lib/rbcodec/dsp/*.c`, `lib/rbcodec/platform.h` |
| `rbcodecplatform.h` | `lib/rbcodec/platform.h` |
| `screens.h` | `uisimulator/common/stubs.c` |
| `plugin.h` | `lib/rbcodec/metadata/hes.c` — vestigial; the include needs nothing, and deleting that one line would let `plugin.h` go entirely. `lib/` is out of scope. This stub forwards to nothing; it **is** the empty header. |

The list covers files this fork compiles. Other targets' sources include these
same headers and are ignored here, for the same reason `list.h` is (below).

## Slashed paths

Outside code also reaches in by slashed path, not just bare name. These mirror
the pre-reorganisation directory layout:

| Stub | Included by | Forwards to |
|---|---|---|
| `gui/yesno.h` | `firmware/usb.c` | `widgets/yesno.h` |
| `gui/skin_engine/skin_engine.h` | `firmware/usb.c`, `firmware/backlight.c` | `skin/skin_engine.h` |

## The one member that cannot live here

`lib/rbcodec/codecs/spc.c` says `#include "../fracmul.h"`. A `../` include
resolves against each `-I` directory, not by name, so no file in `api/` can
satisfy it. That shim lives at `apps-ipod/fracmul.h` — see the comment in that
file.

## Known but not stubbed

Never compiled for this fork's two targets (iPod Video 5G/5.5G and iPod Classic
6G/7G), so no stub:

| Header | Included by |
|---|---|
| `list.h` | `firmware/target/arm/stm32/debug-stm32h7.c`, `firmware/target/mips/ingenic_jz47xx/debug-jz4760.c`, `firmware/target/mips/ingenic_x1000/debug-x1000.c` (the real header is `widgets/list.h`) |

## Rules

- Do not add a header here unless something **outside** `apps-ipod/` includes it
  by bare name. This is not a general "public headers" directory; `apps-ipod/`
  code should include the real header by its path relative to `apps-ipod/`.
- Do not put declarations here. These files forward and nothing else.
- When a real header moves, fix the path in its stub in the same commit.