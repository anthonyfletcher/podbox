# Divergence from upstream Rockbox — everything outside `apps-ipod/`

## Who this is for

Read this when you are **about to merge from Rockbox** and want to know what you
are merging into, or when you have found a file outside `apps-ipod/` that differs
from upstream and want to know whether that was deliberate.

The short answer to the second question is almost always yes — and the
"Deliberately *not* changed" table at the end lists the cases that look like
omissions and are not. Check there before re-doing work someone already decided
against.

## Background

PodBox is a fork of Rockbox that builds two targets: iPod Classic 6G/7G
(`ipod6g`) and iPod Video 5G/5.5G (`ipodvideo`). Its application layer lives in
`apps-ipod/` and is entirely its own — that tree is documented separately, in
`apps-ipod/README.md`, and is **not** covered here.

This document covers **everything else**: the files under `firmware/`, `lib/`,
`tools/` and the repo root that differ from upstream Rockbox, and why.

## Scope

The comparison base is Rockbox commit `24c3779146` — the commit this fork was
rebased onto, not the original fork point. To regenerate the list:

```bash
git diff --name-status $(git merge-base HEAD rockbox/master)..HEAD -- . ':(exclude)apps-ipod'
```

51 tracked files differ. 42 are code, build-system or build-script files (7 of
them new); the rest are docs, EQ presets and the logo.

**`apps/` is byte-identical to upstream, and stays that way.** It is kept so
`git merge rockbox/master` applies without delete/modify conflicts. The same
goes for `manual/`, `uisimulator/`, `android/`, `backdrops/`, `screenshots/` and
every `themes/` entry but Themify_2. Being unused is not a reason to prune them.

Most `firmware/` changes are hardware work inherited from the RockPod fork
(GPLv2), which upstream has no equivalent of. The `tools/` changes are this
fork's own.

---

## firmware/ — core

| File | What changed | Why |
| --- | --- | --- |
| `backlight.c` | `#include "../apps/gui/skin_engine/skin_engine.h"` → `"gui/skin_engine/skin_engine.h"` | Upstream reaches into the app layer by relative path, which lands in the unbuilt `apps/` mirror. Now resolved through the `apps-ipod/api/` stub like every other cross-layer include. |
| `backlight.c` | Backlight off calls `storage_sleep()`; both wake paths post `Q_STORAGE_PRE_WAKE` | Backlight state gates SSD sleep. Pre-waking from ISR context lets the storage thread start powering the SSD up before the UI thread has even processed the button press that caused the wake. Guarded by `storage_get_ssd_mode()`. |
| `backlight.c` | `power_input_present()` → `charger_inserted()` in `backlight_get_current_timeout()` | The 6G now distinguishes a plain USB port from a real charger. `power_input_present()` is true for both, so a data-only connection would wrongly get the plugged-in backlight timeout. |
| `drivers/ata.c`, `export/ata.h` | New `ata_set_storage_mode(int)` / `ata_get_ssd_mode()`, stubbed to no-op and `false` | Declares the SSD-mode interface for all ATA targets. The real implementation is 6G-only (`storage_ata-6g.c`); these stubs let the dispatch macros resolve everywhere else. Modes: 0 auto, 1 HDD, 2 SSD. |
| `export/storage.h`, `storage.c` | New `Q_STORAGE_PRE_WAKE` event, stub functions and macro dispatch for the two calls above | Plumbing so the app and backlight layers can reach SSD mode through the generic `storage_*` interface. |
| `powermgmt.c` | Two call sites stop consulting `charging_state()` — the battery-level test uses `charge_state > DISCHARGING`, and the charger case sets `CHARGING` unconditionally instead of falling through | On the 6G the charge-status line oscillates against weak USB sources, so `charging_state()` reads false while charging is genuinely happening. Upstream's fallthrough flips the reported state back and forth. |
| `export/logf.h` | `MAX_LOGF_SIZE` 16 KiB → 256 KiB | Costs nothing unless `ROCKBOX_HAS_LOGF` is defined — which it now is on the 6G, so it is live there. |

## firmware/ — USB stack

All four files serve one feature: a `host_wrote` flag.

The app layer rebuilds the tag database and dircache on every USB disconnect, on
the assumption the host changed files. If the host only ever read, none of that
work is needed — and Windows produces a spurious disconnect/reconnect on *every*
connect, which used to trigger the full rebuild and then leave the tagcache
thread mid-scan when the reconnect arrived.

| File | What changed | Why |
| --- | --- | --- |
| `usbstack/usb_storage.c` | Static `host_wrote`, cleared on connect, set by `SCSI_WRITE_10` / `SCSI_WRITE_16`; accessor `usb_storage_host_wrote()` | Where the writes actually are. |
| `usbstack/usb_storage.h` | Declares the accessor | — |
| `usbstack/usb_core.c` | `usb_core_host_wrote_storage()`, forwarding when `USB_ENABLE_STORAGE` is set and returning false otherwise | `usb_storage.h` is private to the USB stack, so the app layer cannot include it. |
| `export/usb_core.h` | Declares it | The app layer's entry point. |

## firmware/ — iPod Classic 6G (S5L8702)

| File | What changed | Why |
| --- | --- | --- |
| `target/arm/s5l8702/ipod6g/storage_ata-6g.c` | **SSD storage mode** (~150 lines, the largest single divergence) — see below | SSD/iFlash mods are very common on this player, and upstream's driver treats every device as a spinning disk. |
| `target/arm/s5l8702/ipod6g/power-6g.c` | **Charger classification** (~100 lines) — see below | Upstream reports any USB insertion as a charger. Some sources cannot supply device + charge current. |
| `target/arm/s5l8702/system-s5l8702.c`, `system-target.h` | New 108 MHz clocking level (`CLK_USB`) between the 216 MHz boost and 54 MHz unboost, selected by a new `set_ahb_boost(bool)` | For workloads needing AHB bandwidth rather than CPU cycles. `set_cpu_frequency()` honours the flag on its unboost path, so a CPU boost ending does not undo an active AHB boost. |
| `drivers/audio/cs42l55.c`, `export/cs42l55.h` | New `audiohw_set_hp_power()`, `audiohw_idle_powerdown()`, `audiohw_idle_powerup()` | Idle power-down for the codec. Master-mutes first to avoid a pop, then asserts `PDN_CODEC`; the chip preserves register contents through this, so power-up only clears the bit, waits 200 µs for the DAC, and unmutes. `HPACTL`/`HPBCTL` deliberately untouched. |
| `target/arm/s5l8702/ipod6g/cscodec-6g.c` | `cscodec_power()` now calls `pmu_ldo_power_on/off(3)` | Upstream is a stub: `(void)state; //TODO: Figure out which LDO this is`. It is LDO 3. |
| `export/s5l87xx.h` | `USB_NUM_ENDPOINTS` 6 → 7 | The hardware has 9. USB audio needs the seventh for its isochronous endpoint. |

### SSD mode, in detail

- **Two-stage sleep.** In SSD mode, sleep no longer issues `STANDBY IMMEDIATE`;
  it gates the ATA controller clock. Flash stays powered, GPIOs stay configured,
  controller state survives, and wake takes a fast path with no PATA re-init.
  If 10 s then pass with the backlight off, it drops to *deep* sleep (`PCON`
  registers cleared, IDE power off), which does need the full re-init on wake.
- **Eager wake.** `ata_spin()` and `Q_STORAGE_PRE_WAKE` both power the device up
  immediately rather than letting the first transfer block.
- **No HDD tuning.** `SET FEATURES 0x05/0x80` (lowest power mode) and
  `0x42/0x80` (lowest noise) are spinning-disk features and are skipped.
- **Ranged cache maintenance.** Transfers use `commit_dcache_range()` /
  `commit_discard_dcache_range()` over the actual extent instead of upstream's
  whole-cache flush.
- **Auto-detection** runs in `ata_init()` via `ata_disk_isssd()`, so the mode is
  right before settings are even loaded.

### Charger classification, in detail

- Charging is disabled over GPIO C1 when the USB current commitment is under
  500 mA. Without this, a source that cannot supply device + charge current (an
  MFi DAC with no power bank) causes charge oscillation.
- When the backlight comes on, a 10 ms tick-ISR timeout watches the charge
  status line. This catches sub-500 ms dropouts the 500 ms power-thread poll
  misses — above 80 % battery the charger IC sustains a low charge current from
  weak sources but drops out briefly every 5–10 s.
- An 8-sample debounce decides "real charger" vs. "plain USB". It is
  deliberately asymmetric: true readings never reset the removal counter, so
  roughly 50/50 oscillation still accumulates enough false readings to clear the
  flag.

## firmware/ — config headers

| File | What changed | Why |
| --- | --- | --- |
| `export/config.h` | New `PODBOX_NO_USB_IAP`, ANDed into upstream's `USB_ENABLE_IAP` gate | Upstream's gate is generic (Apple vendor ID + interrupt + isochronous endpoints) and both targets satisfy it. There is no dock or accessory here to test iAP against, and shipping an untestable subsystem invites unreproducible bug reports. Deleting the one define re-enables it — driver, `SOURCES` entries and descriptors are all present. It also brings back `HAVE_MULTIMEDIA_KEYS`, which nothing in `apps-ipod/` uses. |
| `export/config/ipod6g.h` | `HAVE_RECORDING` commented out | DAP-only fork; no recording UI ships. |
| `export/config/ipod6g.h` | `PLUGIN_BUFFER_SIZE` 2 MiB → 3 MiB | There is no plugin system. The name survives for the core scratch buffer (`apps-ipod/system/app_buffer.c`) that core screens allocate from. |
| `export/config/ipod6g.h` | `ROCKBOX_HAS_LOGF` defined | Serial logging on by default for this target, which has never been run on hardware. Pairs with the enlarged `MAX_LOGF_SIZE`. |
| `export/config/ipod6g.h` | `TARGET_EXTRA_THREADS 1` | Raises `MAXTHREADS`. Sits inside the block that enables `IPOD_ACCESSORY_PROTOCOL`, which needs a thread. |
| `export/config/ipodvideo.h` | `HAVE_RECORDING` commented out | As above. |
| `export/config/ipodvideo.h` | `CONFIG_TUNER`, `HAVE_RDS_CAP`, `CONFIG_RDS` commented out | The Apple remote tuner accessory is not a target of this build. Leaving them defined left a whole FM surface reachable and pointless: Radio Screen theme option, Radio Settings menu, main-menu FM entry, alarm-wake-to-FM. Now matches `ipod6g.h`, which never defined them. |

> **USB audio is on for both targets.** RockPod restricted `USB_ENABLE_AUDIO` to
> the S5L8702; that restriction was deliberately not carried forward. The
> generic gate and ARC isochronous support both predate the fork, so dropping it
> restores USB audio on the 5G rather than introducing it. DesignWare (6G) and
> ARC (5G) both set `USB_HAS_ISOCHRONOUS`.

---

## lib/

| File | What changed | Why |
| --- | --- | --- |
| `rbcodec/metadata/mp4.c`, `metadata.h` | New `bool has_video` on `struct mp3entry`, set when the MP4 handler box reads `vide` | Lets tagcache skip music videos in MP4 containers. |
| `skin_parser/tag_table.c` | `find_custom_tag()` declared **weak** and called **first** in `find_tag()` | This fork's skin engine has tags upstream does not (see `custom-skin-tags.md`), and they are registered from the app layer rather than by editing upstream's table. Weak so `lib/skin_parser` still links standalone (the theme editor), where it resolves to NULL and is skipped. |
| `skin_parser/tag_table.h` | Five new `SKIN_TOKEN_*` enum members | The token type field is a 1-byte short-enum, so the values must be members of *this* enum to fit and to be matched. Only the values live here; the tag-table rows stay in the app layer's `custom_tags.c`. |
| `skin_parser/tag_table.h` | New `SKIN_REFRESH_SPECTRUM`, added to `SKIN_REFRESH_NON_STATIC` | So spectrum-bar lines are redrawn on the changes-over-time pass. |

**Why custom tags are looked up first.** Upstream's search tries three
characters after the `%`, then two, then one. A custom tag named `sel` would be
matched as the existing single-char tag `s` and shadowed. Custom lookup does its
own full-length match, so going first makes the longer name win. This is only
safe because custom names are verified not to clash with upstream ones — it was
a real bug before the ordering was fixed.

---

## tools/

### The `--appsdir` wiring

The application layer is `apps-ipod/`, not `apps/`. Four files make that work.
**A configure invocation without `--appsdir=apps-ipod` silently builds `apps/`
instead** — it will succeed, and produce the wrong firmware.

| File | What changed | Why |
| --- | --- | --- |
| `configure` | Accepts and documents `--appsdir=DIR`, with an existence check | The entry point. Fails loudly rather than producing a broken build. |
| `configure` | New `coreapps` / `coreappsdir`, exported as `COREAPPSDIR` | `coreappsdir` always points at the application layer. The existing `appsdir` is what the *current build type* compiles — bootloader, checkwps and database builds repoint it while still needing the core bitmaps and lang files. |
| `configure` | `picklang()` scans `${coreapps}/lang/*.lang` | Was hardcoded to `apps`. |
| `root.make` | New `APPSBUILDDIR`; bitmaps include and the `voice` target use it | Objects mirror their source path under `$(BUILDDIR)`, so the output path has to track `COREAPPSDIR` too. |
| `mkinfo.pl` | Detects a core build by comparing `APPSDIR` against `COREAPPSDIR`, not by matching `/\/apps$/` | Otherwise `rockbox-info.txt` silently loses its `Actual size`, `RAM usage` and `Features` lines. |
| `buildzip.pl` | New `$APPSDIR` from the environment, used for `tagnavi.config` and `lang/Invalid*.talk` only | This file is kept as close to upstream as possible, so it gets the smallest change that works — just the two files genuinely shipped *from* the application layer. Everything else, including its `apps/plugins` paths, is untouched. |
| `voice.pl`, `langstatus` | Hardcoded to `apps-ipod/lang/` | Standalone scripts with no access to `COREAPPSDIR`. Voice builds are unverified here regardless — `VOICE_VERSION` no longer resolves because `talk.h` moved, so `rockbox-info.txt` reports an empty `Voice format:`. |

### New tools

| File | What it is | Why it exists |
| --- | --- | --- |
| `convfnt.c` | Exports one glyph from a Rockbox `.fnt` to an 8-bit greyscale `.bmp`, and imports an edited `.bmp` back | This fork's theme icon fonts are **4 bpp**, and `convbdf` cannot round-trip them (BDF is 1 bpp). The file header documents the `RB12` layout, including that 4 bpp pixels are nibbles, low nibble first, inverted relative to ink (15 = background, 0 = full) while the `.bmp` is the other way round. |
| `art_fetch/art_fetch.py`, `art_fetch/README.md` | Python 3 album and artist artwork fetcher (`requests`, `Pillow`, `mutagen`) | Library maintenance. Not part of any build; run by hand. |

### Build-failure documentation

| File | What changed | Why |
| --- | --- | --- |
| `checkwps/README` | New "DOES NOT BUILD IN THIS FORK" section | `configure` still offers `--type=C`, so someone will try. Its `SOURCES` and `.make` name core files at `apps/` paths that no longer exist. It would also need `lib/skin_parser` built with this fork's custom tags, or it rejects valid themes. |
| `database/README` | New "DOES NOT BUILD IN THIS FORK" section | Same for `--type=D`, plus it needs a replacement for the deleted uisimulator filesystem backend. |

Neither affects a normal firmware build. `(S)imulator` and `(W)arble` are
equally broken and equally unbuilt.

---

## Repo root

`make zip` alone produces an **incomplete** zip: no theme, no first-boot config,
no EQ presets, and a pile of files this fork cannot use. That is because
`buildzip.pl` is kept upstream-shaped. The two bundle scripts make up the
difference, and `build-hw.sh` runs both.

| File | What it is | Why it exists |
| --- | --- | --- |
| `build-hw.sh` | Clean build for either target into `build-hw-<target>/`; accepts `ipod6g`/`6g` or `ipodvideo`/`5g` | Passes `--appsdir=apps-ipod` and runs both bundle scripts after `make zip`. The supported way to produce a shippable build. |
| `bundle-theme.sh` | Injects Themify_2 and a pre-populated `config.cfg` into the zip; deletes files the build cannot use | Lives here rather than in `buildzip.pl` so that file stays byte-identical to upstream. The `config.cfg` makes Themify_2 the first-boot default, applied before any compiled `DEFAULT_WPSNAME` fallback. |
| `bundle-eqs.sh` | Injects `eqs/*.cfg` into `.rockbox/eqs/` | The presets live at the repo root, not the `lib/rbcodec/dsp/eqs/` that `buildzip.pl` copies from — that directory is empty here. |
| `.gitignore` | `/build*` narrowed to `/build-hw-*/`, `/build-hw/`, `/build-sim/`; adds `/notes/`, `/.specifications/` | Local working drafts. |
| `wps/WPSLIST` | `cabbiev2` theme block removed (180 lines); explanatory comment added | `wpsbuild.pl` builds from this file, so delisting is what stops cabbiev2 shipping. The files stay in `wps/` because the tree mirrors upstream. `rockbox_failsafe` is kept — it is the skin engine's emergency fallback if a configured skin fails to parse. |

**What `bundle-theme.sh` deletes, and why the deletions live there:**

- The `classic_statusbar` theme, which `buildzip.pl` copies straight out of
  `wps/` without consulting `WPSLIST`. Both the directory *and* the two loose
  `classic_statusbar.{sbs,rsbs}` files beside it — a `wps/classic_statusbar/*`
  glob does not match those.
- ~380 KB of plugin data (Lua scripts, level files, viewer config) that
  `buildzip.pl` reads from a hardcoded `$ROOT/apps/plugins` path, ignoring
  `--appsdir`, for a loader this fork does not have. Plus `viewers.config`, now
  that the extension-to-viewer mapping is compiled into `filetypes.c`.

Together these take the zip from 163 files to 95.

---

## Deliberately *not* changed

These look like omissions and are not:

| Not changed | Why |
| --- | --- |
| `apps/` | Untouched upstream mirror, kept so merges apply cleanly. `apps-ipod/` is ours; do not edit `apps/`. |
| `usbstack/usb_audio.c` | Upstream's is sink-only (host → player) and reaches both targets. RockPod's added a source mode that only worked on the 6G. |
| `usbstack/iap/` | Upstream's vendored libiap. RockPod's `usb_iap_hid.c` and its transport indirection served a USB MFi/dock-DAC feature this fork no longer carries. |
| `target/arm/s5l8702/usb-designware.c` | RockPod's isochronous plumbing was written for the above and calls `usb_audio_source_streaming()`, which no longer exists. |
| `target/arm/s5l8702/pcm-s5l8702.c` | RockPod's per-start/stop I2S clock gating is **deferred, not rejected**. Upstream's `pcm_sink` refactor removed the functions it patched and the replacements nest, so a naive port would gate the clock on every buffer. It is a 6G power optimisation and there is no 6G here to test it on. |
