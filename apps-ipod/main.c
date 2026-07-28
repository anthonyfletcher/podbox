/***************************************************************************
 * Original code from RockBox
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * Boot: brings up the hardware, filesystem, settings, playback and voice
 * in order, shows the logo, then hands control to the root menu and never
 * returns.
 ****************************************************************************/
#include "config.h"
#include "system.h"

#include "version.h"
#include "gcc_extensions.h"
#include "storage.h"
#include "disk.h"
#include "file_internal.h"
#include "lcd.h"
#include "rtc.h"
#include "debug.h"
#include "led.h"
#include "kernel.h"
#include "rbpaths.h"
#include "../kernel-internal.h"
#include "button.h"
#include "input/core_keymap.h"
#include "screens/browse/browser.h"
#include "files/filetypes.h"
#include "panic.h"
#include "widgets/menu.h"
#include "usb.h"
#include "wifi.h"
#include "powermgmt.h"
#include "adc.h"
#include "i2c.h"
#ifndef DEBUG
#include "serial.h"
#endif
#include "audio.h"
#include "settings/settings.h"
#include "backlight.h"
#include "audio/play_status.h"
#include "screens/system/debug_menu.h"
#include "font.h"
#include "speech/language.h"
#include "bitmaps/podboxlogo.h"
#include "bitmaps/native/podbox_colors.h"
#include "screens/playback/wps.h"
#include "playlist/playlist.h"
#include "core_alloc.h"
#include "rolo.h"
#include "screens/system/usb_screen.h"
#include "power.h"
#include "speech/talk.h"
#include "system/shutdown.h"
#include "dircache.h"
#include "database/tagcache.h"
#include "metadata/art_cache.h"
#include "screens/browse/browser_db.h"
#include "lang.h"
#include "string.h"
#include "widgets/splash.h"
#include "eeprom_settings.h"
#include "draw/icon.h"
#include "draw/viewport.h"
#include "skin/skin_engine.h"
#include "skin/statusbar_skinned.h"
#include "bootchart.h"
#include "logdiskf.h"
#include "bootdata.h"

#include "screens/shortcuts.h"

#include "iap.h"

#include "audio/audio_thread.h"
#include "audio/playback.h"
#include "tdspeed.h"



#include "piezo.h"
#include "dsp_core.h"
#include "rbunicode.h"

#define MAIN_NORETURN_ATTR NORETURN_ATTR



/*#define AUTOROCK*/ /* define this to check for "autostart.rock" on boot */

static void init(void);
/* main(), and various functions called by main() and init() may be
 * be INIT_ATTR. These functions must not be called after the final call
 * to root_menu() at the end of main()
 * see definition of INIT_ATTR in config.h */
int main(void) INIT_ATTR MAIN_NORETURN_ATTR;
int main(void)
{
    CHART(">init");
    init();
    CHART("<init");
    FOR_NB_SCREENS(i)
    {
        screens[i].clear_display();
        screens[i].update();
    }
    list_init();
    browser_init();
    /* Keep the order of this 3
     * Must be done before any code uses the multi-screen API */
    /* All threads should be created and public queues registered by now */
    usb_start_monitoring();

#if !defined(DISABLE_ACTION_REMAP) && defined(CORE_KEYREMAP_FILE)
    if (file_exists(CORE_KEYREMAP_FILE))
    {
        int mapct = core_load_key_remap(CORE_KEYREMAP_FILE);
        if (mapct <= 0)
            splashf(HZ, "key remap failed: %d,  %s", mapct, CORE_KEYREMAP_FILE);
    }
#endif

    allocate_playback_log();
    if (!file_exists(ROCKBOX_DIR"/playername.txt"))
    {
        int fd = open(ROCKBOX_DIR"/playername.txt", O_CREAT|O_WRONLY|O_TRUNC, 0666);
        if(fd >= 0)
        {
            fdprintf(fd, "%s", MODEL_NAME);
            close(fd);
        }
    }

    global_status.last_volume_change = 0;
    /* no calls INIT_ATTR functions after this point anymore!
     * see definition of INIT_ATTR in config.h */
    CHART(">root_menu");
    root_menu();
}

/* The disk isn't ready at boot, rblogo is stored in bin and erased after boot */
int show_logo_boot( void ) INIT_ATTR;
int show_logo_boot( void )
{
    /* Clear to the artwork's background (#000105) so the flash before the
     * full-screen logo lands matches it rather than showing stray colour. */
    lcd_set_background(PODBOX_COLOR_BG);
    lcd_set_foreground(PODBOX_COLOR_FG);
    lcd_clear_display();
    lcd_bmp(&bm_podboxlogo, (LCD_WIDTH - BMPWIDTH_podboxlogo) / 2,
                             (LCD_HEIGHT - BMPHEIGHT_podboxlogo) / 2);
    lcd_update();
    return 0;
}

static int INIT_ATTR init_dircache(bool preinit)
{
    if (preinit)
        dircache_init(MAX(global_status.dircache_size, 0));

    if (!global_settings.dircache)
        return -1;

    int result = -1;

    if (!preinit)
    {
        result = dircache_enable();
        if (result != 0)
        {
            if (result > 0)
            {
                /* Print "Scanning disk..." to the display. */
                splash(0, str(LANG_SCANNING_DISK));
                dircache_wait();
                backlight_on();
                show_logo_boot();
            }

            struct dircache_info info;
            dircache_get_info(&info);
            global_status.dircache_size = info.size;
            status_save(true);
        }
        /* else don't wait or already enabled by load */
    }

    return result;
}

static void init_tagcache(void) INIT_ATTR;
/* Progress is shown, never spoken: this runs before audio is initialised, and
 * the database commit is using the audio buffer anyway. */
static void init_tagcache(void)
{
    bool clear = false;
    tagcache_init();
    art_cache_init();

    while (!tagcache_is_initialized())
    {
        int ret = tagcache_get_commit_step();

        if (ret > 0)
        {
            if (lang_is_rtl())
            {
                splash_progress(ret, tagcache_get_max_commit_step(),
                               "[%d/%d] %s", ret, tagcache_get_max_commit_step(),
                               str(LANG_TAGCACHE_INIT));
            }
            else
            {
                splash_progress(ret, tagcache_get_max_commit_step(),
                                "%s [%d/%d]", str(LANG_TAGCACHE_INIT), ret,
                                tagcache_get_max_commit_step());
            }
            clear = true;
        }
        sleep(HZ/4);
    }
    browser_db_init();

    if (clear)
    {
        backlight_on();
        show_logo_boot();
    }
}


#include "errno.h"

static void init(void) INIT_ATTR;
static void init(void)
{
    int rc;
    bool mounted = false;

    system_init();
    core_allocator_init();
    kernel_init();



    /* early early early! */
    filesystem_init();

    set_cpu_frequency(CPUFREQ_NORMAL);
    cpu_boost(true);

    i2c_init();

    power_init();

    enable_irq();
    enable_fiq();
    /* current_tick should be ticking by now */
    CHART("ticking");

    unicode_init();
    lcd_init();
    FOR_NB_SCREENS(i)
        global_status.font_id[i] = FONT_SYSFIXED;
    font_init();

    settings_reset();

    CHART(">show_logo");
    show_logo_boot();
    CHART("<show_logo");
    lang_init(core_language_builtin, language_strings,
              LANG_LAST_INDEX_IN_ARRAY);

#ifdef DEBUG
    debug_init();
#else
    serial_setup();
#endif

    rtc_init();

    adc_init();

    usb_init();

    backlight_init();

    button_init();

    /* Don't initialize power management here if it could incorrectly
     * measure battery voltage, and it's not needed for charging. */
    powermgmt_init();

    piezo_init();

    /* Keep the order of this 3 (viewportmanager handles statusbars)
     * Must be done before any code uses the multi-screen API */
    CHART(">gui_syncstatusbar_init");
    gui_syncstatusbar_init(&statusbars);
    CHART("<gui_syncstatusbar_init");
    CHART(">sb_skin_init");
    sb_skin_init();
    CHART("<sb_skin_init");
    CHART(">gui_sync_wps_init");
    gui_sync_skin_init();
    CHART("<gui_sync_wps_init");
    CHART(">viewportmanager_init");
    viewportmanager_init();
    CHART("<viewportmanager_init");

    CHART(">storage_init");
    rc = storage_init();
    CHART("<storage_init");
    if(rc)
    {
        lcd_clear_display();
        lcd_putsf(0, 1, "ATA error: %d", rc);
        lcd_puts(0, 3, "Press button to debug");
        lcd_update();
        while(!(button_get(true) & BUTTON_REL)); /* DO NOT CHANGE TO ACTION SYSTEM */
        dbg_ports();
        panicf("ata: %d", rc);
    }



    if (!mounted)
    {
        CHART(">disk_mount_all");
        rc = disk_mount_all();
        CHART("<disk_mount_all");
        if (rc<=0)
        {
            int line=0;
            lcd_clear_display();
            lcd_putsf(0, line++, "No partition found (%d).", rc);
            lcd_puts(0, line++, "Insert USB cable");
            lcd_puts(0, line++, "and fix it.");
            lcd_puts(0, line++, rbversion);

            struct storage_info sinfo;
            storage_get_info(0, &sinfo);
#ifdef MAX_PHYS_SECTOR_SIZE
            lcd_putsf(0, line++, "id: '%s' s:%u*%u", sinfo.product, sinfo.sector_size, sinfo.phys_sector_mult);
#else
            lcd_putsf(0, line++, "id: '%s' s:%u", sinfo.product, sinfo.sector_size);
#endif
            struct partinfo pinfo;
            for (int i = 0 ; i < NUM_VOLUMES ; i++) {
                disk_partinfo(i, &pinfo);
                if (pinfo.type)
                    lcd_putsf(0, line++, "P%d T%02x S%llx",
                              i, pinfo.type, (unsigned long long)pinfo.size);
            }
            lcd_update();

#if defined(MAX_VIRT_SECTOR_SIZE) && defined(DEFAULT_VIRT_SECTOR_SIZE)
                disk_set_sector_multiplier(IF_MD(i,) DEFAULT_VIRT_SECTOR_SIZE/SECTOR_SIZE);
#endif

            usb_start_monitoring();
            while(button_get(true) != SYS_USB_CONNECTED) {};
            gui_usb_screen_run(true, button_get_data());

#if !defined(DEBUG) && !(CONFIG_STORAGE & STORAGE_RAMDISK)
            system_reboot();
#else
            rc = disk_mount_all();
            if (rc <= 0) {
                lcd_putsf(0, 4, "Error mounting: %08x", rc);
                lcd_update();
                sleep(HZ*5);
                system_reboot();
            }
#endif
        }
    }

    pcm_init();
    dsp_init();

    CHART(">settings_load");
    settings_load();
    CHART("<settings_load");

    if (global_settings.clear_settings_on_hold &&
#ifdef SETTINGS_RESET
    /* Reset settings if holding the reset button. (Rec on Archos,
       A on Gigabeat) */
    ((button_status() & SETTINGS_RESET) == SETTINGS_RESET))
#else
    /* Reset settings if the hold button is turned on */
    (button_hold()))
#endif
    {
        splash(HZ*2, str(LANG_RESET_DONE_CLEAR));
        settings_reset();
    }
    CHART(">init_battery_tables");
    init_battery_tables();
    CHART("<init_battery_tables");
    CHART(">init_dircache(true)");
    rc = init_dircache(true);
    CHART("<init_dircache(true)");
    if (rc < 0)
        tagcache_remove_statefile();

    CHART(">settings_apply(true)");
    settings_apply(true);
    CHART("<settings_apply(true)");
    CHART(">init_dircache(false)");
    init_dircache(false);
    CHART("<init_dircache(false)");
    CHART(">init_tagcache");
    init_tagcache();
    CHART("<init_tagcache");

    playlist_init();
    browser_mem_init();
    filetype_init();

    shortcuts_init();

    CHART(">audio_init");
    audio_init();
    CHART("<audio_init");
    talk_announce_voice_invalid(); /* notify user w/ voice prompt if voice file invalid */


    /* runtime database has to be initialized after audio_init() */
    cpu_boost(false);

    car_adapter_mode_init();
    iap_setup(global_settings.serial_bitrate);
    accessory_supply_set(global_settings.accessory_supply);
    lineout_set(global_settings.lineout_active);
    CHART("<settings_apply_skins");
    settings_apply_skins();
    CHART(">settings_apply_skins");
}

#ifdef CPU_PP
void cop_main(void) MAIN_NORETURN_ATTR;
void cop_main(void)
{
/* This is the entry point for the coprocessor
   Anyone not running an upgraded bootloader will never reach this point,
   so it should not be assumed that the coprocessor be usable even on
   platforms which support it.

   A kernel thread is initially setup on the coprocessor and immediately
   destroyed for purposes of continuity. The cop sits idle until at least
   one thread exists on it. */

#if NUM_CORES > 1
    system_init();
    kernel_init();
    /* This should never be reached */
#endif
    while(1) {
        sleep_core(COP);
    }
}
#endif /* CPU_PP */

