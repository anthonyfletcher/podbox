# For the technically curious

## Where it comes from

**Rockbox → PodBox**, with hardware and UI work borrowed from RockPod.

- **[Rockbox](https://www.rockbox.org)** is the long-running open-source firmware
  project covering 80+ players. This fork tracks it directly and merges from it.
- **[RockPod](https://github.com/nuxcodes/rockpod)** is a separate fork of Rockbox and PodBox carries some of its work:
  iPod hardware support (SSD-aware power management, S5L8702 boot and clock
  fixes, codec and charging fixes) and builds on the album-art "dynamic colours".
- **[Themify 2](../../themes/Themify_2/README.md)** is a theme by [Dook](https://d00k.net/).
  RockPod modified it and the version here is modified further again.

## What is different from Rockbox

**The platform is cut down to what these two iPods actually use.**

- **Two targets.** iPod Classic 6G/7G (S5L8702) and iPod Video 5G/5.5G (PP5022).
  The other 80+ targets are still in the tree but nothing selects them.
- **The firmware is bare-metal native only.** No hosted or Android build, and no
  remote LCD, touchscreen, FM radio or recording — none of which these players
  have. There *is* a desktop simulator, which is a development tool rather than
  a build of the firmware; see [Running the simulator](#running-the-simulator).
- **No plugin system.** The handful of plugins worth keeping became ordinary core
  screens: the text and image viewers, properties, playing time, credits. If you
  miss Doom you need help.

**The application layer is a different tree.** `apps-ipod/` replaces Rockbox's
`apps/`: reorganised by purpose to make development easier, and every file
carrying a header describing what it does. See
[`apps-ipod/README.md`](../../apps-ipod/README.md).

**The skin language is extended** with tags this firmware understands — text
measurement, wrapping, selection state and more. See [`custom-skin-tags.md`](custom-skin-tags.md) for more info.
Themes using them will not render on stock Rockbox.

**Everything outside `apps-ipod/` stays close to Rockbox on purpose**, so that
merges keep applying. The exceptions — hardware fixes in `firmware/`, build
scripts, and a handful of others — are catalogued. See
[`upstream-divergence.md`](upstream-divergence.md) and
[`upstream-commit-log.md`](upstream-commit-log.md).

---

# Building and contributing

## Prerequisites

You need the Rockbox ARM cross-compiler. Build it once with
`tools/rockboxdev.sh` (choose the `arm-elf-eabi` toolchain) and put
`arm-elf-eabi-gcc` on your `PATH`. Linux or macOS; the build does not run
natively on Windows. The simulator wants SDL2 and a host compiler instead, and
can be cross-compiled *for* Windows — see
[Running the simulator](#running-the-simulator).

## Build

```sh
./build-hw.sh              # iPod Classic 6G/7G  (default)
./build-hw.sh 5g           # iPod Video 5G/5.5G
```

You get `build-hw-<target>/rockbox.zip`, ready to unzip onto the player.

## Use the script, or read this first

Rockbox builds out of tree, and `build-hw.sh` is only wrapping four steps:

```sh
../tools/configure --target=ipodvideo --type=n --appsdir=apps-ipod
make -j"$(nproc)"
make zip
../bundle-theme.sh && ../bundle-eqs.sh && ../bundle-licenses.sh
```

Two of them are easy to get wrong by hand, and both fail quietly:

- **`--appsdir=apps-ipod` is required.** Rockbox's original `apps/` is still in
  the tree and is never built. Omit the flag and the build compiles `apps/`
  instead — you get working firmware with none of this fork's work in it.
- **`make zip` on its own is incomplete.** `tools/buildzip.pl` is deliberately
  kept close to Rockbox and knows nothing about this fork, so its zip has no
  theme, no first-boot `config.cfg`, no EQ presets, and Rockbox's licence file
  rather than this fork's. The three `bundle-*.sh` scripts add them and strip
  what a plugin-less build cannot use.

For an incremental rebuild, re-run everything except `configure` from inside the
existing build directory.

## Running the simulator

The simulator runs the real application layer against SDL instead of an iPod:
the same skin engine, the same album art, the same 320x240 screen. It is the
fast way to work on a theme — no cable, no sync, and you can read the debug
output.

```sh
./build-sim.sh              # iPod Classic 6G/7G  (default)
./build-sim.sh 5g           # iPod Video 5G/5.5G
./build-sim.sh 5g win       # cross-compile a 64-bit Windows .exe
```

You need SDL2 and a host compiler; the Windows build also wants `mingw-w64` and
an SDL2 mingw development package reachable as
`x86_64-w64-mingw32-sdl2-config`. The result is `build-sim-<target>/rockboxui`,
and `simdisk/` beside it stands in for the player's storage — drop music in
there and it appears in Files.

Like `build-hw.sh` this is a clean build, with one exception: **`simdisk/`
survives it.** Your music and database are worth more than the rebuild.

### Doing it by hand

```sh
mkdir build-sim-ipodvideo && cd build-sim-ipodvideo
../tools/configure --target=ipodvideo --type=s --appsdir=apps-ipod
make -j"$(nproc)"
make zip && ../bundle-theme.sh && ../bundle-eqs.sh && ../bundle-licenses.sh
rm -rf simdisk/.rockbox && unzip -q rockbox.zip -d simdisk/
```

**Do not skip the bundle step.** Without it the simulator starts themeless with
the fallback 6x8 iconset, which looks exactly like a rendering bug and is not
one.

For Windows the build type is `--type=as6`: **(A)dvanced** with the options `s`
(simulator) and `6` (64-bit Windows). Plain `--type=s6` looks equivalent and
silently gives you a *normal* build — `configure` matches the build type one
character at a time, and only `A` forwards the rest.

### What it gives you

| | |
|---|---|
| `--debugwps` | Traces skin parsing: which `.wps` and `.sbs` loaded, and what every `%Sx()` lang lookup resolved to |
| `F5` | Screendump — a pixel-exact 320x240 BMP written into `simdisk/` |
| System > Debug | The portable debug screens, including **Skin Engine RAM usage** |

Controls are the arrow keys, Enter, Esc and Space, or the numeric keypad laid
out like the click wheel. Note that **Right**, not Enter, opens a list item —
that is Rockbox's iPod keymap, not a simulator quirk.

### Where it differs from the player

No dircache and no USB stack; both are stubbed, so the browser reads the disk
directly. The debug menu's hardware screens — disk, battery, S.M.A.R.T., I/O
ports, scroll wheel — are absent. Everything the skin engine does is real.

The simulator needed no change outside `apps-ipod/`: upstream's `uisimulator/`
and SDL backend work as they are. What it needed was for `apps-ipod/` to stop
assuming it was on hardware, which is
[`apps-ipod/sim/`](../../apps-ipod/sim/README.md).

## Finding your way around

| Where | What                                                                                                                                                     |
|---|----------------------------------------------------------------------------------------------------------------------------------------------------------|
| `apps-ipod/` | The application layer — all of this fork's UI work. See [`apps-ipod/README.md`](../../apps-ipod/README.md), which explains the layout and where new code goes. |
| `firmware/` | Hardware abstraction, drivers, kernel. Close to Rockbox; see [`upstream-divergence.md`](upstream-divergence.md) for changes.     |
| `lib/rbcodec/` | Codecs and DSP. Rockbox's, unmodified.                                                                                                                   |
| `apps/` | Rockbox's original application layer. **Never built.** Kept so merges apply cleanly — do not edit it.                              |
| `uisimulator/` | Rockbox's SDL simulator support, unmodified and used as-is. The fork's side of it is [`apps-ipod/sim/`](../../apps-ipod/sim/README.md). |
| `docs/podbox/` | This fork's own documentation.                                                                                                                           |
