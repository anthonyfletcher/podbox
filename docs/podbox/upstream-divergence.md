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

The comparison base is Rockbox commit `789d796120`, the last upstream commit
merged in — not the original fork point, and no longer the `24c3779146` this
fork was rebased onto. It moves with every merge, so derive it rather than
typing it. To regenerate the list:

```bash
git diff --name-status $(git merge-base HEAD rockbox/master)..HEAD -- . ':(exclude)apps-ipod'
```

Most of what that lists is code, build-system or build-script files; the rest is
documentation, the themes and the logo. Run
it rather than trusting a count written down here — this document is a guide to
*why* the files differ, and the set moves every time one is added.

**`apps/` is byte-identical to upstream, and stays that way.** It is kept so
`git merge rockbox/master` applies without delete/modify conflicts. The same
goes for `manual/`, `android/`, `backdrops/`, `screenshots/` and every `themes/`
entry this fork did not convert. Being unused is not a reason to prune them.

**`uisimulator/` is upstream-identical too, but it is no longer unused** — the
simulator builds and runs. See "The simulator needs nothing here" below.

Most `firmware/` changes are hardware work inherited from the RockPod fork
(GPLv2), which upstream has no equivalent of. The `tools/` changes are this
fork's own.

### Not everything the command lists is divergence

The base is the last commit merged, so a file taken from a *later* upstream
commit also shows as modified — identical to current `rockbox/master`,
different from the base, and not a fork patch at all. Distinguish them:

```bash
git diff --stat HEAD rockbox/master -- <file>   # empty output: upstream-identical
```

There are none outstanding as of the merge through `789d796120`. Each one is
recorded in [`upstream-commit-log.md`](upstream-commit-log.md), the per-commit
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
| `SOURCES` | `target/arm/s5l8702/ipod6g/mikey-6g.c` wrapped in `#ifdef HAVE_MIKEY_REMOTE` | Upstream lists the file unguarded, `ipod6g.h` always defining the gate. This is the one hook upstream did not guard, and it is what keeps holding the inline earphone remote out again a one-line change in `export/config.h`: without it the driver compiles regardless, alone, against a gate nothing else honours. A no-op while the feature is on. |
| `export/config.h` | `HAVE_MULTIMEDIA_KEYS` gated on `USB_ENABLE_IAP \|\| HAVE_MIKEY_REMOTE`, and moved out of the `HAVE_USBSTACK` block | The define means "this target can produce multimedia key codes". Upstream has one producer, a dock or head unit over USB iAP, and so writes the gate as USB iAP alone inside the USB block. The 6G's inline earphone remote (`b217a55059`) is a second producer with nothing to do with USB, and `PODBOX_NO_USB_IAP` turns the first one off here -- left as upstream has it, the remote would be a driver whose key codes nothing reads. |
| `export/rbpaths.h` | New `DEFAULTCONFIGFILE`, `.rockbox/default-config.cfg` | The build ships a first-boot config, and it must not be `config.cfg` — that file is the player's, so an install overwriting it resets the player's settings. The firmware reads this one only when no `config.cfg` exists. |
| `drivers/lcd-16bit-common.c`, `export/lcd.h` | New `lcd_blendrect(x, y, w, h, opacity)` and `LCD_BLEND_OPAQUE`, beside `lcd_fillrect` | Fills with the foreground colour blended against what is already there, for the skin engine's `%dr` opacity argument (`custom-skin-tags.md`). The blending itself is upstream's — it reuses `blend_two_colors()`, the primitive the antialiased font path already runs on. What is new is a rectangle case, where colour and opacity are both loop-invariant and the 4bpp alpha stream disappears. Guarded by `HAVE_LCD_COLOR && !DISABLE_ALPHA_BITMAP`, matching the code it sits in. **It is only useful drawn into the backdrop buffer** — see the note at the function, and §5 of `.specifications/COMPOSITED_BACKDROP_LAYER.md` for why. |
| `drivers/lcd-color-common.c`, `export/lcd.h` | New `lcd_alpha_bitmap_part_img()`, and `export/lcd.h` now declares it and `lcd_alpha_bitmap_part()` | Draws an image through a 4bpp alpha mask with a stride per plane, which is what an anti-aliased corner radius on `%dr`, `%Cl` and `%La` needs — the mask is generated per radius and is not the image's own size, so `lcd_bmp_part()` cannot serve. The blitter itself is upstream's antialiased-font path; both declarations were previously private to the drivers. Guarded by `HAVE_LCD_COLOR && !DISABLE_ALPHA_BITMAP`. |
| `drivers/lcd-16bit-common.c` | `lcd_alpha_bitmap_part_mix()`'s `DRMODE_FG` case skips the blend for a fully transparent pixel | Most of a glyph's box is transparent, and the blitter had no early out: `blend_two_colors()` with `ALPHA_MASK` weights the destination alone and hands it straight back, so those pixels were paying about thirty instructions to write themselves unchanged. The test costs two and takes them to twelve — that is every anti-aliased glyph the player draws, as well as the skin's `%Vt` text shadow, which is the caller that made it worth measuring. Only that case: under `DRMODE_SOLID` a transparent pixel is the background and still has to be painted. The alpha is read either way — `READ_ALPHA()` is what advances the stream — into a local that must not be called `alpha`, the name of the pointer the macro walks. |
| `drivers/rtc/rtc_pcf50605.c` | Alarm functions, the `alarm_disable` table and the `rtc_init()` call to `rtc_check_alarm_started()` wrapped in `#ifdef HAVE_RTC_ALARM` | The prototypes in `export/rtc.h` are already guarded, but this driver referenced them unconditionally — it was the only RTC driver without the guard, because every upstream target using the PCF50605 defines `HAVE_RTC_ALARM`. Undefining it for the 5G broke the build. Now matches the shape of `rtc_ds1339_ds3231.c`, `rtc_e8564.c` and the rest, so this is upstream's own convention rather than a fork invention. |

| `common/dircache.c`, `include/dircache.h` | New `dircache_is_ready()`, `dircache_foreach_name()` and `dircache_get_index_path()` | The cache already holds every filename on the player in RAM, but its public API only resolves a path it is *given* — there is no way to ask what it contains, so a whole-player filename search had no source but a disk index of its own. These expose the sweep upstream already performs privately: `dircache_foreach_name()` is `dircache_dump()`'s `FOR_EACH_CACHE_ENTRY` loop with a callback instead of an `fdprintf`, and paths are built per *hit* rather than per entry because each one costs a walk to the root. Two departures from the file's conventions, both deliberate. The sweep takes the filesystem lock as **READER**, where every other entry point takes `dircache_lock()` (the WRITER): it only reads, and it runs long enough that an exclusive lock would hold off the audio thread's buffering throughout — reader still excludes the scanning thread, which holds the writer across a whole build. And `dircache_is_ready()` reads the volume statuses **unlocked**, because `dircache_get_info()` answers the same question behind the writer lock and so blocks for the length of a scan — which is the wait a readiness check exists to avoid. |

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
| `target/arm/s5l8702/ipod6g/mikey-6g.c` | `mikey_init()` returns without starting the polling thread when `rec_hw_ver == 0` | Capture hardware version 0 -- the 80GB and fat 160GB models -- has no jack microphone and no Mikey on the bus (the version table is in `gpio-s5l8702.c`, and `audio_enable_mic()` already returns early on it). Upstream starts the thread regardless and lets it probe until its retry budget is spent, then back off to a 2s poll forever. The gate is safe because `gpio_init()` runs inside `system_init()`, well before `button_init()`. |
| `target/arm/s5l8702/ipod6g/mikey-6g.c`, `mikey-target.h` | New `mikey_probe(unsigned char *reg0)` | A raw read of the mode register that returns the I2C status instead of swallowing it. `mikey_read()` returns 0 both when the bus NAKs and when the register genuinely reads zero, so the debug screen alone could not say whether an absent remote meant an empty jack, a missing chip or a driver fault. |
| `target/arm/s5l8702/debug-s5l8702.c` | Second line under the mikey row: `jack=`, `hw=`, `probe rc=` and `r0=` | Turns the `--` above it into a diagnosis without another build. `rc` is `i2c_rd()`'s: **1** is the address NAK from the register-pointer write, which is what a unit with no Mikey gives; **0** means the chip answered. `hw` is `rec_hw_ver`, which otherwise only appears on the previous page of the screen. |

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
`upstream-commit-log.md` says about it. The row below is how that is
prevented — undefine or gate the feature here, let the code land wherever the
merge puts it, and it compiles to nothing.

Everything else this fork declines happens to sit in a file it already edits, so
those conflict on merge and surface on their own. Audited 2026-08-22; if a
future decline is not in that position, it needs a row here.

| File | What changed | Why |
| --- | --- | --- |
| `export/config.h` | New `PODBOX_NO_USB_IAP`, ANDed into upstream's `USB_ENABLE_IAP` gate | Upstream's gate is generic (Apple vendor ID + interrupt + isochronous endpoints) and both targets satisfy it. There is no dock or accessory here to test iAP against, and shipping an untestable subsystem invites unreproducible bug reports. **Settled 2026-07-29: this stays off permanently** — see below. Upstream also gates `HAVE_MULTIMEDIA_KEYS` on it; that gate names the inline earphone remote as well here, and has a row of its own above. |
| `export/config/ipod6g.h` | `HAVE_RECORDING` commented out | DAP-only fork; no recording UI ships. |
| `export/config/ipod6g.h` | `PLUGIN_BUFFER_SIZE` 2 MiB → 3 MiB | There is no plugin system. The name survives for the core scratch buffer (`apps-ipod/system/app_buffer.c`) that core screens allocate from. |
| `export/config/ipod6g.h` | `ROCKBOX_HAS_LOGF` defined | Serial logging on by default for this target; upstream defines it only inside the disabled bootloader block. The log itself has never been read off hardware. `MAX_LOGF_SIZE` is upstream's 16 KiB — see the `export/logf.h` note in the core table for why it was put back. |
| `export/config/ipod6g.h` | `TARGET_EXTRA_THREADS` 2 when both `IPOD_ACCESSORY_PROTOCOL` and `HAVE_MIKEY_REMOTE` are on, 1 otherwise | Raises `MAXTHREADS`. Upstream has no such define here; the fork added it for the iAP serial link, and `b217a55059` adds the inline remote's polling thread without bumping any count. `BASETHREADS` is 17 (`HAVE_HARDWARE_CLICK`), so `__threads` in `rockbox.elf` should measure 19 entries. Short by one, `create_thread()` returns NULL and neither caller checks it -- the feature is simply absent, with nothing said. |
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

The comment at the `PODBOX_NO_USB_IAP` define says re-enabling needs "nothing
else". True of the build — it compiles — but not of the behaviour, per the
first point.

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
| `skin_parser/tag_table.h` | Six new `SKIN_TOKEN_*` enum members | The token type field is a 1-byte short-enum, so the values must be members of *this* enum to fit and to be matched. Only the values live here; the tag-table rows stay in the app layer's `custom_tags.c`. |
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
| `buildzip.pl` | New `$APPSDIR` from the environment, for `tagnavi.config` and `lang/Invalid*.talk` | This file is kept as close to upstream as possible, so it gets the smallest change that works — just the files genuinely shipped *from* the application layer. Its `apps/plugins` paths are left alone; `bundle-theme.sh` deletes what they ship instead. |
| `buildzip.pl` | New `$APPSBUILDDIR`, the basename of `COREAPPSDIR`, for `lang/*.lng` and `lang/*.zip` | **Two different questions, two different variables.** `$APPSDIR` is a *source* path; the `.lng` files are *build products*, named relative to the build directory buildzip runs in. Upstream's hardcoded `apps/lang/*.lng` exists in the source tree and is empty in the build tree, so every one of the 48 compiled languages was silently dropped from the zip and Settings > Language browsed an empty directory. `COREAPPSDIR` rather than `APPSDIR` because a bootloader build points the latter at `bootloader/`, which has no lang directory at all. |
| `buildzip.pl` | Upstream's `.map`-bundling block deleted, with a comment left in its place | `rockbox.map` is ~4 MB of text, and the zip is `/MIR`-synced onto the player, so it would cost that on the user's disk at every sync. It buys nothing here either — a panic address is resolved with `nm` on the crashing build's `rockbox.elf`, which the release does not ship. The comment is what makes the next merge conflict here instead of quietly restoring the block. |
| `voice.pl`, `langstatus` | Derive the application directory from `COREAPPSDIR`, falling back to whichever of `apps-ipod/` or `apps/` exists | Both were hardcoded to `apps-ipod/lang/`, which solved the same problem two ways in one tree and would break silently on the next `--appsdir` change. `voice.pl` does get `COREAPPSDIR` when make invokes it; the fallback is for a hand-run, which is the only way `langstatus` is ever used. Voice builds are unverified here regardless — `VOICE_VERSION` no longer resolves because `talk.h` moved, so `rockbox-info.txt` reports an empty `Voice format:`. |

### New tools

| File | What it is | Why it exists |
| --- | --- | --- |
| `convfnt.c` | Exports one glyph from a Rockbox `.fnt` to an 8-bit greyscale `.bmp`, and imports an edited `.bmp` back | This fork's theme icon fonts are **4 bpp**, and `convbdf` cannot round-trip them (BDF is 1 bpp). The file header documents the `RB12` layout, including that 4 bpp pixels are nibbles, low nibble first, inverted relative to ink (15 = background, 0 = full) while the `.bmp` is the other way round. |
| `art_fetch/art_fetch.py`, `art_fetch/README.md` | Python 3 album and artist artwork fetcher (`requests`, `Pillow`, `mutagen`) | Library maintenance. Not part of any build; run by hand. |
| `eq_refit/eq_refit.py`, `eq_refit/README.md` | Re-fits an EQ preset onto fewer bands, keeping its response | Every enabled band is a biquad pass over every sample; a nine-band preset spends most of a PP5022 on the equaliser and starves the UI. Run by hand on a preset written for this player. |
| `pfgeom/pfgeom.c`, `pfgeom/README.md` | Host program that mirrors the album-covers carousel's projection, cull and draw order, and renders frames across the settings space | The carousel's occlusion cull fails as a stripe of stale framebuffer on one album shape at one point in one scroll. This decides it on the host instead of on hardware. It is a **mirror** of `apps-ipod/screens/covers/carousel.c`, so a change to the cull has to be made in both or it stops proving anything. |
| `dbfeat/dbfeat.c`, `dbfeat/README.md` | Host program that compiles `apps-ipod/database/db_featured_parse.c` as it stands and runs it over a table of tag strings with the guests each must yield | Every rule in that parser is a guess about how people write tags, and a wrong one reaches the user as a browser row naming somebody who does not exist. Unlike `pfgeom` there is no copy to drift: the parse file compiles here unmodified, and only the library lookup is stood in for. |
| `check-settings-docs.sh` | Whether `settings-help.txt` and `settings-guide.md` still describe the settings that exist. Five checks; silence means they agree | Nothing in the build looks at either document, and every way they go stale is invisible on the device — **Explain** shows nothing, Search cannot find a row, the guide describes a player that does not exist. Run it from the repository root after touching a setting. |
| `spun_testlog.pl` | Synthesises a playback-log family, plus the expected parse | The development device has no listening history, and Spun's reader depends on cases that take months to accumulate — rotation boundaries, a year change, plays logged against an unset clock. |

### The other build types

`(N)ormal`, `(B)ootloader`, `(C)heckWPS`, `(D)atabase` and `(S)imulator` all
build. `(W)arble` is offered by `configure` and does not; nothing here uses it.
Only CheckWPS needed changes, and it needed all of these:

| File | What changed | Why |
| --- | --- | --- |
| `checkwps/SOURCES` | Every application-layer file repointed from `apps/` to `apps-ipod/`, plus `stubs.c` | Upstream's parser is a different parser. It rejects `!rrggbb`, and it reads past the end of a `%dr` given an opacity or a radius and crashes. Linking this fork's `skin_parser.c` is the only way the tool answers the same question the player does. |
| `checkwps/checkwps.make` | Include path rebuilt around `$(COREAPPSDIR)/api` and `$(COREAPPSDIR)`, mirroring `apps-ipod/apps.make`; `features.txt` and `english.lang` taken from there too; `-DSYSFONT_HEIGHT=8` | `apps-ipod/` includes are written relative to the application layer, and `APPSDIR` is the tool's own directory for this build type. `font.h` skips the generated `sysfont.h` under `__PCTOOL__`. |
| `checkwps/include/` | Shadows of `storage.h`, `usb.h` and `powermgmt.h` | Three firmware headers stop declaring things under `__PCTOOL__`; `apps-ipod/` names them unconditionally. Same shape as `apps-ipod/sim/include/`, and the shadow says which declaration it restores. |
| `checkwps/stubs.c` | New: the allocator, the status bar, the settings lookups and every settings callback | The real parser reaches for the running firmware. Each group states what its callers do with the answer, so a stub that starts lying is visible. |
| `checkwps/checkwps.c` | Includes prefixed for the `apps-ipod/` layout; a line pointing at `-v` when a skin fails with no error line | A font or bitmap failure is a `debugf` and prints nothing on its own. |
| `apps-ipod/skin/wps_internals.h` | `VP_DEFAULT_LABEL` is `NULL` under `__PCTOOL__` again | `OFFSETTYPE()` is the real pointer type there, not an offset, so a numeric sentinel does not compile. Upstream carries the same pair. |
| `apps-ipod/features.txt` | `usb_hid` gate takes `__PCTOOL__` as well as `SIMULATOR` | Same reason as the simulator: `settings_list.c` registers the setting unconditionally, so the build fails on the phrase rather than on the feature. |
| `configure` | `if [ -n \`echo $app_type \| grep "sdl"\` ]` → a real grep test | The unquoted backtick collapses to `[ -n ]`, which is always true, so `--type=c` demanded an SDL it never links. That stopped the build on any box without SDL. |

It has to be **run from inside a `.rockbox` directory** — skin font paths are
relative to the on-device layout and will not resolve from anywhere else, and
it then fails on the fonts rather than on the skin.

The database tool runs from the top level of a mounted player and writes the
database files itself, so the player does not have to scan.

### The simulator needs nothing here

The simulator was brought up without changing a single file outside
`apps-ipod/`. That is worth stating in a document about divergence, because the
obvious assumption is the opposite: `uisimulator/`, `firmware/target/hosted/sdl/`
and `tools/root.make`'s `sdl-sim` path are all upstream's, unmodified, and all
of them work.

What had to change was `apps-ipod/` believing it was on hardware — a shim
directory plus a handful of restored upstream `#ifdef`s, documented in
[`apps-ipod/sim/README.md`](../../apps-ipod/sim/README.md).

Two things a merge should know:

- **`uisimulator/common/stubs.c` includes `"screens.h"`** and uses nothing from
  it. That resolves through `apps-ipod/api/screens.h`, an empty boundary stub.
  If upstream ever makes that include meaningful, the stub has to forward to
  whichever `apps-ipod/screens/` header owns the symbol.
- **`firmware/target/hosted/sdl/window-sdl.c` includes `"misc.h"`** and then
  uses `background` from `system-sdl.h`, which upstream's `apps/misc.h` supplied
  transitively via `screen_access.h`. `apps-ipod/api/misc.h` now includes
  `system-sdl.h` under `#ifdef SIMULATOR` to keep that contract.

Both are the `api/` boundary doing its job: an upstream file includes an
application header by bare name, and the stub absorbs the fork's reorganisation.

A **Windows** simulator cross-compiles from the same tree with
`--type=as6`; that needs `mingw-w64` and an SDL2 mingw development package on
the build machine, but no repository change.

---

## Repo root

`make zip` alone produces an **incomplete** zip: no theme, no first-boot
config, no default iconset, no setting explanations, upstream's licence file
rather than this fork's, and a pile of files this fork cannot use. That is
because `buildzip.pl` is kept upstream-shaped. The three bundle scripts make up
the difference, and `build-hw.sh` runs all three.

| File | What it is | Why it exists |
| --- | --- | --- |
| `README.md`, `CLAUDE.md` | The fork's own README, and the instructions an assistant working in this tree is given | Upstream has neither. |
| `build-hw.sh` | Clean build for either target into `build-hw-<target>/`; accepts `ipod6g`/`6g` or `ipodvideo`/`5g` | Passes `--appsdir=apps-ipod` and runs all three bundle scripts after `make zip`. The supported way to produce a shippable build. |
| `build-sim.sh` | The same for the simulator, into `build-sim-<target>[-win32]/`; takes a second argument `native` or `win` | Adds the steps a simulator needs and a device does not: unpacking the finished zip into `simdisk/`, and carrying an existing `simdisk/` across the clean so a rebuild does not destroy the test music and database. `win` selects `--type=as6` — **(A)dvanced** plus `s` and `6`, because `configure` matches the build type one character at a time and plain `--type=s6` silently produces a *normal* build. |
| `bundle-theme.sh` | Injects Scrim, `default-config.cfg` and the default iconset into the zip; deletes files the build cannot use | Lives here rather than in `buildzip.pl` so that file stays upstream-shaped. `default-config.cfg` makes Scrim the first-boot default, applied before any compiled `DEFAULT_WPSNAME` fallback; it is a separate file from `config.cfg` because that one belongs to the player and an install must never overwrite it. The iconset goes in because `buildzip.pl` creates `icons/` and copies nothing into it, so `DEFAULT_ICONSET` names a file that is not on the device and every icon silently falls back to the compiled-in 6x8 blob. The theme is named in the script, not globbed, or a merge from Rockbox would start shipping stock themes nobody converted. Scrim is the only one in the build; the rest of `themes/` is published separately by `release.sh`. |
| `bundle-help.sh` | Ships `docs/podbox/settings-help.txt` as `.rockbox/docs/settings-help.txt` | **The one whose absence is silent.** Without it every **Explain** entry in a setting's context menu finds no file and shows nothing; nothing else misbehaves, so a zip built without it looks finished. |
| `bundle-licenses.sh` | Prepends `docs/podbox/LICENSES` to upstream's `docs/LICENSES` and replaces `.rockbox/docs/LICENSES.txt` | The fork imports fonts and artwork upstream does not, and their licences have to travel with the build. Same reason as the other three: `buildzip.pl` copies upstream's file and is kept upstream-shaped. |
| `docs/podbox/LICENSES` | The fork's own licence notices — Literata and Noto under the OFL, Material Design Icons under Apache 2.0 — with the SIL Open Font License 1.1 reproduced in full. It covers what the build ships, which is Scrim's fonts; a theme published on its own carries its own notices in `.rockbox/docs` | Prepended to upstream's `docs/LICENSES` by `bundle-licenses.sh`. The OFL requires its notice to travel with the font, and the device is offline, so a URL is not enough; upstream's file inlines every licence it references and these follow that. |
| `release.sh` | Builds both targets on a build server from a `git archive` of HEAD, verifies every zip, then replaces the rolling `Themes`, `Simulator` and `latest` releases in that order | Publishing by hand gets four things wrong — asset names colliding, `gh` needing `--repo` on the server, a leftover tag being reused rather than moved, and the publish order, which decides which release GitHub features on the front page. |
| `docs/CREDITS` | Three attribution blocks prepended: Themify, RockPod, and a "For RockBox:" heading before upstream's list | This fork ships themes and inherits a large body of hardware work from another fork, both GPLv2 with named authors. Upstream's list is left untouched below the heading, so a merge from Rockbox applies to it cleanly. |
| `.gitignore` | `/build*` narrowed to `/.build/`, `/build-hw-*/`, `/build-hw/`, `/build-sim-*/`, `/build-sim/`; adds `/notes/`, `/.specifications/`, `/dist/`, `/iconsources/`, `/.theme-dev/`, `/.idea/`, `/.claude/` | Local working drafts, release zips fetched back from the build server, and editor state. The `build-sim-*` glob covers the per-target simulator directories (`build-sim-ipodvideo`, `build-sim-win32`); upstream's bare `/build-sim/` matches none of them. |
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

Between them they drop several dozen entries from the zip. Count them from a
finished build rather than from a figure written here — the total moves every
time a theme or a bundled file is added.

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
