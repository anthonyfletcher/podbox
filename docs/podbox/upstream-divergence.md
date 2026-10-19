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
apply without delete/modify conflicts. `uisimulator/` is built, and
upstream-identical apart from the `--screenshot` argument (see *The
simulator*). The `firmware/` changes are mostly RockPod's hardware work and this
fork's USB work; the `tools/` changes are this fork's.

---

## bootloader/

| File | What changed | Why |
| --- | --- | --- |
| `ipod.c` | The hold switch no longer boots Apple's firmware; only MENU does | Upstream boots Apple's firmware whenever the player starts locked, so a lock left on through a reset or a dead battery starts the wrong OS. |
| `ipod-s5l87xx.c` | The hold switch no longer boots Apple's firmware. MENU at t2 no longer launches it at once: it takes the path the hold switch did, after storage is up. Menu held with Left does the same; Left kept held through disk spin-up overrides the LBA48 refusal, and the refusal screen says so. An unreadable SysCfg counts as no LBA48 support. Before the handover the drive is put in HDD storage mode, so `ata_sleepnow()` powers an SSD off rather than clock-gating it | That path holds the LBA48 check. Upstream's early MENU launch skips it, and starts an OF without LBA48 on a disk past 2^28 sectors. Shared with the Nano 3G/4G, which this fork does not build. |

---

## firmware/ — core

| File | What changed | Why |
| --- | --- | --- |
| `backlight.c` | `#include "../apps/gui/skin_engine/skin_engine.h"` → `"gui/skin_engine/skin_engine.h"` | Upstream's relative path lands in the unbuilt `apps/`. This resolves through the `apps-ipod/api/` stub. |
| `backlight.c` | Backlight off calls `storage_sleep()`; both wake paths post `Q_STORAGE_PRE_WAKE` | The SSD sleeps with the backlight, and starts waking before the UI has handled the button. Only under `storage_get_ssd_mode()`. |
| `backlight.c` | The fade registers the user timer at priority 1, not 0 | Above the 5G's wheel click (`piezo.c`), which gives the timer up instead of switching the backlight straight on or off. |
| `backlight.c` | `power_input_present()` → `charger_inserted()` in `backlight_get_current_timeout()` | A data-only USB port should not get the plugged-in timeout. |
| `drivers/ata.c`, `export/ata.h` | New `ata_set_storage_mode(int)` / `ata_get_ssd_mode()` (0 auto, 1 HDD, 2 SSD). In SSD mode the interface is powered off only while the backlight is off, 10 s or more after it sleeps, and `Q_STORAGE_PRE_WAKE` powers it back up, or puts a power-off not yet made back 10 s | Waking it from off costs ~780 ms, which browsing must not pay, but a flash adapter's standby current is a real share of a 5G's playback draw. See *SSD mode* below. |
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
| `common/dircache.c`, `include/dircache.h` | New `dircache_is_ready()`, `dircache_foreach_name()`, `dircache_get_index_path()` | Whole-player search from the cache. The sweep takes the filesystem lock as **reader**, so audio buffering is not held off, and `dircache_is_ready()` reads unlocked so it does not wait out a scan. `dircache_get_index_path()` returns -1 for an out-of-range or freed index, which a caller holding one across a rebuild can have. |
| `target/arm/ipod/video/lcd-video.c` | `lcd_sleep()` calls `cpu_idle_mode(true)`, `lcd_awake()` `cpu_idle_mode(false)` | With the LCD asleep, the 5G runs unboosted at 24 MHz with the PLL off rather than 30 MHz on it. Audio is clocked from the 24 MHz oscillator, so playback is unaffected; a boost relocks the PLL first. |
| `target/arm/pp/ata-pp5020.c` | A DMA transfer boosts while the CPU is below `CPUFREQ_NORMAL` | The UDMA mode is chosen at build time for 30 MHz (UDMA 2, which upstream rates stable at 30 and not at 24), and the 5G idles at 24 MHz while its LCD sleeps. |
| `system.c` | `cpu_idle_mode()` counts requests rather than setting a flag | USB mode and the 5G's sleeping LCD both ask for idle, and with a flag whichever spoke last won: waking the screen in USB mode, or leaving USB with the screen off, ran at 30 MHz until the next sleep. |
| `target/arm/ipod/piezo.c` | A timed click (`Q_PIEZO_BEEP_FOR_USEC`) is ended by the user timer (`timer_register()`); the old busy-wait stays as the fallback when the timer is taken. Registered at priority 0, with an unregister callback that ends the click | The 5G's wheel click spun the real-time piezo thread for 4 ms a click, which nothing else could use while scrolling. The 6G's driver already ends its click from a timer interrupt. A click must not take the timer from a backlight fade. |
| `pcm.c` | `pcm_switch_sink()` locks both the old and the new sink across the swap, the frequency change and the stop/restart | Neither sink's interrupt takes a mixer buffer mid-switch that never plays; the fork's USB host sink made the window reachable. Upstream bug, not yet reported upstream. |
| `buflib_mempool.c` | `buflib_allocatable()` subtracts the block header from a hole, as `free_space_at_end()` already does at the tail | A request for exactly the size reported never fitted a hole, so `core_alloc_maximum()` failed whenever the largest free space sat below an unmovable block -- the audio buffer's OOM panic after a theme load, with the pinned skin buffer at the top of the pool. Upstream bug, not yet reported upstream. |
| `export/events.h`, `pcm_mixer.c`, `export/pcm_mixer.h` | New `SYS_EVENT_OUTPUT_GONE` and `_BACK`, data an `enum output_kind` (DAC, accessory). `mixer_switch_sink()` sends GONE before a switch from an external sink back to the built-in one and BACK after a switch to an external one; new `mixer_switch_sink_quiet()` sends neither | **Pause on Unplug** for every output: one policy in `apps-ipod/system/output_pause.c` pauses when the output in use goes, before the sound reaches the jack, and resumes when it comes back. |
| `usbstack/iap/audio.c`, `usbstack/usb_iap.c`, `usbstack/usb_iap2_control.c` | The rate-list change switches away and back with `mixer_switch_sink_quiet()`; a dock's or car's own `audio_pause()` on leaving goes, as the switch back sends GONE; an iAP1 accessory that never took the sink sends GONE itself | The departure follows **Pause on Unplug** like every other output, rather than always pausing. |
| `kernel/include/tick.h` | `MAX_NUM_TICK_TASKS` 16, from 8 | Seven tick tasks are resident on either player once **Debug > Buffers and CPU** has been opened, the hang catcher among them; a USB DAC's stream and a volume fade make nine, and a full table panics. |
| `kernel/include/timeout.h` | `MAX_NUM_TIMEOUTS` 24, from 8 | About 15 timeouts can be registered at once with the fork's USB, iAP, DAC search and log timeouts, and a full table drops a registration silently, so that timeout never fires. |
| `events.c` | `MAX_SYS_EVENTS` 48, from 28 | The fork's resident handlers (debug logs, battery log, palette prefetch), queued storage-idle callbacks and the four the theme turns on when leaving Now Playing filled the table, and a full table panics. |
| `kernel/hang_catch.c`, `export/hang_catch.h` (new); `SOURCES` | A tick task that, when a timed wait is five seconds overdue, sends the thread holding the core to a *HANG* screen: its name, the interrupted pc, lr and sp, a backtrace, and the return addresses on its stack, then the clock and disk left as a panic leaves them. Started at the end of boot; `powermgmt.c` stops it before powering off or rebooting, whose last step spins | A thread that never yields freezes a cooperative player with no panic. The screen is drawn by that thread, not the tick, as the 6G's LCD update cannot finish inside an interrupt. A hang with interrupts off is not caught. |

## firmware/ — USB, device side

The player as a disk, a keyboard, a sound card or an iAP accessory's host.

### Host presence

| File | What changed | Why |
| --- | --- | --- |
| `usb.c`, `export/usb.h` | New `usb_host_is_present()` under `USB_FULL_INIT`, and a `USB_NONE` stub | Background work — database scan, album index, file index — stands down while a host enumerates. `usb_inserted()` is true on a charger too; `usb_host_present` turns true on the first control transfer or `SET_ADDRESS` (both targets define `USB_DETECT_BY_REQUEST`) and never for a charger. |
| `usb.c` | `usb_set_host_present()` raises the USB thread to `PRIORITY_REALTIME` until the host goes | Upstream raises it only at `SET_CONFIGURATION`, leaving enumeration below the UI thread. |

### Mass storage

| File | What changed | Why |
| --- | --- | --- |
| `usbstack/usb_storage.c`, `.h`; `usbstack/usb_core.c`; `export/usb_core.h` | `host_wrote`, set by `WRITE_10`/`WRITE_16`, reached as `usb_core_host_wrote_storage()` | The app layer skips the database and dircache rebuild after a host that only read. Windows disconnects and reconnects on every connect. |
| `usbstack/usb_storage.c` | `host_wrote` is cleared once per cable insertion, not per `SET_CONFIGURATION` | A host that re-enumerates after writing the firmware still gets the reboot prompt at unplug. |
| `usbstack/usb_storage.c` | The LUN is bounds-checked; `set_transfer_range()` makes the LBA arithmetic overflow-safe; a zero `block_size_mult` is refused | Host-supplied values upstream uses unchecked. |
| `usbstack/usb_storage.c`, `usb.c`, `export/usb.h` | Ejecting every drive gives the disk back to the player (`usb_storage_ejected()`) until unplug; the drive then answers "medium not present" without waiting for storage, and `usb_host_is_present()` turns false | The player is usable while it charges from the computer, as Apple's firmware is. Upstream records the eject and keeps the disk until the cable is pulled. |
| `usb.c`, `export/usb.h`, `usbstack/usb_core.c` | **Drive Only**, a third USB Mode (`USB_MODE_DRIVE_ONLY`, ADB's value, on targets without ADB): `usb_drive_only()` turns off every interface but mass storage and `offered_configs()` reports one configuration | A car that reads Apple's disk mode and stock Rockbox as a drive takes a player with an accessory configuration for a phone. |
| `usb.c`, `export/usb.h`, `usbstack/usb_core.c`, `export/usb_core.h` | New `usb_host_wants_storage()`: a host is present and has asked for storage or not settled on a configuration (none selected, or the iAP2 storage hold still running); new `usb_core_storage_pending()` behind it | Background work stands down on this, not `usb_host_is_present()`, which a car or dock keeps true all session -- a player booted plugged into a car would otherwise never load the database into RAM. |

### Buffers claimed at boot

| File | What changed | Why |
| --- | --- | --- |
| `usbstack/usb_storage.c` | The ~128K transfer buffer is allocated by `usb_storage_alloc_buffers()` and never freed, so `usb_storage_init_connection()`, run at `SET_CONFIGURATION`, primes the endpoint at once | Upstream allocates it at the storage handover and frees it at unplug, after `audio_init()`, where only shrinking the audio buffer can meet it. |
| `usbstack/usb_audio.c` | `usb_audio_alloc_buffers()` replaces `usb_audio_request_buf()` / `usb_audio_free_buf()` | The same, for the sound card. |
| `usb.c`, `export/usb_core.h`, `export/usb.h` | `usb_init()` calls both, before `audio_init()` | **Not in the drivers' `init()`**: that runs after the device is attached, so a stall lands before `SET_ADDRESS`. PP502x cannot use `USB_STATIC_ALLOC` because 128K does not fit in IRAM. |

### The sound card

| File | What changed | Why |
| --- | --- | --- |
| `usbstack/usb_audio.c` | Mixer start moved to the USB thread through `notify_event` | Starting playback in the interrupt hangs the 5G. |
| `usbstack/usb_audio.c` | The receive ring is full one slot early and always re-arms; a bad packet is dropped and the endpoint re-armed | Upstream stops receiving when the ring fills. |
| `usbstack/usb_audio.c` | The receive buffer is cache-line aligned and read through `UNCACHED_ADDR` | On the 5G a cached read plays as silence. |
| `usbstack/usb_audio.c` | An unanswered endpoint request is stalled | Upstream leaves the host waiting. |
| `usbstack/usb_audio.c` | `start_mixer()` powers the CS42L55 up (6G) | `pcmbuf_play_stop()` powers the codec down, and a USB connect stops playback, so the sound card otherwise plays into a powered-down codec. |
| `usbstack/usb_core.c` | `driver_rank()` / `driver_to_leave_out()`: short of endpoints, HID goes first, then the sound card, then the disk | Upstream can leave out the disk on the 5G's four endpoints. |
| `usbstack/usb_core.c`, `export/config/ipod6g.h` | Product ID `USB_PRODUCT_ID_AUDIO` (0x1209) while the sound card is on | iTunes' driver claims the 6G's own ID and passes on only the disk. |
| `usb.c` | The audio driver is enabled whenever the setting is on, in both USB modes | The setting is off/on here. |
| `export/s5l87xx.h` | `USB_NUM_ENDPOINTS` 6 → 7 | The isochronous endpoint, shared with USB iAP. |

### USB iAP

Upstream's libiap, on both players. An Onkyo ND-S1 plays both over S/PDIF.

| File | What changed | Why |
| --- | --- | --- |
| `usb.c`, `export/usb.h` | New `usb_set_iap(bool)`; the iAP driver is enabled on `USB_INSERTED` only while it is on, and in charge-only (`USB_POWERED`) too (Gerrit 7940, open upstream); a host leaving ends a bus reset's hold on the iAP sink; a car's reconnect in charge-only sets up charge-only's drivers again; a USB extract resets the serial session with `iap_power_reset()` | The **Accessory Protocol** setting, which takes effect at the next connection. Off also gives a computer the disk whatever **Support iPhone Accessories** says. Charge only keeps the disk from a computer, and upstream's also turned docks away. A serial dock that loses its 5 V may still be seated, so losing USB power does not pause it. |
| `usb.c`, `export/usb.h`, `usbstack/usb_core.c`; `usbstack/iap/libiap/iap.c`, `platform.h`; `usbstack/iap/platform.c` | **Identify As**: new `usb_set_identity()` and `usb_identity()`, the USB product ID the device descriptor reports while the iAP driver is on (the sound card's own ID still wins) and the model ReturnIPodModelNum sends, now through a new `iap_platform_get_ipod_model_num()` with the model ID before the string | Some cars judge what a player can do by its model, and the 5G's ID is the iPod Video's, which had no USB audio. Upstream's libiap sends the model string without its ID. |
| `usbstack/iap/libiap/iap.c` | An accessory identifying with no authentication and the Digital Audio lingo is authenticated anyway if it has a device ID, as ipod-gadget does, and otherwise asked for its rates at once | The rates are asked for only once authenticated, so upstream never moves its audio to one that does not authenticate. |
| `usbstack/usb_charging_only.c` | The interface's class is application-specific, not vendor-specific | Linux takes the first configuration whose first interface is not vendor-specific, which with charge only offering iAP too was the iAP one, holding the CPU boost and binding audio and HID drivers for the whole charge. |
| `usbstack/iap/platform.c` | `iap_platform_get_indexed_track_info()` keeps its `mp3entry` static | 3 KB off the USB thread's stack, which a metadata parse there already strains. |
| `usbstack/iap/platform.c`, `platform.h` | Broadcasts `SYS_ACCESSORY_CONNECTED` once the sample rates are accepted, and announces the rate then (`iap_audio_connected()`); answers libiap's four database callbacks from `iap_library.h`, and holds a play back while that library builds the Queue; a Play from stopped is answered once playback starts on either sink, or refused after 5 s (`iap_platform_control_waits()`); refuses a shuffle change while the Music Quiz has the playlist (`iap_library_queue_lent()`) | The "Accessory connected" splash, and browsing the library from an accessory. A dock with no library browsing, such as a Zeppelin Mini, sends no button until the rate is announced, and upstream announces it only when playback starts. Upstream answers a Play from stopped only when the iAP sink starts, so an accessory on the line out never heard back and libiap's tick stood still. |
| `usbstack/iap/libiap/iap.c`, `context.h`, `platform.h`, `spec/lingoes/extended-interface/database.h` | The database commands go to four new platform callbacks; Enter/ExitExtendedInterfaceMode are acked | Upstream answers them with fixed counts, so there is nothing to browse. Onkyo receivers retry the mode commands until acked. |
| `usbstack/iap/libiap/iap.c`; `usbstack/iap/debug.c` | During IDPS a second StartIDPS is acked, an EndIDPS abandon is answered *aborted* and returns to identification, and an unknown command is refused. IdentifyDeviceLingoes carries no transaction ID even after IDPS, and ends their use: it and the packets after it are read, and logged, without one | Upstream answers an unknown IDPS command with an empty RequestIdentify, which restarts the accessory, and fails the abandon. A car that sends StartIDPS twice never got past identification. |
| `usbstack/iap/libiap/iap.c`, `fid-token-values.c`, `spec/lingoes/general/identify-device-lingoes.h` | The identify options are compared under a new `AuthMask`, bits 1:0 | Bits 3:2 are the accessory's power requirement. Upstream compares the whole word, so a dock asking for authentication and power is acked and never authenticated. |
| `usbstack/iap/libiap/iap.c`, `context.h` | Authentication 1.0: a version-only certificate reply is accepted and answered with a 16-byte challenge; any version but 1 or 2 is answered *unsupported* | Upstream reads every reply as 2.0, so a 1.0 accessory is refused and authentication stops. |
| `usbstack/iap/libiap/iap.c` | Display Remote `SetIPodStateInfo` handles play status: Stopped, Playing and Paused go to the platform's Stop, Play and Pause, and the three seek values to its new StartFF, StartRew and EndFFRew | Upstream answers every type *bad parameter*. A car that sends Playing every two seconds while the player plays was refused every time. |
| `usbstack/iap/libiap/iap.c`, `context.h`, `iap.h`, `platform.h`; `usbstack/iap/platform.c`, `platform.h`, `usbstack/usb_iap.c` | Simple Remote Context and Audio buttons keep separate states (new `iap_remote_buttons_tick()`, called every 100ms, lets go 200ms after a button's last report); a held Next or Previous scans after two seconds, the scan bits at once; PlayControl StartFF, StartRew and EndFFRew seek, through the app layer's `remote_seek` (new `iap_platform_get_time_ms()`, and `iap_platform_seek_tick()` and `iap_platform_seek_stop()` for the transport); Previous two seconds into a track restarts it; reserved PlayControl codes are refused (Gerrit 7906, open upstream, except that album and playlist buttons still skip a track) | Upstream acks the seek commands and does nothing, ignores AudioButtonStatus, and holds a button whose release is lost. |
| `usbstack/iap/libiap/iap.c`, `platform.h`; `usbstack/iap/platform.c` | The chapter commands answer from the playing track's cue sheet, through new `iap_platform_get_chapter()` and `iap_platform_set_chapter()`; chapter-aware Next and Previous and the chapter buttons step chapters, else tracks (new controls NextChapter, PrevChapter); SetAudiobookSpeed accepts normal speed; RequestProtocolVersion, RequestIPodName, GetMonoDisplayImageLimits and ResetDBSelectionHierarchy are answered | Upstream answers "no chapters" and refuses the rest. |
| `usbstack/iap/platform.c`; `usbstack/iap/libiap/iap.c`, `spec/lingoes/display-remote/indexed-playing-track-info.h` | A track's art, and whether it has any, come from the art cache through `iap_artwork.h`, for any track index, with the playback engine's slot as the fallback for the playing track, the playing track's answer kept until the track or **Accessory Artwork** changes; RetArtworkFormats sends one format, ID 0x0404 (RockPod's), which the artwork count names too; a transfer that fails ends, so the next request can start another | A car offered format 0 never asks for a cover. Upstream's slot holds art only for tracks buffered after the accessory connected, so a car that connects to a playing player is told no track has art and never asks; its RetArtworkFormats sends a second, uninitialised record. The play status carries the answer and cars poll it about once a second, so a file opened each time keeps the disk awake. Upstream leaves a failed transfer open, and refuses every later request until the cable is pulled. |
| `usbstack/iap/libiap/iap.c`, `fid-token-values.c` | Deferred authentication, in IdentifyDeviceLingoes or an IDPS Identify token, authenticates at once | Upstream refuses it, so such an accessory never connects. |
| `usbstack/iap/libiap/iap.c`, `platform.h`; `usbstack/iap/platform.c` | `GetIPodPreferences` for line-out usage (class 0x03) answers from the **Line Out** setting, through a new `iap_platform_get_lineout_usage()` | Upstream answers 0, *not used*, for every class, so an accessory that asks is told the line output is off. |
| `usbstack/iap/libiap/hid.c`, `iap.h`; `usbstack/usb_iap.c` | A HID report that fails to go out, or completes with an error, drops the rest of its frame and clears the sender (new `iap_notify_send_failed()`); a completion after the disconnect is ignored; `SET_REPORT` is acked whatever libiap makes of the report | Upstream leaves the sender busy after a failed report, so libiap sends nothing more until the cable is pulled, and a completion after the disconnect reached freed buffers. A STALL tells the accessory the transfer failed, not the frame. |
| `usbstack/usb_core.c`, `export/usb_core.h` | New `usb_core_bus_resetting()`; a configuration other than the iAP one ends a bus reset's hold on the iAP sink (Gerrit 7900, adapted) | A bus reset closes the iAP driver as an unplug does, and upstream pauses the dock's track. |
| `usbstack/usb_core.c` | Manufacturer and product strings `PodBox` and `PodBox media player` | The name a computer shows. iAP's name comes from `/.rockbox/library/user/player_name.txt`, which the app layer keeps set. |
| `usbstack/usb_iap.c` | While iAP2 is offered, the stream lists an iPhone's nine rates; reports, ticks and send completions go to `usb_iap2_*` first; libiap's tick calls `iap_audio_tick()`; claims the iAP library on connect and lets its claim go on disconnect (`iap_library_claim`/`iap_library_close(IAP_LIBRARY_USB)`), since the dock's serial line may still be using the lists; a disconnect pauses playback only if the iAP sink had it; it stops the stream first, and a bus reset keeps an iAP1 dock's sink with the track waiting, until the dock's rates return (`usb_iap_end_reset_hold()`, Gerrit 7900, adapted); new `usb_iap2_tick_soon()`, a one-shot tick for iAP2's delayed AuthenticationSucceeded and IdentificationAccepted, cancelled on disconnect | An iAP2 car names rates as indexes into an iPhone's list. An accessory that never took the audio, such as the connection dropped to come back without the sound card, leaves playback alone. |
| `usbstack/iap/audio.c`, `audio.h` | A rate change goes to iAP2 while it holds the connection; new `iap_audio_sampr()`; the stream's state goes to the USB log; new `iap_audio_take_counts()`, what the stream took from playback; `TrackNewAudioAttributes` at every track start, not only at a rate change; the sink's lock sends silence instead of entering the mixer, and stop clears the staged data (Gerrit 7938, open upstream); a new rate plays only once the accessory acks its `TrackNewAudioAttributes`, or after a second, an ack owed for a second is written off, and no rate is announced before the accessory's rates arrive (new `iap_audio_attrs_ack()`, through a new libiap callback `iap_platform_on_audio_attrs_ack()`; Gerrit 7900, adapted); an accessory that sets no rate on the stream within half a second of an announcement plays the announced one; one that never lists its rates but sets one on the stream plays that, once it has sent nothing for two seconds outside authentication (`usb_iap.c` calls a new `iap_audio_accessory_heard()` for every report), or, if it keeps talking, four seconds after the rate or the end of authentication, whichever is later; an accessory listing any one of 48, 44.1 and 32 kHz is accepted, and the sink then offers only the rates it listed, the list logged with the decision (new `iap_audio_accessory_rates()`, called from `platform.c`, and `USB_LOG_IAP_RATES`) | The counters are logged every 5 s while a car takes the audio. Digital Audio 1.01 announces every track, and libiap reports 1.03. Upstream's lock does nothing, so the USB interrupt could pull from the mixer while a thread changed it, and a stopped buffer could be sent again. Upstream plays a new rate before the dock has switched to it. 7900 as written takes the sink at configuration, which would silence an iAP1 accessory playing the line out and an iAP2 car. Upstream needs both 44.1 and 48 kHz, so an older car that lists one rate identified, took control and stayed silent. Some cars never set the rate, and ipod-gadget, which plays in them, never waits for one. A tester's car authenticates, sets 44.1 kHz and never answers GetAccessorySampleRateCaps; waiting out its reports keeps the fallback from taking the audio while a car is still identifying, and the four-second limit plays to one that polls and never lists. libiap sends no notification while an ack is owed, so one owed for a second is written off: otherwise an accessory that never acks hears of no track change again. |

### USB iAP2

A car looking for an iPhone, answered by the player as an iPod. Upstream has no
iAP2.

| File | What changed | Why |
| --- | --- | --- |
| `usbstack/usb_iap2.c`, `usb_iap2.h` (new) | iAP2's transport and link layer over the iAP configuration's HID interface, on the USB thread | **Support iPhone Accessories**. |
| `usbstack/usb_iap2_control.c` (new) | The control session: identification, USB audio, power, the library and its playlists, the queue, now playing, the car's buttons with shuffle and repeat, and cover art | Covers come from the art cache, encoded by `apps-ipod/draw/jpeg_enc.c`. |
| `usbstack/usb_core.c` | Vendor request 0x53 answered as an iPhone answers it, four zero bytes, marking the host a car; under Auto the disk handover waits a second (`storage_hold()`) | A car moves on to the iAP configuration within that second and is never handed the disk; a computer stays, and is. |
| `usb.c`, `export/usb.h` | New `usb_set_iap2_mode()`; the disk withheld while iAP2 is answered; new `usb_car_found()` and the `USB_CAR_RECONNECT` event: a car found while the sound card is on makes the player leave the bus and come back without it, until unplugged | The Mazda stops with an authentication error when it finds the sound card beside the disk. |
| `usbstack/usb_iap.h` | `USB_IAP2_MODE_*`, `usb_iap_set_iap2_mode()`, `usb_iap2_offered()`, `usb_iap_answer_iap2()`, vendor request 0x53; `usb_iap_end_reset_hold()`, which `usb.c`, `usb_core.c` and `platform.c` call | Off refuses the probe, On answers it and offers no disk, Auto answers once the host has sent 0x53. |

### The USB log

**Debug → USB log**. Keep it.

| File | What changed | Why |
| --- | --- | --- |
| `usbstack/usb_log.c`, `export/usb_log.h` | A 512-event ring filled from interrupts, written to `/.rockbox/logs/usb.log` while the debug screen is open or **System → Debug Logs → USB and Accessory Log** is on; `usb_log_sync()` waits for the file before a configuration change or stream start | A USB fault is gone by the time anything could look at it. |
| `export/usb.h`, `usb.c` | The insertion record and waypoints | **Debug → USB info**: whether a connect that did nothing was charging-only or a stuck handover. |
| `usbstack/usb_core.c` | `usb_log()` calls and waypoints in the handlers | Not on the notify path: the ARC driver calls `usb_core_bus_reset()` from its ISR and posts nothing. |
| `usbstack/iap/debug.c`, `debug.h` | New `iap_log_report()`: the iAP packet a HID report starts, to the USB log, with an IdentifyDeviceLingoes's options on a line of their own | The log decodes iAP. |
| `export/usb_drv.h`, `target/arm/usb-drv-arc.c`, `usbstack/usb_core.c` | New `usb_drv_batch_take_counts()`: since the last call, how often a queued iAP audio batch ran dry before it was refilled (ARC only; 0 on the 6G) and how many packets the controller finished with an error | The USB log counts the audio a car missed, which a silent stream otherwise hides. |
| `usbstack/iap/macros.h`, `platform-macros.h`, `export/usb_log.h` | USB iAP's `ERROR()` and libiap's `IAP_ERRORF()` write a `USB_LOG_IAP_ERROR` entry -- function, line and format string -- instead of a `logf` | Neither player has logf. The message's arguments are dropped: the ring holds only static strings. |

## firmware/ — USB, host side

The player driving a USB DAC. Upstream has no host stack. The player supplies
no VBUS.

| File | What changed | Why |
| --- | --- | --- |
| `usbstack/usb_host.c` (new) | Enumeration over `usb_drv_host_control()`; the SET_ADDRESS recovery wait sleeps; the device's interface settings and endpoints go into the USB log, not twice in a row for the same device; a hub on the root port has its ports powered and the first device on one enumerated at address 2, a full-speed one behind a high-speed hub routed through the hub's transaction translator | A DAC through a hub. |
| `export/config.h` | `HAVE_USB_HOST` and `HAVE_USB_HOST_AUDIO` for the ARC and DesignWare controllers | Both players get the host probe and the DAC output. |
| `usbstack/usb_host_audio.c`, `export/usb_host_audio.h` (new) | USB Audio Class 1 and 2 playback as `PCM_SINK_USB_HOST`, 44.1 and 48 kHz, software volume capped at 0 dB; a setting whose packets do not fit the controller is passed over, and "too many channels" reported when none fits; the USB log records why each streaming setting was passed over | The DAC output. |
| `export/usb_drv.h`, `target/arm/usb-drv-arc.c`, `drivers/usb-designware.c` | `usb_drv_host_iso_max_packet()`; the ARC driver carries owed samples over between packets; both hold accepted feedback to what a packet carries | The 5G's high-speed microframe holds 64 bytes, so a wide setting cannot keep up there. Feedback above a packet's capacity would grow the owed samples without bound. |
| `usb.c`, `export/usb.h` | The host probe, `USB_HOST_AUTO`, `USB_HOST_HUB` and the DAC state calls | **USB DAC Output**. A cable still only powered after 2 s is polled for a DAC, unless a serial iAP accessory is talking, which the search would starve on the 5G. **Turn On** refuses while a host answers or a serial accessory is present. A DAC behind a hub has its hub port read once a second while it plays, since taking it off the hub leaves the player's own port connected. |
| `export/usb_drv.h` | The host API | Implemented by both controller drivers. The host calls take a lock and may sleep, so they must run in a thread. |
| `export/pcm_sink.h`, `pcm.c` | `PCM_SINK_USB_HOST` registered | `PCM_SINK_NUM` is 3 on both targets. |
| `kernel/include/queue.h` | `SYS_USB_DAC_ON` / `_OFF` (plug events 8, 9), `SYS_ACCESSORY_CONNECTED` (10) and `SYS_OUTPUT_RESUME` (11) | Past upstream's plug events, which run to 7. |
| `target/arm/s5l8702/ipod6g/power-6g.c` | Taking the port as host commits 500 mA | At 100 mA the 6G would not charge while playing to a DAC. |

## firmware/ — USB controller drivers

| File | What changed | Why |
| --- | --- | --- |
| `target/arm/usb-drv-arc.c` (5G) | `USBCMD_ITC` 1 microframe | Upstream's 8 loses one sound-card packet in eight. |
| `target/arm/usb-drv-arc.c` | `prime_transfer()`'s timeout reads `USEC_TIMER` | `current_tick` stops with interrupts off. |
| `target/arm/usb-drv-arc.c` | EHCI host mode, isochronous through the transaction translator; `read_hw_info()`; `usb_log` calls; host waits sleep, and the control schedule and the port are serialised by a mutex; a full-speed device behind a high-speed hub: the port reset again at forced full speed, so the hub runs at full speed | The 5G's host side. `HWHOST` reports no translator, but full-speed DACs play through one. Control and port waits yield, so a slow DAC does not stall playback. The transmit FIFO is 128 bytes, too small for a 188-byte start split. |
| `drivers/usb-designware.c` (6G) | Host channels; NYET/NAK handling with PING; one isochronous packet per microframe from the SOF interrupt; hardware info; `usb_log` calls; host-side waits sleep, and channel 0 and the port are serialised by a mutex; split transactions to a full-speed device behind a high-speed hub: start and complete splits for control, start splits of up to 188 bytes a microframe for the stream, filled half a frame ahead | The 6G's host side. A high-speed DAC is silent without a packet every microframe. Control and port waits yield, so a slow DAC does not stall playback. |
| `export/usb-designware.h` | `FHMOD` bit | Forces host mode. |
| `target/arm/pp/usb-fw-pp502x.c`, `target/arm/s5l8702/usb-s5l8702.c` | New `usb_host_probe_enable()` | Stops device detection while the probe owns the port. |

## firmware/ — iPod Classic 6G (S5L8702)

| File | What changed | Why |
| --- | --- | --- |
| `target/arm/s5l8702/ipod6g/storage_ata-6g.c` | SSD storage mode, ~150 lines: clock-gate instead of `STANDBY IMMEDIATE`, deep sleep once the backlight is off, 10 s or more after the clock-gate sleep, eager wake on `ata_spin()` and `Q_STORAGE_PRE_WAKE`, no HDD power/noise features, ranged cache maintenance, auto-detection in `ata_init()`; any power-down or hard reset forces the next power-up to be a full init, so the clock-ungate fast path runs only after a clock-gate sleep | Flash mods are common on this player, and upstream treats every device as a spinning disk. |
| `target/arm/s5l8702/ipod6g/power-6g.c` | Charger classification, ~100 lines: with the backlight off, charging is disabled unless a charger is confirmed or 500 mA is committed, and a confirmed charger keeps it enabled when the stack's no-host timeout drops the commitment to 100 mA; a 10 ms watch on the charge-status line with the backlight on and for 10 s after the cable goes in; an asymmetric 8-sample debounce | Upstream calls any USB insertion a charger, and a source that cannot supply device + charge current oscillates. A configured computer, **USB Charging = Force** and the DAC host all commit 500 mA. |
| `drivers/audio/cs42l55.c`, `export/cs42l55.h` | `audiohw_idle_powerdown()`, `audiohw_idle_powerup()` | Idle codec power-down, muted first to avoid a pop. `HPACTL`/`HPBCTL` are left alone. |
| `target/arm/s5l8702/ipod6g/cscodec-6g.c` | `cscodec_power()` switches LDO 3 | Upstream is a stub. |
| `target/arm/s5l8702/ipod6g/audio-6g.c` | `audio_input_mux()` keeps its last source only with `HAVE_RECORDING` | Read only on the recording paths, it is a set-but-unused warning in a build without them. |
| `target/arm/s5l8702/ipod6g/mikey-6g.c` | `mikey_init()` returns early on `rec_hw_ver == 0` | The 80GB and fat 160GB have no Mikey to poll. |
| `target/arm/s5l8702/ipod6g/mikey-6g.c`, `mikey-target.h` | New `mikey_probe()` | Returns the I2C status, so the debug screen can tell an empty jack from a missing chip. |
| `target/arm/s5l8702/ipod6g/mikey-6g.c`, `export/button.h` | Centre clicks counted over 360 ms: two are next, three previous. `mikey_set_track_skip()` and `mikey_supported()` | Upstream's remote has no next or previous. Counting delays play/pause, so **Remote Track Skip** can turn it off. |
| `target/arm/s5l8702/debug-s5l8702.c` | `dbg_hw_info()` becomes `dbg_hw_info_lines()`, the lines of a list; adds the LTC4066 charger pins and a Mikey line: `jack=`, `hw=`, `probe rc=`, `r0=` | Hardware info keeps the theme. Mikey `rc` 1 means no Mikey answered; 0 means it did. |
| `target/arm/s5l8702/ipod6g/mikey-6g.c` | Centre button read from reg4 bit 0 alone; `mikey_line_gate()` turns a generic headset's after-release reg5 event into one volume step, and drops the false one its centre button raises | A generic three-button headset sets bit 2 for every button, so each one played or paused, and its late events moved the volume at random. |
| `target/arm/s5l8702/ipod6g/mikey-6g.c`, `export/button.h`, `target/arm/s5l8702/debug-s5l8702.c` | `mikey_scope_*()`: while on, the polling thread reads all eight registers and logs every change; the Hardware info line shows the thread's r4/r5 rather than reading them | **Debug > Mikey remote**. A second reader of reg5 takes volume events away from the remote. |

### SSD mode: one setting, two mechanisms

| | `drivers/ata.c` (5G) | `storage_ata-6g.c` (6G) |
| --- | --- | --- |
| First sleep stage | `STANDBY IMMEDIATE`, as for a disk | Gates the clock instead of `STANDBY IMMEDIATE` |
| Power-off | Once the backlight is off, 10 s or more after it sleeps | The same |
| Wake cost hidden by | `Q_STORAGE_PRE_WAKE` | `Q_STORAGE_PRE_WAKE` |

---

## firmware/ — TV output

| File | What changed | Why |
| --- | --- | --- |
| `export/videoout.h` | New `videoout_set_format(double_size, pal)`, and `videoout_requested()`, true while TV Out is not Off whether or not a picture is showing | **TV Out > Size** and **TV Out > Standard**; the Turn On/Turn Off row is labelled from `videoout_requested()`. |
| `target/arm/ipod/video/lcd-video.c` | TV output for the 5G. One 24-bit bitmap command puts the BCM's TV encoder in PAL or NTSC; a background thread then copies changed LCD rows into its TV framebuffer, 1x or doubled, at most 30 times a second and resting as long as each pass took, from a 150 KB snapshot taken from core memory while it runs, so each pass is one whole UI frame. A 1x pass starts at the beginning of a TV field, read from the BCM's scan-line register at `0x100008ac`, and runs straight through, so it does not tear. The LCD is given a black frame while it runs. Turning it off blacks the TV picture and updates the LCD again; the BCM stays in TV mode until the next boot, and `lcd_sleep()` refuses until then. While `tv_start()` sends its BCM commands, `lcd_update_rect()` only marks the screen dirty, so a scroll-thread update cannot land between them; with interrupts off, a panic, it updates the LCD. With no thread slot free, TV out does not start. Implements `videoout_set_mode()`, `_set_format()`, `_active()`, `_requested()` and `_disable()` | Upstream has no 5G video output. The framebuffer is 24-bit BGR at TV levels, 2112 bytes a line whatever the bitmap's width, measured on hardware. Another bitmap command drops the signal, and an LCD update disturbs it. Taking the BCM out of TV mode hangs the player, by the sleep path or by cutting its power. The commands come from Rockbox FS#9787. |
| `target/arm/s5l8702/ipod6g/videoout-6g.c` | Two output frames, 225 KB, taken from `core_alloc()`, pinned, while the mode is not Off. Each update is written to the frame not on show, which the VP takes at the next field; the next update waits for the encoder's field bits (`0x39300040`, bits 0-1) to change before writing to the other, yielding while it waits (at most 40 ms) and dropping the update if the output stopped meanwhile. The VP's destination is the LCD at 1x, centred, or upstream's 648x432; setting the size it already has does not restart the output | Upstream reserves one frame on every 6G, written while the TV shows it, so a fast-changing screen tears. Taking the frames rebuffers playback once. |

---

## firmware/ — config headers

A feature this fork declines, whose wiring is in files this fork never edits,
merges in **enabled**. Switch it off here and it compiles to nothing. Every
other decline sits in a file this fork edits, so it conflicts on merge.

| File | What changed | Why |
| --- | --- | --- |
| `export/config/ipod6g.h` | `HAVE_RECORDING` commented out | No recording UI ships. |
| `export/config/ipod6g.h` | `PLUGIN_BUFFER_SIZE` 2 MiB → 3 MiB | The core scratch buffer (`apps-ipod/system/app_buffer.c`) keeps the name. |
| `export/config/ipod6g.h` | `TARGET_EXTRA_THREADS` 2 with both `IPOD_ACCESSORY_PROTOCOL` and `HAVE_MIKEY_REMOTE`, else 1 | Upstream adds the remote's poller without a thread for it. Short by one, the feature is silently absent. `__threads` should measure 19. |
| `export/config/ipodvideo.h` | `HAVE_COMPOSITE_VIDEO_OUT` and new `HAVE_VIDEOOUT_STANDARD`, outside the bootloader | **TV Out**, and the 5G's PAL/NTSC choice. See *TV output*. |
| `export/config/ipodvideo.h` | `HAVE_RECORDING` commented out | As above. |
| `export/config/ipodvideo.h` | `CONFIG_TUNER`, `HAVE_RDS_CAP`, `CONFIG_RDS` commented out | No FM accessory. Matches `ipod6g.h`. |
| `export/config/ipodvideo.h` | `HAVE_RTC_ALARM` commented out | The Apple bootloader clears the alarm flag, so a wake is guessed and usually missed. The apps-side alarm is removed. |

## firmware/ — serial iAP and remote buttons

Serial iAP is `apps-ipod/iap/`. It reaches these firmware files:

| File | What changed | Why |
| --- | --- | --- |
| `export/iap.h` | New `iap_enable(bool)`, `iap_accessory_present()`, `iap_power_reset()` and `iap_volume_set()` | The serial half of **Accessory Protocol**; whether frames are arriving, which the DAC search checks; a session reset that leaves playback alone; and a volume the player sets ending an accessory's mute. |
| `drivers/button.c` | A remote button, and the 6G inline remote's multimedia codes, are never swallowed by **First Button Press**, on targets whose remote has no screen | Whoever presses it cannot see the screen it would wake, so Play had to be pressed twice. From nuxcodes/rockpod. |
| `export/iap.h`, `export/config/ipodvideo.h`, `target/arm/pp/uart-pp.c` | New `HAVE_IAP_ACCESSORY_POLL` and `iap_accessory_poll()`: the 5G reads its accessory-detect ADC from the iAP thread and resets the session after a second without an accessory; the baud divisor is written with interrupts off | Nothing told the 5G an accessory had gone, so the next one inherited its session. RBR and DLL share an address, and the receive interrupt spun for ever if it ran while the latch was open. From nuxcodes/rockpod. |
| `export/iap.h`, `target/arm/pp/uart-pp.c` | New `iap_accessory_plugged()`, what the last `iap_accessory_poll()` saw | The iAP tick idles once no accessory is in, and on the 5G only the tick's poll can see one go. |
| `target/arm/s5l8702/ipod6g/serial-6g.c` | The debounced unplug calls `iap_reset_state()`; with no accessory, `tx_rdy()` says ready and `tx_writec()` drops the byte | Upstream only closed the UART on unplug, so the session outlived the accessory. From nuxcodes/rockpod. |
| `target/arm/ipod/button-target.h`, `target/arm/s5l8702/ipod6g/button-target.h` | `BUTTON_REMOTE` includes `BUTTON_RC_MENU`; new `BUTTON_RC_NEXT_ALBUM`, `_PREV_ALBUM`, `_NEXT_PLAYLIST`, `_PREV_PLAYLIST` (0x4000–0x800) | Upstream lists `BUTTON_RC_PLAY` twice and omits Menu. The new codes carry the album and playlist buttons an Onkyo DS-A3 remote sends. |
| `target/arm/ipod/button-clickwheel.c` | `button_read_device()` lets the inline remote's multimedia code replace the wheel button bits rather than ORing into them | Those codes reuse the wheel's bit values, so SELECT plus the remote read as PLAYPAUSE and PLAY plus the remote as VOLUME_UP. Upstream bug, not yet reported upstream. |

---

## lib/

| File | What changed | Why |
| --- | --- | --- |
| `rbcodec/metadata/mp4.c`, `metadata.h` | `has_video` on `struct mp3entry`, set only when a `vide` track's first sample entry is moving video (`avc1`/`avc3`/`hvc1`/`hev1`/`mp4v`/`av01`/`vp09`/`encv`) | Tagcache skips music videos, but keeps M4B/M4A files whose `vide` tracks hold only chapter pictures or covers. |
| `rbcodec/metadata/mp4.c` | `chpl`: reads the version-0 header, and takes a lead trim only from a single record | A chapter list whose first chapter starts after 0:00 had that much audio cut from the start of the book. |
| `rbcodec/metadata/dsd.c` | `parse_iso_area()` reads into the master TOC's buffer instead of a second 2 KB one; a `.dff` chunk whose size runs past the end of the file ends the parse | A dock asking for an `.iso`'s tags runs this on the 6.8 KB USB thread. A 64-bit chunk size wrapped the 32-bit seek back onto the same chunk, and the walk never ended. Upstream bug, not yet reported upstream. |
| `rbcodec/metadata/metadata.c` | **Debug > Metadata log** writes `/.rockbox/logs/metadata.log` instead of `/metadata.log` | Beside the other logs, which head it with the time and version when it is switched on. |
| `skin_parser/tag_table.c` | `find_custom_tag()` declared **weak** and tried **first** in `find_tag()` | Custom tags are registered from the app layer. First, because upstream's shortest-match search would read `%sel` as `%s`. Weak, so the parser links standalone. |
| `skin_parser/tag_table.h` | Six new `SKIN_TOKEN_*` members | The token field is a 1-byte enum; the table rows stay in `custom_tags.c`. |
| `skin_parser/tag_table.h` | `SKIN_REFRESH_SPECTRUM`, in `SKIN_REFRESH_NON_STATIC` | Spectrum lines redraw with time. |
| `rbcodec/codecs/libffmpegFLAC/decoder.c` | New `flac_lpc_32()` runs the 64-bit LPC filter (`flac_lpc_32_arm()`, or `flac_lpc_32_c()` elsewhere) 1024 samples at a time with a yield between; upstream's ARMv5E `FLAC_LPC32_NARROW_ASM` kernel, off by default, is not wired | 24-bit FLAC took the C loop, and one channel of a frame ran ~50 ms without yielding on the 5G -- a visible stutter in anything animating while it played. |
| `rbcodec/codecs/libalac/alac.c` | 24-bit frames merge their uncompressed low bytes below the decoded sample before the scale to 29 bits: a stereo frame with one such byte reads them from the frame as it merges, any other keeps them in a buffer of their own, `shift_buffer` | Upstream reads them into the output buffers, which the Rice data then overwrites, so every 24-bit ALAC file decoded with its low byte zero. Upstream bug, not yet reported upstream. |
| `rbcodec/codecs/libalac/alac.c` | A zero run is clamped to the samples the frame has left; the parameter estimate counts leading zeros over 32 bits; a Rice limit outside 1 to 25, or a frame of more than 4096 samples, fails the frame | A corrupt run length or frame size wrote past the output buffer, and a history past 16 bits indexed the 256-byte table beyond its end. Apple writes a limit of 14. Upstream bugs, not yet reported upstream. |
| `rbcodec/codecs/libalac/alac.c`, `arm.S` (new), `SOURCES` | A 32-bit bit cache for the Rice decoder and the low bytes; the adaptive filter at orders 1 to 8 specialised per order; on ARM both in assembly, handing back to the C for codes over 7 bits of prefix, zero runs and coefficients near the 16-bit limit, and the 24-bit stereo merge in assembly as well | ALAC needed about 36 MHz for a 44.1 kHz stream on the 5G, which runs at 30 unboosted, so it played boosted throughout. Now about 17 (16-bit) and 20 (24-bit), estimated under emulation. Output is unchanged but for the low bytes above. |
| `rbcodec/codecs/alac.c` | A frame that fails to decode ends the track with `CODEC_ERROR` | The wrapper advanced by a byte count the failed frame never set and inserted -1 samples. Upstream bug, not yet reported upstream. |
| `rbcodec/codecs/libm4a/m4a.c` | `m4a_seek()` and `m4a_seek_raw()` multiply a time-to-sample entry's count by its duration in 64 bits | The 32-bit product overflowed past 2^32 samples, about 27 hours at 44.1 kHz, so in a longer book a chapter jump landed near the end and a resume showed the wrong time. Upstream bug, not yet reported upstream. |

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
| `check-zip-langs.sh` | Fails on a language file under 1000 bytes in a firmware zip; a clean build can write one as a bare header with no error. `build-hw.sh` runs it after `make zip`. |
| `upstream-apps-port.sh` | Lists the upstream commits to `apps/` in a range, each touched file mapped to its `apps-ipod/` counterpart through the `was:` markers, and marks those that also touch `firmware/` or `lib/`. Run at every upstream merge. |
| `soundscan/` | Runs the player's sound analysis over a music folder on a computer. Ships as `.rockbox/tools/soundscan.exe`; compiles the app layer's analysis sources. |
| `database_pb/` | Runs the player's database Update or Rebuild against a mounted player. Ships as `.rockbox/tools/database_pb.exe`; compiles `apps-ipod/database/tagcache.c` with `DBTOOL`. Upstream's `database/`, the (D)atabase build, writes upstream's database and is left as it is. |
| `artcache_pb/` | Runs the player's art cache pass against a mounted player. Ships as `.rockbox/tools/artcache_pb.exe`; compiles `apps-ipod/metadata/art_cache.c` and its image code with `DBTOOL`. |
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
| `checkwps/stubs.c` | The allocator, status bar, settings lookups, a `string_to_cfg()` that checks the name only, and every settings callback | A new setting whose callback is missing here fails the link. Both a theme `.cfg` and a skin are checked from the theme's root folder. |
| `checkwps/checkwps.h`, `checkwps.c`, `README` | `--viewports`; a pointer to `-v` when a skin fails with no error line; a `playback_claim_aa_slot_keyed()` stub; `rbpaths.h` included | A missing font or bitmap prints nothing without `-v`. The skin parser keys an in-place `%Cl` chain's slot. |
| `apps-ipod/skin/wps_internals.h` | `VP_DEFAULT_LABEL` is `NULL` under `__PCTOOL__` | As upstream has it. |
| `apps-ipod/features.txt` | `usb_hid` also under `__PCTOOL__` | `settings_list.c` registers it unconditionally. |
| `configure` | The SDL check is a real test | Upstream's is always true, so `--type=c` demands SDL. |

### The simulator

Everything it needed to build is inside `apps-ipod/`; see
`apps-ipod/sim/README.md`. Three files outside it carry one addition, for
screenshots, and three a fix for a crash at boot:

| File | What changed | Why |
| --- | --- | --- |
| `firmware/target/hosted/sdl/system-sdl.c` | `--screenshot` argument, setting `sim_screenshot` | Screenshots that show the same clock and battery every time. |
| `firmware/target/hosted/sdl/system-sdl.c`, `kernel-sdl.c`, `system-sdl.h` | `system_init()` calls `sim_kernel_init()`, no longer static, before it starts the event thread; `tick_start()` no longer does | The event thread enters the irq handler for every event. In upstream, an event that arrives before `tick_start()` waits on a condition not yet created, which in SDL 2.30 jumps to address 0: on Windows, 1 boot in 3 to 8 crashes with 0xC0000005. |
| `firmware/target/hosted/rtc.c` | Under `SIMULATOR`, `sim_screenshot` makes `rtc_read_datetime()` return Monday 2024-01-01 09:00:00 | Every clock and date in the UI reads it. |
| `uisimulator/common/powermgmt-sim.c` | `sim_screenshot` stops `battery_status_update()`, so the battery stays at 4300 mV, 100 %, not charging | Otherwise the simulated battery drains 1 % a second and cycles through charging. |

Two upstream files reach the app layer by bare include name, through `api/` stubs:

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
| `bundle-tools.sh` | Adds `soundscan.exe`, `database_pb.exe` and `artcache_pb.exe`, rebuilt by make on each run, and the host-built codecs soundscan loads, as `.rockbox/tools/`. A missing Windows simulator is reported, not fatal; a failed tool build is fatal; `release.sh` requires it. |
| `build-bootloader.sh`, `tools/bootloader-installer/` | Builds both bootloaders and Windows `ipodpatcher.exe` and `mks5lboot.exe` from `utils/`, unchanged, and zips them with the install scripts and README as `podbox-bootloader.zip`. The 5G bootloader is passed to ipodpatcher by `install-5g.cmd`: embedding it needs every iPod model's bootloader beside it. |
| `release.sh` | Builds both targets on the build server, verifies the zips, then replaces the `Themes`, `Simulator`, `Bootloader` and `latest` releases, in that order. |
| `docs/CREDITS` | PodBox, RockPod, Spun, accessory protocol, car protocol and themes-and-fonts blocks above `For Rockbox:`, a heading this fork adds. Upstream's list below it is untouched, trailing newline included, so merges apply. |
| `.gitignore` | `/build*` narrowed to the build directories; local drafts, `dist/`, editor state and `build-bootloader.sh`'s output added. |
| `wps/WPSLIST` | `cabbiev2` block removed, so it does not ship. `rockbox_failsafe` stays: it is the parse-failure fallback. |

---

## Deliberately *not* changed

| Not changed | Why |
| --- | --- |
| `apps/` | Upstream mirror. Do not edit it. |
| RockPod's source mode in `usbstack/usb_audio.c` | The source here is USB iAP's configuration 2, on both players. |
| `usbstack/iap/libiap/` transport | Upstream's USB transport; RockPod's `usb_iap_hid.c` is absent. Only the changes in the rows above differ. |
| RockPod's isochronous plumbing in `drivers/usb-designware.c` | Serves its source mode. |
| `target/arm/s5l8702/pcm-s5l8702.c` | RockPod's I2S clock gating is **deferred**. Upstream's `pcm_sink` calls nest, so a naive port gates the clock on every buffer. |
