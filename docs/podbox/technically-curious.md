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
- **Bare-metal native only.** No simulator, no hosted/Android/SDL build, and no
  remote LCD, touchscreen, FM radio or recording — none of which these players
  have.
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
natively on Windows.

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

## Finding your way around

| Where | What                                                                                                                                                     |
|---|----------------------------------------------------------------------------------------------------------------------------------------------------------|
| `apps-ipod/` | The application layer — all of this fork's UI work. See [`apps-ipod/README.md`](../../apps-ipod/README.md), which explains the layout and where new code goes. |
| `firmware/` | Hardware abstraction, drivers, kernel. Close to Rockbox; see [`upstream-divergence.md`](upstream-divergence.md) for changes.     |
| `lib/rbcodec/` | Codecs and DSP. Rockbox's, unmodified.                                                                                                                   |
| `apps/` | Rockbox's original application layer. **Never built.** Kept so merges apply cleanly — do not edit it.                              |
| `docs/podbox/` | This fork's own documentation.                                                                                                                           |
