# Upstream commit ledger

PodBox is a fork of [Rockbox](https://www.rockbox.org/) that builds two targets:
iPod Classic 6G/7G (`ipod6g`) and iPod Video 5G/5.5G (`ipodvideo`). This file is
the per-commit record of what upstream Rockbox has landed and what PodBox did
about each one — adopted it, declined it, or judged it inapplicable.

Its companion, [`upstream-divergence.md`](upstream-divergence.md), answers the
by-path question instead: which files differ from upstream, and why.

## Two baselines, two parents

The tree has two histories. Conflating them is the main way to misread this
file.

| Tree | Baseline | What it means |
| --- | --- | --- |
| **`apps-ipod/`** — the application layer | **`dd21a1d1d9`** — 2026-02-10 | The rebase did **not** update this tree. It came from [RockPod](https://github.com/nuxcodes/rockpod.git), which was already ~5 months behind upstream. |
| `firmware/`, `lib/`, `tools/`, `apps/`, and everything else | **`2d2b03d314`** — merged 2026-08-08 | The last upstream commit merged in. Everything at or before it is **inherited**; no action, ever. It supersedes the `24c3779146` rebase point of 2026-07-24, and moves again with each merge. |

**`apps-ipod/` therefore has two parents, not one.** Rockbox is upstream of
everything, but RockPod is upstream of this directory specifically, and it has
gone on developing since `dd21a1d1d9`. A defect fixed there is usually cheaper
to take than the same defect fixed in Rockbox, because the files are closer —
often identical apart from include paths. It is also easier to miss, because
nothing about a Rockbox merge will ever mention it.

The two are tracked in separate tables below.

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
| **Superseded** | Both parents fixed the same thing and PodBox took the other one. The row says which, so the unused fix is not later mistaken for a gap. |
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

Two rules, both earned:

1. **Check for a later commit on the same function before porting.** Upstream
   reverses itself; this file records two cancelling pairs and one dead end.
   `git log --oneline <base>..rockbox/master -- <file>`
2. **Port the net, not the sequence,** wherever a cluster of commits converges
   on one design.

---

# Rockbox commit log

| Date | Upstream | Summary | Status | Note |
| --- | --- | --- | --- | --- |
| 2025-11-21 | `c2e1094383` | playback: reserve an aa slot for iap | **Adopted** | `MAX_MULTIPLE_AA` +1 under `USB_ENABLE_IAP`. Permanently inert — see *USB iAP and serial iAP* below — but harmless, and the count is then right by construction. |
| 2025-12-12 | `fad99773e3` | send iap status change notifications | **Declined** | USB iAP is off by policy and staying off — see *USB iAP and serial iAP* below. Every `iap_on_*` is an empty inline stub, so this is ~36 lines across 5 files compiling to nothing. |
| 2026-02-05 | `7eeb4e4302` | firmware: refactor CACHEALIGN_BITS/SIZE | **Adopted** | Compile-blocking after the rebase. |
| 2026-02-12 | `76d63246c5` | playback: don't hardcode pcm sink in audio_set_playback_frequency | **Adopted (in part)** | Only the compile-blocking parts were taken. Nothing in `apps-ipod/` calls `pcm_current_sink()` or `pcm_sink_caps()`, which is correct for a single-sink build — see *USB iAP and serial iAP* below. |
| 2026-02-13 | `f343168051` | settings_list: apply playback freq changes only when sink is builtin | **Declined** | Guards against a non-builtin PCM sink. There is only ever one sink here — see *USB iAP and serial iAP* below — so the guard can never change a decision. |
| 2026-02-13 | `f87ff3a9b2` | playback: support non-builtin sinks in audio_guess_frequency | **Adopted (in part)** | Compile-blocking parts only. `audio_guess_frequency()` (`playback.c`) is still the sink-unaware switch on 44100/48000, which is all a single-sink build needs. |
| 2026-02-18 | `c199d9a369` | playback: fix single mode leaking next track before pausing | **Adopted** | Taken as upstream's *net* state, not this commit — upstream amended it since. The decision now happens when the change is scheduled. |
| 2026-02-19 | `3373ed6744` | playback: fix single mode with auto frequency switch | **Adopted** | With the above, as one net port. |
| 2026-02-21 | `017dd72ff3` | plugins: convert all plugins to mixer API | **N/A** | No plugin system. |
| 2026-02-23 | `c86fd2318d` | retain file browser directory on reboots | **Declined** | Not wanted. 116 lines across 8 files into browser code PodBox has reworked heavily. |
| 2026-02-23 | `e15451815a` | tagcache: prevent infinite scan/commit loop | **Adopted (independently)** | The same guard exists here, with a fuller comment. Nothing to take. |
| 2026-02-24 | `17edcbd42a` | talk: improvements in voicing "years" | **Adopted** | Only the `talk.c` half was missing: `english.lang` documents a `Y`/`y` distinction that `talk.c` ignored, voicing 2020 as "two thousand twenty" for "dAY". |
| 2026-03-02 | `eafcbd3fd6` | debug_menu: 2nd SD/MMC card only if NUM_DRIVES > 1 | **N/A** | Other targets. |
| 2026-03-25 | `6928581bf9` | open_plugin_import fails to import full path | **N/A** | No plugin system. |
| 2026-03-31 | `4b9c78e01b` | filetree: restrict keep_directory to Files menu | **N/A** | Follow-up to declined `c86fd2318d`. |
| 2026-03-31 | `cb04b8167c` | pcm_mixer: introduce mixer_play_cbs | **Adopted** | Compile-blocking — the callback argument became a struct. |
| 2026-03-31 | `cfb01cfd58` | pcmbuf: remove pcmbuf_sampr | **Adopted** | Compile-blocking. |
| 2026-04-02 | `c765addd24` | eliminate default browser setting | **Declined** | It does apply — `browser_default` / `LANG_DEFAULT_BROWSER` are still live in `settings_list.c` and `root_menu.c` — but it *removes* a setting in favour of resuming whichever browser was last used. A UX opinion with no defect behind it, and `root_menu.c` has diverged here. PodBox keeps the explicit setting. |
| 2026-04-07 | `5ac105c837` | tagtree: add "Show in Files" | **Adopted** | Landed with the context-menu rework. |
| 2026-04-07 | `e405858b9e` | wps: replace "Open With"/"Delete" with "Show in Files" | **Adopted** | Same. `HOTKEY_OPEN_WITH` removed — it served a plugin system that does not exist. |
| 2026-04-09 | `27ebdfcb25` | settings: fix mismatched resume setting variable types | **Adopted** | `last_screen` and `resume_modified` are `int`, and `root_menu_setup_screens()` guards `new_screen >= NUM_ITEMS`. The narrow types were the defect: `SYSTEM_STATUS` flags both `F_T_INT` and `settings.c` loads and saves through an `int` pointer, which stays in bounds only on alignment padding. No migration is involved — `.resume.cfg` is a text cfg, so a member's C type never reaches the disk format. Deviation: the `(char)` cast at `root_menu.c` is dropped, where upstream keeps its two. The Start Screen and resume paths are not separately exercised on hardware. |
| 2026-04-13 | `719f0f1a3b` | settings: move USB settings to their own submenu | **Adopted (independently)** | Reached by this fork's own settings re-cut rather than ported: `usb_mode`, `usb_hid`, `usb_keypad_mode` and `usb_audio` sit in a `usb_menu` titled **USB** under System (`screens/settings/general_settings.c`), `usb_audio` under `#ifdef USB_ENABLE_AUDIO`. Upstream's `HAVE_USB_MODE` gate is not used, and the commit's bulk is `manual/`, which is not built. Nothing to take. |
| 2026-04-13 | `e85f120190` | playlist_viewer: character-based Now Playing indicator | **Adopted** | The playing track is bracketed `[like this]`, so it reads without colour or an icon. |
| 2026-04-15 | `f4dc4d89dc` | imageviewer: hide info by default when loading | **Adopted (in part)** | Lands on `viewers/image_viewer/`, the core port, not on `screens/covers/` — only a 2-line `pictureflow.c` hunk touches the carousel. Taken: the 250ms grace before a progress dialog appears, as `splash_progress_set_delay(HZ/4)` at the decode call. Not taken: the `hide_info` **setting**, which this viewer does not need — `cb_progress()` already shows nothing during slideshows and only reports on a first decode or a zoom. |
| 2026-04-16 | `a1ccb79727` | pitchscreen: adjust keymaps for ipod and fiiom3k | **N/A** | There is no pitch screen here: screen, keymap context, `ACTION_PS_*` codes and settings are all gone. |
| 2026-04-16 | `cc7418dd8b` | dsp: add option to swap left and right channels | **Adopted** | Only the setting was missing; `lib/rbcodec` implements `SOUND_CHAN_SWAP` already. |
| 2026-04-16 | `fd7ae09e7a` | FS#13864: last char of folder/filename not voiced | **Adopted** | |
| 2026-04-21 | `9ac6edf750` | add panicf to plugin and codec API | **Adopted** | Compile-blocking — new trailing member, left silently NULL. |
| 2026-04-24 | `2690418551` | imageviewer: use theme in all submenus | **Adopted (independently)** | `viewers/image_viewer/image_viewer.c` already has the shape upstream restructures towards: one `viewportmanager_theme_enable`/`_undo` bracket around `do_menu()` **and** all three submenus, with the backdrop reset after the undo rather than inside. Nothing to take. |
| 2026-04-24 | `c145d19e85` | gui: align display updates, reduce UI glitches | **Declined** ⚠ | A dead end, superseded by `c0a8303a9c`. `skin_defer_rendering` does not exist here, so nothing depends on it. |
| 2026-04-26 | `5bbf1c8e5b` | tree: gui_synclist_scroll_stop on uninitialized list | **Adopted** | `update_dir()` could return -1 with the list uninitialised. Reachable with "remember last folder" pointing at a deleted directory. |
| 2026-04-26 | `6cf705886d` | skin: custom scrollbar OBOE | **Adopted** | `last_shown` was the item count, not the last index. Visible — Themify_2 draws its own scrollbar. |
| 2026-04-26 | `792a230c00` | FS#13877: use FONT_UI in the Equalizer sliders | **Adopted** | Sliders size off the font with a 6px floor, rather than a fixed 6px against a forced `FONT_SYSFIXED`. |
| 2026-04-26 | `bf0fa29a30` | WPS Context Menu configurable entry | **Adopted** | 740 lines. The bottom five rows are assignable from Settings > WPS, sharing one action list with the browser hotkey. |
| 2026-04-28 | `7ab1a81806` | simple_viewer: use UI viewport and SBS title | **Adopted (in part)** | The `gui_synclist_scroll_stop()` from the `apps/screens.c` half, at `screens/playback/track_info.c`; without it a mid-scroll row animates on under the opened text view. The plugin API and `simple_viewer.c` halves are N/A. The theme enable/undo removal is declined: `view_text()` owns the full screen with no themed SBS, which is this fork's intended design. |
| 2026-04-29 | `121c65b32a` | FS#13857: keylock with USB (Fiio M3K) | **N/A** | Other target. |
| 2026-04-29 | `c41beebcda` | gui: delay updating SBS when setting list title | **Declined** | Half of a cancelling pair with `160905b1b8`. PodBox's `set_title` already matches upstream's settled version. |
| 2026-04-29 | `dbcee0deae` | gui: defer deadspace viewport update | **Adopted** | Part of the refresh campaign, taken as net state. |
| 2026-04-30 | `52edc2e069` | allow displaying the WPS/tree hotkey menu on hotkey press | **Adopted** | With the context-menu rework. |
| 2026-05-01 | `88d4903d10` | gui: fix "lock screens" making UI viewport disappear | **Adopted** | Refresh campaign, net state. |
| 2026-05-01 | `f886bfc572` | misc: GCC 16 + binutils 2.46 issues | **Adopted** | Compile-blocking, 5 files. |
| 2026-05-02 | `83e55164f4` | gui: remove SBS lock/unlock redraw lag | **Adopted** | Refresh campaign, net state. |
| 2026-05-03 | `42841d493f` | gui: inbuilt statusbar: defer viewport update | **Adopted** | Refresh campaign, net state. |
| 2026-05-03 | `6d699f08f4` | imageviewer: fix incomplete previous commits | **N/A** | Its substance is hiding upstream's `"resizing %d*%d"` overlay behind `hide_info`. This viewer never draws that: `image_viewer.c` puts up a `splash_progress()` dialog over the previous image or name splash instead, suppressed during slideshows. The rest is `hide_info` menu plumbing, declined with `f4dc4d89dc`. |
| 2026-05-03 | `7e6ae1e0d8` | echoplayer: enable plugins | **N/A** | Other target, no plugins. |
| 2026-05-04 | `1d5aa53321` | playback: don't switch to a sampr the sink doesn't support | **Declined** | As `f343168051` — the builtin sink supports both 44.1 and 48 kHz, so the fallback it adds is unreachable. See *USB iAP* below. |
| 2026-05-04 | `89d24f3bd4` | list: fix GUI_EVENT_THEME_CHANGED timing | **Adopted** | Also removed a write through an `int*` to a `long` that only existed to pass a variable back to itself via the event system. |
| 2026-05-06 | `20194cb606` | gui: wps: render SBS and WPS in one batch | **Adopted** | Refresh campaign, net state. |
| 2026-05-06 | `7aca1d46b8` | quickscreen: fix flickering for GUI_EVENT_NEED_UI_UPDATE | **Adopted** | Only portable after the update-model swap. Viewports moved into `struct gui_quickscreen` so the callback paints directly. |
| 2026-05-06 | `b4c308d698` | splash: rework word wrap, escape characters | **Declined** | Head of a 140-line rework that PodBox's dialog framing, physical-display centring and padding would have to be re-applied onto. Only two splash calls use escapes, both `\n`. |
| 2026-05-07 | `05f1a6605d` | gui: skin_engine: fix dirty & force_waiting across screens | **Declined** | A fix *to* the `c145d19e85` dead end, and equally moot. |
| 2026-05-07 | `ce403586e0` | playlist_viewer: loading splash after delay | **Adopted** | Completes the pair with `04e557898f`. `is_open`/`loading_tick` on the viewer struct; a large playlist that takes over ~330ms to load now says so rather than looking hung, repeating every 10s. Self-limiting — on a playlist that loads quickly the splash never appears. |
| 2026-05-08 | `325a028af4` | properties: clear UI viewport at startup | **Adopted** | The *net* with `bc528c4079` at `viewers/properties.c`. Alone it makes the viewport flash on directories, so the two only make sense together. |
| 2026-05-08 | `ae871d25a9` | gui: skin_engine: reduce updates | **Declined** | Fix to the dead end. |
| 2026-05-09 | `bc528c4079` | properties: don't clear UI viewport for dirs | **Adopted** | The net with `325a028af4`. `struct viewport` must be `static` here as upstream had it — the scroll engine keeps the pointer, not a copy. Tested on 5G. |
| 2026-05-11 | `51abd937d5` | playlist viewer: retrieve track name id3 from db | **Adopted (independently)** | `playlist/viewer.c` already tries tagcache before falling back to a disk read. Upstream gates its version on `METADATA_EXCLUDE_ID3_PATH`; PodBox's is unconditional, which is the better choice on a spinning disk. Left as is. |
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
| 2026-05-22 | `edecad823e` | gui: list-skinned: fix scrollbar lag | **Adopted** | `sb_skin_force_next_update()` only bypasses the status bar's rate limiter; it does not force a full refresh. |
| 2026-05-23 | `6a252576f5` | bookmark: stop scrolling for skinned context menu | **Adopted** | |
| 2026-05-23 | `eb6746c1d6` | albumart: fix warning with GCC16 | **Adopted** | Compile-blocking. |
| 2026-05-24 | `c0a8303a9c` | gui: simplify screen updates | **Adopted** ★ | The anchor of the refresh campaign and the largest single port. `skin_render()` no longer flushes; `skin_update()` marks dirty and one place flushes at end of action. See the deviations note below. |
| 2026-05-25 | `21e9d3f449` | Hotkey Tree shares code with WPS Context | **Adopted** | With the context-menu rework. |
| 2026-05-25 | `e471fe4115` | FixRed: Tree Hotkey without HAVE_HOTKEY | **Adopted** | With the above. |
| 2026-05-26 | `239ba599fd` | FS#13908: hotkeys not saved when language changes | **Adopted** | Depends on the configurable context menu from `bf0fa29a30`. Hotkeys are stored in `config.cfg` by name, so changing language does not lose them. |
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
| 2026-06-03 | `85adf518ac` | shortcuts: go to WPS for ACTION_TREE_WPS | **Adopted** | `screens/shortcuts.c` returns `GO_TO_WPS` on `selection == -2`. Required rather than optional once `e6b4ec81ff` is in: this screen tests `selection == -1`, so a `-2` would otherwise reach `get_shortcut()` as an index. |
| 2026-06-03 | `e6b4ec81ff` | simplelist: support ACTION_TREE_WPS | **Adopted (in part)** | The `ACTION_TREE_WPS` handler in `simplelist_do_button_loop`, but **not** the switch from `CONTEXT_LIST` to `CONTEXT_TREE`: on this target that context also binds held PLAY to `ACTION_TREE_STOP`, so it would put stop-playback on a hold in every simple list. Instead `keymap_ipod.c` binds `ACTION_TREE_WPS` to a PLAY *tap* in `button_context_standard`, where it was unused. Opt-in per caller via `simplelist_info.wps_on_play`, since a list with no `GO_TO_*` to return must ignore PLAY rather than have it read as "back". Held PLAY is claimed as `ACTION_NONE` in both `button_context_standard` and `button_context_tree`, so it does nothing on any menu — it meant a second "back" in lists and stop in the tree, which the main menu and Music share, and one hold could chain through all three. Stop remains held PLAY in the playing screen. |
| 2026-06-04 | `0836ebbd45` | shortcuts: 'File' shortcuts fail when dir filter set | **Adopted** | A shortcut to a file hidden by the current filter failed with "Failed reading". |
| 2026-06-05 | `1add6b0dd5` | shortcuts: eliminate unnecessary nesting | **Declined** | A 477-line refactor for GNU Complexity scores, with no behaviour change. `screens/shortcuts.c` is 885 lines and differs from upstream's throughout, so there is no cheap way to apply it. |
| 2026-06-05 | `74905f4796` | skin_engine: remove get_skin_filename call | **Adopted** | It was called purely to fill a buffer nobody read. |
| 2026-06-11 | `4d773a3329` | onplay wps context menu plugin item namebuf | **N/A** | `HOTKEY_PLUGIN` has no meaning here and has been removed. |
| 2026-06-12 | `a824085057` | skin: add %pX tag for time-based playlist progress | **Adopted** | Only the renderer was missing; `lib/skin_parser` advertised `%pX` already. Ported from upstream's current file rather than the commit. Track lengths are cached (500 max), with a size-based estimate on ATA. Inert until a theme uses the tag. |
| 2026-06-14 | `58ce77fbe2` | tagtree: letter menus voiced with talkmenu off | **Adopted** | |
| 2026-06-17 | `d737cbb931` | Sansa As3525 debug menu scroll buttons | **N/A** | Other target. |
| 2026-06-19 | `81962808a2` | use core_alloc for Radio Presets | **N/A** | No radio on these targets. |
| 2026-06-24 | `0e3355de50` | keyboard: fix RTL (Hebrew/Arabic) on-screen keyboard | **N/A** | PodBox's keyboard is a 529-line click-wheel replacement, not a modification of upstream's 1634-line one. None of the four functions it patches exist. Whether PodBox's keyboard is RTL-correct is a separate, untested question. |
| 2026-06-27 | `3cd286d8f8` | metadata: add audio_fmt to get_metadata_ex | **Adopted** | Compile-blocking, 4 files. |
| 2026-06-27 | `3e08b86e4b` | FixRed for %pX: checkwps, ATA builds | **Adopted** | With `a824085057`. |
| 2026-06-28 | `24b0254d96` | metadata.c small cleanup | **Adopted** | Compile-blocking. |
| 2026-06-30 | `d87755c535` | FS#13944: FONT_UI loads last loaded font | **Adopted** | Only the call was missing; `firmware/font.c` defines `set_ui_font()` already. Without it FONT_UI picks up a theme's icon font instead of the configured UI font. |
| 2026-06-30 | `f4e9ba7f17` | FS#13943: single mode tracks under one second don't play | **N/A** | Reverted upstream two days later. Net effect is nothing. |
| 2026-07-02 | `ce88de54b8` | hosted: fix USB mode not initialized | **N/A** | Hosted targets. |
| 2026-07-02 | `ddc31e8ddc` | Revert FS#13943 | **N/A** | The revert of the above. |
| 2026-07-03 | `f11c89aae2` | usb: fix usb mode on DX50/DX90 | **N/A** | Other targets. |
| 2026-07-15 | `943b73851e` | playback: prevent crossfade of new track after pause | **Adopted** | Audio left in the PCM buffer after a manual pause got crossfaded into the next hand-picked track. |
| 2026-07-21 | `ea775fa501` | hibyr1: add USB DAC scaffolding | **N/A** | Other target. |
| 2026-07-26 | `31dfd5da2e` | playback: add Playlist Single Mode option | **Adopted** | `settings.h`, `settings_list.c`, `playback.c`. The enum value is appended last, so stored settings keep their meaning. |
| 2026-07-26 | `4f6aac445f` | hiby: raise plugin buffer to 2MiB on 64MB targets | **N/A** | `hibylinux.h` only — not a PodBox target, and no plugin system. |
| 2026-07-27 | `5f129ef299` | tools: mkinfo handles echor1 symbols | **Adopted** | Two regex broadenings in `mkinfo.pl`, affecting `rockbox-info.txt` only, never the binary. Both are inert on these targets and safe: no `_bssend` symbol exists, and although `ipod6g`'s map carries a bare `loadaddress = 0x8000000` beside `_loadaddress = .`, the pattern requires `= .` and matches only the latter. Reported RAM usage is unchanged. PodBox's own `mkinfo.pl` change is the `COREAPPSDIR` hunk elsewhere in the file, so there was no conflict; both regexes arrived with the merge through `2d2b03d314`. |
| 2026-07-28 | `c54dddc2ac` | playback: fix Playlist Single Mode pause behavior | **Adopted** | `playback.c`. Makes the option added by `31dfd5da2e` usable — without it Playlist mode pauses after every track. See below for the mechanism. Verified on 5G. |
| 2026-07-29 | `3af4e20792` | skin_engine: fix div by 0 for `%pP` tag | **Adopted (in part)** | Two independent divide-by-zeros; PodBox had one of them. Taken: the `skin_tokens.c` half, where `playlist_amount()` divides into `current_pos`. The playback thread creates a playlist before adding any indices to it, so a skin update landing in that window — reachable with *Auto-Change Directory* on — divides by zero. The `draw_progressbar()` half is *Adopted (independently)*: `skin_display.c` already clamps a zero range, added here for a list scrollbar on a fully-visible list. One deliberate difference — PodBox clamps to a **full** bar, upstream to an empty one, which is right for the scrollbar the clamp exists for. The fork's own `%pX` was already safe; `wps_playlist_percent_prepare()` and `wps_get_playlist_percent()` both guard `amount <= 0`. |
| 2026-07-29 | `58d4d2b221` | desktop: drop the 'version' fields | **N/A** | `utils/`. |
| 2026-07-29 | `d42dcdcba2` | rbutilqt: Apple code signing ID from the environment | **N/A** | `utils/`. |
| 2026-07-30 | `db87622e7d` | fix yellow in 3af4e20 | **N/A** | Touches only the `skin_display.c` half, which PodBox reached independently. |
| 2026-07-30 | `1007216fc4` | FS#13961 add audio status, file_attr constants to lua | **N/A** | No plugin system, no lua. |
| 2026-07-30 | `e764656ab7` | allow customizing EQ filter types | **Adopted** | Any band can be any filter type, rather than a hardcoded shelf-peak-…-peak-shelf by position. The `lib/rbcodec/dsp/eq.{c,h}` half arrived with the merge; the `apps/menus/eq_menu.c` → `screens/settings/eq_settings.c` half was ported by hand. The preset question is settled: the old setting names survive as `F_DEPRECATED` entries whose loaders supply the type the name used to imply, so every bundled `eqs/*.cfg` still reads back as the filters it always meant. Two deviations — `skip_whitespace()` comes from `system/strutil.h` here, and the screen's dead local `enum eq_type` is dropped now that `eq.h` defines the real one. |
| 2026-07-30 | `b5d512c409` | iriver H300: fix remote-hold boot entry | **N/A** | Other target, bootloader. |
| 2026-07-31 | `511d4dd90b` | FS#13876 strcasestr doesn't finds utf8 characters | **Adopted** | The size-optimised branch compared through `char`, so any byte ≥ 0x80 sign-extended and UTF-8 never matched. `-Os` defines `__OPTIMIZE_SIZE__`, so that is the branch this fork builds; the other one already used `unsigned char`, which is why upstream only fixed one. The fork's database text search is the caller that makes it visible — a search for an accented artist name found nothing. |
| 2026-07-31 | `94ce143e06` | translation updates (english-us, polski, slovak) | **N/A** | See *Why the translation commits are N/A* below. |
| 2026-08-01 | `00bf7f97ba` | rbutil: support building with QT 6.6 | **N/A** | `utils/`. |
| 2026-08-02 | `104f57252b` | iap: increase the IAP thread's stack from 6K to 8K | **Superseded** | RockPod `99b21cd` raises the same stack to 12K from measurement, and that is the one taken. 8K is 1.23× the measured worst case; every other thread in the image runs at 1.8× or better. |
| 2026-08-02 | `aa99dc51c1` | translation updates (chinese-simp, moldoveneste, romaneste) | **N/A** | See below. |
| 2026-08-02 | `ab863dc40c` | iap: clean up use of logf.h | **N/A** | Removes an `#include` and two macros from `iap-core.h` that this copy does not have. |
| 2026-08-04 | `b4db6ffbff` | FS#13971 updated Italian translation | **N/A** | See below. |
| 2026-08-04 | `1b6767a7d7` | FS#13972 fix crash creating voice files under Windows | **N/A** | `utils/rbutilqt/`. |
| 2026-08-04 | `a467bfc55f` | FS#13972 improve rbutil SAPI5 stability | **Adopted (in part)** | The `tools/sapi_voice.vbs` half, which this fork has never modified. The `utils/rbutilqt/` half is N/A. Voice builds are unverified here either way, so tracking upstream is the cheaper default. |
| 2026-08-05 | `20c763ff89` | FS#13970 lcd_drawline() different depending on drawing direction | **Adopted** | With `dcdb539ca5`, as one net port. A line rasterised differently depending on which end it was given, so drawing it right-to-left did not land on the same pixels as left-to-right. |
| 2026-08-05 | `dcdb539ca5` | FS#13970 lcd_drawline() … try#2 | **Adopted** | A rewrite of `20c763ff89`, not an addition — take the pair or neither. `lcd-bitmap-common.c` is `#include`d rather than compiled, and on this target only by `lcd-16bit.c`. |
| 2026-08-05 | `b217a55059` | ipod6g: add inline earphone remote support | **Open** | **Held out in code as of 2026-08-08** -- `config.h` undefines `HAVE_MIKEY_REMOTE` after the target configs, because a merge would otherwise take the three unedited files its wiring lives in and build the driver; see `upstream-divergence.md`. Decodes the three-button remote on Apple's earphones via the jack "Mikey" controller (I2C bus 0, 0x72): a 342-line `mikey-6g.c` plus `mikey-target.h`, wired through `button-clickwheel.c`, `button.h`, `config/ipod6g.h`, `debug-s5l8702.c`, `audio-6g.c` and `firmware/SOURCES`. It applies to `ipod6g` and nothing prevents taking it. Deferred as of 2026-08-07 on maturity: upstream's commit message records the protocol as reverse engineered on-device, and its debug hunk exists because the remote-ID behaviour *varies between units*, which is a thin base for a driver running a polling thread on one of two shipped targets. Worth revisiting once it settles upstream. Scoped already — see *What the Mikey remote would need* below. |
| 2026-08-05 | `290b06c869` | plugins/fft: do not starve other threads | **N/A** | No plugin system. |
| 2026-08-05 | `20f4f9539a` | hiby: usb dac: fix crackling from sample rate mismatch | **N/A** | Other target, hosted. |
| 2026-08-06 | `2d2b03d314` | build: bundle the main .map files into the zip | **Declined** | ~4MB of text into a zip that is `/MIR`-synced onto the device, so it costs that much of the user's disk on every sync, permanently. It also buys nothing here: resolving a panic address goes through `nm` on the crashing build's `rockbox.elf`, which the release does not ship either. The merge took it and it was deleted again; `tools/buildzip.pl` now carries a comment where the block was, so the next sync conflicts there rather than restoring it silently. |

Complete through `2d2b03d314` (2026-08-06), which is also `rockbox/master`'s
current tip.

## What the Mikey remote would need (`b217a55059`)

The port is larger than its diff, and none of the extra work is visible from the
commit. Recorded so the scoping is not repeated.

**Taken as written it would compile, be reachable from the debug menu, and do
nothing.** The driver reports the remote as multimedia key codes, and
`HAVE_MULTIMEDIA_KEYS` is defined *only* by `USB_ENABLE_IAP` (`config.h`),
which `PODBOX_NO_USB_IAP` suppresses. PodBox then dropped the handler block from
`apps-ipod/system/shutdown.c` as dead code, the define never having been on.
Four things are therefore needed beyond upstream's diff:

1. **Decouple `HAVE_MULTIMEDIA_KEYS` from USB iAP.** It means "this target can
   produce multimedia key codes"; the remote is a second producer independent of
   iAP. `#if defined(USB_ENABLE_IAP) || defined(HAVE_MIKEY_REMOTE)`.
2. **Restore the handler block** in `shutdown.c` — PLAYPAUSE/NEXT/PREV/STOP,
   about 30 lines, *plus* the 8 upstream adds for volume. Not the 8-line diff it
   appears to be.
3. **`TARGET_EXTRA_THREADS` 1 → 2.** That define is this fork's, for the iAP
   thread; upstream's `ipod6g.h` has none, and the commit adds a polling thread
   without bumping any count. `BASETHREADS` is 17 here (`HAVE_HARDWARE_CLICK`).
4. **Recording is off in this fork**, so `audio_enable_mic()` and with it
   `mikey_set_mic_capture()` compile out. Harmless — there is no capture path to
   hand over to — but that half of the driver would ship untested.

Two things that look like obstacles and are not: `audio-6g.c` is **not** locally
patched, and nothing outside it references `mikey_*`, so moving the three
register helpers into `mikey-6g.c` is clean. Of the files the commit touches,
only `config/ipod6g.h` and `docs/CREDITS` carry local changes.

`button.h` and `button-clickwheel.c` are shared with `ipodvideo`. Every hook is
`HAVE_MIKEY_REMOTE`-gated, but `button.h` widens `BUTTON_MULTIMEDIA_ALL`
unconditionally, so the `ipodvideo` binary wants proving unchanged with the
byte-identity gate rather than assuming.

### Why this cannot work on `ipodvideo`

**The 5G has no microphone input on the headphone jack**, and an Apple inline
remote is a set of resistances switched onto the mic line — with no mic pin
there is nothing to sense.

| | `INPUT_SRC_CAPS` |
| --- | --- |
| `ipod6g.h` | `SRC_CAP_MIC \| SRC_CAP_LINEIN` |
| `ipodvideo.h` | `SRC_CAP_LINEIN \| SRC_CAP_FMRADIO` |

The 5G's line-in and radio sources are both dock-connector accessories; its jack
is audio-out only. Mikey is an Apple part on the 6G's I2C bus and the 5G is a
PP5022 with a WM8758, which has no such device.

The 5G is not without a remote: it defines `IPOD_ACCESSORY_PROTOCOL`
(`ipodvideo.h`) and `button-clickwheel.c` already ORs in
`remote_control_rx()`, so dock-connector remotes work there over serial iAP.

## Why the translation commits are N/A

Four commits above are N/A, and the reason is not the obvious one.

`apps-ipod/lang/english.lang` is the fork's own string set and has diverged far
enough from upstream's that a translation update written against `apps/lang/`
does not correspond to it. Untranslated strings fall back to English per string,
so a partial translation degrades rather than breaks, but taking upstream's
`.lang` edits wholesale would not improve any of the 48 languages this fork
ships.

All 48 are shipped, as of 2026-08-07. Until then they were compiled and then
silently dropped from the zip by `tools/buildzip.pl` — see the `buildzip.pl`
rows under *The `--appsdir` wiring* in
[`upstream-divergence.md`](upstream-divergence.md), which is where the
build-system side of that belongs.

---

# RockPod commit log

The other parent. Same status vocabulary; the triage rules differ, because
RockPod's `apps/` **is** this fork's `apps-ipod/` rather than a directory
nothing builds.

| RockPod path | Default |
| --- | --- |
| `apps/` | **Port by hand** into `apps-ipod/`. No `was:` map is needed — the correspondence is the filename. Files usually differ only in include paths and comments, so the hunks apply nearly as written. |
| `apps/plugins/pictureflow/` | **Check first.** It is `screens/covers/carousel.c` here, but heavily reworked: the track list is gone, so anything touching it is N/A. |
| `themes/Themify_2/` | **N/A by default.** This fork's copy is a rewrite in its own skin language. |
| `firmware/`, `lib/`, `tools/` | **N/A.** RockPod is pre-rebase there; take from Rockbox instead. |

RockPod is not a remote of this repo, and should not be — a fork that takes
selectively does not want a merge available. Diff the two checkouts instead.

| Date | RockPod | Summary | Status | Note |
| --- | --- | --- | --- | --- |
| 2026-07-13 | `9e30268` | fix empty list on LCD wake | **Declined as written** ⚠ | Removes the `current_lists = NULL` at `widgets/list.c`. **Ported 2026-08-07 and reverted the same day: it crashes.** `_lists_uiviewport_update_callback()` calls `gui_synclist_draw(current_lists)`, which dereferences the get_name/get_icon/get_talk function pointers out of the struct. A `struct gui_synclist` normally belongs to the screen that owns it and dies with it, so the `NULL` is not defensive tidiness — it is the only thing bounding that pointer's lifetime. Without it, a full status bar refresh arriving after the owning screen has exited calls through whatever now occupies that stack. The panic reads *"Undefined instruction at e3a01ff2, pc: e3a01ff3"*: a PC equal to an ARM instruction word rather than any address in the image, which is the signature of a call through reused stack rather than a decode fault. The same stale pointer repaints the previous list underneath a dialog, unthemed, on USB insertion. **The bug behind it is real and is now fixed here by other means** — the pointer was never the fault. `GUI_EVENT_ACTIONUPDATE` is sent from the *top* of `get_action_worker()`, so the callback already runs with `current_lists` armed; what stops the repaint reaching the LCD is `list_do_action()`'s flush inhibition, set immediately before `get_action()` blocks. The callback now calls `skin_flush_dirty()` after drawing, which is what that function exists for. Worth reading as a caution about the shape of the original diagnosis rather than about upstream: a plausible mechanism was accepted without checking the event ordering it depended on. |
| 2026-07-30 | `3b6fd8d` | iap: adopt upstream's remote fixes and tighten spec conformance | **N/A** | Introduces IDPS transaction-ID handling. There is no IDPS state in this copy of `iap/` at all, so this and the six commits that build on it have nothing to land on. |
| 2026-07-30 | `4d80394` | fix PictureFlow track list highlight using wrong text color | **N/A** | No track list — see below. |
| 2026-07-30 | `3e29bfa` | revert PictureFlow track list to the selector text colour | **N/A** | Same. |
| 2026-07-30 | `82d6fa2` | PictureFlow: full line of spacing on the album/artist lines | **Adopted** | As part of `320c006b4f`, which sizes a caption band and centres the text in it rather than tuning padding. |
| 2026-07-30 | `5696534` | PictureFlow: widen the bottom offset only for two-line mode | **Adopted** | With the above, as one net port. |
| 2026-07-30 | `8dcef26` | revert PictureFlow layout tweaks and the Themify 2 font swap | **Adopted** | The revert is part of the same net. |
| 2026-07-30 | `e844e56` | update Themify 2 to the latest upstream release | **Declined** | This fork's Themify_2 is a rewrite in its own skin language; re-importing the release would discard it. The `.fnt` → `.fnticons` rename is RockPod's own convention — icon fonts live in `wps/Themify_2/` here, with editable sources in `iconsources/`. The licence half is closed: `docs/podbox/LICENSES` carries Literata, League Spartan and Noto under the OFL, Material Design Icons under Apache 2.0 and Themify 2 under CC BY-SA, and `bundle-licenses.sh` ships it. |
| 2026-07-30 | `b8bd8d6` / `2f9e202` / `e1d9acc` | Themify 2 fonts and menu centring | **Declined** | With `e844e56`. |
| 2026-07-31 | `a64efb6` | iap: fix a 4GB memmove and a buffer-full check that inverted | **Adopted (in part)** | Taken: the `(iap_rxlen-2)` underflow in `iap_getc()`, which wraps to ~4G once the buffer fills to within one byte and then admits every frame past the end of the region. Live here because this copy carries the `iap_rxlen` decrement — it is inert in a tree that only increments. Not taken: the negative-`memmove` fix, reachable only when `iap_reset_buffers()` runs from the USB thread, which `PODBOX_NO_USB_IAP` compiles out; and the corrupt-length guard, already `RX_BUFLEN+2` here. |
| 2026-07-31 | `77fe839` | iap: fix a panic on long track tags and an unbounded database loop | **Adopted** | `strlcpy()` returns `strlen(src)`, not what it copied, and that went to `iap_send_pkt()` as a length — a stack over-read past 66 characters and `panicf()` beyond ~124. `RetrieveCategorizedDatabaseRecords` bounded `start_index + read_count`, which wraps on the spec's own count of -1, and only for two of seven categories. Deviation: the rewrite bounds the start and clamps the count rather than adding them, since PodBox's guards were shaped differently from RockPod's pre-fix ones. |
| 2026-07-31 | `53bdc10` | iap: stop an accessory locking the device up via audio_skip() | **Adopted** | `audio_skip()` walks an out-of-range offset back one track at a time under `id3_mutex` without yielding. Also taken from this commit: the volume clamp and the `GetNumPlayingTracks` fall-through. Extended beyond it — the Simple Remote track index command (`iap-lingo3.c`) has the same unchecked `audio_skip()` and is fixed here too, and RockPod has not fixed it. |
| 2026-07-31 | `99b21cd` | iap: enlarge the thread stack | **Adopted** ★ | 6KB against a ~6.5KB measured worst case, abutting the RX buffer with no gap, and only `stack[0]` is canary-checked — so the overflow corrupts packets silently rather than panicking. Now `DEFAULT_STACK_SIZE*12`, `0x3000` in the linked image. |
| 2026-07-31 | `be4fb2f` `b09947a` `ebd26f4` `e605740` `9815533` `feb1924` | iap: IDPS session state, transaction IDs, lingo version | **N/A** | All build on `3b6fd8d`. |
| 2026-07-31 | `8655fb3` | Themify 2: match the PictureFlow selector to the menu highlight | **N/A** | Configures a track list this fork does not have. |
| 2026-07-31 | `af38f7e` | PictureFlow: honour "selector type" instead of assuming one style | **N/A** | See below. |
| 2026-07-31 | `2b3dbc1` `42e80a7` `82f13a0` | PictureFlow selector draw mode and mode classification | **N/A** | Follow-ups to `af38f7e`. |
| 2026-07-31 | `7fa092c` | PictureFlow: advance the flip by elapsed time, not frame count | **Adopted** | `320c006b4f`. |
| 2026-07-31 | `bf6a974` | PictureFlow: don't snap the centre slide when no time has passed | **Adopted** | With the above. |
| 2026-07-31 | `0a3446e` | PictureFlow: fix the centre-slide flash properly, and bound the advance | **Adopted** | `320c006b4f` for the bound, `bdb8a73e8d` for the flash — reached separately here, from the alpha ramp rather than the centre derivation. |
| 2026-08-02 | — | *(upstream `104f57252b`, iap stack 6K → 8K)* | **Superseded** | Rockbox raised the same stack to 8KB. RockPod's 12KB comes from measurement and is the one taken; 8KB is 1.23× the measured worst case, where every other thread in the image runs at 1.8× or better. |

## Why the PictureFlow selector commits are N/A

Seven of them, and the reason is one fact worth stating once: **this fork's
carousel has no track list.** `enum pf_states` is `{ pf_idle, pf_scrolling }`
(`screens/covers/carousel.c`), there is no `draw_gradient()` or
`draw_track_selector()`, and selecting an album either opens the browser or
plays it. Everything RockPod did to make that list agree with the core list's
"selector type" setting has nothing here to act on.

One leftover: `pf_lse_color` (`carousel.c`) is still resolved every frame
and never drawn with. It served the track list. Dead, and recorded rather than
removed.

# Noted exceptions

## USB iAP and serial iAP are different things

Many rows above hinge on this, and commits titled `iap:` land on either side of
it.

**iAP is one protocol with two transports, and PodBox runs only one of them.**

| | Serial iAP | USB iAP |
| --- | --- | --- |
| Switch | `IPOD_ACCESSORY_PROTOCOL` | `USB_ENABLE_IAP` |
| State | **On**, both targets | **Off**, both targets |
| Code | `apps-ipod/iap/` | `firmware/usbstack/iap/` (vendored [libiap](https://github.com/mojyack/libiap)) |
| Wire | UART pins on the dock connector | USB, HID-framed |
| Carries | Commands only | Commands **and digital audio** |

So an upstream `iap:` commit is triaged by which column it touches. Serial iAP
is live and built, and its commits are ported by hand like any other
`apps-ipod/` work. USB iAP is compiled out by `PODBOX_NO_USB_IAP`, so commits
touching only it are Declined — including the several above declined on the
grounds that there is only ever one PCM sink, which is a consequence of that
switch rather than a separate judgement.

**Why USB iAP is off, and what re-enabling would require, is a property of the
tree rather than of any commit: see *USB iAP stays off* in
[`upstream-divergence.md`](upstream-divergence.md).** Suppressing the define
also suppresses `HAVE_MULTIMEDIA_KEYS`, which is why `b217a55059` above needs
more than its diff.

## Where `c0a8303a9c` deviates from upstream, and why each matters

The refresh-model swap follows upstream except in three places, all deliberate:

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
- **The flush is per-region.** A frame sends the rectangles the skin actually
  repainted, not the whole screen. Upstream has no equivalent, so a merge
  touching its flush path is landing on a different model rather than the same
  one with a patch on it.

`skin_flush_dirty()` exists for code that draws outside the action loop and must
be visible immediately — background-task indicators, the USB screen, a settings
screen that repaints on its own. **A new screen that renders without an action
following it needs this too**; it is the one way to get a stale screen in this
model.

### `c54dddc2ac` — the failure mode it removes

Recorded because the shape recurs: a `single_mode` value the tag comparison
cannot describe.

`single_mode_get_id3_tag()` (`playback.c`) has no `SINGLE_MODE_PLAYLIST`
case and returns `NULL` for it. Any path that falls through to the tag
comparison therefore short-circuits on `previous_tag == NULL` and answers
`true` — a pause. Playlist mode must be answered before that point, in both
directions:

```c
if (global_settings.single_mode == SINGLE_MODE_PLAYLIST)
    return skip_pending == TRACK_SKIP_AUTO_NEW_PLAYLIST;
```

Any future `single_mode` value needs the same treatment, or it pauses after
every track.

---

## Keeping this current

All read-only. **Both parents need checking** — a Rockbox sync says nothing
about the other one.

### Rockbox

```bash
git fetch rockbox

# New commits since the last row in the Rockbox table
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

### RockPod

It is a separate checkout, not a remote, so compare content across the two
trees rather than asking git.

```bash
RP=../rockpod                      # wherever the RockPod checkout lives
git -C "$RP" fetch origin && git -C "$RP" log --oneline <last-listed>..origin/master

# What a commit touches, and whether this fork has the file at all
git -C "$RP" show --stat --format='' <commit>

# Divergence in one file: PodBox's copy against RockPod's
diff -u "$RP/apps/<path>" "apps-ipod/<path>"
```

Two things that mislead here:

1. **Most of the diff is not divergence.** `apps-ipod/` rewrote include paths
   (`playlist.h` → `playlist/playlist.h`) and its comments throughout, so
   `iap/` alone shows ~1650 diff lines with almost no behavioural difference.
   Judge a commit by whether its *own* hunks apply, not by the file's diff size.
2. **Check the feature still exists** before triaging a fix to it.
   Seven PictureFlow commits above are N/A for one reason: the screen they fix
   was removed. `grep` for the identifier the commit touches — if it returns
   nothing in `apps-ipod/`, that is the answer.
