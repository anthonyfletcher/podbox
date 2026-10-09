# Upstream commit ledger

What PodBox did about each upstream commit: adopted, declined, or not
applicable. [`upstream-divergence.md`](upstream-divergence.md) answers the
by-file question instead: which files differ from upstream, and why.

## Baselines

| Tree | Baseline | Parent |
| --- | --- | --- |
| `apps-ipod/` | **`dd21a1d1d9`**, 2026-02-10 | [RockPod](https://github.com/nuxcodes/rockpod.git), which was already ~5 months behind Rockbox. Merges from Rockbox never update this tree, and RockPod's later fixes are tracked in their own table below. |
| Everything else | **`636ae404f7`**, merged 2026-09-27 | Rockbox. Everything at or before it is **Inherited**. Moves with each merge. |

## Status

| Status | Meaning |
| --- | --- |
| **Adopted** | In PodBox, often restyled, or ported as upstream's *net* state rather than commit by commit. |
| **Adopted (in part)** | The row says which half is here. |
| **Adopted (independently)** | The behaviour exists here already. Nothing to take. |
| **Declined** | Deliberately not taken, for the reason given. |
| **Superseded** | Both parents fixed it; the row says which fix was taken. |
| **N/A** | A target or subsystem PodBox does not build. |
| **Postponed** | Applies, left for later. The row says what taking it involves. |
| **Inherited** | Arrived with a baseline. |

## Triage

First match wins.

| Upstream path | Default |
| --- | --- |
| `apps/plugins/` | **N/A**: there is no plugin system. Check first, though: several plugins became core screens. |
| `apps/` | **Port by hand** into `apps-ipod/`. A clean merge into `apps/` changes nothing, because nothing builds it, and that includes `apps/lang/english.lang`. Each `apps-ipod/` file names its upstream counterpart in a `was: apps/…` header line. |
| `apps/lang/` other than English | **N/A**. `apps-ipod/lang/english.lang` has diverged too far for upstream's translations to line up. Untranslated strings fall back to English, one by one. |
| `firmware/target/…`, `firmware/export/config/…` for other targets | **N/A**: only `ipod6g` and `ipodvideo` build. |
| `firmware/` core, `lib/`, `tools/` | **Adopt**, after checking `upstream-divergence.md` for a local change to the same file. A feature this fork declines has to be switched off in a config header, or the merge brings it in enabled. |
| `iap:` commits | Two transports, both built on both targets: serial iAP is `apps-ipod/iap/` (`IPOD_ACCESSORY_PROTOCOL`, port by hand), USB iAP is `firmware/usbstack/iap/` (`USB_ENABLE_IAP`, vendored libiap, merges). |
| `manual/`, `uisimulator/`, `android/`, `utils/`, `wps/`, other `themes/` | **N/A**. Mirrored so merges apply; `uisimulator/` is built for the simulator but unmodified. |

Before porting, check for a later upstream commit on the same function, since
upstream reverses itself (`git log --oneline <base>..rockbox/master --
<file>`). Where a cluster of commits converges on one design, port the net.

---

# Rockbox commit log

| Date | Upstream | Summary | Status | Note |
| --- | --- | --- | --- | --- |
| 2025-11-21 | `c2e1094383` | playback: reserve an aa slot for iap | **Adopted** | `MAX_MULTIPLE_AA` +1 under `USB_ENABLE_IAP`. |
| 2025-12-12 | `fad99773e3` | send iap status change notifications | **Adopted** | The `iap_on_*` call sites USB iAP needs to hear about every track, not just the first. The touchscreen hunk has no home here. |
| 2026-02-05 | `7eeb4e4302` | firmware: refactor CACHEALIGN_BITS/SIZE | **Adopted** | Compile-blocking. |
| 2026-02-12 | `76d63246c5` | playback: don't hardcode pcm sink in audio_set_playback_frequency | **Adopted** | Walks the sink's own rate list. |
| 2026-02-13 | `f343168051` | settings_list: apply playback freq changes only when sink is builtin | **Adopted** | The setting leaves an accessory's rate alone. |
| 2026-02-13 | `f87ff3a9b2` | playback: support non-builtin sinks in audio_guess_frequency | **Adopted** | With `76d63246c5`. |
| 2026-02-18 | `c199d9a369` | playback: fix single mode leaking next track before pausing | **Adopted** | As upstream's net state, which amends this commit. |
| 2026-02-19 | `3373ed6744` | playback: fix single mode with auto frequency switch | **Adopted** | With the above. |
| 2026-02-21 | `017dd72ff3` | plugins: convert all plugins to mixer API | **N/A** | No plugin system. |
| 2026-02-23 | `c86fd2318d` | retain file browser directory on reboots | **Declined** | Not wanted, and 116 lines into browser code PodBox has reworked. |
| 2026-02-23 | `e15451815a` | tagcache: prevent infinite scan/commit loop | **Adopted (independently)** | The same guard is here. |
| 2026-02-24 | `17edcbd42a` | talk: improvements in voicing "years" | **Adopted** | The `talk.c` half was the missing one. |
| 2026-03-02 | `eafcbd3fd6` | debug_menu: 2nd SD/MMC card only if NUM_DRIVES > 1 | **N/A** | Other targets. |
| 2026-03-25 | `6928581bf9` | open_plugin_import fails to import full path | **N/A** | No plugin system. |
| 2026-03-31 | `4b9c78e01b` | filetree: restrict keep_directory to Files menu | **N/A** | Follows declined `c86fd2318d`. |
| 2026-03-31 | `cb04b8167c` | pcm_mixer: introduce mixer_play_cbs | **Adopted** | Compile-blocking. |
| 2026-03-31 | `cfb01cfd58` | pcmbuf: remove pcmbuf_sampr | **Adopted** | Compile-blocking. |
| 2026-04-02 | `c765addd24` | eliminate default browser setting | **Declined** | Replaces the **Default Browser** setting with resuming the last browser used. PodBox keeps the setting. |
| 2026-04-07 | `5ac105c837` | tagtree: add "Show in Files" | **Adopted** | With the context-menu rework. |
| 2026-04-07 | `e405858b9e` | wps: replace "Open With"/"Delete" with "Show in Files" | **Adopted** | Same. `HOTKEY_OPEN_WITH` removed with the plugin system. |
| 2026-04-09 | `27ebdfcb25` | settings: fix mismatched resume setting variable types | **Adopted** | `last_screen` and `resume_modified` are `int`. `.resume.cfg` is text, so nothing migrates. Upstream's `(char)` cast in `root_menu.c` is dropped. |
| 2026-04-13 | `719f0f1a3b` | settings: move USB settings to their own submenu | **Adopted (independently)** | They are in **System > USB** already. |
| 2026-04-13 | `e85f120190` | playlist_viewer: character-based Now Playing indicator | **Adopted** | |
| 2026-04-15 | `f4dc4d89dc` | imageviewer: hide info by default when loading | **Adopted (in part)** | Taken: the 250 ms grace before the decode progress dialog. Not taken: the `hide_info` setting; this viewer shows nothing during a slideshow already. |
| 2026-04-16 | `a1ccb79727` | pitchscreen: adjust keymaps for ipod and fiiom3k | **N/A** | No pitch screen. |
| 2026-04-16 | `cc7418dd8b` | dsp: add option to swap left and right channels | **Adopted** | Only the setting was missing. |
| 2026-04-16 | `fd7ae09e7a` | FS#13864: last char of folder/filename not voiced | **Adopted** | |
| 2026-04-21 | `9ac6edf750` | add panicf to plugin and codec API | **Adopted** | Compile-blocking. |
| 2026-04-24 | `2690418551` | imageviewer: use theme in all submenus | **Adopted (independently)** | `image_viewer.c` already has this shape. |
| 2026-04-24 | `c145d19e85` | gui: align display updates, reduce UI glitches | **Declined** | A dead end, superseded by `c0a8303a9c`. |
| 2026-04-26 | `5bbf1c8e5b` | tree: gui_synclist_scroll_stop on uninitialized list | **Adopted** | |
| 2026-04-26 | `6cf705886d` | skin: custom scrollbar OBOE | **Adopted** | |
| 2026-04-26 | `792a230c00` | FS#13877: use FONT_UI in the Equalizer sliders | **Adopted** | |
| 2026-04-26 | `bf0fa29a30` | WPS Context Menu configurable entry | **Adopted** | The bottom five rows are assignable from Settings > WPS. |
| 2026-04-28 | `7ab1a81806` | simple_viewer: use UI viewport and SBS title | **Adopted (in part)** | Taken: the `gui_synclist_scroll_stop()`, in `screens/playback/track_info.c`. Not taken: the theme enable/undo removal, since `view_text()` owns the whole screen here. The plugin halves are N/A. |
| 2026-04-29 | `121c65b32a` | FS#13857: keylock with USB (Fiio M3K) | **N/A** | Other target. |
| 2026-04-29 | `c41beebcda` | gui: delay updating SBS when setting list title | **Declined** | Cancels out with `160905b1b8`. |
| 2026-04-29 | `dbcee0deae` | gui: defer deadspace viewport update | **Adopted** | Refresh campaign, net state. |
| 2026-04-30 | `52edc2e069` | allow displaying the WPS/tree hotkey menu on hotkey press | **Adopted** | With the context-menu rework. |
| 2026-05-01 | `88d4903d10` | gui: fix "lock screens" making UI viewport disappear | **Adopted** | Refresh campaign, net state. |
| 2026-05-01 | `f886bfc572` | misc: GCC 16 + binutils 2.46 issues | **Adopted** | Compile-blocking. |
| 2026-05-02 | `83e55164f4` | gui: remove SBS lock/unlock redraw lag | **Adopted** | Refresh campaign, net state. |
| 2026-05-03 | `42841d493f` | gui: inbuilt statusbar: defer viewport update | **Adopted** | Refresh campaign, net state. |
| 2026-05-03 | `6d699f08f4` | imageviewer: fix incomplete previous commits | **N/A** | Hides an overlay this viewer never draws; the rest is `hide_info`, declined with `f4dc4d89dc`. |
| 2026-05-03 | `7e6ae1e0d8` | echoplayer: enable plugins | **N/A** | Other target. |
| 2026-05-04 | `1d5aa53321` | playback: don't switch to a sampr the sink doesn't support | **Adopted** | With `f343168051`. |
| 2026-05-04 | `89d24f3bd4` | list: fix GUI_EVENT_THEME_CHANGED timing | **Adopted** | |
| 2026-05-06 | `20194cb606` | gui: wps: render SBS and WPS in one batch | **Adopted** | Refresh campaign, net state. |
| 2026-05-06 | `7aca1d46b8` | quickscreen: fix flickering for GUI_EVENT_NEED_UI_UPDATE | **Adopted** | Viewports live in `struct gui_quickscreen`. |
| 2026-05-06 | `b4c308d698` | splash: rework word wrap, escape characters | **Declined** | 140 lines against PodBox's own dialog framing. Only two splash calls use escapes, both `\n`. |
| 2026-05-07 | `05f1a6605d` | gui: skin_engine: fix dirty & force_waiting across screens | **Declined** | A fix to the `c145d19e85` dead end. |
| 2026-05-07 | `ce403586e0` | playlist_viewer: loading splash after delay | **Adopted** | With `04e557898f`. A playlist taking over ~330 ms to load says so. |
| 2026-05-08 | `325a028af4` | properties: clear UI viewport at startup | **Adopted** | With `bc528c4079`, as one net. |
| 2026-05-08 | `ae871d25a9` | gui: skin_engine: reduce updates | **Declined** | A fix to the dead end. |
| 2026-05-09 | `bc528c4079` | properties: don't clear UI viewport for dirs | **Adopted** | Its `struct viewport` must stay `static`: the scroll engine keeps the pointer. |
| 2026-05-11 | `51abd937d5` | playlist viewer: retrieve track name id3 from db | **Adopted (independently)** | `playlist/viewer.c` tries tagcache first, unconditionally. |
| 2026-05-11 | `9bda6389ce` | quickscreen: fix UI update when USB connected | **Adopted** | |
| 2026-05-12 | `1c39495ec2` | playlist_viewer: simplify format_line | **Declined** | Refactor only. |
| 2026-05-16 | `21fe45caad` | splash: string split logic, tab justify | **Declined** | No tabs anywhere in the tree. |
| 2026-05-16 | `d8db60b34a` | splash: infinite loop when viewport too small | **Adopted** | |
| 2026-05-17 | `6e27ba80e4` | splash: trailing `\t` should not add spaces | **Declined** | With the splash chain. |
| 2026-05-17 | `d97e4425c6` | playlist_viewer: NULL instead of 0 in init | **Adopted** | |
| 2026-05-18 | `13a0e58b1c` | gui: usb_screen drawing adjustments | **Declined** | `screens/system/usb_screen.c` is rewritten and skinned here. |
| 2026-05-18 | `58f75311d8` | merge font_getstringnsize and font_measurestring | **Inherited** | Same contract; no `apps-ipod/` change. |
| 2026-05-19 | `0492021247` | fix yellow in 13a0e58b1c | **Declined** | With `13a0e58b1c`. |
| 2026-05-19 | `bf8328fbe0` | rbcodec: fix build failure with DEBUG but no LOGF | **Adopted** | Compile-blocking. |
| 2026-05-21 | `04e557898f` | playlist: delay loading splash when adding indices | **Adopted** | |
| 2026-05-21 | `ae17d606be` | playlist_viewer: UI feedback when loading is delayed | **Declined** | Changes `playlist_viewer_init()` and buffer sizing PodBox has changed. |
| 2026-05-22 | `edecad823e` | gui: list-skinned: fix scrollbar lag | **Adopted** | |
| 2026-05-23 | `6a252576f5` | bookmark: stop scrolling for skinned context menu | **Adopted** | |
| 2026-05-23 | `eb6746c1d6` | albumart: fix warning with GCC16 | **Adopted** | Compile-blocking. |
| 2026-05-24 | `c0a8303a9c` | gui: simplify screen updates | **Adopted** | The anchor of the refresh campaign: `skin_update()` marks dirty, and one place flushes at the end of an action. Three deviations a merge lands on: flush inhibition is kept, in `viewportmanager_update`, so `action_userabort()` cannot paint the status bar over a progress splash; lists keep a partial `update_viewport()` path, with `skin_is_dirty()` clearing as it reads; and the flush sends only the regions repainted. A screen that draws with no action following must call `skin_flush_dirty()`. |
| 2026-05-25 | `21e9d3f449` | Hotkey Tree shares code with WPS Context | **Adopted** | With the context-menu rework. |
| 2026-05-25 | `e471fe4115` | FixRed: Tree Hotkey without HAVE_HOTKEY | **Adopted** | |
| 2026-05-26 | `239ba599fd` | FS#13908: hotkeys not saved when language changes | **Adopted** | |
| 2026-05-26 | `2a29dedeb6` | gui: skin_display: draw album art first | **Adopted** | So a mask can be drawn over it. |
| 2026-05-27 | `018994e8c7` | gui: skinned lists: fix off-screen selection | **Adopted** | Refresh campaign, net state. |
| 2026-05-27 | `0c464c3d49` | gui: list-skinned: scrollbar not disappearing | **Adopted** | |
| 2026-05-27 | `358c6056ef` | gui: skinned list: set cfg to NULL when toggling theme | **Adopted** | |
| 2026-05-28 | `160905b1b8` | gui: list: update skin in gui_synclist_set_title | **Declined** | Cancels out with `c41beebcda`. |
| 2026-05-28 | `35270d08e9` | bookmark: stop scrolling when leaving select screen | **Adopted** | Without upstream's brace de-nesting. |
| 2026-05-28 | `3b2555bd4d` | onplay wps context menus cleanup | **Adopted** | With the context-menu rework. |
| 2026-05-28 | `9f20c45a5e` | properties: fix stack overflow in db | **Adopted** | Lands on `screens/browse/browser_db.c`. |
| 2026-05-29 | `3507f32d01` | properties: further reduce stack pressure | **Adopted** | |
| 2026-05-29 | `f0d3d76b26` | gui: list: clear skinlist cfg when selected_size isn't 1 | **Adopted** | |
| 2026-05-31 | `892fbe8d8f` | action: touchscreen: fix stuck repeated state | **N/A** | No touchscreen. |
| 2026-06-01 | `d54b9e6f8d` | chore: remove all vestigial CVS `$Id:$` tags | **N/A** | Cosmetic; inflates the diffs of the commits around it. |
| 2026-06-02 | `a39e4f2a06` | skin_engine: get rid of skin_unload_all | **Adopted** | |
| 2026-06-03 | `78ec149555` | allow softlock in additional screens | **N/A** | No software keylock; `ALLOW_SOFTLOCK` is 0. |
| 2026-06-03 | `85adf518ac` | shortcuts: go to WPS for ACTION_TREE_WPS | **Adopted** | Needed with `e6b4ec81ff`: `screens/shortcuts.c` would otherwise read `-2` as an index. |
| 2026-06-03 | `e6b4ec81ff` | simplelist: support ACTION_TREE_WPS | **Adopted** | Simple lists run in `CONTEXT_TREE`, so held LEFT/RIGHT scrolls a long row. One deviation: PLAY returns `-2` only where `simplelist_info.wps_on_play` is set, because most simple lists have no `GO_TO_*` to return. Held PLAY is `ACTION_NONE` in every list context, so stop is held PLAY on the playing screen only. |
| 2026-06-04 | `0836ebbd45` | shortcuts: 'File' shortcuts fail when dir filter set | **Adopted** | |
| 2026-06-05 | `1add6b0dd5` | shortcuts: eliminate unnecessary nesting | **Declined** | 477 lines with no behaviour change, against a file that differs throughout. |
| 2026-06-05 | `74905f4796` | skin_engine: remove get_skin_filename call | **Adopted** | |
| 2026-06-11 | `4d773a3329` | onplay wps context menu plugin item namebuf | **N/A** | No `HOTKEY_PLUGIN`. |
| 2026-06-12 | `a824085057` | skin: add %pX tag for time-based playlist progress | **Adopted** | From upstream's current file. Inert until a theme uses `%pX`. |
| 2026-06-14 | `58ce77fbe2` | tagtree: letter menus voiced with talkmenu off | **Adopted** | |
| 2026-06-17 | `d737cbb931` | Sansa As3525 debug menu scroll buttons | **N/A** | Other target. |
| 2026-06-19 | `81962808a2` | use core_alloc for Radio Presets | **N/A** | No radio. |
| 2026-06-24 | `0e3355de50` | keyboard: fix RTL (Hebrew/Arabic) on-screen keyboard | **N/A** | PodBox's keyboard is its own click-wheel screen. Whether it handles RTL is untested. |
| 2026-06-27 | `3cd286d8f8` | metadata: add audio_fmt to get_metadata_ex | **Adopted** | Compile-blocking. |
| 2026-06-27 | `3e08b86e4b` | FixRed for %pX: checkwps, ATA builds | **Adopted** | With `a824085057`. |
| 2026-06-28 | `24b0254d96` | metadata.c small cleanup | **Adopted** | Compile-blocking. |
| 2026-06-30 | `d87755c535` | FS#13944: FONT_UI loads last loaded font | **Adopted** | |
| 2026-06-30 | `f4e9ba7f17` | FS#13943: single mode tracks under one second don't play | **N/A** | Reverted upstream by `ddc31e8ddc`. |
| 2026-07-02 | `ce88de54b8` | hosted: fix USB mode not initialized | **N/A** | Hosted targets. |
| 2026-07-02 | `ddc31e8ddc` | Revert FS#13943 | **N/A** | |
| 2026-07-03 | `f11c89aae2` | usb: fix usb mode on DX50/DX90 | **N/A** | Other targets. |
| 2026-07-15 | `943b73851e` | playback: prevent crossfade of new track after pause | **Adopted** | |
| 2026-07-21 | `ea775fa501` | hibyr1: add USB DAC scaffolding | **N/A** | Other target. |
| 2026-07-26 | `31dfd5da2e` | playback: add Playlist Single Mode option | **Adopted** | The enum value is appended, so stored settings keep their meaning. |
| 2026-07-26 | `4f6aac445f` | hiby: raise plugin buffer to 2MiB on 64MB targets | **N/A** | Other target. |
| 2026-07-27 | `5f129ef299` | tools: mkinfo handles echor1 symbols | **Adopted** | Inert on these targets; `rockbox-info.txt` is unchanged. |
| 2026-07-28 | `c54dddc2ac` | playback: fix Playlist Single Mode pause behavior | **Adopted** | Verified on 5G. `single_mode_get_id3_tag()` has no Playlist case and returns `NULL`, which the tag comparison reads as "pause", so Playlist mode is answered before it. A new `single_mode` value needs the same, or it pauses after every track. |
| 2026-07-29 | `3af4e20792` | skin_engine: fix div by 0 for `%pP` tag | **Adopted** | Both halves. One deviation: in `draw_progressbar()` a list scrollbar with nothing to scroll clamps to a full bar, where upstream clamps every bar to empty. |
| 2026-07-29 | `58d4d2b221` | desktop: drop the 'version' fields | **N/A** | `utils/`. |
| 2026-07-29 | `d42dcdcba2` | rbutilqt: Apple code signing ID from the environment | **N/A** | `utils/`. |
| 2026-07-30 | `db87622e7d` | fix yellow in 3af4e20 | **N/A** | A warning in a declaration shaped differently here. |
| 2026-07-30 | `1007216fc4` | FS#13961 add audio status, file_attr constants to lua | **N/A** | No lua. |
| 2026-07-30 | `e764656ab7` | allow customizing EQ filter types | **Adopted** | `screens/settings/eq_settings.c` ported by hand. The old setting names survive as `F_DEPRECATED` entries, so an older preset still loads as the filters it meant. |
| 2026-07-30 | `b5d512c409` | iriver H300: fix remote-hold boot entry | **N/A** | Other target. |
| 2026-07-31 | `511d4dd90b` | FS#13876 strcasestr doesn't finds utf8 characters | **Adopted** | `-Os` builds the branch that was broken; database text search showed it. |
| 2026-07-31 | `94ce143e06` | translation updates (english-us, polski, slovak) | **N/A** | Translation. |
| 2026-08-01 | `00bf7f97ba` | rbutil: support building with QT 6.6 | **N/A** | `utils/`. |
| 2026-08-02 | `104f57252b` | iap: increase the IAP thread's stack from 6K to 8K | **Superseded** | By RockPod `99b21cd`'s 12K, which is measured. |
| 2026-08-02 | `aa99dc51c1` | translation updates (chinese-simp, moldoveneste, romaneste) | **N/A** | Translation. |
| 2026-08-02 | `ab863dc40c` | iap: clean up use of logf.h | **N/A** | This copy has neither the include nor the macros. |
| 2026-08-04 | `b4db6ffbff` | FS#13971 updated Italian translation | **N/A** | Translation. |
| 2026-08-04 | `1b6767a7d7` | FS#13972 fix crash creating voice files under Windows | **N/A** | `utils/`. |
| 2026-08-04 | `a467bfc55f` | FS#13972 improve rbutil SAPI5 stability | **Adopted (in part)** | `tools/sapi_voice.vbs` only. |
| 2026-08-05 | `20c763ff89` | FS#13970 lcd_drawline() different depending on drawing direction | **Adopted** | With `dcdb539ca5`, which rewrites it: take both or neither. |
| 2026-08-05 | `dcdb539ca5` | FS#13970 lcd_drawline() … try#2 | **Adopted** | |
| 2026-08-05 | `b217a55059` | ipod6g: add inline earphone remote support | **Adopted (in part)** | Working on a 7G. Five local changes hold it up, and a merge can revert any of them silently: four are rows in `upstream-divergence.md`, and the fifth is the multimedia key handler in `default_event_handler_ex()` (`system/shutdown.c`), without which nothing acts on the keys. Not taken: `mikey_set_mic_capture()` and `manual/`. The 5G has no mic line for a remote. |
| 2026-08-05 | `290b06c869` | plugins/fft: do not starve other threads | **N/A** | No plugin system. |
| 2026-08-05 | `20f4f9539a` | hiby: usb dac: fix crackling from sample rate mismatch | **N/A** | Other target. |
| 2026-08-06 | `2d2b03d314` | build: bundle the main .map files into the zip | **Declined** | ~4 MB on the player at every sync. `release.sh` attaches each build's map to the release instead. The block is replaced by a comment in `tools/buildzip.pl`, so a merge conflicts there. |
| 2026-07-30 | `4f65dfa649` `eecd4ec98b` `842492d77b` | hiby: r1_patcher pack/unpack split, SD hotplug, macOS | **N/A** | Other target. |
| 2026-07-31 | `21d48d5ae3` | iap: improve `IAPGeneralCommandID_RequestIPodName` | **Adopted (in part)** | Every transport takes the name from one copy of `/.rockbox/playername.txt`, read at boot by `iap_player_name_load()`, and a missing, empty or model-name file is set to `PodBox`. libiap asks `iap_platform_get_ipod_name()` instead of reading the file, so a merge touching that case conflicts. |
| 2026-08-02 | `49600dd77c` | filebrowse.lua sort by type; `.bmp`, `.mod` as known filetypes | **Declined** | `.bmp` stays an image so the image viewer opens it; `.mod` means nothing on either player. The rest is lua. |
| 2026-08-07 | `85c1ff8667` | build scripts: make reproducible builds possible | **Adopted (in part)** | With `SOURCE_DATE_EPOCH` exported, two builds of one tree give the same `rockbox.bin`. The `lang.make` hunk is mirrored into `apps-ipod/lang/lang.make`. `REPRODUCIBLE_ZIP` needs `strip-nondeterminism`, which the build server lacks. |
| 2026-08-07 | `f44bf5c66d` `0db3308e43` | translation updates (russian) | **N/A** | Translation. |
| 2026-08-09 | `21e95258f2` `0726ec9351` `612453da48` `eded05f0cb` | rbutil: SAPI5 test output, voice/talk responsiveness, talk exclusions, Italian | **N/A** | `utils/`. |
| 2026-08-09 | `233ea05ea2` `fae5c8d067` | x1000: NAND init guard, don't close fd 0 on a failed backup | **N/A** | Other target. |
| 2026-08-09 | `6ad1fd074a` | usb: acknowledge SET_LINE_CODING in the serial driver | **N/A** | `USB_ENABLE_SERIAL` is off on both targets. |
| 2026-08-09 | `a13368d0d6` | axp2101: fix the enable test in `axp2101_supply_get_voltage()` | **N/A** | Other target. |
| 2026-08-09 | `ef20bc4c78` | usb: keep the USB Serial setting across connects | **N/A** | As `6ad1fd074a`. |
| 2026-08-11 | `444c9ce4bd` | manual: add touchscreen settings section | **N/A** | |
| 2026-08-11 | `51d7d56803` `459c5e7937` | ap80max: new hosted port, then moved to the 'unstable' list | **N/A** | Conflicts in `wps/WPSLIST`, whose blocks this fork deleted: keep ours. |
| 2026-08-11 | `fd8d6f10a1` | synopsys-dwmac: bugfixes | **Adopted** | With `b616047311`, which corrects it. The 6G's USB driver; the sound card relies on both. The driver carries this fork's host mode, so later upstream changes to it conflict. |
| 2026-08-13 | `0ca22b9a71` `814747492c` `a89e1f999d` `c93c7bfdcb` `0e0982cb29` `7e53e85cb2` `9641d54fee` | touchscreen: flick detector and kinetic scrolling v2/v3, plus two build fixes | **N/A** | All inside `HAVE_TOUCHSCREEN`. |
| 2026-08-14 | `0b52c28933` | dircache: use alloca to avoid VLA in struct extension | **Adopted** | |
| 2026-08-14 | `9a7ffac2e4` `8f8a0e5230` | pcm_sink: per-sink swvol/hwvol selection | **Adopted** | Both targets resolve to `PCM_SINK_HWVOL`, as before. |
| 2026-08-14 | `df85814e74` `9a80f5af55` `83d6948301` | erosqnative: runtime hwvol on es9018k2m, plus documentation | **N/A** | Other target. |
| 2026-08-15 | `b616047311` | usb-designware: fix ISO frame scheduling | **Adopted** | The other half of `fd8d6f10a1`. |
| 2026-08-16 | `8cfa4bfd8c` | quickscreen: update SBS after each button press | **Adopted (in part)** | Guarded on `button != ACTION_NONE`, or the bar repaints five times a second while the screen is open. |
| 2026-08-16 | `be62759005` | quickscreen: fix MIN_LINES off-by-one | **Adopted** | |
| 2026-08-16 | `cacbd9aad2` | skin_engine/quickscreen: unused code, comment, naming | **Adopted (in part)** | Taken: the dead prototype, and `skin_load()` made static. Not taken: the reflow. |
| 2026-08-18 | `b621a519e2` `f69ee23fa1` `11817f99f8` | stm32h7 sdmmc: bus-frequency helper, CMD12 ordering | **N/A** | Other target. |
| 2026-08-19 | `2c5482cab9` `e3f622a851` | splash: iPod reFresh colour workaround made pixel-format independent | **N/A** | `widgets/splash.c` has no such workaround. |
| 2026-08-13 | `789d796120` | quickscreen: make %QT hide built-in UI | **Adopted (independently)** | As `eaa80e9feb`, wider: any `%Q` tag in any skin stands the built-in layout down. |
| 2026-08-15 | `30a5f1d858` | quickscreen: redraw only the relevant viewports | **Adopted (in part)** | Only the built-in layout uses it, which draws under themes without `%Q` tags. `quickscreen_update()` also checks for a skinned quickscreen; the icon reflow is not taken. |
| 2026-08-16 | `50b13493d2` | skin_engine: make skin file type available to parser | **Adopted** | Merge-blocking: CheckWPS, which builds here, calls the new signature. |
| 2026-08-17 | `3d44fe94f3` | skin_display: simplify skin_wait_for_action | **Declined** | A simplification. Its behaviour changes need the FM screen or `NB_SCREENS > 1`. |
| 2026-08-17 | `9cd4352256` | quickscreen: fix missing redraw for QS_VOL action | **Adopted** | |
| 2026-08-19 | `9039355bf8` | timeout: include `<stdint.h>` for intptr_t | **Adopted** | |
| 2026-06-26 | `2be2aeb2a2` `0e2a3cc0c6` `4d3c7aed03` `9cef2a3aef` `ce47229857` | 3DS: circle pad, software tone controls, clean power-off, touchscreen gating, data directories | **N/A** | Other target. |
| 2026-07-25 | `ec59f570dd` | keyboard: point-mode virtual keyboard for touchscreens | **N/A** | Touchscreen only. |
| 2026-08-15 | `c04c2c3e0b` | FS#13892: voice feedback in the Cuesheet browser | **Adopted** | The directory is cut with `strmemccpy()`; upstream's `snprintf()` return puts the NUL past what was written. |
| 2026-08-15 | `aeaee5da45` | voice.pl: extract PERFORMER and TITLE from cuesheets | **Adopted** | |
| 2026-08-17 | `1f1db58d44` `afe5e7191d` `bafcb38ec9` `cdad8cde64` `94a0fe608a` `5e71adddf9` `69a36026f8` `a95f9debb2` `99c18f336c` | sdmmc: SCR in `tCardInfo`, cache-buffer helper, CMD23, activity LED, generic polling helper, init cleanup, single-block timeout quirk | **N/A** | Both targets are `STORAGE_ATA`. |
| 2026-08-18 | `f40cae6cdc` | x1000: rewrite the SD driver using sdmmc_host | **N/A** | Other target. |
| 2026-08-19 | `a8f8aa40b9` | lastfm_scrobbler: fetch rbversion from the plugin API | **N/A** | No plugin system. |
| 2026-08-22 | `7fef95dc08` | warble: fix the build on targets with HAVE_RECORDING | **Adopted** | Inert: `HAVE_RECORDING` is off. |
| 2026-08-22 | `5d016a2c04` | fix building Warble when configured as the iPod Video | **Adopted** | Inert. |
| 2026-08-25 | `3f1ec2385f` | FS#13988: Hungarian translation update | **N/A** | Translation. |
| 2026-08-25 | `fd2b070bb1` | FS#13984: Rockbox Utility SAPI5 voice volume control | **Adopted (in part)** | `tools/sapi_voice.vbs` only. |
| 2026-08-28 | `8536d981a8` | checkwps: print the file name extension instead of "WPS" | **Adopted** | Conflicts in `tools/checkwps/checkwps.c`: take upstream's `parsed OK` line and keep this fork's `--viewports` block after it. |
| 2026-08-28 | `70fd3e1f1e` | ipodcolor: fall back to UDMA 1 | **N/A** | The 5G keeps UDMA 2. |
| 2026-08-28 | `28e5a125ab` | configure: macos: fix checkwps error messages | **N/A** | macOS only. |
| 2026-09-11 | `b7fe01393a` | usb-drv-arc: defer SET_ADDRESS until status completion | **Superseded** | The fix this fork carried as `0f1436aea2`. Both replaced by `841007dfa1`. |
| 2026-09-20 | `841007dfa1` | usb: let controller drivers handle SET_ADDRESS requests | **Adopted** | It broke hosts that send `SET_ADDRESS` first. Fixed here (`a0103fa46a`) and not yet reported upstream; see `upstream-divergence.md`. |
| 2026-09-20 | `9aa2d7fe94` `d1fab121f8` | usb: accept a replacement SETUP; usb arc: flush both EP0 directions when SETUP replaces a transfer | **Adopted** | |
| 2026-09-20 | `63978def70` `b1385d831e` | usb arc: discard stale transfers and audio work on bus reset; reset data toggles when clearing halt | **Adopted** | |
| 2026-09-20 | `3bd18f5a44` | usb iap: correct sample rate descriptors and packet cadence | **Adopted** | |
| 2026-09-20 | `404e2c7626` | usb: validate configuration descriptor indices and failed drivers | **Adopted** | The driver `driver_to_leave_out()` drops is now absent from the descriptor too. |
| 2026-09-17 | `6958f6638e` `93b49594d5` | usb storage: report read-only drives as write protected; fix BOT residue, rejected commands and ATA IDENTIFY | **Adopted** | Conflicts beside this fork's unsupported-LUN sense code: keep both. |
| 2026-09-21 | `2b664d6025` `e2ee665cce` | usb: preserve exclusive storage across repeated configuration; defer commands until the storage handover completes | **Adopted** | Conflicts in `usb.c`: this fork's insertion record stays first. |
| 2026-09-03 | `3a57f2f721` `861e53095f` `98a55f623b` | Add "rbfs" prefix to native filesystem functions | **Adopted** | `filesize()` is `ffilesize()`; `apps-ipod/` and `tools/soundscan/` follow. |
| 2026-09-23 | `bd615cf125` | Have audio_hard_stop() kill the PCM output path entirely | **Adopted** | |
| 2026-09-23 | `9101f35519` | ROLO: get rid of redundant call to audio_hard_stop() | **Adopted** | Both callers here stop audio first. |
| 2026-09-09 | `f2985dc8a2` | option_get_valuestring: fix trailing whitespace for UNIT_INT | **Adopted** | |
| 2026-09-04 | `569c2a53c8` `7e3782868c` | quickscreen: string size in the UI viewport's font; icon x-position off-by-one | **Adopted** | |
| 2026-08-24 | `8eefac3638` `81cf120983` `1a37ac8568` `da207d6067` `10ec9bd530` | quickscreen: prevent out-of-bounds viewports, always refresh SBS when leaving, refactor quickscreen_fix_viewports | **Adopted (in part)** | As one net port. The layout keeps every text viewport inside a UI viewport of any size, which matters because themes shrink it to hide this screen — Themify_2 uses a 30×30 one. Leaving always refreshes the status bar, and the browser and playlist viewer no longer make up for it. Not taken: the renames, `FOR_QS_ITEMS` and the `setup()` split. The viewports are still laid out under a skinned quickscreen, so its draw and cleanup never read uninitialised ones. |
| 2026-09-15 | `dc37bb1ba0` | tagtree: warn if tagnavi.config is missing instead of freezing | **Adopted** | As `browser_db_ready()`, checked in `root_menu.c` before each way into the database browser. |
| 2026-08-27 | `d2ae775f0a` `27332d64ad` `76a2fdfc1f` | ipod6g: composite video output driver and setting | **Adopted (in part)** | The driver, as the 6G half of **TV Out**: its framebuffer is taken only while the picture is on, and the picture can be 1x. Not taken: the Off/Auto/On setting, whose Auto missed the dock too often; **TV Out** has a Turn On/Off action instead, never saved, beside **Size**. |
| 2026-09-05 | `e0136bc7f4` `d27af08ff6` `29eef25ac7` | ipod6g: hardware H.264 video playback | **Declined** | The player is a plugin. Nothing calls the VPU drivers, so the link drops them. |
| 2026-09-11 | `b18f5d6d65` `d514ee9282` `9e7b81f269` `f26c9557c2` `8eb6b05945` `31f2a27f1e` `08d3332edf` `0c4345475a` `ae223933bf` `9a972e7f51` `0918a068eb` `07557038b4` `ff762858c7` `7b4d1a7f75` `da9df96c30` `40fdf6ac11` `a51adaac4f` `93d564426b` `5a6935047e` `b12ef5e6f8` | opus: ARM kernels, IRAM placement on PP5022, decoder without the encoder | **Adopted** | |
| 2026-09-05 | `064c165367` | libm4a: handle sparse chunk maps and video-first MP4 | **Adopted** | |
| 2026-09-19 | `305acca1f0` `af9b65485b` | dsp: `.type` on ARM asm; build: `-mthumb-interwork` moved into configure | **Adopted** | |
| 2026-09-08 | `190822f261` | firmware: limit system_memory_guard() to coldfire targets | **Adopted** | |
| 2026-09-12 | `57a91121f6` `a7ab67f459` `2507be9af2` `1784c9b8a7` `45bd2b970b` `284af2aee8` `2ec4760117` `a3e93c496e` | plugin API and plugins: ACTIVITY_UNKNOWN hack, backlight_on_button_hold, keyremap, lastfm, disktidy, lua | **N/A** | No plugin system. |
| 2026-09-19 | `34a18e7616` `76f8925d23` `e4c010be98` `1ad17c9b57` `66bc0728d5` `2adcfa08cf` `aed1945c5d` `b58c7505a0` `4dad9a0489` `9ee5873770` `01925dd5d0` `00829f2258` `413f17b8ce` `bd24fddb7e` `af2a3b74f4` `bdbbb753c5` | iPod Nano 3G port, its tools and its bootloader QR code | **N/A** | Other target. The S5L87xx bootloader the 6G shares carries these commits, but their QR code and NAND check build only for the Nano 3G. |
| 2026-08-31 | `e498c0171a` `94d422f1c4` `20fa5f017d` `c6abf3382a` `4e4198af7a` `5448dd99a3` `44e7c009ae` `d23a19dc2d` `387b36fab7` `f349e85154` `9d86c9b201` `8ad69d6649` `376db9bf5b` `4f54dbec79` | 3DS, iriver, as3525, iBasso, erosq, ingenic, sdmmc_host, HiBy, touchscreen | **N/A** | Other targets. |
| 2026-09-08 | `dd164cadb1` `0a0b877dc2` `3e996ec73e` `6719578e29` `7f01029439` `cbaf66f372` `d43cc0e829` | jztool, ingenic usbboot, rbutil, theme editor | **N/A** | `utils/`. |
| 2026-08-30 | `95e9d227aa` `420537c864` `cbd8b68e06` `be35fdfe1a` `636ae404f7` | manual, CREDITS, forum URL | **N/A** | |
| 2026-08-31 | `3664373ce7` `8e965d9159` `a70f30adf1` `54b26ac9f1` `a967c5a018` | translation updates | **N/A** | Translation. |

Complete through `636ae404f7` (2026-09-27), merged as `76df1a5859`. Both
targets were built either side of that merge and compared object by object:
only `version.o` and `panic.o` (the commit hash) and `credits.o` (a new name)
differ. Compare objects, not sections, because a few changed bytes shift every
later address.

---

# RockPod commit log

| RockPod path | Default |
| --- | --- |
| `apps/` | **Port by hand** into `apps-ipod/`, same filename. Usually only include paths and comments differ. |
| `apps/plugins/pictureflow/` | **Check first.** It is `screens/covers/carousel.c` here, without the track list. |
| `themes/Themify_2/` | **N/A**: this fork's copy is a rewrite. |
| `firmware/`, `lib/`, `tools/` | **N/A**: RockPod is pre-rebase there. Take from Rockbox. |

| Date | RockPod | Summary | Status | Note |
| --- | --- | --- | --- | --- |
| 2026-07-13 | `9e30268` | fix empty list on LCD wake | **Declined** | Removing `current_lists = NULL` in `widgets/list.c` crashes: the NULL is all that bounds the pointer's lifetime, and a status-bar refresh after the owning screen exits calls through reused stack. The empty list is fixed here instead by `skin_flush_dirty()` in the list's update callback. |
| 2026-07-30 | `3b6fd8d` | iap: adopt upstream's remote fixes and tighten spec conformance | **N/A** | Builds on IDPS state this copy of `iap/` does not have. |
| 2026-07-30 | `4d80394` | fix PictureFlow track list highlight using wrong text color | **N/A** | No track list. |
| 2026-07-30 | `3e29bfa` | revert PictureFlow track list to the selector text colour | **N/A** | Same. |
| 2026-07-30 | `82d6fa2` | PictureFlow: full line of spacing on the album/artist lines | **Adopted** | As part of `320c006b4f`. |
| 2026-07-30 | `5696534` | PictureFlow: widen the bottom offset only for two-line mode | **Adopted** | Same net. |
| 2026-07-30 | `8dcef26` | revert PictureFlow layout tweaks and the Themify 2 font swap | **Adopted** | Same net. |
| 2026-07-30 | `e844e56` | update Themify 2 to the latest upstream release | **Declined** | Would discard this fork's rewrite. |
| 2026-07-30 | `b8bd8d6` / `2f9e202` / `e1d9acc` | Themify 2 fonts and menu centring | **Declined** | With `e844e56`. |
| 2026-07-31 | `a64efb6` | iap: fix a 4GB memmove and a buffer-full check that inverted | **Adopted (in part)** | Taken: the `(iap_rxlen-2)` underflow in `iap_getc()`. The rest is already bounded here. |
| 2026-07-31 | `77fe839` | iap: fix a panic on long track tags and an unbounded database loop | **Adopted** | The record bound clamps the count rather than adding it to the start. |
| 2026-07-31 | `53bdc10` | iap: stop an accessory locking the device up via audio_skip() | **Adopted** | Extended to the Simple Remote track-index command in `iap-lingo3.c`, which RockPod has not fixed. |
| 2026-07-31 | `99b21cd` | iap: enlarge the thread stack | **Adopted** | 12K. At 6K the overflow corrupted packets silently. |
| 2026-07-31 | `be4fb2f` `b09947a` `ebd26f4` `e605740` `9815533` `feb1924` | iap: IDPS session state, transaction IDs, lingo version | **N/A** | Build on `3b6fd8d`. |
| 2026-07-31 | `8655fb3` | Themify 2: match the PictureFlow selector to the menu highlight | **N/A** | No track list. |
| 2026-07-31 | `af38f7e` | PictureFlow: honour "selector type" instead of assuming one style | **N/A** | No track list. |
| 2026-07-31 | `2b3dbc1` `42e80a7` `82f13a0` | PictureFlow selector draw mode and mode classification | **N/A** | Follow `af38f7e`. |
| 2026-07-31 | `7fa092c` | PictureFlow: advance the flip by elapsed time, not frame count | **Adopted** | `320c006b4f`. |
| 2026-07-31 | `bf6a974` | PictureFlow: don't snap the centre slide when no time has passed | **Adopted** | With the above. |
| 2026-07-31 | `0a3446e` | PictureFlow: fix the centre-slide flash properly, and bound the advance | **Adopted** | `320c006b4f` for the bound; the flash was fixed separately in `bdb8a73e8d`. |
| 2026-08-28 | `3b6d477` | iap: overhaul accessory protocol support | **Adopted (in part)** | 154 files, taken in parts: see below. |

Complete through `3b6d477` (2026-08-28). Check with `git ls-remote`, not a
local clone, which reads as "nothing new" whether or not it is.

## What was taken from `3b6d477`

The Extended Interface browsing it adds — `iap-db.c`, `iap-media.c`, the
artwork and chapter readers and the test rig — is **Declined**. The hardening
underneath was judged against this tree, since RockPod's `firmware/` and
`lib/` are pre-rebase.

| Part | Status | What |
| --- | --- | --- |
| `lib/rbcodec/metadata/mp4.c` | **Adopted (in part)** | A `chpl` box shorter than its header no longer wraps `size` into a 4 GB seek. The chapters feature is declined. |
| `database/tagcache.c`, `.h` | **Adopted (in part)** | A read error is no longer taken for "no match", so a truncated index cannot pass a short list off as the whole one. Snapshot accessors declined. |
| `usbstack/usb_storage.c` | **Adopted (independently)** | `set_transfer_range()` makes the LBA arithmetic overflow-safe for all four READ/WRITE commands, and the CBW's LUN is bounds-checked. |
| `playlist/playlist.c` | **Adopted (in part)** | `get_track_filename()` and `playlist_get_track_info()` read the indices under the lock; the control file checks `fsync()` and `lseek()`. Staged/snapshot API declined. |
| `iap/iap-core.c`, `iap-lingo*.c` | **Adopted (in part)** | `iap_getc()` drops a malformed length. Not RockPod's long-form check, though: the Onkyo DS-A3 sends its certificate in long frames below 0xFD. The rest is IDPS and EI browsing. |
| `s5l8702/ipod6g/storage_ata-6g.c` | **Declined** | A second implementation of the SSD sleep this fork already has. |
| `usbstack/usb_audio.c`, `usb-designware.c` | **Declined** | RockPod's USB audio source. Here USB iAP's configuration 2 is the source. |

---

## Keeping this current

Both parents need checking: a Rockbox sync says nothing about RockPod.

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

To list which `apps-ipod/` file each upstream `apps/` file became:

```bash
find apps-ipod \( -name '*.c' -o -name '*.h' \) -print0 | xargs -0 awk '
/was:/ && !done[FILENAME] {
  l=$0; sub(/.*was:[ \t]*/,"",l); gsub(/[ \t\r]+$/,"",l)
  if (l ~ /^apps\//) { print l "\t" FILENAME; done[FILENAME]=1 }
}'
```

### RockPod

A separate checkout, not a remote, so compare the two trees directly.

```bash
RP=../rockpod                      # wherever the RockPod checkout lives
git -C "$RP" fetch origin && git -C "$RP" log --oneline <last-listed>..origin/master

# What a commit touches, and whether this fork has the file at all
git -C "$RP" show --stat --format='' <commit>

# Divergence in one file: PodBox's copy against RockPod's
diff -u "$RP/apps/<path>" "apps-ipod/<path>"
```

Judge a commit by whether its own hunks apply, not by the size of the file
diff: include paths and comments differ throughout. And check that the feature
still exists before triaging a fix to it — if `grep` finds the identifier
nowhere in `apps-ipod/`, that is the answer.
