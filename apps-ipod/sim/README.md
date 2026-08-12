# `apps-ipod/sim/` — making the simulator fit the application layer

`apps-ipod/` carries no `SIMULATOR` conditionals. It is written for the two
targets this fork builds and assumes their capabilities unconditionally.

A simulator build withdraws some of those capabilities:
`firmware/export/config/sim.h` `#undef`s `HAVE_DIRCACHE`, `HAVE_USBSTACK`,
`IPOD_ACCESSORY_PROTOCOL`, `CONFIG_I2C` and more, and `firmware/SOURCES` puts
the internal filesystem layer behind `CONFIG_PLATFORM & PLATFORM_NATIVE`. The
symbols behind all of that stop existing.

This directory supplies them, so that **almost no existing file in `apps-ipod/`
has to change**. The alternative — re-wrapping ~225 call sites in
`#ifdef HAVE_DIRCACHE` and friends the way upstream `apps/` does — is a large
invasive edit to the fork's own code for a build nobody ships.

The exceptions, where a shim genuinely cannot reach and the guard had to go back
into `apps-ipod/`, are all cases where the *code* would not compile rather than
a symbol that was merely missing:

| Where | Why a shim could not do it |
|---|---|
| `draw/jpeg_load.c`, `viewers/image_viewer/decoders/jpeg_decoder.c`, `system/fracmul.h` | inline ARM assembly with the `CPU_ARM` guard stripped. `sim.h` sets `CONFIG_CPU 0`, so `ARCH != ARCH_ARM` and `CPU_ARM` is never defined — the assembler sees ARM opcodes on x86. Upstream's C fallbacks restored |
| `draw/jpeg_load.c` | the 1/2/4/8-point IDCT passes come from `jpeg_idct_arm.S`, which `SOURCES` builds under `CPU_ARM`. Upstream's `#ifndef CPU_ARM` C versions restored |
| `audio/codecs.c`, `audio/codec_thread.c` | `codec_load_buf()` needs `lc_open_from_mem()`, which exists only for `BINFMT_ROCK`. Upstream's `HAVE_CODEC_BUFFERING` guard restored; the sim loads codecs from file, which the caller already falls back to |
| `audio/codecs.c` | `CACHEALIGN_SIZE` exists only on targets with a CPU cache, and `cpu.h` refuses to let anything else define it. Changed to upstream's `MEM_ALIGN_SIZE`, which is the same value on both targets |
| `audio/codecs.c` | the codec API table must match `lib/rbcodec/codecs/codecs.h`, which keys `debugf` on `SIMULATOR` too. Omitting it puts every later member off by one |
| `features.txt` | three lang gates (`dircache`, `serial_port`, `usb_hid`) were stricter than the settings that name their phrases, which `apps-ipod/` registers unconditionally |
| `api/misc.h`, `api/screens.h` | upstream sim sources include apps headers by bare name and the fork moved them. This is what `api/` is for |
| `screens/browse/browser.h` | `getcwd()` takes an `int` on Windows and mingw's own declaration must not be contradicted. Upstream's `#ifdef WIN32` restored — only the Windows build cares |
| `audio/codecs.c` | the codec header check compares `hdr->load_addr` against `codecbuf`, which only means anything when the codec was loaded *into* `codecbuf`. Upstream's `PLATFORM_NATIVE` guard restored — see below |
| `settings/settings_list.c` | `DEFAULT_BACKLIGHT_TIMEOUT` is 0 in a sim, as upstream has it. There is no battery to save, and a blanked window looks exactly like a crash |

Two of those were found by running it, and both are worth knowing about because
the symptom points nowhere near the cause:

- **Every track finished the instant it started**, with no error on screen. The
  codec loads fine and exports `__header` fine; it is the *next* check that
  fails, because a host shared object lands wherever the loader puts it and
  never at `codecbuf`. `codec_load_ram()` returns `CODEC_ERROR`, playback
  advances to the next track, and the playlist empties in a second.
- **The screen went black after fifteen idle seconds.** That is the backlight,
  and the fork's own wake-redraw brings the list straight back on the next
  keypress — so it only looks like a crash until you press something.

Every one of those is inert on hardware: both targets' objects are byte-identical
across the change.

`screens/system/debug_menu.c` builds, with its target-only screens guarded out
the way upstream guards them — the disk and S.M.A.R.T. screens behind
`PLATFORM_NATIVE`, the battery graph behind `!SIMULATOR` (its second half reads
PP GPIO registers directly), the wheel and IAP screens likewise, `dbg_cpufreq`
behind `HAVE_ADJUSTABLE_CPU_FREQ`, and the 6G SysCfg/bootflash pair behind
`!SIMULATOR`. What is left is everything portable: **Skin Engine RAM usage**,
Screendump, View OS stacks, View buflib allocs, the dircache and database
screens, Spun stats and the metadata log.

Guarding a screen's menu entry alone is not enough here. Upstream can do that
because an unreferenced `static` is discarded silently; this fork's warnings
are worth keeping clean, so the definitions are guarded to match.

## Why stubbing is honest here rather than a hack

Every API stubbed below already has a graceful-degradation path in its callers,
because upstream's builds without these features exercise it. The behaviour the
deleted `#ifdef`s used to select is reachable through **return values**.

`tagcache.c` is the clearest case: it reads a filename out of the ramcache via
`dircache_get_fileref_path()` and, on a negative return, falls through to
`open_files()` and a disk read. A stub returning -1 therefore yields correct
filenames, just slower.

## The rule for this directory

A shim function must fail in a way the caller **already knows how to handle**,
not in a new way. If a stub's return value has no correct choice, that is a
signal the call site genuinely needs a guard — put the guard in `apps-ipod/` and
say why, rather than inventing a value here.

Two return values are load-bearing, and getting either wrong hangs the boot
silently rather than failing:

- `dircache_enable()` returns **0**. Anything else sends `main.c` into
  `dircache_boot_wait()`, which spins until it sees `DIRCACHE_SCANNING` — a stub
  reporting `DIRCACHE_IDLE` never satisfies that, and the boot bar stops
  forever.
- `disk_mount_all()` returns **> 0**. Otherwise `init()` shows "No partition
  found" and blocks on `button_get(true) != SYS_USB_CONNECTED`.

## How the declarations arrive

`apps-ipod/` includes `dircache.h` by its real name, and in a simulator that
header declares nothing. Rather than edit those includes, `sim/include/` holds
**shadows** of the headers that stop declaring what `apps-ipod/` uses, and
`apps.make` puts that directory first:

```make
ifeq ($(APP_TYPE),sdl-sim)
INCLUDES := -I$(APPSDIR)/sim/include $(INCLUDES)
endif
```

Prepending, not appending: `tools/root.make` reads `firmware.make` (line 73)
long before `apps.make` (line 136), so `-I$(FIRMDIR)/include` and the target
include path have already claimed both names. Only `uisimulator.make` is read
afterwards and it appends, so nothing is lost by flattening `INCLUDES` here.

Each shadow includes the real header with `#include_next` and adds to it, and
owns a `PODBOX_SIM_*` guard rather than reusing the real one — see the comment
in `include/dircache.h` for why that matters (`firmware/include/file_internal.h`
takes the real `dircache.h` out from under you).

There are three:

| Shadow | Adds |
|---|---|
| `dircache.h` | the whole API and its two structs and two enums |
| `system-sim.h` | the general home. `enable_fiq()`, `USEC_TIMER`, `power_init()`, `dbg_ports()`, `battery_default_capacity()`, `usb_set_hid()`, `filesystem_init()`, `ns_volume_is_visible()` |
| `storage.h` | `storage_get_info()` |

`system-sim.h` rather than `system.h`: `system.h` reaches for it **only** under
`SIMULATOR` (it takes `system-target.h` otherwise), so shadowing it puts these
declarations in every translation unit of a sim build and none of a hardware
one. Shadowing `system-target.h` looks like the obvious choice and is wrong for
exactly that reason.

**Do not switch this to a forced include** (`-include`). It looks tidier and it
breaks the build in a way that takes a while to read: `INCLUDES` also feeds
`PPCFLAGS`, which preprocesses every `SOURCES` file, so the header's
declarations get emitted into those file lists as bogus source names. The first
symptom is `codecs.make:173: *** missing separator`.

## The hardware build must not move

Everything here is behind `ifeq ($(APP_TYPE),sdl-sim)` in `apps.make` and
`#ifdef SIMULATOR` in `apps-ipod/SOURCES`. A hardware build sees no new flag, no
new object, no changed header.

Verify that by diffing **objects**, not `rockbox.bin` — `RBVERSION` embeds the
build date, so the binary is never reproducible across days.

## Building one

```sh
mkdir build-sim-ipodvideo && cd build-sim-ipodvideo
../tools/configure --target=ipodvideo --type=s --appsdir=apps-ipod
make -j"$(nproc)"
```

`--appsdir=apps-ipod` matters as much here as anywhere: without it the sim
silently builds upstream's `apps/`. The output is `rockboxui`, and `configure`
creates a `simdisk/` beside it to stand in for the player's storage.

`ipod6g` works the same way. Both need SDL2 and a host compiler, so this happens
on the build server — see `.specifications/BUILD_SERVER.md`.

A **Windows** build cross-compiles from the same server:

```sh
export PATH=$HOME/bin:$PATH        # holds x86_64-w64-mingw32-sdl2-config
mkdir build-sim-win32 && cd build-sim-win32
../tools/configure --target=ipodvideo --type=as6 --appsdir=apps-ipod
```

`--type=as6` is **(A)dvanced** with the options `s` (simulator) and `6` (64-bit
Windows cross-compile). Plain `--type=s` will not take the extra letters —
`configure` matches the build type with `[Ss]`, exactly one character, and only
`[Aa]*` forwards the rest to `whichadvanced`. `--type=s6` silently gives you a
*normal* build.

The server needs `mingw-w64` (`apt`) and an SDL2 mingw development package
unpacked under `~/sdl2-mingw`, reached through a `~/bin/x86_64-w64-mingw32-sdl2-config`
wrapper — `findsdl` looks for the cross-prefixed name first, which keeps it off
the host's own `sdl2-config`. SDL links statically, so `rockboxui.exe` needs no
DLL beside it.

Either way, follow the build with `make zip` and the three `bundle-*.sh` scripts
and unzip the result into `simdisk/`, or the sim boots themeless with the 6x8
fallback iconset — which looks exactly like a rendering bug and is not one.
