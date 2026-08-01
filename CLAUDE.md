# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Behavioral guidelines to reduce common LLM coding mistakes. Merge with project-specific instructions as needed.

**Tradeoff:** These guidelines bias toward caution over speed. For trivial tasks, use judgment.

## 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

## 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

## 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

## 4. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.

## 5. Documentation

Comments, commit descriptions and documentation should NOT describe the journey - only the destination.  The purpose
of the documentation is for programmers reading the output - they don't need to know how you got there.
The only exception is if you think there's a trap they might themselves fall into - then you can include it. Be brief!
Keep your documentation short and snappy.

---

**These guidelines are working if:** fewer unnecessary changes in diffs, fewer rewrites due to overcomplication, and clarifying questions come before implementation rather than after mistakes.

## Project Overview

PodBox is a fork of Rockbox, the open-source replacement firmware for
digital audio players. Written in C (gnu11) with ARM assembly where performance
demands it. Licensed under GPLv2.

Upstream Rockbox supports 80+ targets across several architectures; **this fork
builds two**, both ARM and both 320x240. The wider target trees under
`firmware/target/` are still present but are neither built nor tested here.

## Custom Build Targets

This tree is a custom build for **iPod Classic 6G/7G** and **iPod Video 5G/5.5G**. Changes may diverge from upstream Rockbox to suit these targets. The two iPods share the same 320x240 LCD and most app-layer code, but have different SoCs, USB controllers, and board-level drivers:

- **iPod Classic (6G/7G):** S5L8702 SoC, DesignWare USB OTG, CS42L55 codec. Config: `ipod6g`. Full feature set including MFi digital audio, SSD power management.
- **iPod Video (5G/5.5G):** PP5022 SoC, ARC USB OTG, WM8758 codec. Config: `ipodvideo`. UI features (Cover Flow, dynamic colors, themes). USB audio is **on** here: `config.h` uses upstream's generic `USB_HAS_ISOCHRONOUS` gate for `USB_ENABLE_AUDIO`, and the ARC controller declares it, so `rockbox-info.txt` lists `usbdac`. This is new since the Rockbox rebase — RockPod had narrowed the gate to `CONFIG_CPU == S5L8702`, which excluded this target — and it has **never been exercised on hardware**.

USB iAP is separately **off** on both targets: `config.h` defines `PODBOX_NO_USB_IAP`, which suppresses the otherwise-automatic `USB_ENABLE_IAP`. Delete that define to re-enable; nothing else is needed.

## Build Commands

Rockbox requires out-of-tree builds. Cross-compiler toolchains are built via 
`tools/rockboxdev.sh`.

**Environment note:** the cross-compiler (`arm-elf-eabi-gcc`) may not be present
on the machine this repo is checked out on. If it is missing, the build cannot
run locally -- ask rather than installing a toolchain, and do not assume `sudo`
is available.

**The application layer is `apps-ipod/`, so every configure invocation needs
`--appsdir=apps-ipod`.** `build-hw.sh` passes it; a hand-rolled configure that
omits it will silently build against whatever is in `apps/` instead.

```bash
# Hardware builds (clean) — output in build-hw-<target>/
./build-hw.sh                # iPod Classic 6G (default)
./build-hw.sh 5g             # iPod Video 5G
./build-hw.sh ipod6g         # explicit target name
./build-hw.sh ipodvideo      # explicit target name

# Incremental rebuild
cd build-hw-ipodvideo && make -j"$(nproc)" && make zip && ../bundle-theme.sh && ../bundle-eqs.sh && ../bundle-licenses.sh

# Non-interactive configure (reference)
../tools/configure --target=ipodvideo --type=n --appsdir=apps-ipod  # 5G
../tools/configure --target=ipod6g    --type=n --appsdir=apps-ipod  # 6G

# Other make targets
make codecs                 # codecs only
make bin                    # binary only
make zip                    # create deployment zip (themeless -- see below)
make reconf                 # reconfigure after tools/configure changes
make clean / make veryclean
```

**Theme bundling — `make zip` is not enough.** `tools/buildzip.pl` is kept as
close to upstream as possible and knows nothing about this fork's theme, so a
zip straight from `make zip` has **no Themify_2, no first-boot `config.cfg` and
no EQ presets**. Follow it with `../bundle-theme.sh` and `../bundle-eqs.sh`.
`./build-hw.sh` runs both; a bare `make zip` does not.

`bundle-theme.sh` also deletes the `classic_statusbar` theme, which
`buildzip.pl` copies straight out of `wps/` without consulting `WPSLIST`. It
removes the directory *and* the two loose `classic_statusbar.{sbs,rsbs}` files
written beside it -- a `wps/classic_statusbar/*` pattern does not match those.

**Configure build types:** (N)ormal, (B)ootloader, (C)heckWPS and (D)atabase
all work. (S)imulator and (W)arble are offered by `tools/configure` and do not.

```bash
mkdir /tmp/cwps && cd /tmp/cwps
<root>/tools/configure --target=ipodvideo --type=c --appsdir=apps-ipod && make
```
Two fork-specific notes, since the tools' own READMEs are upstream's and say
nothing about either:

- **CheckWPS must run from inside a `.rockbox` directory.** Skin font paths are
  relative to the on-device layout and will not resolve from anywhere else --
  it then fails on the fonts rather than the skin.
- `tools/checkwps/SOURCES` carries one addition, `apps-ipod/skin/custom_tags.c`.
  Without it `find_custom_tag()` (weak, see `lib/skin_parser/tag_table.c`)
  resolves to NULL and every skin using a fork tag fails on the first one. With
  it, both Themify_2 skins report "WPS parsed OK" from
  `themes/Themify_2/.rockbox`.

The database tool runs from the top level of a mounted player and writes the
database files itself, so the player does not have to scan.

**Sanitizers:** `--with-address-sanitizer` and `--with-ubsan` flags to configure.

**Default CFLAGS:** `-W -Wall -Wextra -Wundef -Os -nostdlib -ffreestanding -Wstrict-prototypes -pipe -std=gnu11`

## Architecture

### Layer Structure

```
bootloader/    — Minimal boot code, loads main firmware
firmware/      — HAL, kernel, drivers, filesystem, low-level services
lib/           — Shared libraries (rbcodec, skin_parser, fixedpoint, tlsf)
apps-ipod/      — Application layer: UI, playback engine, codecs loader, i18n
```

The application layer is `apps-ipod/`, not `apps/`. `tools/configure --appsdir`
selects it and `build-hw.sh` passes it; see the `COREAPPSDIR`/`APPSBUILDDIR`
variables in `tools/root.make`.

`apps-ipod/` is grouped into domain subdirectories rather than upstream's flat
directory. **Read `apps-ipod/README.md` before adding or moving a file there** -- it
documents the layout and what belongs in each directory.

### Target Tree

Hardware abstraction is organized hierarchically under `firmware/target/`:
```
firmware/target/<cpu_arch>/<soc>/<manufacturer>/<model>/
```
Each target has a config header at `firmware/export/config/<modelname>.h` defining all hardware capabilities via `#define` macros. The central `firmware/export/config.h` includes an auto-generated `autoconf.h` (from configure) that selects the correct target header.

### SOURCES Files (Build System)

Source file selection uses `SOURCES` files (not per-target Makefiles). These are preprocessed with the C preprocessor, using `#ifdef` conditionals based on target config defines. This is how a single build system handles all 80+ targets. Key SOURCES files: `firmware/SOURCES`, `apps-ipod/SOURCES`.

### Plugins: removed

There is no plugin system. Upstream's dynamically loaded `.rock` files, the
`plugin.h` API struct and `plugin_start()` are all gone; `ROCKS` is empty and
`make rocks` builds nothing. What were plugins are now either core screens
(text and image viewers, properties, playing time) or deleted outright. Do not
add one -- put the code in the core.

**Watch for stubs left behind by the removal.** A plugin-backed feature could
survive as an entry point that compiles, is reachable from a menu and does
nothing. The pitch screen was the last of these and is now gone entirely --
screen, keymap context, `ACTION_PS_*` codes, activity and settings. Nothing in
`apps-ipod/` can change pitch or speed any more, so **do not go looking for a
pitch UI**: `HAVE_PITCHCONTROL` is still defined because it gates real DSP code
in `lib/rbcodec`, and `global_status.resume_pitch`/`resume_speed` still exist
because `firmware/sound.c` and `lib/rbcodec/dsp/tdspeed.c` write to them. The
`%Sp`/`%Ss` skin tokens keep their renderers for the same class of reason --
the tags are defined in `lib/skin_parser`, so dropping the renderer would leave
a tag that parses cleanly and draws nothing.

`apps-ipod/plugins/` still exists but holds only support files. Live ones:
`plugin.lds` (the codec link, via `lib/rbcodec/codecs/codecs.make`),
`credits.pl` (`apps.make`), `bitmaps/` and `plugins.make` (`tools/root.make`).
`viewers.config`, `rockbox-fonts.config` and `CATEGORIES` are vestigial --
`tools/buildzip.pl` is byte-identical to upstream and reads its copies from
`apps/plugins/`, never from here.

### Codec System

Audio codecs live in `lib/rbcodec/` and are loaded as `.codec` files with their own API struct (`codecs.h`). The codec framework includes DSP processing (EQ, crossfeed, replaygain). Supports MP3, FLAC, Vorbis, Opus, AAC, ALAC, WavPack, APE, WMA, and many more.

### Memory Management

- **buflib** — Rockbox's custom compacting, handle-based allocator (`firmware/buflib*`). Two backends: `buflib_mempool` (bare-metal) and `buflib_malloc` (hosted platforms).
- **core_alloc** — Core allocation interface built on buflib.
- **TLSF** — Used for hosted/application builds (`lib/tlsf/`).

### Platform Types

This fork builds bare-metal native firmware only (`PLATFORM_NATIVE`), and
`apps-ipod/` no longer carries `SIMULATOR` or `PLATFORM_HOSTED` conditionals.
`tools/configure` still offers `--type=s`, but the simulator will not build.

**The repository mirrors upstream; only `apps-ipod/` is ours.** `manual/`,
`uisimulator/`, `android/`, `backdrops/`, `screenshots/`, the stock `wps/`
themes and every `themes/` entry but Themify_2 are all still present and all
unbuilt. They are kept deliberately so `git merge rockbox/master` applies
without delete/modify conflicts. Do not prune them to tidy the tree -- being
unused is not a reason to remove something here.

### Threading

Native assembler threads (ARM) with cooperative multitasking.

## Code Style

- **C only** (gnu11). Assembly only when necessary for performance.
- **4-space indentation**, no tabs. 80-column line limit. Unix LF line endings. UTF-8.
- **Naming:** all lowercase for variables, functions, structs, enums. UPPER_CASE for preprocessor symbols and enum constants. No mixed case. No typedefs for structs.
- **Comments:** `/* C-style only */`. Use `#if 0` to comment out blocks. No `//` comments.
- **Function braces** on a new line. Otherwise follow existing file style.
- When editing existing code, follow the style already present in that file.

## Key Tools

- `tools/configure` — build configuration (~4900-line shell script)
- `tools/rockboxdev.sh` — cross-compiler toolchain builder
- `tools/genlang` — language file processor
- `tools/bmp2rb` — bitmap converter for Rockbox
- `tools/convbdf` — BDF font converter
- `tools/scramble` / `tools/descramble` — firmware file format tools
- `tools/buildzip.pl` — creates deployment ZIP (kept byte-identical to upstream)
- `tools/convfnt` — this fork's 4bpp icon-font tool. Theme icon fonts are 4bpp
  and `convbdf`/BDF cannot round-trip them, so use this instead
- `tools/art_fetch/art_fetch.py` — fetches album and artist artwork
- `tools/voice.pl` — voice file generator (TTS). Paths point at `apps-ipod/` but
  voice builds are unverified here; `VOICE_VERSION` no longer resolves because
  `talk.h` moved, so `rockbox-info.txt` reports an empty `Voice format:`

## Release Workflow

**Use `./release.sh`.** It does the whole cycle from a clean tree:

```bash
export PODBOX_BUILD_SERVER=user@host   # or pass --server; never committed
./release.sh --dry-run vX.Y            # rehearse: builds + verifies, publishes nothing
./release.sh vX.Y                      # for real
```

It builds both targets on the build server from a `git archive` of HEAD, checks
each zip really contains the theme, the EQ presets and the binary, and only then
tags, pushes and creates the release -- so a failed build leaves no tag behind.
A `-alpha`/`-beta`/`-rc` suffix marks it a prerelease automatically. Release
notes come from the commits since the previous tag; `--since REF` overrides that
and is needed only when the previous tag isn't reachable from HEAD.

Three things about publishing that are easy to get wrong by hand, all handled
inside the script:

- **`gh`'s `file#text` sets a display LABEL, not the asset filename.** Both
  targets build a file called `rockbox.zip`, so uploading them as
  `build-hw-ipod6g/rockbox.zip#rockbox-ipod6g.zip` sends two assets both named
  `rockbox.zip`; the second collides and `gh` then deletes the release it just
  made, leaving a pushed tag and no release. Copy the zips to their published
  names *before* upload instead.
- **`gh` runs on the build server, so it needs `--repo`.** It infers the repo
  from the origin of the checkout it runs in, and the server's origin is
  upstream Rockbox, not this fork. Run from *this* machine, origin is the fork
  and `--repo` is unnecessary -- which is why the flag looks droppable and is
  not.
- **Commit with explicit paths, never `git add`** -- work is often left staged
  deliberately and a bare commit sweeps it in. `release.sh` does not commit; it
  refuses to run unless the tree is already clean.

Releases belong to this fork. Upstream remotes are not writable from here.