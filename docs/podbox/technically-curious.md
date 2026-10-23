# For the technically curious

## Where it comes from

**PodBox began as a fork of RockPod and is now built directly on Rockbox.**

- **[RockPod](https://github.com/nuxcodes/rockpod)**, by Nux Li, is an
  iPod-focused fork of Rockbox, and PodBox started life as a fork of it. Much
  of PodBox still comes from there: the application layer began as RockPod's;
  the serial iAP work behind docks and remotes — identification, notifications,
  track information, authentication timing, album art to accessories — is the
  base PodBox's accessory support is built on; and so are its iPod hardware
  fixes (SSD-aware power management, S5L8702 boot and clock fixes, codec and
  charging fixes) and the album-art "dynamic colours". Where each iAP piece
  came from is in [`iap-sources.md`](iap-sources.md).
- **[Rockbox](https://www.rockbox.org)** is the long-running open-source firmware
  project covering 80+ players. RockPod follows its own course rather than
  merging Rockbox regularly, and PodBox wanted Rockbox's newer work as it lands
  — USB accessory support on both players among it — so in July 2026 PodBox
  moved its base to Rockbox itself. It now tracks Rockbox directly and merges
  from it, and still picks up RockPod's fixes where they apply.

## What is different from Rockbox

**The platform is cut down to what these two iPods actually use.**

- **Two targets.** iPod Classic 6G/7G (S5L8702) and iPod Video 5G/5.5G (PP5022).
  The other 80+ targets are still in the tree but nothing selects them.
- **The firmware is bare-metal native only.** No hosted or Android build, and no
  remote LCD, touchscreen, FM radio or recording — none of which these players
  have. There *is* a desktop simulator, which is a development tool rather than
  a build of the firmware; see [Running the simulator](#running-the-simulator).
- **No plugin system.** The handful of plugins worth keeping became ordinary core
  screens: the text and image viewers, properties, playing time, credits. Two
  more were written as core screens from the start — the lyrics viewer and the
  Playback Report listening statistics.

**The application layer is a different tree.** `apps-ipod/` replaces Rockbox's
`apps/`: reorganised by purpose to make development easier, and every file
carrying a header describing what it does. See
[`apps-ipod/README.md`](../../apps-ipod/README.md).

**The skin language is extended** with tags this firmware understands — text
measurement, wrapping, selection state and more. See [`custom-skin-tags.md`](custom-skin-tags.md) for more info.
Themes using them will not render on stock Rockbox.

**The playlist engine measures the music itself.** What it measures, how it
builds a playlist from that, and the desktop tool that does the analysis are
in [`playlist-engine.md`](playlist-engine.md).

**Everything outside `apps-ipod/` stays close to Rockbox on purpose**, so that
merges keep applying. The large exception is the USB stack: host mode, iAP2
and the sound card are substantially this fork's own. The rest — hardware
fixes in `firmware/`, codec fixes in `lib/`, tools and build scripts — are
catalogued. See
[`upstream-divergence.md`](upstream-divergence.md) and
[`upstream-commit-log.md`](upstream-commit-log.md).

---

# Building and contributing

## Prerequisites

You need the Rockbox ARM cross-compiler. Build it once with
`bash ./tools/rockboxdev.sh` (choose the `arm-elf-eabi` toolchain) and put
`arm-elf-eabi-gcc` on your `PATH`. **Invoke it through `bash`**, not as
`./tools/rockboxdev.sh` — the script says `#!/bin/sh` but locates its own
`toolchain-patches/` through `BASH_SOURCE`, so under a `sh` that is not bash it
looks in the wrong place and quietly builds an *unpatched* gcc. Linux or macOS;
the build does not run natively on Windows. The simulator wants SDL2 and a host
compiler instead, and can be cross-compiled *for* Windows — see
[Running the simulator](#running-the-simulator).

## Build

```sh
./build-hw.sh              # iPod Classic 6G/7G  (default)
./build-hw.sh 5g           # iPod Video 5G/5.5G
```

You get `build-hw-<target>/rockbox.zip`, ready to unzip onto the player.

## Use the script, or read this first

Rockbox builds out of tree, and `build-hw.sh` is only wrapping five steps:

```sh
../tools/configure --target=ipodvideo --type=n --appsdir=apps-ipod
make -j"$(nproc)"
make zip
sh ../tools/check-zip-langs.sh rockbox.zip
../bundle-theme.sh && ../bundle-help.sh && ../bundle-trim.sh && ../bundle-speech.sh && ../bundle-tools.sh
```

Three of them are easy to get wrong by hand, and all fail quietly:

- **`--appsdir=apps-ipod` is required.** Rockbox's original `apps/` is still in
  the tree and is never built. Omit the flag and the build compiles `apps/`
  instead — you get working firmware with none of this fork's work in it.
- **`make zip` on its own is incomplete.** `tools/buildzip.pl` is deliberately
  kept close to Rockbox and knows nothing about this fork, so its zip has no
  theme, no first-boot config, no iconset, no setting explanations, no
  title trimming patterns and no speech dictionary. The `bundle-*.sh` scripts add them and strip what a
  plugin-less build cannot use; `bundle-tools.sh` also adds the desktop
  tools `soundscan.exe`, `database_pb.exe` and `artcache_pb.exe`, and skips
  them with a warning unless a Windows simulator build
  (`./build-sim.sh 5g win`) is there to link against.
  `bundle-help.sh` is the one to watch: skip it and every **Explain** entry in
  a setting's context menu simply shows nothing, while everything else looks
  finished.
- **A clean build can ship an empty language.** A language file can come out
  as a bare 10-byte header with no error. `check-zip-langs.sh` fails on one;
  build again if it does.

For an incremental rebuild, re-run everything except `configure` from inside the
existing build directory.

`bundle-speech.sh` fetches the CMU Pronouncing Dictionary the first time, into
`~/.cache/podbox-speech`, and needs `python3` to turn it into the player's
`cmudict.bin`.

## Building the voices

The recorded voices — a `.voice` file per language, for English and the eleven
translations — are not in the firmware zip. `build-voices.sh` makes them inside
a finished build directory, into `voices.zip`, and `release.sh` publishes them as
the `Voices` release. A voice file is numbered by the build that made it, so
it has to be made in the build directory it goes with.

They are spoken by [Piper](https://github.com/OHF-Voice/piper1-gpl), set up once
in a Python environment of its own, `~/piper` unless `$PIPER_PYTHON` names
another:

```sh
sudo apt install python3-venv python3-dev     # dev: pyopenjtalk compiles
python3 -m venv ~/piper
~/piper/bin/pip install piper-tts pyopenjtalk  # pyopenjtalk is for Japanese
./build-voices.sh build-hw-ipodvideo           # or name some languages after it
```

Each language's model is downloaded the first time it is wanted, into
`~/piper-models`. Every clip is kept in `~/.cache/podbox-voice-pool`, so the
first run takes about two minutes a language and later ones only remake the
words that changed. `release.sh --no-voices` releases without them.

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
`x86_64-w64-mingw32-sdl2-config`. The result is `build-sim-<target>/rockboxui`
— or `build-sim-<target>-win32/rockboxui.exe`, which you copy to a Windows
machine together with its `simdisk/`. That directory stands in for the player's
storage: drop music in there and it appears in Files.

Like `build-hw.sh` this is a clean build, with one exception: **`simdisk/`
survives it.** Your music and database are worth more than the rebuild.

### Doing it by hand

```sh
mkdir build-sim-ipodvideo && cd build-sim-ipodvideo
../tools/configure --target=ipodvideo --type=s --appsdir=apps-ipod
make -j"$(nproc)"
make zip && ../bundle-theme.sh && ../bundle-help.sh && ../bundle-trim.sh
unzip -oq rockbox.zip -d simdisk/
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
| System > Debug | The portable debug screens, including **Skin memory**. The row is hidden until you turn on Settings > System > Show Debug Menu |

Controls are the arrow keys, Enter, Esc and Space, or the numeric keypad laid
out like the click wheel. Note that **Right**, not Enter, opens a list item —
that is Rockbox's iPod keymap, not a simulator quirk.

### Where it differs from the player

No dircache and no USB stack; both are stubbed, so the browser reads the disk
directly. The debug menu's hardware screens — disk, battery, S.M.A.R.T.,
hardware info, click wheel — are absent. Everything the skin engine does is real.

Almost all of the simulator's support is inside `apps-ipod/`, in
[`apps-ipod/sim/`](../../apps-ipod/sim/README.md), which supplies what a hosted
build lacks. Outside it, a few files of upstream's SDL backend and
`uisimulator/` carry the `--screenshot` argument and a fix for a crash at boot;
[`upstream-divergence.md`](upstream-divergence.md) lists them under *The
simulator*.

## Finding your way around

| Where | What                                                                                                                                                     |
|---|----------------------------------------------------------------------------------------------------------------------------------------------------------|
| `apps-ipod/` | The application layer — all of this fork's UI work. See [`apps-ipod/README.md`](../../apps-ipod/README.md), which explains the layout and where new code goes. |
| `bootloader/` | Rockbox's bootloader, except that the hold switch no longer boots Apple's firmware; only MENU does. |
| `firmware/` | Hardware abstraction, drivers, kernel. Close to Rockbox apart from the USB stack, whose host mode, iAP2 and sound card are substantially this fork's own; see [`upstream-divergence.md`](upstream-divergence.md) for changes. |
| `lib/` | Codecs, DSP and the skin parser. Rockbox's, plus codec fixes and speedups (see [`upstream-divergence.md`](upstream-divergence.md)) and the skin parser's hooks for the extra tags. |
| `apps/` | Rockbox's original application layer. **Never built.** Kept so merges apply cleanly — do not edit it.                              |
| `uisimulator/` | Rockbox's SDL simulator support, upstream's apart from `--screenshot`. The fork's side of it is [`apps-ipod/sim/`](../../apps-ipod/sim/README.md). |
| `tools/` | Rockbox's build tools, plus this fork's: `check-settings-docs.sh`, `upstream-apps-port.sh`, `check-zip-langs.sh`, and the desktop tools `soundscan/`, `database_pb/` and `artcache_pb/`. |
| `docs/podbox/` | This fork's own documentation.                                                                                                                           |
