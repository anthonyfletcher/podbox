# Upstream commit ledger

PodBox is a fork of [Rockbox](https://www.rockbox.org/) that builds two targets:
iPod Classic 6G/7G (`ipod6g`) and iPod Video 5G/5.5G (`ipodvideo`). This file is
the per-commit record of what upstream Rockbox has landed and what PodBox did
about each one — adopted it, declined it, or judged it inapplicable.

Its companion, [`upstream-divergence.md`](upstream-divergence.md), answers the
by-path question instead: which files differ from upstream, and why.

## Two baselines

The tree has two histories. Conflating them is the main way to misread this
file.

| Tree | Baseline | What it means |
| --- | --- | --- |
| **`apps-ipod/`** — the application layer | **`dd21a1d1d9`** — 2026-02-10 | The rebase did **not** update this tree. It came from [RockPod](https://github.com/nuxcodes/rockpod.git), which was already ~5 months behind upstream. |
| `firmware/`, `lib/`, `tools/`, `apps/`, and everything else | **`24c3779146`** — 2026-07-24 | The rebase point. PodBox `master` descends from it, so all 39,454 upstream commits at or before it are **inherited**. No action, ever. |

### Why `apps-ipod/` and not `apps/`

PodBox's application layer is `apps-ipod/`. Upstream's is `apps/`. The `apps/`
directory still exists here as a byte-identical upstream mirror, kept so that
merges from Rockbox apply without delete/modify conflicts — **nothing builds
it**.

The consequence is the single most important fact in this file:

> **A clean merge is not adoption.** An upstream commit touching `apps/` merges
> without conflict and has **no effect on the firmware**. The counterpart under
> `apps-ipod/` must be ported by hand.

The same trap applies to `apps/lang/english.lang`, which looks authoritative
and is not compiled — `apps-ipod/lang/english.lang` is the one that builds.

Files in `apps-ipod/` carry a `was: apps/…` header marker giving the exact
upstream correspondence. Roughly a third of what looks applicable is not,
and some plugin-titled commits *are* applicable, because several upstream
plugins became core screens here.

## Status vocabulary

| Status | Meaning |
| --- | --- |
| **Adopted** | In PodBox. Often restyled, or ported from upstream's *net* current state rather than commit-by-commit. |
| **Adopted (in part)** | Some of the commit is here and some deliberately is not. The row says which half, so a plain **Adopted** is not read as more than happened. |
| **Adopted (independently)** | The behaviour exists here, reached separately rather than ported. Nothing to take. |
| **Declined** | Deliberately not taken. The reason is recorded so it is not re-litigated. |
| **N/A** | Touches a target, or a subsystem, that PodBox does not build. |
| **Open** | Applies, discretionary, not yet decided. |
| **Pending** | Applies, wanted, not yet done. Actionable. |
| **Inherited** | Arrived via a baseline. No action. |

## Triage by path

First match wins.

| Upstream path | Default |
| --- | --- |
| `apps/` | **Port by hand** into `apps-ipod/`, via the `was:` marker map. |
| `apps/plugins/` | **N/A** — PodBox has no plugin system. Check the file first: several plugins became core screens. |
| `firmware/target/…`, `firmware/export/config/…` for other targets | **N/A** — only `ipod6g` and `ipodvideo` build. |
| `firmware/` core, `lib/`, `tools/` | **Adopt** — check `upstream-divergence.md` for a local patch in the same file first. |
| `manual/`, `uisimulator/`, `android/`, `wps/`, other `themes/` | **N/A** — mirrored so merges apply; unbuilt, and not pruned deliberately. |

Two habits that repeatedly paid off:

1. **Check for a later commit on the same function before porting.** Upstream
   reverses itself. Two cancelling pairs and one four-week dead end were caught
   this way — each would have cost real effort to port and then unport.
   `git log --oneline <base>..rockbox/master -- <file>`
2. **Port the net, not the sequence,** wherever a cluster of commits converges
   on one design.

---

# Commit log

| Date | Upstream | Summary | Status | Note |
| --- | --- | --- | --- | --- |
| 2025-11-21 | `c2e1094383` | playback: reserve an aa slot for iap | **Adopted** | `MAX_MULTIPLE_AA` +1 under `USB_ENABLE_IAP`. Permanently inert — see *USB iAP* below — but harmless, and the count is then right by construction. |
| 2025-12-12 | `fad99773e3` | send iap status change notifications | **Declined** | USB iAP is off by policy and staying off — see *USB iAP* below. Every `iap_on_*` is an empty inline stub, so this is ~36 lines across 5 files compiling to nothing. |
| 2026-02-05 | `7eeb4e4302` | firmware: refactor CACHEALIGN_BITS/SIZE | **Adopted** | Compile-blocking after the rebase. |
| 2026-02-12 | `76d63246c5` | playback: don't hardcode pcm sink in audio_set_playback_frequency | **Adopted (in part)** | Only the compile-blocking parts were taken. Nothing in `apps-ipod/` calls `pcm_current_sink()` or `pcm_sink_caps()`, which is correct for a single-sink build — see *USB iAP* below. |
| 2026-02-13 | `f343168051` | settings_list: apply playback freq changes only when sink is builtin | **Declined** | Guards against a non-builtin PCM sink. There is only ever one sink here — see *USB iAP* below — so the guard can never change a decision. |
| 2026-02-13 | `f87ff3a9b2` | playback: support non-builtin sinks in audio_guess_frequency | **Adopted (in part)** | Compile-blocking parts only. `audio_guess_frequency()` (`playback.c:4368`) is still the sink-unaware switch on 44100/48000, which is all a single-sink build needs. |
| 2026-02-18 | `c199d9a369` | playback: fix single mode leaking next track before pausing | **Adopted** | Taken as upstream's *net* state, not this commit — upstream amended it since. The decision now happens when the change is scheduled. |
| 2026-02-19 | `3373ed6744` | playback: fix single mode with auto frequency switch | **Adopted** | With the above, as one net port. |
| 2026-02-21 | `017dd72ff3` | plugins: convert all plugins to mixer API | **N/A** | No plugin system. |
| 2026-02-23 | `c86fd2318d` | retain file browser directory on reboots | **Declined** | Not wanted. 116 lines across 8 files into browser code PodBox has reworked heavily. |
| 2026-02-23 | `e15451815a` | tagcache: prevent infinite scan/commit loop | **Adopted** | Already fixed here independently, with a better comment. Left as is. |
| 2026-02-24 | `17edcbd42a` | talk: improvements in voicing "years" | **Adopted** | Half-landed: `english.lang` documented `Y`/`y`, `talk.c` ignored the distinction, so "dAY" voiced 2020 as "two thousand twenty". |
| 2026-03-02 | `eafcbd3fd6` | debug_menu: 2nd SD/MMC card only if NUM_DRIVES > 1 | **N/A** | Other targets. |
| 2026-03-25 | `6928581bf9` | open_plugin_import fails to import full path | **N/A** | No plugin system. |
| 2026-03-31 | `4b9c78e01b` | filetree: restrict keep_directory to Files menu | **N/A** | Follow-up to declined `c86fd2318d`. |
| 2026-03-31 | `cb04b8167c` | pcm_mixer: introduce mixer_play_cbs | **Adopted** | Compile-blocking — the callback argument became a struct. |
| 2026-03-31 | `cfb01cfd58` | pcmbuf: remove pcmbuf_sampr | **Adopted** | Compile-blocking. |
| 2026-04-02 | `c765addd24` | eliminate default browser setting | **Declined** | It does apply — `browser_default` / `LANG_DEFAULT_BROWSER` are still live in `settings_list.c` and `root_menu.c` — but it *removes* a setting in favour of resuming whichever browser was last used. A UX opinion with no defect behind it, and `root_menu.c` has diverged here. PodBox keeps the explicit setting. |
| 2026-04-07 | `5ac105c837` | tagtree: add "Show in Files" | **Adopted** | Landed with the context-menu rework. |
| 2026-04-07 | `e405858b9e` | wps: replace "Open With"/"Delete" with "Show in Files" | **Adopted** | Same. `HOTKEY_OPEN_WITH` removed — it served a plugin system that does not exist. |
| 2026-04-09 | `27ebdfcb25` | settings: fix mismatched resume setting variable types | **Open** | Not separately decided. |
| 2026-04-13 | `719f0f1a3b` | settings: move USB settings to their own submenu | **Open** | Not present; PodBox's settings tree has diverged. |
| 2026-04-13 | `e85f120190` | playlist_viewer: character-based Now Playing indicator | **Adopted** | The playing track is bracketed `[like this]`, so it reads without colour or an icon. |
| 2026-04-15 | `f4dc4d89dc` | imageviewer: hide info by default when loading | **Open** | Plugin-titled but lands on `screens/covers/carousel.c`, which PodBox ships. |
| 2026-04-16 | `a1ccb79727` | pitchscreen: adjust keymaps for ipod and fiiom3k | **Adopted**, since removed | An iPod-specific fix, taken — then the pitch screen was deleted entirely as unreachable. |
| 2026-04-16 | `cc7418dd8b` | dsp: add option to swap left and right channels | **Adopted** | Half-landed: `lib/rbcodec` already implemented `SOUND_CHAN_SWAP`; only the setting was missing. |
| 2026-04-16 | `fd7ae09e7a` | FS#13864: last char of folder/filename not voiced | **Adopted** | |
| 2026-04-21 | `9ac6edf750` | add panicf to plugin and codec API | **Adopted** | Compile-blocking — new trailing member, left silently NULL. |
| 2026-04-24 | `2690418551` | imageviewer: use theme in all submenus | **Open** | As `f4dc4d89dc`. |
| 2026-04-24 | `c145d19e85` | gui: align display updates, reduce UI glitches | **Declined** ⚠ | A dead end upstream deleted four weeks later in `c0a8303a9c`. Ironically credited to RockPod's own anti-flicker work — but neither RockPod nor PodBox ever had `skin_defer_rendering`, so there was nothing to unwind. |
| 2026-04-26 | `5bbf1c8e5b` | tree: gui_synclist_scroll_stop on uninitialized list | **Adopted** | `update_dir()` could return -1 with the list uninitialised. Reachable with "remember last folder" pointing at a deleted directory. |
| 2026-04-26 | `6cf705886d` | skin: custom scrollbar OBOE | **Adopted** | `last_shown` was the item count, not the last index. Visible — Themify_2 draws its own scrollbar. |
| 2026-04-26 | `792a230c00` | FS#13877: use FONT_UI in the Equalizer sliders | **Adopted** | Sliders were a fixed 6px against a forced `FONT_SYSFIXED`; now sized off the font, minimum 6. |
| 2026-04-26 | `bf0fa29a30` | WPS Context Menu configurable entry | **Adopted** | 740 lines. The bottom five rows are assignable from Settings > WPS, sharing one action list with the browser hotkey. |
| 2026-04-28 | `7ab1a81806` | simple_viewer: use UI viewport and SBS title | **Adopted (in part)** | Only the `gui_synclist_scroll_stop()` from the `apps/screens.c` half, at `screens/playback/track_info.c:508` — without it a mid-scroll row keeps animating under the opened text view. The plugin API and `simple_viewer.c` halves are N/A. The theme enable/undo removal is **not** taken: this fork's `view_text()` owns the full screen with no themed SBS, a different design from upstream's. Tested on 5G. |
| 2026-04-29 | `121c65b32a` | FS#13857: keylock with USB (Fiio M3K) | **N/A** | Other target. |
| 2026-04-29 | `c41beebcda` | gui: delay updating SBS when setting list title | **Declined** | Half of a cancelling pair with `160905b1b8`. PodBox's `set_title` already matches upstream's settled version. |
| 2026-04-29 | `dbcee0deae` | gui: defer deadspace viewport update | **Adopted** | Part of the refresh campaign, taken as net state. |
| 2026-04-30 | `52edc2e069` | allow displaying the WPS/tree hotkey menu on hotkey press | **Adopted** | With the context-menu rework. |
| 2026-05-01 | `88d4903d10` | gui: fix "lock screens" making UI viewport disappear | **Adopted** | Refresh campaign, net state. |
| 2026-05-01 | `f886bfc572` | misc: GCC 16 + binutils 2.46 issues | **Adopted** | Compile-blocking, 5 files. |
| 2026-05-02 | `83e55164f4` | gui: remove SBS lock/unlock redraw lag | **Adopted** | Refresh campaign, net state. |
| 2026-05-03 | `42841d493f` | gui: inbuilt statusbar: defer viewport update | **Adopted** | Refresh campaign, net state. |
| 2026-05-03 | `6d699f08f4` | imageviewer: fix incomplete previous commits | **Open** | As `f4dc4d89dc`. |
| 2026-05-03 | `7e6ae1e0d8` | echoplayer: enable plugins | **N/A** | Other target, no plugins. |
| 2026-05-04 | `1d5aa53321` | playback: don't switch to a sampr the sink doesn't support | **Declined** | As `f343168051` — the builtin sink supports both 44.1 and 48 kHz, so the fallback it adds is unreachable. See *USB iAP* below. |
| 2026-05-04 | `89d24f3bd4` | list: fix GUI_EVENT_THEME_CHANGED timing | **Adopted** | Also removed a write through an `int*` to a `long` that only existed to pass a variable back to itself via the event system. |
| 2026-05-06 | `20194cb606` | gui: wps: render SBS and WPS in one batch | **Adopted** | Refresh campaign, net state. |
| 2026-05-06 | `7aca1d46b8` | quickscreen: fix flickering for GUI_EVENT_NEED_UI_UPDATE | **Adopted** | Only portable after the update-model swap. Viewports moved into `struct gui_quickscreen` so the callback paints directly. |
| 2026-05-06 | `b4c308d698` | splash: rework word wrap, escape characters | **Declined** | Head of a 140-line rework that PodBox's dialog framing, physical-display centring and padding would have to be re-applied onto. Only two splash calls use escapes, both `\n`. |
| 2026-05-07 | `05f1a6605d` | gui: skin_engine: fix dirty & force_waiting across screens | **Declined** | A fix *to* the `c145d19e85` dead end, and equally moot. |
| 2026-05-07 | `ce403586e0` | playlist_viewer: loading splash after delay | **Open** | One of four discretionary playlist_viewer enhancements. |
| 2026-05-08 | `325a028af4` | properties: clear UI viewport at startup | **Adopted** | Taken as the *net* with `bc528c4079`, not as a pair — this one alone made the viewport flash. `viewers/properties.c:373`. |
| 2026-05-08 | `ae871d25a9` | gui: skin_engine: reduce updates | **Declined** | Fix to the dead end. |
| 2026-05-09 | `bc528c4079` | properties: don't clear UI viewport for dirs | **Adopted** | The net with `325a028af4`. `struct viewport` must be `static` here as upstream had it — the scroll engine keeps the pointer, not a copy. Tested on 5G. |
| 2026-05-11 | `51abd937d5` | playlist viewer: retrieve track name id3 from db | **Adopted (independently)** | `playlist/viewer.c:274` already tries tagcache before falling back to a disk read. Upstream gates its version on `METADATA_EXCLUDE_ID3_PATH`; PodBox's is unconditional, which is the better choice on a spinning disk. Left as is. |
| 2026-05-11 | `9bda6389ce` | quickscreen: fix UI update when USB connected | **Adopted** | Exit goes through `default_event_handler_ex` with a cleanup callback, firing only for `SYS_USB_CONNECTED` and before `system_flush`. |
| 2026-05-12 | `1c39495ec2` | playlist_viewer: simplify format_line | **Declined** | Refactor, not a fix. |
| 2026-05-16 | `21fe45caad` | splash: string split logic, tab justify | **Declined** | No tabs anywhere in the tree, so it buys nothing. |
| 2026-05-16 | `d8db60b34a` | splash: infinite loop when viewport too small | **Adopted** | The one real crash fix in the splash chain — a viewport too narrow for one space spun the word wrap forever. |
| 2026-05-17 | `6e27ba80e4` | splash: trailing `\t` should not add spaces | **Declined** | With the splash chain. |
| 2026-05-17 | `d97e4425c6` | playlist_viewer: NULL instead of 0 in init | **Adopted** | |
| 2026-05-18 | `13a0e58b1c` | gui: usb_screen drawing adjustments | **Declined** | A genuine upstream change — `struct usb_screen_vps_t` predates the fork point — but it restructures viewports in a screen PodBox has rewritten. `screens/system/usb_screen.c` draws a skinned variant and its logo viewport is explicitly vestigial, so porting upstream's drawing adjustments is high cost for nothing visible. |
| 2026-05-18 | `58f75311d8` | merge font_getstringnsize and font_measurestring | **Inherited** | The firmware half landed with the rebase. Verified no silent semantic change: `font_getstringnsize` is now a wrapper, same contract, NULL-safe on `h`. No `apps-ipod/` change needed. |
| 2026-05-19 | `0492021247` | fix yellow in 13a0e58b1c | **Declined** | With `13a0e58b1c`. A `NULL` initialiser silencing a warning in code that is not ported. |
| 2026-05-19 | `bf8328fbe0` | rbcodec: fix build failure with DEBUG but no LOGF | **Adopted** | Compile-blocking. |
| 2026-05-21 | `04e557898f` | playlist: delay loading splash when adding indices | **Adopted** | The splash appeared for playlists indexed too fast to need it. |
| 2026-05-21 | `ae17d606be` | playlist_viewer: UI feedback when loading is delayed | **Declined** | Changes `playlist_viewer_init`'s signature and touches plugin-buffer sizing PodBox has changed. |
| 2026-05-22 | `edecad823e` | gui: list-skinned: fix scrollbar lag | **Adopted** | Deferred once on a wrong assumption, then taken: `sb_skin_force_next_update()` only bypasses the status bar's rate limiter, it does not force a full refresh. |
| 2026-05-23 | `6a252576f5` | bookmark: stop scrolling for skinned context menu | **Adopted** | |
| 2026-05-23 | `eb6746c1d6` | albumart: fix warning with GCC16 | **Adopted** | Compile-blocking. |
| 2026-05-24 | `c0a8303a9c` | gui: simplify screen updates | **Adopted** ★ | The anchor of the refresh campaign and the largest single port. `skin_render()` no longer flushes; `skin_update()` marks dirty and one place flushes at end of action. See the deviations note below. |
| 2026-05-25 | `21e9d3f449` | Hotkey Tree shares code with WPS Context | **Adopted** | With the context-menu rework. |
| 2026-05-25 | `e471fe4115` | FixRed: Tree Hotkey without HAVE_HOTKEY | **Adopted** | With the above. |
| 2026-05-26 | `239ba599fd` | FS#13908: hotkeys not saved when language changes | **Adopted** | Declined at first — it fixes a configurable context menu PodBox did not yet have — then taken once `bf0fa29a30` landed. Hotkeys are now stored in `config.cfg` by name. |
| 2026-05-26 | `2a29dedeb6` | gui: skin_display: draw album art first | **Adopted** | So mask images can be drawn over it. Themify_2 does exactly this. |
| 2026-05-27 | `018994e8c7` | gui: skinned lists: fix off-screen selection | **Adopted** | Refresh campaign, net state. |
| 2026-05-27 | `0c464c3d49` | gui: list-skinned: scrollbar not disappearing | **Adopted** | `needs_scrollbar` cleared with the cfg it belongs to. |
| 2026-05-27 | `358c6056ef` | gui: skinned list: set cfg to NULL when toggling theme | **Adopted** | So a scrollbar cannot survive a theme toggle that does not change activity. |
| 2026-05-28 | `160905b1b8` | gui: list: update skin in gui_synclist_set_title | **Declined** | The other half of the cancelling pair with `c41beebcda`. |
| 2026-05-28 | `35270d08e9` | bookmark: stop scrolling when leaving select screen | **Adopted** | The fix is one `gui_synclist_scroll_stop()` on exit; the work was making every exit path reach it. Upstream's ~40 lines of brace de-nesting not taken. |
| 2026-05-28 | `3b2555bd4d` | onplay wps context menus cleanup | **Adopted** | With the context-menu rework. |
| 2026-05-28 | `9f20c45a5e` | properties: fix stack overflow in db | **Adopted** | Plugin-titled, lands on `screens/browse/browser_db.c`, which PodBox ships. The filename buffer and `tagcache_search` struct now come from the caller. |
| 2026-05-29 | `3507f32d01` | properties: further reduce stack pressure | **Adopted** | With the above. |
| 2026-05-29 | `f0d3d76b26` | gui: list: clear skinlist cfg when selected_size isn't 1 | **Adopted** | A multi-row selection cannot be rendered from a stale cfg. |
| 2026-05-31 | `892fbe8d8f` | action: touchscreen: fix stuck repeated state | **N/A** | No touchscreen. |
| 2026-06-01 | `d54b9e6f8d` | chore: remove all vestigial CVS `$Id:$` tags | **N/A** | Cosmetic sweep. Inflates the raw diff of every commit around it — the refresh campaign is 525 real lines, not the 800–2300 the file diffs suggest. |
| 2026-06-02 | `a39e4f2a06` | skin_engine: get rid of skin_unload_all | **Adopted** | Had no callers left. |
| 2026-06-03 | `78ec149555` | allow softlock in additional screens | **N/A** | `ALLOW_SOFTLOCK` is `#define`d to 0 and `do_softlock()` is empty — PodBox dropped software keylock for the hardware hold switch, so `CONTEXT_TREE\|ALLOW_SOFTLOCK` is literally `CONTEXT_TREE`. |
| 2026-06-03 | `85adf518ac` | shortcuts: go to WPS for ACTION_TREE_WPS | **Declined** | Only works paired with `e6b4ec81ff`. |
| 2026-06-03 | `e6b4ec81ff` | simplelist: support ACTION_TREE_WPS | **Declined** ⚠ | Switches every simple list from `CONTEXT_LIST` to `CONTEXT_TREE`, which on this target rebinds PLAY from `ACTION_STD_CANCEL` to `ACTION_TREE_WPS` app-wide. A convenience feature, not a fix. |
| 2026-06-04 | `0836ebbd45` | shortcuts: 'File' shortcuts fail when dir filter set | **Adopted** | A shortcut to a file hidden by the current filter failed with "Failed reading". |
| 2026-06-05 | `1add6b0dd5` | shortcuts: eliminate unnecessary nesting | **Declined** | Cosmetic. |
| 2026-06-05 | `74905f4796` | skin_engine: remove get_skin_filename call | **Adopted** | It was called purely to fill a buffer nobody read. |
| 2026-06-11 | `4d773a3329` | onplay wps context menu plugin item namebuf | **N/A** | `HOTKEY_PLUGIN` has no meaning here and has been removed. |
| 2026-06-12 | `a824085057` | skin: add %pX tag for time-based playlist progress | **Adopted** | Half-landed — `lib/skin_parser` already advertised `%pX` while nothing rendered it. Taken from upstream's current file, not the commit. Track lengths cached (500 max), with a size-based estimate on ATA. Inert until a theme uses it. |
| 2026-06-14 | `58ce77fbe2` | tagtree: letter menus voiced with talkmenu off | **Adopted** | |
| 2026-06-17 | `d737cbb931` | Sansa As3525 debug menu scroll buttons | **N/A** | Other target. |
| 2026-06-19 | `81962808a2` | use core_alloc for Radio Presets | **N/A** | No radio on these targets. |
| 2026-06-24 | `0e3355de50` | keyboard: fix RTL (Hebrew/Arabic) on-screen keyboard | **N/A** | PodBox's keyboard is a 529-line click-wheel replacement, not a modification of upstream's 1634-line one. None of the four functions it patches exist. Whether PodBox's keyboard is RTL-correct is a separate, untested question. |
| 2026-06-27 | `3cd286d8f8` | metadata: add audio_fmt to get_metadata_ex | **Adopted** | Compile-blocking, 4 files. |
| 2026-06-27 | `3e08b86e4b` | FixRed for %pX: checkwps, ATA builds | **Adopted** | With `a824085057`. |
| 2026-06-28 | `24b0254d96` | metadata.c small cleanup | **Adopted** | Compile-blocking. |
| 2026-06-30 | `d87755c535` | FS#13944: FONT_UI loads last loaded font | **Adopted** | Half-landed — `firmware/font.c` already had `set_ui_font()` and nothing called it, so FONT_UI could pick up a theme's icon font instead of the configured UI font. |
| 2026-06-30 | `f4e9ba7f17` | FS#13943: single mode tracks under one second don't play | **N/A** | Reverted upstream two days later. Net effect is nothing. |
| 2026-07-02 | `ce88de54b8` | hosted: fix USB mode not initialized | **N/A** | Hosted targets. |
| 2026-07-02 | `ddc31e8ddc` | Revert FS#13943 | **N/A** | The revert of the above. |
| 2026-07-03 | `f11c89aae2` | usb: fix usb mode on DX50/DX90 | **N/A** | Other targets. |
| 2026-07-15 | `943b73851e` | playback: prevent crossfade of new track after pause | **Adopted** | Audio left in the PCM buffer after a manual pause got crossfaded into the next hand-picked track. |
| 2026-07-21 | `ea775fa501` | hibyr1: add USB DAC scaffolding | **N/A** | Other target. |
| 2026-07-26 | `31dfd5da2e` | playback: add Playlist Single Mode option | **Adopted** | `settings.h:115`, `settings_list.c:927`, `playback.c:2806`. The enum value is appended last, so stored settings keep their meaning. |
| 2026-07-26 | `4f6aac445f` | hiby: raise plugin buffer to 2MiB on 64MB targets | **N/A** | `hibylinux.h` only — not a PodBox target, and no plugin system. |
| 2026-07-27 | `5f129ef299` | tools: mkinfo handles echor1 symbols | **Pending** | Merges clean; PodBox's local `mkinfo.pl` hunk is elsewhere. Affects `rockbox-info.txt` reporting only, not the binary. `_bssend` is inert here, but `_?loadaddress` now also matches the bare symbol both PodBox linker scripts define alongside `_loadaddress` — compare the reported RAM size on `ipod6g` either side. |
| 2026-07-28 | `c54dddc2ac` | playback: fix Playlist Single Mode pause behavior | **Adopted** | Fixes the feature two rows above. Before it, Playlist mode paused after *every* track — see below. `playback.c:2806`. Tested on 5G: Playlist mode plays through and pauses only at the playlist boundary. |

# Noted exceptions

## USB iAP — off by policy, and staying off

Four rows above turn on one define, so the reasoning lives here rather than
being repeated.

**iAP is one protocol with two transports, and PodBox runs only one of them.**

| | Serial iAP | USB iAP |
| --- | --- | --- |
| Switch | `IPOD_ACCESSORY_PROTOCOL` | `USB_ENABLE_IAP` |
| State | **On**, both targets | **Off**, both targets |
| Code | `apps-ipod/iap/` | `firmware/usbstack/iap/` (vendored [libiap](https://github.com/mojyack/libiap)) |
| Wire | UART pins on the dock connector | USB, HID-framed |
| Carries | Commands only | Commands **and digital audio** |

`USB_ENABLE_IAP` is **applicable to this hardware**, however, **It stays off.** The payoff is digital audio out to an MFi dock or DAC — a
small, discontinued category of accessory. 

## Two deviations from upstream in `c0a8303a9c`, both load-bearing

The refresh-model swap follows upstream except in two places, and both are
deliberate:

- **Flush inhibition is kept.** Upstream deleted its equivalent; PodBox cannot.
  `action_userabort()` polls for cancel while a progress splash owns the screen,
  and without inhibition the status bar is redrawn over the splash and flushed
  on top of it. The flag moved from `skin_render` to `viewportmanager_update` —
  the layer that now flushes — and dirty flags survive it, so the next update
  still paints.
- **The list keeps its partial `update_viewport()` path.** Upstream leaves all
  flushing to the action handler; PodBox picks full-versus-partial per draw,
  which is worth keeping on this hardware. The old "pending flush" term became
  `skin_is_dirty()`, which clears as it reads, so the list and the action
  handler can never both flush one frame.

`skin_flush_dirty()` exists for the three places that draw outside the action
loop and must be visible immediately: the working indicator, the album-art build
spinner, and the status bar after the playlist viewer pops its activity. **A new
screen that renders without an action following it needs this too** — it is the
one way to get a stale screen in this model.

### `c54dddc2ac` — what it fixed

Worth keeping because the bug was worse than upstream's commit message suggests,
and the shape of it recurs: a `single_mode` value the tag comparison cannot
describe.

Before the fix, when the mode was `PLAYLIST` and the skip was *not* a new
playlist, the function fell through to the tag comparison — and that code cannot
handle this mode. `single_mode_get_id3_tag()` (`playback.c:2773`) has no
`SINGLE_MODE_PLAYLIST` case, so it returned `NULL`, the `previous_tag == NULL`
test short-circuited, and the function returned `true`. **Playlist mode paused
after every track**, making it unusable.

The fix answers for that mode in both directions rather than only the pausing
one:

```c
if (global_settings.single_mode == SINGLE_MODE_PLAYLIST)
    return skip_pending == TRACK_SKIP_AUTO_NEW_PLAYLIST;
```

**Untested on hardware.** To verify on `ipodvideo`: set Single Mode to Playlist,
play a multi-track playlist through — it should run to the end without pausing
between tracks, and pause when playback crosses into a new playlist.

---

## Keeping this current

All read-only.

```bash
git fetch rockbox

# New commits since the last row in section 2
git log --reverse --format='%h | %ad | %s' --date=short <last-listed>..rockbox/master

# What a commit touches -- this picks the triage rule
git show --stat --format='' <commit>

# Later commits on the same file? (cancelling pairs, dead ends)
git log --oneline <base>..rockbox/master -- <file>

# Already in the application layer? Never trust a clean merge.
grep -rn "<identifier>" apps-ipod/
```

To rebuild the `was:` marker map that decides whether an upstream `apps/` commit
lands on a file PodBox has:

```bash
find apps-ipod \( -name '*.c' -o -name '*.h' \) -print0 | xargs -0 awk '
/was:/ && !done[FILENAME] {
  l=$0; sub(/.*was:[ \t]*/,"",l); gsub(/[ \t\r]+$/,"",l)
  if (l ~ /^apps\//) { print l "\t" FILENAME; done[FILENAME]=1 }
}'
```