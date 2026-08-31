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

**Comments describe the destination, not the journey.** A reader needs what the
code does and why it is that way, not how you got there.

**The trap exception is about content, not form.** A trap someone would
otherwise fall into earns its comment - but state it as a present-tense rule.
These two carry the identical warning:

```c
/* Trap: writing along rows instead is ~20% slower - the per-column state
 * moves from registers into arrays. */
```
```c
/* Turning this inside out to write along rows was tried and is ~20% slower. */
```

The second is the same fact told as a war story. Only the first is allowed.
This is the rule that gets broken most, because almost any rejected alternative
can be called a trap - so the test is the wording, not the justification. If you
write *was tried*, *used to*, *previously*, *at first*, *the old code*,
*turned out* or *we tried*, you are narrating. Rewrite the sentence as a rule.

**Be brief - and check it, don't feel it:**
- A blank line inside a comment block usually means it is too long. One
  paragraph is the target; a second needs a reason.
- A comment longer than the code it explains is suspect. File headers are
  exempt; they carry the file's parts map.

The journey belongs in the commit message, if anywhere.

**A document has a reader. Write to them, not to the next implementer.**
A guide, a reference or a README is read by someone trying to *use* the
thing - a theme author, a player owner, whoever runs the script. They are not
deciding whether the design was right, and telling them it was is noise. This
is the same fault as narrating in a comment, one level up: it is writing to
the wrong reader.

Three things to cut:

- **Design defences.** *and there should not be*, *that is deliberate*, *on
  purpose*, *considered and rejected*, *would be worse*, *is the right call*.
  The reader cannot act on any of it. A recommendation is not a defence -
  "use `auto` unless you need to pin the colours" tells them what to do, and
  stays.
- **Mechanism they cannot see.** Internals earn a line only when they predict
  something the reader will hit. "one cached bitmap serves every row" earns
  its place, because it says why no filter is offered; "the row config is
  remembered once the tag renders and is never cleared" does not.
- **Source paths and line numbers**, in any document whose reader does not
  have the tree open. `settings_list.c` means nothing to someone writing a
  theme. An internals document is the exception and should say so at the top.

The test, sentence by sentence: **what does the reader do differently for
having read this?** If the answer is nothing, it belongs in the commit message
or in `.specifications/`, not in the document. Apply it hardest to the
sentence you were proudest of.

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
# Simulator builds (clean) — output in build-sim-<target>[-win32]/
./build-sim.sh               # iPod Classic 6G (default), for this machine
./build-sim.sh 5g            # iPod Video 5G
./build-sim.sh 5g win        # cross-compile a 64-bit Windows .exe

# Hardware builds (clean) — output in build-hw-<target>/
./build-hw.sh                # iPod Classic 6G (default)
./build-hw.sh 5g             # iPod Video 5G
./build-hw.sh ipod6g         # explicit target name
./build-hw.sh ipodvideo      # explicit target name

# Incremental rebuild
cd build-hw-ipodvideo && make -j"$(nproc)" && make zip && ../bundle-theme.sh && ../bundle-help.sh && ../bundle-trim.sh

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
zip straight from `make zip` has **no Scrim, no first-boot `config.cfg`, no
setting explanations and no title trimming patterns**. Follow it with all three
bundle scripts -- `../bundle-theme.sh`, `../bundle-help.sh`,
`../bundle-trim.sh`. `./build-hw.sh` and `./build-sim.sh` both run all three; a
bare `make zip` runs none.

Scrim is the only theme in the build. The others in `themes/` are published
as their own release by `release.sh`, one zip each.

`bundle-help.sh` and `bundle-trim.sh` are the two whose absence is silent.
The first ships `docs/podbox/settings-help.txt` as
`.rockbox/docs/settings-help.txt`, and without it every **Explain** entry in a
setting's context menu finds no file and shows nothing. The second ships
`apps-ipod/trim.config` as `.rockbox/trim.config`, which is the whole of what
**Trim Titles** trims -- there is no compiled-in copy, so without it the
setting switches on and does nothing. Neither makes anything else misbehave, so
a zip built by hand without them looks finished.

`bundle-theme.sh` also deletes the `classic_statusbar` theme, which
`buildzip.pl` copies straight out of `wps/` without consulting `WPSLIST`. It
removes the directory *and* the two loose `classic_statusbar.{sbs,rsbs}` files
written beside it -- a `wps/classic_statusbar/*` pattern does not match those.

**Configure build types:** (N)ormal, (B)ootloader, (C)heckWPS, (D)atabase and
(S)imulator all work. (W)arble is offered by `tools/configure` and does not.

**The simulator builds and runs** -- SDL2 and a host compiler, output
`rockboxui`, storage in `simdisk/` beside it. A Windows `.exe` cross-compiles
with `--type=as6` (that is **(A)dvanced** plus `s` and `6`; plain `--type=s6`
silently gives a *normal* build). Follow either with `make zip` and the four
`bundle-*.sh` scripts, then unzip into `simdisk/`, or it starts themeless.
Read `apps-ipod/sim/README.md` before touching any of it.

```bash
mkdir /tmp/cwps && cd /tmp/cwps
<root>/tools/configure --target=ipodvideo --type=c --appsdir=apps-ipod && make
```
Three fork-specific notes, since the tools' own READMEs are upstream's and say
nothing about any of them:

- **CheckWPS must run from inside a `.rockbox` directory.** Skin font paths are
  relative to the on-device layout and will not resolve from anywhere else --
  it then fails on the fonts rather than the skin.
- **It links this fork's skin engine, not upstream's**, so it accepts every tag
  the player does. `tools/checkwps/SOURCES`, `checkwps.make` and
  `tools/checkwps/include/` are all fork files now; `tools/checkwps/stubs.c`
  stands in for the rest of the firmware and says which caller relies on each
  answer. A new setting whose callback is not in there fails the link.
- **A failure after the parse prints no line and no caret** -- a missing font or
  bitmap is a `debugf`, which only `-v` shows. On a case-sensitive filesystem
  that is usually a filename's capitalisation, which FAT on the player does not
  care about.

The `--type=c` build needs no SDL. `configure`'s SDL check used to run for it
anyway, since `[ -n \`echo $app_type | grep sdl\` ]` collapses to a bare
`[ -n ]` and is always true; it is a real test now, so CheckWPS and warble skip
it.

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
`tools/buildzip.pl` reads its copies from a hardcoded `apps/plugins/`, never
from here.

### Codec System

Audio codecs live in `lib/rbcodec/` and are loaded as `.codec` files with their own API struct (`codecs.h`). The codec framework includes DSP processing (EQ, crossfeed, replaygain). Supports MP3, FLAC, Vorbis, Opus, AAC, ALAC, WavPack, APE, WMA, and many more.

### Memory Management

- **buflib** — Rockbox's custom compacting, handle-based allocator (`firmware/buflib*`). Two backends: `buflib_mempool` (bare-metal) and `buflib_malloc` (hosted platforms).
- **core_alloc** — Core allocation interface built on buflib.
- **TLSF** — Used for hosted/application builds (`lib/tlsf/`).

### Platform Types

The **firmware** is bare-metal native only (`PLATFORM_NATIVE`). The
**simulator** is `PLATFORM_HOSTED|PLATFORM_SDL` and does build.

`apps-ipod/` still carries almost no `SIMULATOR` conditionals: the sim is made
to fit it, not the other way round, by a shim directory (`apps-ipod/sim/`) that
supplies the symbols a hosted build lacks. The exceptions -- places where the
code itself would not compile, mostly ARM inline assembly whose `CPU_ARM` guard
had been flattened -- are tabulated in `apps-ipod/sim/README.md`. **Read that
file before adding a `#ifdef SIMULATOR` anywhere**; the rule is that a shim
must fail the way the caller already handles, and a guard is the escalation.

**The repository mirrors upstream; only `apps-ipod/` is ours.** `manual/`,
`android/`, `backdrops/`, `screenshots/`, the stock `wps/` themes and every
`themes/` entry this fork did not convert are all still present and all
unbuilt. They are
kept deliberately so `git merge rockbox/master` applies without delete/modify
conflicts. Do not prune them to tidy the tree -- being unused is not a reason
to remove something here.

`uisimulator/` is upstream-identical as well, but it is **built** -- the
simulator uses it as-is, along with `firmware/target/hosted/sdl/`. Neither
needed a fork change.

### Threading

Native assembler threads (ARM) with cooperative multitasking.

## Adding, Removing or Renaming a Setting

A setting lives in five places. Change one and the other four go stale
silently -- nothing in the build checks any of this, and the failures are all
invisible: **Explain** shows nothing, Search cannot find the row, and the guide
describes a player that no longer exists.

Touch all five, in this order:

1. `apps-ipod/settings/settings_list.c` -- the setting itself, and its cfg name.
2. `apps-ipod/lang/english.lang` -- the name shown on screen.
3. `apps-ipod/settings/settings_tags.c` -- the topic it is about, and whether
   it is advanced. **Untagged means unfindable**: Search skips it, and the
   completeness check below goes blind to it.
4. `docs/podbox/settings-help.txt` -- the **Explain** text, keyed by cfg name.
5. `docs/podbox/settings-guide.md` -- one table row, in the section for the
   screen the setting sits on, in menu order.

A setting with no menu entry at all -- remembered state such as the quickscreen
slots, the root menu order or the start directory -- takes step 1 only. Search
skips anything with no name to show, so the other four have nothing to say.

An **action** row (`MENUITEM_FUNCTION`) has no cfg name, so it is keyed in
`settings-help.txt` by its own on-screen label under an `action: ` namespace.
Renaming the row orphans the stanza. Dynamic-text rows are excluded on purpose
and take no stanza.

`settings-guide.md` is written from `settings-help.txt`: the same text, with
the paragraphs joined into one table cell. Where the two disagree the file is
right and the guide is stale -- edit the guide, not the file. The guide's own
**Library -- Maintenance** table is the exception; it summarises the action
rows in its own shorter words.

### Checking it

```sh
sh tools/check-settings-docs.sh    # from the repository root
```

Silence means the documents agree with the settings; every line it prints names
a setting that has fallen out of step and the file it is missing from. It runs
five checks, and check 2 is the one that earns its keep: check 1 asks
`settings_tags.c` what the settings are, so an untagged setting is absent from
both sides of that comparison and passes it without ever having been looked at.
That is how **Audiobook Art Rows** and **Segregate Audiobooks** shipped with no
explanation.

Check 2 reads `settings_list.c` with a regexp and so has a tail of known false
positives -- the remembered-state settings and three lang description strings.
They are filtered by a `KNOWN_UNDOCUMENTED` list in the script; a genuinely
undocumented new setting goes in the documents, not in that list.

Check 5 covers the action rows, which the other four cannot see: a stanza keyed
`action: ` is checked against the phrases in `english.lang`, so renaming a row
is caught rather than silently orphaning its explanation.

**What no check sees.** A setting reached through an action row has *two*
stanzas -- one under its cfg name, one under `action: ` -- and only the second
is ever shown. `settings-guide.md` quotes the first. Change one and the other
does not follow.

### Writing the text

Write for someone who has just found the setting and does not know what it
does. Say what it changes, and where it is useful or costly. Two or three
sentences: the screen is 320 pixels wide, and the reader truncates a line past
160 characters. Wrap at the file's usual width -- lines within a paragraph are
joined when read.

`bundle-help.sh` ships the file as `/.rockbox/docs/settings-help.txt`. A bare
`make zip` does not run it, and nothing else misbehaves without it, so a
hand-built zip with every **Explain** entry silently empty looks finished.

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
- `tools/buildzip.pl` — creates deployment ZIP. Kept as close to upstream as
  possible: its only local change is an `$APPSDIR` for the two files genuinely
  shipped *from* the application layer. Fork packaging goes in the
  `bundle-*.sh` scripts instead
- `tools/convfnt` — this fork's 4bpp icon-font tool. Theme icon fonts are 4bpp
  and `convbdf`/BDF cannot round-trip them, so use this instead
- `tools/art_fetch/art_fetch.py` — fetches album and artist artwork
- `tools/check-settings-docs.sh` — whether `settings-help.txt` and
  `settings-guide.md` still describe the settings that exist. See **Adding,
  Removing or Renaming a Setting** above
- `tools/voice.pl` — voice file generator (TTS). Paths point at `apps-ipod/` but
  voice builds are unverified here; `VOICE_VERSION` no longer resolves because
  `talk.h` moved, so `rockbox-info.txt` reports an empty `Voice format:`
**Theme Lens lives outside the tree**, in the git-ignored `.build/theme-lens/`
— the skin reader, linter and previewer. `python serve.py <theme folder>` (or
double-click `ThemeLens.cmd`) edits that folder live; `ctl.py` drives a running
one from a script. **It carries its own copy of the tag table**, so a change to
`apps-ipod/skin/custom_tags.c` has to be mirrored into `index.html` or the lens
explains tags that no longer exist. Its list fixtures are not mirrored —
`extract-lists.py` reads them out of this checkout, so re-run it with `--embed
index.html` after a menu change.

It is not ready to ship, so it is not versioned here. A fresh clone will not
have it.

## Debugging a Skin

**Read `.specifications/claude-skin-helper.md` before reading the skin engine.**
It is the resolution model -- what decides a viewport's colours, font and
rectangle, and the cross-file coupling that a skin file itself does not show.
`.specifications/skin-tag-reference.md` is the companion, one entry per tag.

Most skin questions are "what did the parser decide", and three tools answer
that without reading C. Cheapest first:

```sh
.build/skinlint.exe theme.sbs             # syntax only, instant, git-ignored
checkwps --viewports theme.sbs theme.wps  # the resolved viewport table
```

`--viewports` prints each viewport's rectangle, font and colours, and marks
every colour `set`, `sbs` or `cfg` -- named on that viewport's own line,
inherited from the `.sbs` `%Vi`, or neither. **Name the `.sbs` first**: the
`.wps` then resolves against it the way it does on the player, which is the
only way that inheritance is visible at all. It ends with a count, and a `.wps`
reporting most of its viewports as `sbs` is a theme living off the browser
list's colours. Theme Lens (above) is the third tool, for looking at the result
rather than the decisions.

**Build CheckWPS on the build server**, in a scratch directory rather than the
tree:

```sh
mkdir ~/cwps && cd ~/cwps
~/podbox/tools/configure --target=ipodvideo --type=c --appsdir=apps-ipod && make -j"$(nproc)"
```

It does not build on a Windows host, and the reason is worth knowing because it
is not specific to this tool: `tools/functions.make:24` delimits a `sed`
expression with `:`, so a `C:` drive letter splits it and `preprocess` returns
an **empty source list** -- make then links a binary with no objects rather than
reporting anything. Forcing Git-Bash-style paths past that reaches POSIX calls
in `firmware/target/hosted/filesystem-app.c` (`os_lstat`, `S_ISLNK`,
`localtime_r`) that MinGW does not have.

## Release Workflow

**There are no version tags.** Every release is the same rolling one, `latest`,
whose tag moves to the commit that was built. The release page always shows the
current build and nothing else.

**Use `./release.sh`.** It does the whole cycle from a clean tree:

```bash
export PODBOX_BUILD_SERVER=user@host   # or pass --server; never committed
./release.sh --dry-run                 # rehearse: builds + verifies, publishes nothing
./release.sh                           # for real
```

It builds both targets on the build server from a `git archive` of HEAD, checks
each zip really contains the theme and the binary, and only then replaces the
releases -- so a failed build leaves the previous ones standing. Three releases
go out: `Themes`, `Simulator` and then `latest`. **`latest` is published last
on purpose** -- GitHub features the release created most recently, and that is
the one the repository's front page offers.
Release notes list every commit since the last release, using the `latest` tag
itself as the start point -- read from origin, since nothing is tagged locally
(a rolling tag in the dev checkout only goes stale). That tag always names a
commit this script published, which is why no `--since` escape hatch is needed
any more.

Four things about publishing that are easy to get wrong by hand, all handled
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
- **Delete the old tag before creating the release.** `gh release create` reuses
  an existing tag rather than moving it, so a leftover `latest` would publish the
  new zips against an old commit. `gh release delete --cleanup-tag` handles the
  normal case; a `git push --delete` covers a tag orphaned by a failed run.
- **Commit with explicit paths, never `git add`** -- work is often left staged
  deliberately and a bare commit sweeps it in. `release.sh` does not commit; it
  refuses to run unless the tree is already clean.

Releases belong to this fork. Upstream remotes are not writable from here.

That workflow invokes `rockboxdev.sh` as `bash ./tools/rockboxdev.sh`, not
`./tools/rockboxdev.sh`. The script says `#!/bin/sh` but uses `BASH_SOURCE` to
locate its own `toolchain-patches/`, so under a `sh` that isn't bash it looks in
the wrong directory and quietly builds an *unpatched* gcc.