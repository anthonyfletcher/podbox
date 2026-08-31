# `apps-ipod/` — the application layer

## What this is

Everything above the hardware: the user interface, the playback engine, the
database, settings and input. If you are changing what PodBox *does*
rather than how it talks to the iPod, you are almost certainly working in here.

It began as a copy of Rockbox's `apps/` and has been reorganised — from one flat
directory of ~200 files into directories named for what the code is *for*, with
the largest catch-all files split up. `apps/` is still in the tree beside it,
untouched and never built, so that merges from Rockbox keep applying cleanly.

**The build only finds this directory because it is told to.**
`tools/configure --appsdir=apps-ipod` is what selects it. Leave that flag off and
the build compiles Rockbox's `apps/` instead and succeeds — producing firmware
with none of this work in it. `build-hw.sh` always passes it.

## Finding a file's Rockbox original

Nearly every file here carries a `was:` line in its header recording where it
came from:

```c
/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/skin_engine/skin_engine.c
 ...
```

That line is the map between the two trees, and it is worth knowing about before
you need it. Use it in both directions:

- **Porting a Rockbox fix.** Find which file here corresponds to the `apps/`
  file upstream changed, rather than guessing from the name.
- **Understanding a file.** `git log` on the upstream path shows history this
  tree does not have.

Build the whole map at once with:

```sh
grep -rn "was: apps/" --include=*.c --include=*.h .
```

Files with no `was:` line were written for this fork, or came from RockPod;
upstream changes cannot reach them by definition.

**The two header lines go together.** A banner saying `Original code from
RockBox` must carry a `was:` line as well, even where the path is unchanged
from `apps/`. Eleven files once said the first without the second — `main.c`,
`root_menu.{c,h}` and all of `iap/` — which left holes in the map at exactly
the files an upstream fix is most likely to touch, and made absence-of-`was:`
useless as a test of who wrote a file. Anything reading these headers to tell
fork code from upstream code is entitled to assume the pair.

## The organising idea

One distinction does most of the work — **how much of the screen and the input
loop does this code own?** It runs in four steps, and where a file sits in that
sequence decides its directory:

```
draw/      →   widgets/    →   screens/    →   viewers/
pixels         controls        whole           whole screen
only           you call        screens         + a file format
```

`draw/` is handed a place to put pixels and knows nothing about why.
`widgets/` is called by someone, does its job and hands control back.
`screens/` is navigated *to* and runs its own loop until the user leaves.
`viewers/` does that too, and additionally owns a file format and a document
model.

Everything else is grouped by subject: audio code in `audio/`, settings in
`settings/`, and so on.

## Layout

`main.c`, `root_menu.c/.h` — startup and top-level navigation. The only code
that belongs to no single domain.

### The device

| Directory | Contents |
|---|---|
| `audio/` | The playback pipeline: playback, pcmbuf, buffering, codecs, the codec and audio threads, voice, beep, A-B repeat, and the peak/spectrum meters (they sample PCM; nothing calls them). |
| `database/` | The tag database itself: tagcache. The database *browser* is `screens/browse/browser_db.c` — a screen backend, not a data layer. |
| `playlist/` | Playlists in memory and on disk, plus the playlist viewer and catalog. |
| `metadata/` | Per-track data: album art, the art cache (album **and** artist), cuesheets, multi-file ID3 aggregation. |
| `files/` | The disk seen as files rather than as music: what each extension opens, file operations, the flat document and image indexes, path lists. |

### The interface

Ordered by how much they own, as above:

| Directory | Rule |
|---|---|
| `draw/` | Stateless drawing. Knows pixels; knows nothing about features. viewport, line, scrollbar, round_rect, progress_bar, icon, bmp, resize, jpeg decode, screen_access. |
| `widgets/` | Reusable controls. Called by someone else, hand control back. list, dialog, yesno, splash, option_select, colour picker, folder select, keyboard, menu, text_box. |
| `screens/` | Full screens. Navigated *to*; run their own loop until the user leaves. |
| `viewers/` | File-format applications: their own model, view and input. `text_viewer/` (streaming document engine), `image_viewer/`, `lyric_viewer/`, `playback_viewer/` (the Spun deck), properties, playing_time, and the text reel behind the credits and About pages. |
| `games/` | Games. One directory each: `spike/`. A game owns the screen and the whole keymap the way a viewer owns a file format, and runs on a clock of its own rather than on user input, which is why it is not a `screens/` entry. |
| `skin/` | The skin interpreter, backdrops, and the skinned status bar. |

`screens/` is subdivided:

- `browse/` — the browser and its two backends (disk, db), album charts,
  featured artists
- `covers/` — cover flow: album covers, artist portraits
- `playback/` — wps, track info, quick screen
- `settings/` — the settings screens, every `*_settings` file
- `system/` — debug menu, log viewer, usb, runtime info, time set, the three
  searches (database, files, playlist names), background tasks, art health,
  about

and at the top level: bookmark, context_menu, shortcuts, main_menu, and the two
menu editors behind it (main_menu_config, music_menu_config).

### Services

| Directory | Contents |
|---|---|
| `settings/` | The settings model and its table. |
| `input/` | Buttons, actions, keymaps. |
| `speech/` | Voice output and translation. |
| `system/` | Cross-cutting glue: shutdown and system events, the activity stack, string/path helpers, time formatting, volume, and `app_util` (assorted small UI helpers). |

### Boundaries

| Directory | Contents |
|---|---|
| `iap/` | Apple accessory protocol. |
| `sim/` | What the desktop simulator needs and hardware does not. Built only in a simulator build, and **read `sim/README.md` before adding a `#ifdef SIMULATOR` anywhere** — the rule is that a shim fails the way the caller already handles. |
| `api/` | The headers that `firmware/` and `lib/` include by name. **Read `api/README.md` before touching anything here** — moving a header inside `apps-ipod/` can break a file outside it that you cannot edit. |

### Pinned by the build system, not by code

These three directories cannot be renamed or moved, because something outside
`apps-ipod/` names their paths directly:

| Directory | Named from |
|---|---|
| `bitmaps/` | `tools/root.make` (`include $(COREAPPSDIR)/bitmaps/bitmaps.make`) |
| `lang/` | `tools/root.make` (`lang/lang.make`), and `tools/configure`'s `picklang()`. **This is the language file that builds** — upstream's `apps/lang/` is not compiled, however authoritative it looks. |
| `plugins/` | Contains no plugin. `tools/root.make` names these paths, and `plugin.lds` is the **live** codec linker script — see `lib/rbcodec/codecs/codecs.make` and `plugins/README.md`. |

## Where does new code go?

Apply the four-step distinction in order:

| Question | Destination |
|---|---|
| Does it put pixels somewhere it was handed, with no input loop and no knowledge of what it is drawing for? | `draw/` |
| Is it called by something else, and does it hand control back when done (a question, a picker)? | `widgets/` |
| Is it navigated to, owning the screen until the user leaves? | `screens/` |
| Does it also own a file format and a document model? | `viewers/` |
| Does it also own the whole keymap, and run on a clock rather than on input? | `games/` |

Everything else goes by subject matter. If a file would land in `system/`, stop
and check whether it really belongs to a domain — `system/` is the residue, not
the default.

## Conventions

Includes are written relative to `apps-ipod/`:

```c
#include "widgets/splash.h"      /* not "splash.h"   */
#include "draw/viewport.h"       /* not "viewport.h" */
```

Every file carries a header giving provenance, attribution, licence and a short
description of what it does — including the `was:` line described above.

C (gnu11), 4-space indent, no tabs, 80 columns, `/* C-style comments */`, and
match the style of the file you are in.
