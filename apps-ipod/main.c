/***************************************************************************
 * Original code from RockBox
 * was: apps/main.c
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * Boot: brings up the hardware, filesystem, settings, playback and voice
 * in order, shows the boot screen, then hands control to the root menu and
 * never returns.
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
#include "screens/playback/wps.h"
#include "playlist/playlist.h"
#include "core_alloc.h"
#include "rolo.h"
#include "screens/system/usb_screen.h"
#include "power.h"
#include "speech/talk.h"
#include "system/shutdown.h"
#include "dircache.h"
#include "metadata/tag_trim.h"
#include "database/tagcache.h"
#include "metadata/art_cache.h"
#include "database/db_summary.h"
#include "files/file_index.h"
#include "screens/browse/browser_db.h"
#include "lang.h"
#include "string.h"
#include "widgets/splash.h"
#include "eeprom_settings.h"
#include "draw/icon.h"
#include "draw/viewport.h"
#include "draw/progress_bar.h"
#include "skin/skin_albumart_color.h"
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

#if (CONFIG_PLATFORM & PLATFORM_SDL)
/* The simulator takes a command line -- --debugwps traces skin parsing,
 * --nobackground drops the player bezel, and there are a few more in
 * sys_handle_argv(). Without argv they are all silently unavailable. */
#include "sim_tasks.h"
#include "system-sdl.h"
#define HAVE_ARGV_MAIN
/* Don't use SDL_main on Windows -> no more stdio redirection */
#if defined(WIN32)
#undef main
#endif
#endif /* PLATFORM_SDL */

/*#define AUTOROCK*/ /* define this to check for "autostart.rock" on boot */

static void init(void);
/* main(), and various functions called by main() and init() may be
 * be INIT_ATTR. These functions must not be called after the final call
 * to root_menu() at the end of main()
 * see definition of INIT_ATTR in config.h */
#ifdef HAVE_ARGV_MAIN
int main(int argc, char *argv[]) INIT_ATTR MAIN_NORETURN_ATTR;
int main(int argc, char *argv[])
{
    sys_handle_argv(argc, argv);
#else
int main(void) INIT_ATTR MAIN_NORETURN_ATTR;
int main(void)
{
#endif
    CHART(">init");
    init();
    CHART("<init");
    /* Hand the screen over to the UI in the colours it will use.
     *
     * The boot screen paints in its own palette and leaves it set --
     * settings_apply() establishes the theme's during init(), but boot stages
     * run after it and each repaint puts the palette's back. clear_display()
     * fills with whatever background is current, so without this the boot
     * background is what shows through wherever the theme does not paint.
     *
     * The album's colours, not the theme's, where the last session left a
     * resume point: seeding here is what lets the first screen appear already
     * carrying them, and the colours have to be resolved for the clear below
     * or the theme's background shows through until something repaints. */
    dynamic_colors_seed_resume();
    unsigned int boot_fg = dynamic_colors_resolve(global_settings.fg_color);
    unsigned int boot_bg = dynamic_colors_resolve(global_settings.bg_color);
    FOR_NB_SCREENS(i)
    {
        screens[i].set_foreground(boot_fg);
        screens[i].set_background(boot_bg);
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

/* The boot screen: a progress bar with a caption underneath, on a flat
 * background. Every stage of boot that has something to say redraws the whole
 * thing rather than overlaying a splash, so nothing has to be undone
 * afterwards.
 *
 * The bar is the part that always works. A caption needs the theme's font,
 * the font needs the disk, and the disk can be busy serving a USB host for
 * the whole of boot -- so on a cabled boot the bar may be the only thing that
 * moves. It is driven by boot_progress() below rather than by the captions.
 *
 * Captions are sticky: pass NULL to leave the current one up. They draw in the
 * built-in fixed font, so they need neither the disk nor the theme and are
 * available from the first paint; the earliest stages simply have nothing to
 * say yet and pass NULL.
 *
 * Boot only. INIT_ATTR memory is reclaimed once root_menu() starts, so calling
 * this after that point jumps into whatever was laid over it. Errors keep
 * using splash() -- they can happen at any time, and the fatal ones (no disk,
 * no partition) print diagnostics and never reach a normal screen anyway. */
#define BOOT_BAR_W      160
#define BOOT_BAR_H        7
#define BOOT_BAR_X      ((LCD_WIDTH - BOOT_BAR_W) / 2)
/* The bar alone is centred, with the caption hanging below it, rather than the
 * two centred as a pair: the bar is the thing the eye tracks, and it must not
 * move the moment a caption appears under it. */
#define BOOT_BAR_Y      ((LCD_HEIGHT - BOOT_BAR_H) / 2)
#define BOOT_CAPTION_PAD 10                             /* bar to caption gap */
#define BOOT_CAPTION_Y  (BOOT_BAR_Y + BOOT_BAR_H + BOOT_CAPTION_PAD)

/* A colour set. The background fills the screen, the caption draws in `fg` and
 * the progress bar in `accent`. Nothing else is on screen, so a set has to
 * carry the whole look on its own -- both `fg` and `accent` need to read
 * against `bg`, and `accent` has to survive being only seven pixels tall.
 *
 * All the backgrounds are dark on purpose: boot often happens in the dark,
 * and a screen that lights up white is unpleasant to be handed. */
struct boot_palette
{
    unsigned bg;
    unsigned fg;
    unsigned accent;
};

/* 0xrrggbb literals, packed to whatever the LCD wants. */
#define BOOT_RGB(c)  LCD_RGBPACK(((c) >> 16) & 0xff, ((c) >> 8) & 0xff, \
                                 (c) & 0xff)
#define BOOT_PAL(bg, fg, accent) \
    { BOOT_RGB(bg), BOOT_RGB(fg), BOOT_RGB(accent) }

static const struct boot_palette boot_palettes[] =
{
    BOOT_PAL(0x031835, 0xfff2f9, 0xc78dbe),     /* podbox blue     */
    BOOT_PAL(0x1e0d0a, 0xffe9df, 0xff7a45),     /* ember           */
    BOOT_PAL(0x0a1a12, 0xe7f5ec, 0x5fbf8a),     /* moss            */
    BOOT_PAL(0x10151c, 0xe6edf3, 0x7aa2f7),     /* slate           */
    BOOT_PAL(0x1a0d20, 0xf7eaf8, 0xb98cf5),     /* plum            */
    BOOT_PAL(0x0d0d0f, 0xf5f5f2, 0xe8b44a),     /* ink and gold    */
    BOOT_PAL(0x05202a, 0xe2f4f8, 0x35c0cf),     /* teal            */
    BOOT_PAL(0x1d0a11, 0xffe6ec, 0xff5d7e),     /* rose            */
    BOOT_PAL(0x121504, 0xf2f6e2, 0xb5d334),     /* lime            */
    BOOT_PAL(0x0b0f2a, 0xe8e9ff, 0x6c7bff),     /* indigo          */
    BOOT_PAL(0x16060f, 0xffe9f6, 0xff5fc8),     /* magenta         */
    BOOT_PAL(0x101014, 0xe9ecf1, 0x9aa8b8),     /* steel           */
};

/* Picked once, on the first paint, and held for the whole of boot. The clock
 * is the only thing that differs between two boots this early: storage is not
 * up, so there is nowhere to have remembered the last choice. A player whose
 * clock does not tick gets the same set every time, which is dull rather than
 * broken. */
static const struct boot_palette *boot_pal;   /* NULL until the first paint */

static const struct boot_palette *boot_palette(void) INIT_ATTR;
static const struct boot_palette *boot_palette(void)
{
    if (!boot_pal)
    {
        struct tm t;
        unsigned n = 0;

        if (rtc_read_datetime(&t) >= 0)
            n = t.tm_sec + t.tm_min * 7 + t.tm_hour * 13 + t.tm_yday * 31;

        boot_pal = &boot_palettes[n % ARRAYLEN(boot_palettes)];
    }

    return boot_pal;
}

/* Stages in the order init() reaches them, each worth a share of the bar.
 * boot_progress() paints the *start* of a stage, so the bar always shows work
 * that is finished, never work still to come. A stage that turns out to have
 * nothing to do is never painted and the next one credits its chunks, which
 * is why the bar can jump. */
enum boot_stage
{
    BOOT_STORAGE,       /* the screen is up; spinning the disk up */
    BOOT_MOUNT,
    BOOT_SETTINGS,
    BOOT_THEME,         /* settings_apply(): fonts, backdrops, colours */
    BOOT_DIRCACHE,
    BOOT_TAGCACHE,
    BOOT_AUDIO,
    BOOT_SKINS,
    BOOT_STAGE_COUNT
};

/* Chunks each stage is worth. Guesses at relative duration -- the bar only
 * has to keep moving, and the two stages that can take minutes report from
 * inside themselves anyway. BOOT_TAGCACHE is left out on purpose: see
 * boot_stage_chunks(). */
static const unsigned char boot_weight[BOOT_STAGE_COUNT] =
{
    [BOOT_STORAGE]  = 2,
    [BOOT_MOUNT]    = 1,
    [BOOT_SETTINGS] = 1,
    [BOOT_THEME]    = 3,
    [BOOT_DIRCACHE] = 2,
    [BOOT_AUDIO]    = 2,
    [BOOT_SKINS]    = 2,
};

static const char *boot_caption;  /* NULL until a stage sets one */

/* A database commit is worth its own step count, so one commit step moves the
 * bar exactly one chunk. */
static int boot_stage_chunks(enum boot_stage stage) INIT_ATTR;
static int boot_stage_chunks(enum boot_stage stage)
{
    return stage == BOOT_TAGCACHE ? tagcache_get_max_commit_step()
                                  : boot_weight[stage];
}

/* Chunks done when `stage` starts; BOOT_STAGE_COUNT gives the whole bar. */
static int boot_chunks_done(enum boot_stage stage) INIT_ATTR;
static int boot_chunks_done(enum boot_stage stage)
{
    int chunks = 0;

    for (int i = 0; i < stage; i++)
        chunks += boot_stage_chunks(i);

    return chunks;
}

/* num/den divides the stage's own chunks, for a stage that reports progress
 * from inside itself; 0/0 for the whole of it. */
static int boot_step(enum boot_stage stage, int num, int den) INIT_ATTR;
static int boot_step(enum boot_stage stage, int num, int den)
{
    int step = boot_chunks_done(stage);

    if (den > 0)
        step += (boot_stage_chunks(stage) * MIN(num, den)) / den;

    return step;
}

static void boot_viewport(struct viewport *vp) INIT_ATTR;
static void boot_viewport(struct viewport *vp)
{
    /* Drawing goes through a viewport rather than the lcd_ calls, so the font
     * and colour used here are not left behind for whatever draws next.
     *
     * buffer and flags first: viewport_set_fullscreen() sets neither, and a
     * stack viewport carrying whatever was on the stack can silently swallow
     * every transfer to the LCD. viewport_set_defaults() would do it, but it
     * hands back the SBS area when a theme is enabled, and this wants the
     * whole screen. */
    vp->buffer = NULL;                  /* the default framebuffer */
    vp->flags = VP_DEFAULT_FLAGS;
    viewport_set_fullscreen(vp, SCREEN_MAIN);
    /* The built-in fixed-pitch font, not the theme's: it is compiled in, so it
     * needs neither the disk nor settings_apply() and is there from the first
     * paint. It is also small, which is what this caption wants -- it labels
     * the bar rather than competing with it. */
    vp->font = FONT_SYSFIXED;
    vp->drawmode = DRMODE_FG;
    vp->fg_pattern = boot_palette()->fg;
}

static void boot_paint(int step, int total) INIT_ATTR;
static void boot_paint(int step, int total)
{
    const struct boot_palette *pal = boot_palette();
    struct screen *screen = &screens[SCREEN_MAIN];
    struct viewport vp;
    struct viewport *last_vp;

    lcd_set_background(pal->bg);
    lcd_set_foreground(pal->fg);
    lcd_clear_display();

    boot_viewport(&vp);
    last_vp = screen->set_viewport(&vp);

    if (boot_caption)
    {
        int tw, th;
        screen->getstringsize(boot_caption, &tw, &th);
        screen->putsxy((vp.width - tw) / 2, BOOT_CAPTION_Y, boot_caption);
    }

    vp.fg_pattern = pal->accent;
    progress_bar_draw(screen, BOOT_BAR_X, BOOT_BAR_Y, BOOT_BAR_W, BOOT_BAR_H,
                      step, total, global_settings.progress_bar_radius);

    screen->set_viewport(last_vp);
    lcd_update();
}

/* The bar alone, over what the last full paint left on the LCD, because a
 * repaint four times a second is otherwise a full-screen blit each time. The
 * fill only ever grows, so drawing over the old one needs no clearing.
 *
 * Only valid while that paint is still on screen: anything that puts up a
 * screen of its own -- the tagcache thread's commit prompt -- has to be
 * followed by a full paint before this is used again. */
static void boot_paint_bar(int step, int total) INIT_ATTR;
static void boot_paint_bar(int step, int total)
{
    struct screen *screen = &screens[SCREEN_MAIN];
    struct viewport vp;
    struct viewport *last_vp;

    boot_viewport(&vp);
    vp.fg_pattern = boot_palette()->accent;
    last_vp = screen->set_viewport(&vp);

    progress_bar_draw(screen, BOOT_BAR_X, BOOT_BAR_Y, BOOT_BAR_W, BOOT_BAR_H,
                      step, total, global_settings.progress_bar_radius);

    screen->set_viewport(last_vp);
    lcd_update_rect(BOOT_BAR_X, BOOT_BAR_Y, BOOT_BAR_W, BOOT_BAR_H);
}

static void boot_progress(enum boot_stage stage, int num, int den,
                          const char *caption) INIT_ATTR;
static void boot_progress(enum boot_stage stage, int num, int den,
                          const char *caption)
{
    if (caption)
        boot_caption = caption;

    boot_paint(boot_step(stage, num, den),
               boot_chunks_done(BOOT_STAGE_COUNT));
}

static void boot_progress_bar(enum boot_stage stage, int num,
                              int den) INIT_ATTR;
static void boot_progress_bar(enum boot_stage stage, int num, int den)
{
    boot_paint_bar(boot_step(stage, num, den),
                   boot_chunks_done(BOOT_STAGE_COUNT));
}

/* dircache_wait() blocks, which would leave the bar dead for the length of a
 * scan. Poll instead and keep it moving: `size` grows as the cache is built,
 * and the previous build's size is a fair guess at where it will stop. A
 * first-ever build has no previous size and simply holds at the start of the
 * stage. */
static void dircache_boot_wait(void) INIT_ATTR;
static void dircache_boot_wait(void)
{
    struct dircache_info info;
    bool scanning = false;

    while (1)
    {
        dircache_get_info(&info);

        /* One sight of SCANNING first: the thread sets it a moment after
         * dircache_enable() hands back, so an IDLE seen straight away means
         * "not started yet", not "finished". */
        if (info.status == DIRCACHE_SCANNING)
            scanning = true;
        else if (scanning || info.status == DIRCACHE_READY)
            break;

        if (info.last_size > 0)
            boot_progress_bar(BOOT_DIRCACHE, (int)info.size,
                              (int)info.last_size);

        sleep(HZ/4);
    }

    dircache_wait();    /* it is done; reap the thread */
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
                boot_progress(BOOT_DIRCACHE, 0, 0, str(LANG_SCANNING_DISK));
                dircache_boot_wait();
                backlight_on();
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
    bool committed = false;

    /* Ahead of tagcache_init(), which starts the thread that may put up the
     * "commit now?" prompt -- painting after that point is what the loop
     * below has to avoid. */
    boot_progress(BOOT_TAGCACHE, 0, 0, str(LANG_WAIT));

    tagcache_init();
    db_summary_init();
    file_index_init();
    art_cache_init();

    while (!tagcache_is_initialized())
    {
        int ret = tagcache_get_commit_step();

        /* Nothing is drawn until the commit is actually running. That is what
         * keeps this off the screen while the tagcache thread's "commit now?"
         * prompt is up -- see tagcache_thread(). The first paint has to be a
         * full one for the same reason: the prompt may have owned the screen
         * up to this point. */
        if (ret > 0)
        {
            int max = tagcache_get_max_commit_step();

            if (committed)
                boot_progress_bar(BOOT_TAGCACHE, ret, max);
            else
                boot_progress(BOOT_TAGCACHE, ret, max,
                              str(LANG_TAGCACHE_INIT));
            committed = true;
        }
        sleep(HZ/4);
    }
    browser_db_init();

    if (committed)
        backlight_on();
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
#ifdef SIMULATOR
    /* The sim's own background thread: F5 screendumps, and the headphone and
     * hotswap triggers from its menu. */
    sim_tasks_init();
#endif
    lcd_init();
    FOR_NB_SCREENS(i)
        global_status.font_id[i] = FONT_SYSFIXED;
    font_init();

    settings_reset();

    /* Before the first paint: the boot palette is chosen from the clock. */
    rtc_init();

    /* Bare, and as early as possible. Nothing can be written under the bar
     * yet -- not for want of a font, since the caption uses the built-in one,
     * but because language_strings[] is not filled in until lang_init() below.
     * The first caption goes up at the stage after that. */
    CHART(">show_boot_screen");
    boot_progress(BOOT_STORAGE, 0, 0, NULL);
    CHART("<show_boot_screen");
    lang_init(core_language_builtin, language_strings,
              LANG_LAST_INDEX_IN_ARRAY);

#ifdef DEBUG
    debug_init();
#else
    serial_setup();
#endif

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
        boot_progress(BOOT_MOUNT, 0, 0, NULL);
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

    boot_progress(BOOT_SETTINGS, 0, 0, NULL);
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

    boot_progress(BOOT_THEME, 0, 0, NULL);
    CHART(">settings_apply(true)");
    settings_apply(true);
    CHART("<settings_apply(true)");
    boot_progress(BOOT_DIRCACHE, 0, 0, NULL);
    CHART(">init_dircache(false)");
    init_dircache(false);
    CHART("<init_dircache(false)");
    CHART(">init_tagcache");
    init_tagcache();
    CHART("<init_tagcache");

    playlist_init();
    browser_mem_init();
    filetype_init();
    tag_trim_init();

    shortcuts_init();

#ifdef USB_ENABLE_AUDIO
    /* The only safe moment for these: the settings are loaded, so the setting
     * can be read, and audio_init() below has not claimed the RAM yet, so
     * nothing has to be shrunk to make room. The driver cannot claim them
     * itself -- doing it from the USB thread wedges the player. Skipped
     * entirely when the setting is off, which is what keeps the ~129K off
     * everyone who does not use a USB-DAC. */
    if (global_settings.usb_audio != 0)
        usb_audio_alloc_buffers();
#endif

    boot_progress(BOOT_AUDIO, 0, 0, str(LANG_WAIT));
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
    /* The last stage, and a slow one -- parsing the WPS and SBS, then loading
     * their backdrops, bitmaps and fonts. It gets its own caption because the
     * bar sits nearly full for the whole of it, and LANG_WAIT left over from
     * audio_init() does not explain the pause. */
    boot_progress(BOOT_SKINS, 0, 0, str(LANG_APPLYING_THEME));
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

