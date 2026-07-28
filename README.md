<p align="center">
<img src="docs/podbox/podbox-logo.png" alt="PodBox" width="400"/>
<p>

# PodBox

**New firmware for the iPod Classic and iPod Video** with a focus on simplifying
RockBox whilst providing album and artist art everywhere with colour schemes that 
follows the music.
---

## Does it run on my iPod?

| iPod | Generation | Years |
|---|---|---|
| **iPod Classic** | 6th / 7th gen | 2007–2014 |
| **iPod Video** | 5th / 5.5th gen | 2005–2006 |

Nothing else — not the Nano, Mini, Shuffle or Touch. If you have one of those,
you want [Rockbox](https://www.rockbox.org) itself, which supports 80+ players.

## What you get

**Your artwork, everywhere.**
Album covers sit beside the rows in the album browser, and artist photos beside
artist rows. Two carousels — **Album Covers** and **Artist Portraits** — hang off
the main menu, and you can go straight from a cover into that album, or from an
artist into their albums. A thumbnail cache builds quietly in the background
while the database is idle, so browsing stays fast.  There's also an art fetcher
application to help you populate your library with art.

**Colours that follow the music.**
The interface recolours itself from the current album art, through the lists and
the now-playing screen.

**A keyboard you can actually use.**
The pop-up grid keyboard is gone, replaced by a single-line editor driven
entirely by the click wheel.

**Themed throughout.**
A reworked version of the **Themify 2** theme ships as the default. Dialogs,
splashes and prompts have been standardised and im proved, and screens that used to 
break out of the theme no longer do.  There's also a mini spectrum visualiser for 
the now-playing screen.

**Your music library.**
The tag database is always on, is called Music rather than Database, and its
views can be promoted onto the main menu.

**Documents and pictures too.**
A text viewer that handles txt, fb2, epub, docx, pdf, md, html and rtf and an
image viewer that supports bmp, gif, jpeg and png.

## Installing

Grab `rockbox.zip` for your player and unzip it into the root of the iPod's
disk, so that the `.rockbox` folder sits alongside your music. That is the whole
update.

> **First time on this iPod?** A fresh player also needs the Rockbox bootloader
> installed once, which is a separate step and is not covered here — follow the
> [Rockbox installation guide](https://www.rockbox.org/manual.shtml) for your
> model first. If you are already running Rockbox or RockPod, unzipping is all
> you need.

Want your library to have artist photos and clean album art to show off? There
is a fetcher tool — see [`tools/art_fetch`](tools/art_fetch/README.md).

---

# For the technically curious

## Where it comes from

**Rockbox → PodBox**, with hardware and UI work borrowed from RockPod.

- **[Rockbox](https://www.rockbox.org)** is the long-running open-source firmware
  project covering 80+ players. This fork tracks it directly and merges from it.
- **[RockPod](https://github.com/nuxcodes/rockpod)** is a separate fork of
  Rockbox for these same two players. PodBox carries some of its work:
  iPod hardware support (SSD-aware power management, S5L8702 boot and clock
  fixes, codec and charging fixes) and builds on the album-art "dynamic colours".
- **Themify 2** is a theme by [Dook](https://d00k.net/).
  RockPod modified it; the version here is modified further again.

## What is different from Rockbox

**The platform is cut down to what these two iPods actually use.**

- **Two targets.** iPod Classic 6G/7G (S5L8702) and iPod Video 5G/5.5G (PP5022).
  The other 80+ targets are still in the tree but nothing selects them.
- **Bare-metal native only.** No simulator, no hosted/Android/SDL build, and no
  remote LCD, touchscreen, FM radio or recording — none of which these players
  have.
- **No plugin system.** The plugin loader and `open_plugin` are gone. The
  handful of plugins worth keeping became ordinary core screens: the text and
  image viewers, properties, playing time, credits.

**The application layer is a different tree.** `apps-ipod/` replaces Rockbox's
`apps/`: reorganised by purpose to make development easier, and every file 
carrying a header describing what it does. See 
[`apps-ipod/README.md`](apps-ipod/README.md).

**The skin language is extended** with tags this firmware understands — text
measurement, wrapping, selection state and more. See
[custom skin tags](docs/podbox/custom-skin-tags.md). Themes using them will
not render on stock Rockbox.

**Everything outside `apps-ipod/` stays close to Rockbox on purpose**, so that
merges keep applying. The exceptions — hardware fixes in `firmware/`, build
scripts, and a handful of others — are catalogued with their reasons in
[upstream divergence](docs/podbox/upstream-divergence.md).

`tools/checkwps` and `tools/database` both build and work -- CheckWPS
understands this fork's custom skin tags, so it can validate the shipped theme.
The simulator does not build.

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
../bundle-theme.sh && ../bundle-eqs.sh
```

Two of them are easy to get wrong by hand, and both fail quietly:

- **`--appsdir=apps-ipod` is required.** Rockbox's original `apps/` is still in
  the tree and is never built. Omit the flag and the build compiles `apps/`
  instead — you get working firmware with none of this fork's work in it.
- **`make zip` on its own is incomplete.** `tools/buildzip.pl` is deliberately
  kept close to Rockbox and knows nothing about this fork, so its zip has no
  theme, no first-boot `config.cfg` and no EQ presets. The two `bundle-*.sh`
  scripts add them and strip what a plugin-less build cannot use.

For an incremental rebuild, re-run everything except `configure` from inside the
existing build directory.

## Finding your way around

| Where | What |
|---|---|
| `apps-ipod/` | The application layer — all of this fork's UI work. **Start at [`apps-ipod/README.md`](apps-ipod/README.md)**, which explains the layout and where new code goes. |
| `firmware/` | Hardware abstraction, drivers, kernel. Close to Rockbox; changes are listed in [upstream divergence](docs/podbox/upstream-divergence.md). |
| `lib/rbcodec/` | Codecs and DSP. Rockbox's, unmodified. |
| `apps/` | Rockbox's original application layer. **Never built.** Kept so merges apply cleanly — do not edit it, and do not delete it. |
| `docs/podbox/` | This fork's own documentation. |

## Credits

I am not a C programmer; most of the larger changes here were made with Claude.

Built on the work of the [Rockbox](https://www.rockbox.org/) project, the
[RockPod](https://github.com/nuxcodes/rockpod) project, the [Themify 2](https://git.sr.ht/~dook/Themify) theme
by [Dook](https://d00k.net/) and art by [Thomodoro](https://thomodoro.com/).

## Licence

[GNU General Public License v2.0](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html)

See also [third-party licences](docs/LICENSES).
