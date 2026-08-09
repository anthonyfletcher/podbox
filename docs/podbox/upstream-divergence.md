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

Most of what that lists is code, build-system or build-script files; the rest is
documentation, the Themify_2 theme, the EQ presets and the logo. Run it rather
than trusting a count written down here — this document is a guide to *why* the
files differ, and the set moves every time one is added.

**`apps/` is byte-identical to upstream, and stays that way.** It is kept so
`git merge rockbox/master` applies without delete/modify conflicts. The same
goes for `manual/`, `uisimulator/`, `android/`, `backdrops/`, `screenshots/` and
every `themes/` entry but Themify_2. Being unused is not a reason to prune them.

Most `firmware/` changes are hardware work inherited from the RockPod fork
(GPLv2), which upstream has no equivalent of. The `tools/` changes are this
fork's own.

### Not everything the command lists is divergence

The base is a fixed commit, so a file taken from a *later* upstream commit also
shows as modified — identical to current `rockbox/master`, different from the
rebase point, and not a fork patch at all. Distinguish them:

```bash
git diff --stat HEAD rockbox/master -- <file>   # empty output: upstream-identical
```

Current forward-ports, held here so they are not mistaken for local work:
`firmware/common/strcasestr.c`, `firmware/drivers/lcd-bitmap-common.c`,
`lib/rbcodec/dsp/eq.c`, `lib/rbcodec/dsp/eq.h`. Each is recorded in
[`upstream-commit-log.md`](upstream-commit-log.md), which is the per-commit
companion to this file: it answers *what was done about a given upstream
commit*, where this one answers *why a given file differs*.

---

## firmware/ — core

| File | What changed | Why |
| --- | --- | --- |
| `backlight.c` | `#include "../apps/gui/skin_engine/skin_engine.h"` → `"gui/skin_engine/skin_engine.h"` | Upstream reaches into the app layer by relative path, which lands in the unbuilt `apps/` mirror. Now resolved through the `apps-ipod/api/` stub like every other cross-layer include. |
| `backlight.c` | Backlight off calls `storage_sleep()`; both wake paths post `Q_STORAGE_PRE_WAKE` | Backlight state gates SSD sleep. Pre-waking from ISR context lets the storage thread start powering the SSD up before the UI thread has even processed the button press that caused the wake. Guarded by `storage_get_ssd_mode()`. |
| `backlight.c` | `power_input_present()` → `charger_inserted()` in `backlight_get_current_timeout()` | The 6G now distinguishes a plain USB port from a real charger. `power_input_present()` is true for both, so a data-only connection would wrongly get the plugged-in backlight timeout. |
| `drivers/ata.c`, `export/ata.h` | New `ata_set_storage_mode(int)` / `ata_get_ssd_mode()`, backed by a file-scope `ata_ssd_mode`. In SSD mode `ata_sleepnow()` no longer arms `power_off_tick`. | Declares the SSD-mode interface for all ATA targets, and implements the one part of it that is not 6G-specific. Modes: 0 auto (asks `ata_disk_isssd()`), 1 HDD, 2 SSD; the 6G's richer handling is in `storage_ata-6g.c`. The generic driver is what the 5G runs, and it was powering the interface off after seven idle seconds (`sleep_timeout` then `ATA_POWER_OFF_TIMEOUT`) regardless of the setting. Waking from off runs `ata_power_on()` — a fixed `HZ/4` supply settle, hard reset, two IDENTIFYs — measured at ~780 ms on a 5G with an SD adapter, paid by the next file access. Flash saves almost nothing by being powered off rather than merely in standby. |
| `export/storage.h`, `storage.c` | New `Q_STORAGE_PRE_WAKE` event, stub functions and macro dispatch for the two calls above | Plumbing so the app and backlight layers can reach SSD mode through the generic `storage_*` interface. |
| `powermgmt.c` | The charger case sets `CHARGING` unconditionally instead of falling through, and the battery-level test keys off `charge_state > DISCHARGING` | On the 6G the charge-status line oscillates against weak USB sources, so `charging_state()` reads false while charging is genuinely happening. Upstream's fallthrough flips the reported state back and forth, and with it the voltage-to-percentage curve, which moves the reading several points. |
| `powermgmt.c` | New `charge_finished`, debounced 8 samples off `charging_state()` and held until unplug; the 99 % cap now needs it as well as `charge_state` | The change above answered two questions with one variable and got the second wrong. Curve selection wants debounced charger *presence*; the "< 100 % until charging is finished" cap wants "is charge still going in?", which `charge_state` cannot say — it reads `CHARGING` from plug-in to unplug, so **a full battery never showed 100 %**. Splitting them keeps the oscillation fix and releases the cap. Other targets never set the flag, so they keep upstream's behaviour exactly. |
| `export/config/ipod6g.h` | `ROCKBOX_HAS_LOGF` defined for non-bootloader 6G builds (upstream defines it only inside the disabled bootloader block) | The 6G keeps a serial log. **`export/logf.h` is no longer touched:** `MAX_LOGF_SIZE` had been raised 16 KiB → 256 KiB, which on this target is a 256 KiB always-resident `logfbuffer` in `.bss` — jointly the largest object in `rockbox.elf`. Restored to upstream's 16 KiB, so that header is byte-identical again and the buffer costs what upstream intends. Note `logf()` here is a real `vsnprintf`, not `do {} while(0)`, and `apps/playback.c` and `apps/codecs.c` arm `LOGF_ENABLE` *upstream* — so the 6G's audio path does log on every buffering and codec event. Nothing of this reaches the 5G, which leaves `ROCKBOX_HAS_LOGF` undefined. |
| `drivers/rtc/rtc_pcf50605.c` | Alarm functions, the `alarm_disable` table and the `rtc_init()` call to `rtc_check_alarm_started()` wrapped in `#ifdef HAVE_RTC_ALARM` | The prototypes in `export/rtc.h` are already guarded, but this driver referenced them unconditionally — it was the only RTC driver without the guard, because every upstream target using the PCF50605 defines `HAVE_RTC_ALARM`. Undefining it for the 5G broke the build. Now matches the shape of `rtc_ds1339_ds3231.c`, `rtc_e8564.c` and the rest, so this is upstream's own convention rather than a fork invention. |

## firmware/ — USB stack

Four features: keeping other work out of an enumerating host's way, the
`host_wrote` flag, when the mass-storage buffers are claimed, and an insertion
diagnostic.

### `usb_host_is_present()`

| File | What changed | Why |
| --- | --- | --- |
| `usb.c` | New `usb_host_is_present()`, returning the existing static `usb_host_present`. A stub returning false joins the `USB_NONE` dummies | The app layer distinguishes an enumerating host from a cable that only supplies power. |
| `export/usb.h` | Declares it | — |
| `usb.c` | `usb_set_host_present()` raises the USB thread to `PRIORITY_REALTIME`, and restores `PRIORITY_SYSTEM` when the host goes | Upstream raises it in `usb_core_do_set_config()`, leaving enumeration at `PRIORITY_SYSTEM` (18) — below the UI thread's 16. |

Background work — a database scan or commit, an album index build, a file
index walk — stands down while a host is enumerating, and the tagcache's
boot-time pass defers rather than starting on top of one.

This is contention, not correctness. It keeps a background pass from holding
the disk, the bus and the locks a mounting host is waiting on. It is **not**
the cause of the `SET_ADDRESS` enumeration failures on the iPod Video, which
remain open — see *Insertion diagnostic* below for the signature and how to
read it.

`usb_inserted()` is unsuitable as the signal. It covers `USB_POWERED` as well
as `USB_INSERTED`, so a charger satisfies it too, and a player kept on charge
would never finish indexing.

`usb_host_present` carries the needed meaning. Both targets define
`USB_DETECT_BY_REQUEST` (`config.h`, under `USBOTG_ARC` and under
`CONFIG_CPU == S5L8702`), so it turns true on the first completed control
transfer: after enumeration starts, before `SET_ADDRESS`, and never for a
charger.

`SYS_USB_CONNECTED` does not serve here. Nothing is broadcast until
`SET_CONFIGURATION`, which is after `SET_ADDRESS`.

### `host_wrote`

The app layer rebuilds the tag database and dircache on every USB disconnect, on
the assumption the host changed files. If the host only ever read, none of that
work is needed — and Windows produces a spurious disconnect/reconnect on
*every* connect, which without the flag triggers the full rebuild and leaves
the tagcache thread mid-scan when the reconnect arrives.

| File | What changed | Why |
| --- | --- | --- |
| `usbstack/usb_storage.c` | Static `host_wrote`, cleared on connect, set by `SCSI_WRITE_10` / `SCSI_WRITE_16`; accessor `usb_storage_host_wrote()` | Where the writes actually are. |
| `usbstack/usb_storage.h` | Declares the accessor | — |
| `usbstack/usb_core.c` | `usb_core_host_wrote_storage()`, forwarding when `USB_ENABLE_STORAGE` is set and returning false otherwise | `usb_storage.h` is private to the USB stack, so the app layer cannot include it. |
| `export/usb_core.h` | Declares it | The app layer's entry point. |

### When the mass-storage buffers are claimed

| File | What changed | Why |
| --- | --- | --- |
| `usbstack/usb_storage.c` | The ~128K transfer-buffer allocation moves out of `usb_storage_init_connection()` into a new idempotent `usb_storage_alloc_buffers()`; `usb_storage_disconnect()` no longer frees it | Upstream allocates it inside the host's `SET_CONFIGURATION`. It is immovable buflib, so after `audio_init()` it can only be met by shrinking the audio buffer — whose callback stops playback with a synchronous `queue_send`, blocking the USB thread mid-control-transfer. Freeing on disconnect would make the next connection pay again. |
| `export/usb_core.h` | Declares it | `usbstack/` is not on `usb.c`'s include path. |
| `usb.c` | `usb_init()` calls it | Before `audio_init()`, so there is nothing to shrink. Costs ~128K of audio buffer for the life of the firmware. |

**Do not move this to `usb_storage_init()`.** `usb_core_init()` runs
`usb_drv_init()` — which sets `USBCMD_RUN` and attaches the device — *before*
the class drivers' `init()`, so a stall there lands before `SET_ADDRESS` and
stops enumeration outright. Boot is the only point where nothing can be waiting.

Targets defining `USB_STATIC_ALLOC` use BSS and never had this. PP502x is
excluded from that list because `USB_DEVBSS_ATTR` is `IBSS_ATTR` there and 128K
does not fit in IRAM.

### Insertion diagnostic

Scaffolding for the open `SET_ADDRESS` failures, reachable at
**Debug → View USB info**. Plain counters, no allocation and no disk, none of
it in the timing-critical path.

| File | What changed | Why |
| --- | --- | --- |
| `export/usb.h` | `struct usb_insert_record`, `enum usb_waypoint`, `usb_get_insert_record()`, `usb_record_waypoint()` | What the last insertion decided and how far enumeration got. |
| `usb.c` | Static record, zeroed per insertion; captures `button_status()`, the ignore mask, `usb_mode`, `usb_power_only`, and the ack bookkeeping | A connect that does nothing is either charging-only or a stuck handover, and nothing on screen tells them apart. |
| `usbstack/usb_core.c` | Waypoints in `usb_core_bus_reset()`, `usb_core_setup_received()`, `usb_core_do_set_addr()`, `usb_core_do_set_config()`, and the driver loop | Placed at the handlers, not at `usb_core_handle_notify()`: the ARC driver calls `usb_core_bus_reset()` straight from its ISR and posts no notifications, so counters on the notify path read zero even on a healthy connection. |

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

**One setting, two mechanisms — worth knowing before changing either driver.**
Both are after the same thing, idling flash more cheaply than a disk. They get
there differently, and neither driver can see the other:

| | `drivers/ata.c` (5G) | `storage_ata-6g.c` (6G) |
| --- | --- | --- |
| What SSD mode changes | Stops the interface being powered off at all | Makes the first sleep stage cheaper — clock-gate instead of `STANDBY IMMEDIATE` |
| Does power-off still happen? | No | Yes, as a second stage: ten more seconds with the backlight off |
| Wake cost hidden how? | Nothing to hide | `Q_STORAGE_PRE_WAKE` |

That last row follows from the one above it. `backlight.c` posts
`Q_STORAGE_PRE_WAKE` on **both** targets under `storage_get_ssd_mode()`, and
only the 6G handles it — because only the 6G has a power-off to hide. Give the
generic driver a power-off path and the pre-wake will not fire for it, bringing
back the ~780 ms wake measured on the 5G, unhidden. Noted at both
`ata_ssd_mode` declarations for that reason.

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

**A decline has to be expressed here, not in a document.** `git merge
rockbox/master` only stops to ask about files this fork also edits; it takes
upstream's copy of everything else silently. So an upstream feature whose wiring
lives in files this fork has never touched arrives *enabled*, whatever
`upstream-commit-log.md` says about it. The two rows below are how that is
prevented — undefine or gate the feature here, let the code land wherever the
merge puts it, and it compiles to nothing.

Everything else this fork declines happens to sit in a file it already edits, so
those conflict on merge and surface on their own. Audited 2026-08-08; if a
future decline is not in that position, it needs a row here.

| File | What changed | Why |
| --- | --- | --- |
| `export/config.h` | `#undef HAVE_MIKEY_REMOTE`, after the target configs are included | Holds out the iPod Classic inline earphone remote (upstream `b217a55059`), whose wiring is in `firmware/SOURCES`, `export/button.h` and `target/arm/ipod/button-clickwheel.c` — none of which this fork edits, so a merge would take all three and build the driver. Every hook is `#ifdef`'d on this and `mikey-6g.c` is only reached through them. Deferred on maturity, not applicability: see the commit log, which also lists what taking it would need beyond the upstream diff. |
| `export/config.h` | New `PODBOX_NO_USB_IAP`, ANDed into upstream's `USB_ENABLE_IAP` gate | Upstream's gate is generic (Apple vendor ID + interrupt + isochronous endpoints) and both targets satisfy it. There is no dock or accessory here to test iAP against, and shipping an untestable subsystem invites unreproducible bug reports. **Settled 2026-07-29: this stays off permanently** — see below. It also suppresses `HAVE_MULTIMEDIA_KEYS`, which nothing in `apps-ipod/` uses. |
| `export/config/ipod6g.h` | `HAVE_RECORDING` commented out | DAP-only fork; no recording UI ships. |
| `export/config/ipod6g.h` | `PLUGIN_BUFFER_SIZE` 2 MiB → 3 MiB | There is no plugin system. The name survives for the core scratch buffer (`apps-ipod/system/app_buffer.c`) that core screens allocate from. |
| `export/config/ipod6g.h` | `ROCKBOX_HAS_LOGF` defined | Serial logging on by default for this target, which has never been run on hardware. Pairs with the enlarged `MAX_LOGF_SIZE`. |
| `export/config/ipod6g.h` | `TARGET_EXTRA_THREADS 1` | Raises `MAXTHREADS`. Sits inside the block that enables `IPOD_ACCESSORY_PROTOCOL`, which needs a thread. |
| `export/config/ipodvideo.h` | `HAVE_RECORDING` commented out | As above. |
| `export/config/ipodvideo.h` | `CONFIG_TUNER`, `HAVE_RDS_CAP`, `CONFIG_RDS` commented out | The Apple remote tuner accessory is not a target of this build. Leaving them defined left a whole FM surface reachable and pointless: Radio Screen theme option, Radio Settings menu, main-menu FM entry. Now matches `ipod6g.h`, which never defined them. |
| `export/config/ipodvideo.h` | `HAVE_RTC_ALARM` commented out | The wake-up alarm could never be made to work, and the 5G was the only target that built it — `ipod6g.h` already had it commented out. The Apple bootloader clears the PCF interrupt registers before Rockbox runs, so `rtc_check_alarm_started()` guesses instead: it calls it a wake if the clock matches the alarm registers to within ten seconds, comparing seconds as raw BCD so the real window is 0–9. A 5G booting off a spinning disk usually misses it, and misses silently. The whole apps-side alarm — screen, wake image, menu entry, `CONTEXT_ALARMSCREEN` — is removed rather than left compiled out. |

> **USB audio is on for both targets.** RockPod restricted `USB_ENABLE_AUDIO` to
> the S5L8702; that restriction was deliberately not carried forward. The
> generic gate and ARC isochronous support both predate the fork, so dropping it
> restores USB audio on the 5G rather than introducing it. DesignWare (6G) and
> ARC (5G) both set `USB_HAS_ISOCHRONOUS`.

### USB iAP stays off — decided, not deferred

`PODBOX_NO_USB_IAP` is a policy decision, not a hardware limitation. Both
targets satisfy upstream's gate, and RockPod shipped MFi digital audio on the
6G, so the feature is genuinely applicable. It is off because:

- **The application layer is not written for a second PCM sink.** Enabling it
  defines `PCM_SINK_IAP`, taking `PCM_SINK_NUM` from 1 to 2. Nothing in
  `apps-ipod/` calls `pcm_current_sink()` or `pcm_sink_caps()`, and
  `audio_guess_frequency()` (`apps-ipod/audio/playback.c`) is sink-unaware.
  Upstream's fixes for that (`f343168051`, `1d5aa53321`) are declined in
  `upstream-commit-log.md` *because* there is only one sink — so re-enabling
  makes them prerequisites rather than dead rows.
- **The payoff is narrow.** Digital audio out to an MFi dock or DAC. Ordinary
  docks and car AUX take analogue off the line-out pins and need no protocol.
- **It has never run on a 5G.** The prior art is 6G/DesignWare; the 5G is ARC.
  `USB_ENABLE_AUDIO` is already on there and untested, so enabling this too
  would stack two unproven USB features on one controller.

The comment at `config.h:1402` says re-enabling needs "nothing else". True of
the build — it compiles — but not of the behaviour, per the first point.

**Serial iAP is unaffected and stays on.** It is a different transport
(`IPOD_ACCESSORY_PROTOCOL`, UART pins on the dock connector), implemented in
`apps-ipod/iap/` and so outside this document's scope — `upstream-commit-log.md`
records that it carries no RockPod code and tracks upstream.

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
| `buildzip.pl` | New `$APPSDIR` from the environment, for `tagnavi.config` and `lang/Invalid*.talk` | This file is kept as close to upstream as possible, so it gets the smallest change that works — just the files genuinely shipped *from* the application layer. Everything else, including its `apps/plugins` paths, is untouched. |
| `buildzip.pl` | New `$APPSBUILDDIR`, the basename of `COREAPPSDIR`, for `lang/*.lng` and `lang/*.zip` | **Two different questions, two different variables.** `$APPSDIR` is a *source* path; the `.lng` files are *build products*, named relative to the build directory buildzip runs in. Upstream's hardcoded `apps/lang/*.lng` exists in the source tree and is empty in the build tree, so every one of the 48 compiled languages was silently dropped from the zip and Settings > Language browsed an empty directory. `COREAPPSDIR` rather than `APPSDIR` because a bootloader build points the latter at `bootloader/`, which has no lang directory at all. |
| `voice.pl`, `langstatus` | Derive the application directory from `COREAPPSDIR`, falling back to whichever of `apps-ipod/` or `apps/` exists | Both were hardcoded to `apps-ipod/lang/`, which solved the same problem two ways in one tree and would break silently on the next `--appsdir` change. `voice.pl` does get `COREAPPSDIR` when make invokes it; the fallback is for a hand-run, which is the only way `langstatus` is ever used. Voice builds are unverified here regardless — `VOICE_VERSION` no longer resolves because `talk.h` moved, so `rockbox-info.txt` reports an empty `Voice format:`. |

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
no EQ presets, upstream's licence file rather than this fork's, and a pile of
files this fork cannot use. That is because `buildzip.pl` is kept
upstream-shaped. The three bundle scripts make up the difference, and
`build-hw.sh` runs all three.

| File | What it is | Why it exists |
| --- | --- | --- |
| `build-hw.sh` | Clean build for either target into `build-hw-<target>/`; accepts `ipod6g`/`6g` or `ipodvideo`/`5g` | Passes `--appsdir=apps-ipod` and runs both bundle scripts after `make zip`. The supported way to produce a shippable build. |
| `bundle-theme.sh` | Injects Themify_2 and a pre-populated `config.cfg` into the zip; deletes files the build cannot use | Lives here rather than in `buildzip.pl` so that file stays upstream-shaped. The `config.cfg` makes Themify_2 the first-boot default, applied before any compiled `DEFAULT_WPSNAME` fallback. |
| `bundle-eqs.sh` | Injects `eqs/*.cfg` into `.rockbox/eqs/` | The presets live at the repo root, not the `lib/rbcodec/dsp/eqs/` that `buildzip.pl` copies from — that directory is empty here. |
| `bundle-licenses.sh` | Prepends `docs/podbox/LICENSES` to upstream's `docs/LICENSES` and replaces `.rockbox/docs/LICENSES.txt` | The fork imports fonts and artwork upstream does not, and their licences have to travel with the build. Same reason as the other two: `buildzip.pl` copies upstream's file and is kept upstream-shaped. |
| `docs/podbox/LICENSES` | The fork's own licence notices — Literata, League Spartan, Themify 2 — with the SIL Open Font License 1.1 reproduced in full | Prepended to upstream's `docs/LICENSES` by `bundle-licenses.sh`. The OFL requires its notice to travel with the font, and the device is offline, so a URL is not enough; upstream's file inlines every licence it references and these follow that. |
| `release.sh` | Builds both targets on a build server from a `git archive` of HEAD, verifies each zip, then replaces the rolling `latest` release | Publishing by hand gets three things wrong — asset names colliding, `gh` needing `--repo` on the server, and a leftover tag being reused rather than moved. |
| `docs/CREDITS` | Three attribution blocks prepended: Themify, RockPod, and a "For RockBox:" heading before upstream's list | This fork ships a theme and inherits a large body of hardware work from another fork, both GPLv2 with named authors. Upstream's list is left untouched below the heading, so a merge from Rockbox applies to it cleanly. |
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
| `usbstack/iap/` | Upstream's vendored libiap. RockPod's `usb_iap_hid.c` and its transport indirection served a USB MFi/dock-DAC feature this fork no longer carries. Compiled out entirely by `PODBOX_NO_USB_IAP`, which is now a settled decision — kept upstream-identical so it stays mergeable rather than because it builds. |
| `target/arm/s5l8702/usb-designware.c` | RockPod's isochronous plumbing was written for the above and calls `usb_audio_source_streaming()`, which no longer exists. |
| `target/arm/s5l8702/pcm-s5l8702.c` | RockPod's per-start/stop I2S clock gating is **deferred, not rejected**. Upstream's `pcm_sink` refactor removed the functions it patched and the replacements nest, so a naive port would gate the clock on every buffer. That nesting is the whole difficulty; a 6G is available to measure the result on. |
