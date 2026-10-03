# Divergence from upstream Rockbox — everything outside `apps-ipod/`

Every file outside `apps-ipod/` that differs from upstream Rockbox, and why.
Read it before merging from Rockbox, or when a file here differs from upstream
and you want to know whether that was meant. The application layer has its own
document, `apps-ipod/README.md`. What was done about each upstream *commit* is
in [`upstream-commit-log.md`](upstream-commit-log.md).

To list the files, against the last upstream commit merged:

```bash
git diff --name-status $(git merge-base HEAD rockbox/master)..HEAD -- . ':(exclude)apps-ipod'
```

A file taken from an upstream commit *after* that base also shows up, and is
not a fork change. `git diff --stat HEAD rockbox/master -- <file>` prints
nothing for those.

`apps/`, `manual/`, `android/`, `backdrops/`, `screenshots/` and every
unconverted `themes/` entry are upstream-identical and unbuilt, kept so merges
apply without delete/modify conflicts. `uisimulator/` is upstream-identical and
built. The `firmware/` changes are RockPod's hardware work and this fork's USB
work; the `tools/` changes are this fork's.

---

## firmware/ — core

| File | What changed | Why |
| --- | --- | --- |
| `backlight.c` | `#include "../apps/gui/skin_engine/skin_engine.h"` → `"gui/skin_engine/skin_engine.h"` | Upstream's relative path lands in the unbuilt `apps/`. This resolves through the `apps-ipod/api/` stub. |
| `backlight.c` | Backlight off calls `storage_sleep()`; both wake paths post `Q_STORAGE_PRE_WAKE` | The SSD sleeps with the backlight, and starts waking before the UI has handled the button. Only under `storage_get_ssd_mode()`. |
| `backlight.c` | `power_input_present()` → `charger_inserted()` in `backlight_get_current_timeout()` | A data-only USB port should not get the plugged-in timeout. |
| `drivers/ata.c`, `export/ata.h` | New `ata_set_storage_mode(int)` / `ata_get_ssd_mode()` (0 auto, 1 HDD, 2 SSD). In SSD mode `ata_sleepnow()` does not arm `power_off_tick` | The 5G's generic driver powered the interface off after seven idle seconds, and waking it costs ~780 ms. See *SSD mode* below. |
| `export/storage.h`, `storage.c` | `Q_STORAGE_PRE_WAKE`, stubs and dispatch for the two calls above | Reaches SSD mode through the generic `storage_*` interface. |
| `powermgmt.c` | The charger case sets `CHARGING` unconditionally; the battery-level test keys off `charge_state > DISCHARGING` | The 6G's charge-status line oscillates on weak USB sources, which flipped the voltage curve and moved the reading several points. |
| `powermgmt.c` | New `charge_finished`, debounced over 8 samples and held until unplug; the 99 % cap needs it | Otherwise a full battery never shows 100 %. Other targets never set it, so they keep upstream's behaviour. |
| `SOURCES` | `mikey-6g.c` wrapped in `#ifdef HAVE_MIKEY_REMOTE` | So the inline remote can be switched off with one line in `export/config.h`. |
| `export/font.h` | `MAXUSERFONTS` 12 → 16 | Screens that load their own fonts share the slots with the theme. |
| `export/config.h` | `HAVE_MULTIMEDIA_KEYS` gated on `USB_ENABLE_IAP \|\| HAVE_MIKEY_REMOTE`, outside the `HAVE_USBSTACK` block | The 6G's inline remote produces the same key codes as USB iAP, without USB. |
| `export/rbpaths.h` | New `DEFAULTCONFIGFILE`, `.rockbox/default-config.cfg` | The shipped first-boot config, read only when no `config.cfg` exists, so an install never resets the player's settings. |
| `drivers/lcd-16bit-common.c`, `export/lcd.h` | New `lcd_blendrect()` and `LCD_BLEND_OPAQUE` | The skin engine's `%dr` opacity. Only useful drawn into the backdrop buffer. |
| `drivers/lcd-color-common.c`, `export/lcd.h` | New `lcd_alpha_bitmap_part_img()`; `lcd.h` declares it and `lcd_alpha_bitmap_part()` | Draws an image through an alpha mask, for rounded corners on `%dr`, `%Cl` and `%La`. |
| `drivers/lcd-16bit-common.c` | `lcd_alpha_bitmap_part_mix()`'s `DRMODE_FG` case skips fully transparent pixels | Every anti-aliased glyph gets cheaper. The local must not be named `alpha`, which `READ_ALPHA()` uses. |
| `drivers/rtc/rtc_pcf50605.c` | Alarm code wrapped in `#ifdef HAVE_RTC_ALARM` | The 5G undefines it, and this was the one RTC driver without the guard. |
| `export/system.h`, `target/arm/pp/debug-pp.c` | New `dbg_hw_info_lines()` for `IPOD_6G` and `IPOD_VIDEO`; `debug-pp.c`'s `dbg_hw_info()` becomes it | Hardware info as a themed list. `debug-pp.c` is shared by every PP target, and the others now have no `dbg_hw_info()`. |
| `common/dircache.c`, `include/dircache.h` | New `dircache_is_ready()`, `dircache_foreach_name()`, `dircache_get_index_path()` | Whole-player search from the cache. The sweep takes the filesystem lock as **reader**, so audio buffering is not held off, and `dircache_is_ready()` reads unlocked so it does not wait out a scan. |

## firmware/ — USB, device side

The player as a disk, a keyboard, a sound card or an iAP accessory's host.

### Host presence

| File | What changed | Why |
| --- | --- | --- |
| `usb.c`, `export/usb.h` | New `usb_host_is_present()`, and a `USB_NONE` stub | Background work — database scan, album index, file index — stands down while a host enumerates. `usb_inserted()` is true on a charger too; `usb_host_present` turns true on the first control transfer or `SET_ADDRESS` (both targets define `USB_DETECT_BY_REQUEST`) and never for a charger. |
| `usb.c` | `usb_set_host_present()` raises the USB thread to `PRIORITY_REALTIME` until the host goes | Upstream raises it only at `SET_CONFIGURATION`, leaving enumeration below the UI thread. |
| `usb.c` | The `SET_ADDRESS` notification sets `USB_INSERTED` and the host present | Since `841007dfa1` the driver answers `SET_ADDRESS` itself, so a host that sends it first — an Onkyo ND-S1 — never enabled the drivers. Not yet reported upstream. |
| `usbstack/usb_core.c` | `usb_core_set_address()` assigns interfaces and endpoints if the core is still `DEFAULT` | The same host otherwise got every interface numbered 0. |

### Mass storage

| File | What changed | Why |
| --- | --- | --- |
| `usbstack/usb_storage.c`, `.h`; `usbstack/usb_core.c`; `export/usb_core.h` | `host_wrote`, set by `WRITE_10`/`WRITE_16`, reached as `usb_core_host_wrote_storage()` | The app layer skips the database and dircache rebuild after a host that only read. Windows disconnects and reconnects on every connect. |
| `usbstack/usb_storage.c` | The LUN is bounds-checked; `set_transfer_range()` makes the LBA arithmetic overflow-safe; a zero `block_size_mult` is refused | Host-supplied values upstream uses unchecked. |

### Buffers claimed at boot

| File | What changed | Why |
| --- | --- | --- |
| `usbstack/usb_storage.c` | The ~128K transfer buffer is allocated by `usb_storage_alloc_buffers()` and never freed | Upstream allocates it at `SET_CONFIGURATION`, after `audio_init()`, where only shrinking the audio buffer can meet it — and that stops playback with a blocking `queue_send` mid-transfer. |
| `usbstack/usb_audio.c` | `usb_audio_alloc_buffers()` replaces `usb_audio_request_buf()` / `usb_audio_free_buf()` | The same, for the sound card. |
| `usb.c`, `export/usb_core.h`, `export/usb.h` | `usb_init()` calls both, before `audio_init()` | **Not in the drivers' `init()`**: that runs after the device is attached, so a stall lands before `SET_ADDRESS`. PP502x cannot use `USB_STATIC_ALLOC` because 128K does not fit in IRAM. |

### The sound card

| File | What changed | Why |
| --- | --- | --- |
| `usbstack/usb_audio.c` | Mixer start moved to the USB thread through `notify_event` | Starting playback in the interrupt hangs the 5G. |
| `usbstack/usb_audio.c` | The receive ring is full one slot early and always re-arms; a bad packet is dropped and the endpoint re-armed | Upstream stops receiving when the ring fills. |
| `usbstack/usb_audio.c` | The receive buffer is cache-line aligned and read through `UNCACHED_ADDR` | On the 5G a cached read plays as silence. |
| `usbstack/usb_audio.c` | An unanswered endpoint request is stalled | Upstream leaves the host waiting. |
| `usbstack/usb_core.c` | `driver_rank()` / `driver_to_leave_out()`: short of endpoints, HID goes first, then the sound card, then the disk | Upstream can leave out the disk on the 5G's four endpoints. |
| `usbstack/usb_core.c`, `export/config/ipod6g.h` | Product ID `USB_PRODUCT_ID_AUDIO` (0x1209) while the sound card is on | iTunes' driver claims the 6G's own ID and passes on only the disk. |
| `usb.c` | The audio driver is enabled whenever the setting is on, in both USB modes | The setting is off/on here. |
| `export/s5l87xx.h` | `USB_NUM_ENDPOINTS` 6 → 7 | The isochronous endpoint, shared with USB iAP. |

### USB iAP

Upstream's libiap, on both players. An Onkyo ND-S1 plays both over S/PDIF.

| File | What changed | Why |
| --- | --- | --- |
| `usb.c`, `export/usb.h` | New `usb_set_iap(bool)`; the iAP driver is enabled on `USB_INSERTED` only while it is on | The **Accessory Protocol** setting, which takes effect at the next connection. Off also gives a computer the disk whatever **iAP2 Accessories** says. |
| `usbstack/iap/platform.c`, `platform.h` | Broadcasts `SYS_ACCESSORY_CONNECTED` once the sample rates are accepted; answers libiap's four database callbacks from `iap_library.h`, and holds a play back while that library builds the Queue | The "Accessory connected" splash, and browsing the library from an accessory. |
| `usbstack/iap/libiap/iap.c`, `context.h`, `platform.h`, `spec/lingoes/extended-interface/database.h` | The database commands go to four new platform callbacks; Enter/ExitExtendedInterfaceMode are acked | Upstream answers them with fixed counts, so there is nothing to browse. Onkyo receivers retry the mode commands until acked. |
| `usbstack/usb_core.c` | Manufacturer and product strings `PodBox` and `PodBox media player` | The name a computer shows. iAP's name comes from `/.rockbox/playername.txt`, which the app layer keeps set. |
| `usbstack/usb_iap.c` | While iAP2 is offered, the stream lists an iPhone's nine rates; reports, ticks and send completions go to `usb_iap2_*` first; `iap_library_close()` on disconnect; a disconnect pauses playback only if the iAP sink had it | An iAP2 car names rates as indexes into an iPhone's list. An accessory that never took the audio, such as the connection dropped to come back without the sound card, leaves playback alone. |
| `usbstack/iap/audio.c`, `audio.h` | A rate change goes to iAP2 while it holds the connection; new `iap_audio_sampr()`; the stream's state goes to the USB log; new `iap_audio_take_counts()`, what the stream took from playback | The counters are logged every 5 s while a car takes the audio. |

### USB iAP2

A car looking for an iPhone, answered by the player as an iPod. Upstream has no
iAP2.

| File | What changed | Why |
| --- | --- | --- |
| `usbstack/usb_iap2.c`, `usb_iap2.h` (new) | iAP2's transport and link layer over the iAP configuration's HID interface, on the USB thread | **iAP2 Accessories**. |
| `usbstack/usb_iap2_control.c` (new) | The control session: identification, USB audio, power, the library and its playlists, the queue, now playing, the car's buttons with shuffle and repeat, and cover art | Covers come from the art cache, encoded by `apps-ipod/draw/jpeg_enc.c`. |
| `usbstack/usb_core.c` | Vendor request 0x53 answered as an iPhone answers it, four zero bytes, marking the host a car; under Auto the disk handover waits a second (`storage_hold()`) | A car moves on to the iAP configuration within that second and is never handed the disk; a computer stays, and is. |
| `usb.c`, `export/usb.h` | New `usb_set_iap2_mode()`; the disk withheld while iAP2 is answered; new `usb_car_found()` and the `USB_CAR_RECONNECT` event: a car found while the sound card is on makes the player leave the bus and come back without it, until unplugged | The Mazda stops with an authentication error when it finds the sound card beside the disk. |
| `usbstack/usb_iap.h` | `USB_IAP2_MODE_*`, `usb_iap_set_iap2_mode()`, `usb_iap2_offered()`, `usb_iap_answer_iap2()`, vendor request 0x53 | Off refuses the probe, On answers it and offers no disk, Auto answers once the host has sent 0x53. |

### The USB log

**Debug → USB log**. Keep it.

| File | What changed | Why |
| --- | --- | --- |
| `usbstack/usb_log.c`, `export/usb_log.h` | A 512-event ring filled from interrupts, written to `/.rockbox/usb-log.txt` while the debug screen is open or **System → USB → Write Debug Log** is on; `usb_log_sync()` waits for the file before a configuration change or stream start | A USB fault is gone by the time anything could look at it. |
| `export/usb.h`, `usb.c` | The insertion record and waypoints | **Debug → USB info**: whether a connect that did nothing was charging-only or a stuck handover. |
| `usbstack/usb_core.c` | `usb_log()` calls and waypoints in the handlers | Not on the notify path: the ARC driver calls `usb_core_bus_reset()` from its ISR and posts nothing. |
| `usbstack/iap/debug.c`, `debug.h` | New `iap_log_report()`: the iAP packet a HID report starts, to the USB log | The log decodes iAP. |

## firmware/ — USB, host side

The player driving a USB DAC. Upstream has no host stack. The player supplies
no VBUS.

| File | What changed | Why |
| --- | --- | --- |
| `usbstack/usb_host.c` (new) | Enumeration over `usb_drv_host_control()` | Root port only; no hubs. |
| `export/config.h` | `HAVE_USB_HOST` and `HAVE_USB_HOST_AUDIO` for the ARC and DesignWare controllers | Both players get the host probe and the DAC output. |
| `usbstack/usb_host_audio.c`, `export/usb_host_audio.h` (new) | USB Audio Class 1 and 2 playback as `PCM_SINK_USB_HOST`, 44.1 and 48 kHz, software volume capped at 0 dB | The DAC output. |
| `usb.c`, `export/usb.h` | The host probe, `USB_HOST_AUTO` and the DAC state calls | **USB DAC Output**. A cable still only powered after 2 s is polled for a DAC, unless a serial iAP accessory is talking, which the search would starve on the 5G. |
| `export/usb_drv.h` | The host API | Implemented by both controller drivers. |
| `export/pcm_sink.h`, `pcm.c` | `PCM_SINK_USB_HOST` registered | `PCM_SINK_NUM` is 3 on both targets. |
| `kernel/include/queue.h` | `SYS_USB_DAC_ON` / `_OFF` (plug events 8, 9) and `SYS_ACCESSORY_CONNECTED` (10) | Past upstream's plug events, which run to 7. |
| `target/arm/s5l8702/ipod6g/power-6g.c` | Taking the port as host commits 500 mA | At 100 mA the 6G would not charge while playing to a DAC. |

## firmware/ — USB controller drivers

| File | What changed | Why |
| --- | --- | --- |
| `target/arm/usb-drv-arc.c` (5G) | `USBCMD_ITC` 1 microframe | Upstream's 8 loses one sound-card packet in eight. |
| `target/arm/usb-drv-arc.c` | `prime_transfer()`'s timeout reads `USEC_TIMER` | `current_tick` stops with interrupts off. |
| `target/arm/usb-drv-arc.c` | EHCI host mode, isochronous through the transaction translator; `read_hw_info()`; `usb_log` calls | The 5G's host side. `HWHOST` reports no translator, but full-speed DACs play through one. |
| `drivers/usb-designware.c` (6G) | Host channels; NYET/NAK handling with PING; one isochronous packet per microframe from the SOF interrupt; hardware info; `usb_log` calls | The 6G's host side. A high-speed DAC is silent without a packet every microframe. |
| `export/usb-designware.h` | `FHMOD` bit | Forces host mode. |
| `target/arm/pp/usb-fw-pp502x.c`, `target/arm/s5l8702/usb-s5l8702.c` | New `usb_host_probe_enable()` | Stops device detection while the probe owns the port. |

## firmware/ — iPod Classic 6G (S5L8702)

| File | What changed | Why |
| --- | --- | --- |
| `target/arm/s5l8702/ipod6g/storage_ata-6g.c` | SSD storage mode, ~150 lines: clock-gate instead of `STANDBY IMMEDIATE`, deep sleep after 10 s more with the backlight off, eager wake on `ata_spin()` and `Q_STORAGE_PRE_WAKE`, no HDD power/noise features, ranged cache maintenance, auto-detection in `ata_init()` | Flash mods are common on this player, and upstream treats every device as a spinning disk. |
| `target/arm/s5l8702/ipod6g/power-6g.c` | Charger classification, ~100 lines: with the backlight off, charging is disabled unless a charger is confirmed or 500 mA is committed; a 10 ms watch on the charge-status line with the backlight on; an asymmetric 8-sample debounce | Upstream calls any USB insertion a charger, and a source that cannot supply device + charge current oscillates. A configured computer, **USB Charging = Force** and the DAC host all commit 500 mA. |
| `target/arm/s5l8702/system-s5l8702.c`, `system-target.h` | A 108 MHz level between boost and unboost, selected by `set_ahb_boost(bool)` | For AHB-bound work. Nothing calls it yet. |
| `drivers/audio/cs42l55.c`, `export/cs42l55.h` | `audiohw_set_hp_power()`, `audiohw_idle_powerdown()`, `audiohw_idle_powerup()` | Idle codec power-down, muted first to avoid a pop. `HPACTL`/`HPBCTL` are left alone. |
| `target/arm/s5l8702/ipod6g/cscodec-6g.c` | `cscodec_power()` switches LDO 3 | Upstream is a stub. |
| `target/arm/s5l8702/ipod6g/mikey-6g.c` | `mikey_init()` returns early on `rec_hw_ver == 0` | The 80GB and fat 160GB have no Mikey to poll. |
| `target/arm/s5l8702/ipod6g/mikey-6g.c`, `mikey-target.h` | New `mikey_probe()` | Returns the I2C status, so the debug screen can tell an empty jack from a missing chip. |
| `target/arm/s5l8702/ipod6g/mikey-6g.c`, `export/button.h` | Centre clicks counted over 360 ms: two are next, three previous. `mikey_set_track_skip()` and `mikey_supported()` | Upstream's remote has no next or previous. Counting delays play/pause, so **Remote Track Skip** can turn it off. |
| `target/arm/s5l8702/debug-s5l8702.c` | `dbg_hw_info()` becomes `dbg_hw_info_lines()`, the lines of a list; adds the LTC4066 charger pins and a Mikey line: `jack=`, `hw=`, `probe rc=`, `r0=` | Hardware info keeps the theme. Mikey `rc` 1 means no Mikey answered; 0 means it did. |

### SSD mode: one setting, two mechanisms

| | `drivers/ata.c` (5G) | `storage_ata-6g.c` (6G) |
| --- | --- | --- |
| What SSD mode changes | The interface is never powered off | The first sleep stage gates the clock instead of `STANDBY IMMEDIATE` |
| Power-off still happens? | No | Yes, 10 s later with the backlight off |
| Wake cost hidden by | Nothing to hide | `Q_STORAGE_PRE_WAKE` |

`backlight.c` posts `Q_STORAGE_PRE_WAKE` on both targets, and only the 6G
handles it. Give the 5G driver a power-off path and its ~780 ms wake returns,
unhidden.

## firmware/ — config headers

A feature this fork declines, whose wiring is in files this fork never edits,
merges in **enabled**. Switch it off here and it compiles to nothing. Every
other decline sits in a file this fork edits, so it conflicts on merge.

| File | What changed | Why |
| --- | --- | --- |
| `export/config/ipod6g.h` | `HAVE_RECORDING` commented out | No recording UI ships. |
| `export/config/ipod6g.h` | `PLUGIN_BUFFER_SIZE` 2 MiB → 3 MiB | The core scratch buffer (`apps-ipod/system/app_buffer.c`) keeps the name. |
| `export/config/ipod6g.h` | `ROCKBOX_HAS_LOGF` for non-bootloader builds | Upstream defines it only in the bootloader block. Raising `MAX_LOGF_SIZE` costs that much `.bss`. |
| `export/config/ipod6g.h` | `TARGET_EXTRA_THREADS` 2 with both `IPOD_ACCESSORY_PROTOCOL` and `HAVE_MIKEY_REMOTE`, else 1 | Upstream adds the remote's poller without a thread for it. Short by one, the feature is silently absent. `__threads` should measure 19. |
| `export/config/ipod6g.h` | `HAVE_COMPOSITE_VIDEO_OUT` commented out | Nothing outputs video, and the driver reserves 112.5 KB. `serial-6g.c` and `power-6g.c` test the define. |
| `export/config/ipodvideo.h` | `HAVE_RECORDING` commented out | As above. |
| `export/config/ipodvideo.h` | `CONFIG_TUNER`, `HAVE_RDS_CAP`, `CONFIG_RDS` commented out | No FM accessory. Matches `ipod6g.h`. |
| `export/config/ipodvideo.h` | `HAVE_RTC_ALARM` commented out | The Apple bootloader clears the alarm flag, so a wake is guessed and usually missed. The apps-side alarm is removed. |

## firmware/ — serial iAP and remote buttons

Serial iAP is `apps-ipod/iap/`. It reaches two firmware headers:

| File | What changed | Why |
| --- | --- | --- |
| `export/iap.h` | New `iap_enable(bool)` and `iap_accessory_present()` | The serial half of **Accessory Protocol**, and whether frames are arriving, which the DAC search checks. |
| `target/arm/ipod/button-target.h`, `target/arm/s5l8702/ipod6g/button-target.h` | `BUTTON_REMOTE` includes `BUTTON_RC_MENU`; new `BUTTON_RC_NEXT_ALBUM`, `_PREV_ALBUM`, `_NEXT_PLAYLIST`, `_PREV_PLAYLIST` (0x4000–0x800) | Upstream lists `BUTTON_RC_PLAY` twice and omits Menu. The new codes carry the album and playlist buttons an Onkyo DS-A3 remote sends. |

---

## lib/

| File | What changed | Why |
| --- | --- | --- |
| `rbcodec/metadata/mp4.c`, `metadata.h` | `has_video` on `struct mp3entry` | Tagcache skips music videos. |
| `skin_parser/tag_table.c` | `find_custom_tag()` declared **weak** and tried **first** in `find_tag()` | Custom tags are registered from the app layer. First, because upstream's shortest-match search would read `%sel` as `%s`. Weak, so the parser links standalone. |
| `skin_parser/tag_table.h` | Six new `SKIN_TOKEN_*` members | The token field is a 1-byte enum; the table rows stay in `custom_tags.c`. |
| `skin_parser/tag_table.h` | `SKIN_REFRESH_SPECTRUM`, in `SKIN_REFRESH_NON_STATIC` | Spectrum lines redraw with time. |

---

## tools/

### The `--appsdir` wiring

**A configure without `--appsdir=apps-ipod` builds `apps/`**, successfully,
and produces the wrong firmware.

| File | What changed | Why |
| --- | --- | --- |
| `configure` | `--appsdir=DIR`, checked to exist | The entry point. |
| `configure` | `coreappsdir`, exported as `COREAPPSDIR` | Always the application layer; `appsdir` is what the current build type compiles. |
| `configure` | `picklang()` scans `${coreapps}/lang/` | |
| `root.make` | `APPSBUILDDIR` for the bitmaps include and `voice` | Objects mirror their source path. |
| `mkinfo.pl` | `VOICE_VERSION` from `COREAPPSDIR`; a core build detected by comparing `APPSDIR` with it | Otherwise `rockbox-info.txt` loses its size, RAM and feature lines. |
| `buildzip.pl` | `$APPSDIR` for `tagnavi.config` and `lang/Invalid*.talk` | Kept as close to upstream as possible; fork packaging goes in the `bundle-*.sh` scripts. |
| `buildzip.pl` | `$APPSBUILDDIR` for `lang/*.lng` and `lang/*.zip` | Build products, not sources. With upstream's path every language was dropped from the zip. |
| `buildzip.pl` | Upstream's `.map` block replaced by a comment | ~4 MB on the player at every sync. `release.sh` attaches the maps to the release; the comment makes a merge conflict here. |
| `voice.pl`, `langstatus` | The application directory from `COREAPPSDIR`, falling back to whichever of `apps-ipod/` or `apps/` exists | `make voice` has never run here: no TTS engine on the build server. |

### New tools

| File | What it is |
| --- | --- |
| `convfnt.c` | Exports a glyph from a 4 bpp `.fnt` to a greyscale `.bmp` and back. `convbdf` cannot round-trip 4 bpp. |
| `art_fetch/` | Fetches album and artist artwork. Run by hand. |
| `eq_refit/` | Re-fits an EQ preset onto fewer bands; each band costs a pass over every sample. |
| `pfgeom/` | Mirrors the carousel's projection and cull on the host. Change it with `screens/covers/carousel.c`, or it proves nothing. |
| `dbfeat/` | Runs `apps-ipod/database/db_featured_parse.c`, unmodified, over a table of tags and the guests each must yield. |
| `check-settings-docs.sh` | Whether `settings-help.txt` and `settings-guide.md` match the settings. Silence means they do. |
| `soundscan/` | Runs the player's sound analysis over a music folder on a computer. Ships as `.rockbox/tools/soundscan.exe`; compiles the app layer's analysis sources. |
| `spun_testlog.pl` | Synthesises playback logs, and the expected parse, for Spun. |

### CheckWPS

It links this fork's skin parser, because upstream's rejects `!rrggbb` and
crashes on a `%dr` with opacity. (W)arble is the one build type that does not
build.

| File | What changed | Why |
| --- | --- | --- |
| `checkwps/SOURCES` | Application files from `apps-ipod/`, plus `stubs.c` | |
| `checkwps/checkwps.make` | Include path around `$(COREAPPSDIR)/api` and `$(COREAPPSDIR)`; `-DSYSFONT_HEIGHT=8` | Mirrors `apps-ipod/apps.make`. |
| `checkwps/include/` | Shadows of `storage.h`, `usb.h`, `powermgmt.h` | They declare nothing under `__PCTOOL__`. |
| `checkwps/stubs.c` | The allocator, status bar, settings lookups and every settings callback | A new setting whose callback is missing here fails the link. |
| `checkwps/checkwps.h`, `checkwps.c`, `README` | `--viewports`; a pointer to `-v` when a skin fails with no error line | A missing font or bitmap prints nothing without `-v`. |
| `apps-ipod/skin/wps_internals.h` | `VP_DEFAULT_LABEL` is `NULL` under `__PCTOOL__` | As upstream has it. |
| `apps-ipod/features.txt` | `usb_hid` also under `__PCTOOL__` | `settings_list.c` registers it unconditionally. |
| `configure` | The SDL check is a real test | It was always true, so `--type=c` demanded SDL. |

### The simulator

Nothing outside `apps-ipod/` changed for it; see `apps-ipod/sim/README.md`. Two
upstream files reach the app layer by bare include name, through `api/` stubs:

- `uisimulator/common/stubs.c` includes `"screens.h"`, an empty stub. If
  upstream starts using it, forward the stub to the `apps-ipod/screens/`
  header that owns the symbol.
- `firmware/target/hosted/sdl/window-sdl.c` uses `background` via `"misc.h"`,
  so `apps-ipod/api/misc.h` includes `system-sdl.h` under `SIMULATOR`.

---

## Repo root

| File | What it is |
| --- | --- |
| `README.md`, `CLAUDE.md` | The fork's README, and an assistant's instructions. |
| `build-hw.sh` | Clean build for `ipod6g`/`6g` or `ipodvideo`/`5g`, with `--appsdir` and all four bundle scripts. |
| `build-sim.sh` | The same for the simulator, unpacked into `simdisk/`, which it keeps across the clean. `win` uses `--type=as6`; plain `s6` gives a normal build. |
| `bundle-theme.sh` | Adds Scrim, `default-config.cfg` and the default iconset to the zip. Deletes `classic_statusbar` (the directory and the loose `.sbs`/`.rsbs` beside it) and the plugin data `buildzip.pl` copies from `apps/plugins/`. The theme is named, not globbed, so a merge cannot start shipping stock themes. |
| `bundle-help.sh` | Ships `settings-help.txt`. Without it every **Explain** is empty and nothing else looks wrong. |
| `bundle-trim.sh` | Ships `trim.config`, the whole of what **Trim Titles** trims. |
| `bundle-tools.sh` | Adds `soundscan.exe` and the host-built codecs it loads, as `.rockbox/tools/`. A missing tool is reported, not fatal; `release.sh` requires it. |
| `release.sh` | Builds both targets on the build server, verifies the zips, then replaces the `Themes`, `Simulator` and `latest` releases, in that order. |
| `docs/CREDITS` | PodBox, RockPod and Spun blocks above `For RockBox:`. Upstream's list below is untouched, trailing newline included, so merges apply. |
| `.gitignore` | `/build*` narrowed to the build directories; local drafts, `dist/` and editor state added. |
| `wps/WPSLIST` | `cabbiev2` block removed, so it does not ship. `rockbox_failsafe` stays: it is the parse-failure fallback. |

---

## Deliberately *not* changed

| Not changed | Why |
| --- | --- |
| `apps/` | Upstream mirror. Do not edit it. |
| RockPod's source mode in `usbstack/usb_audio.c` | The source here is USB iAP's configuration 2, on both players. |
| `usbstack/iap/libiap/` transport | Upstream's USB transport; RockPod's `usb_iap_hid.c` is absent. Only the database handlers differ, above. |
| RockPod's isochronous plumbing in `drivers/usb-designware.c` | Serves its source mode. |
| `target/arm/s5l8702/pcm-s5l8702.c` | RockPod's I2S clock gating is **deferred**. Upstream's `pcm_sink` calls nest, so a naive port gates the clock on every buffer. |
