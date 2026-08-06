/***************************************************************************
 * Original code from RockBox
 * was: apps/debug_menu.c
 * Copyright (C) 2002 Heikki Hannikainen
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * The debug menu and its screens: hardware state, buffers, threads, disk,
 * battery and view-log entries. Development aid, not user-facing.
 *
 * Almost every entry is one self-contained dbg_*() function that puts up a
 * list and returns when the user backs out, so entries are largely
 * independent of one another and of everything else -- read only the one you
 * care about. Screens that refresh live do so by returning a formatted string
 * from a getname callback the list widget polls, rather than drawing.
 *
 * The menu itself is assembled at the very bottom of the file; that is the
 * index to what is here.
 *
 * Parts, in order:
 *   - threads, OS and CPU state
 *   - buffering, buflib allocations and partitions
 *   - power: PMU registers, CPU frequency, battery history
 *   - disk: SMART attributes, identify data, dircache and tagcache stats
 *   - dumps and logs: ROM, screendump, metadata log
 *   - input, voice, USB and stored config
 *   - the menu declaration tying them together
 ****************************************************************************/

#include "config.h"
#include "timefuncs.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string-extra.h>
#include "lcd.h"
#include "lang.h"
#include "rbpaths.h"
#include "widgets/menu.h"
#include "debug_menu.h"
#include "kernel.h"
#include "input/action.h"
#include "debug.h"
#include "thread.h"
#include "powermgmt.h"
#include "system.h"
#include "font.h"
#include "audio.h"
#include "settings/settings.h"
#include "widgets/list.h"
#include "dir.h"
#include "panic.h"
#include "system/strutil.h"
#include "widgets/splash.h"
#include "screens/shortcuts.h"
#include "dircache.h"
#include "draw/viewport.h"
#include "database/tagcache.h"
#include "crc32.h"
#include "logf.h"
#include "disk.h"
#include "adc.h"
#include "usb.h"
#include "rtc.h"
#include "storage.h"
#include "fs_defines.h"
#include "eeprom_24cxx.h"
#include "ata.h"
#include "power.h"


#include "draw/scrollbar.h"
#include "audio/peak_meter.h"
#include "skin/skin_engine.h"
#include "log_viewer.h"
#include "core_alloc.h"
#include "audio/pcmbuf.h"
#include "audio/buffering.h"
#include "audio/playback.h"
#include "hwcompat.h"
#include "button.h"
#if CONFIG_RTC == RTC_PCF50605
#include "pcf50605.h"
#endif
#include "system/appevents.h"




#include "usb_core.h"
#include "usb_drv.h"
#ifdef USB_ENABLE_AUDIO
#include "../usbstack/usb_audio.h"
#endif

#include "speech/talk.h"


#if defined(IPOD_6G)
#include "norboot-target.h"
#endif

#include "iap.h"


#define SCREEN_MAX_CHARS (LCD_WIDTH / SYSFONT_WIDTH)

static const char* threads_getname(int selected_item, void *data,
                                   char *buffer, size_t buffer_len)
{
    int *x_offset = (int*) data;

#if NUM_CORES > 1
    if (selected_item < (int)NUM_CORES)
    {
        struct core_debug_info coreinfo;
        core_get_debug_info(selected_item, &coreinfo);
        snprintf(buffer, buffer_len, "Idle (%2d): %2d%%", selected_item,
                 coreinfo.idle_stack_usage);
        return buffer;
    }

    selected_item -= NUM_CORES;
#endif

    const char *fmtstr = "%2d: ---";

    struct thread_debug_info threadinfo;
    if (thread_get_debug_info(selected_item, &threadinfo) > 0)
    {
        fmtstr = "%2d:" IF_COP(" (%d)") " %s%n" IF_PRIO(" %2d %2d")
                 IFN_SDL(" %2d%% %2d%%") " %s";
    }
    int status_len;
    size_t len = snprintf(buffer, buffer_len, fmtstr,
             selected_item,
             IF_COP(threadinfo.core,)
             threadinfo.statusstr,
             &status_len,
             IF_PRIO(threadinfo.base_priority, threadinfo.current_priority,)
             IFN_SDL(threadinfo.stack_usage_cur, threadinfo.stack_usage,)
             threadinfo.name);

    int start = 0;
    (void) x_offset;
    (void) len;
    return &buffer[start];
}

static int dbg_threads_action_callback(int action, struct gui_synclist *lists)
{

    if (action == ACTION_NONE)
    {
        return ACTION_REDRAW;
    }
    (void) lists;
    return action;
}
/* Test code!!! */
static bool dbg_os(void)
{
    struct simplelist_info info;
    int xoffset = 0;

    simplelist_info_init(&info, IF_COP("Core and ") "Stack usage:",
                         MAXTHREADS IF_COP( + NUM_CORES ), &xoffset);
    info.scroll_all = false;
    info.action_callback = dbg_threads_action_callback;
    info.get_name = threads_getname;
    return simplelist_show_list(&info);
}

#ifdef __linux__
#include "cpuinfo-linux.h"

#define MAX_STATES 16
static struct time_state states[MAX_STATES];

static const char* get_cpuinfo(int selected_item, void *data,
                                   char *buffer, size_t buffer_len)
{
    (void)data;(void)buffer_len;
    const char* text;
    long time, diff;
    struct cpuusage us;
    static struct cpuusage last_us;
    int state_count = *(int*)data;

    if (cpuusage_linux(&us) != 0)
        return NULL;

    switch(selected_item)
    {
        case 0:
            diff = abs(last_us.usage - us.usage);
            sprintf(buffer, "Usage: %ld.%02ld%% (%c %ld.%02ld)",
                                    us.usage/100, us.usage%100,
                                    (us.usage >= last_us.usage) ? '+':'-',
                                    diff/100, diff%100);
            last_us.usage = us.usage;
            return buffer;
        case 1:
            text = "User";
            time = us.utime;
            diff = us.utime - last_us.utime;
            last_us.utime = us.utime;
            break;
        case 2:
            text = "Sys";
            time = us.stime;
            diff = us.stime - last_us.stime;
            last_us.stime = us.stime;
            break;
        case 3:
            text = "Real";
            time = us.rtime;
            diff = us.rtime - last_us.rtime;
            last_us.rtime = us.rtime;
            break;
        case 4:
            return "*** Per CPU freq stats ***";
        default:
        {
            int cpu = (selected_item - 5) / (state_count + 1);
            int cpu_line = (selected_item - 5) % (state_count + 1);

            /* scaling info */
            int min_freq = min_scaling_frequency(cpu);
            int cur_freq = current_scaling_frequency(cpu);
            /* fallback if scaling frequency is not available */
            if(cur_freq <= 0)
                cur_freq = frequency_linux(cpu);
            int max_freq = max_scaling_frequency(cpu);
            char governor[20];
            bool have_governor = current_scaling_governor(cpu, governor, sizeof(governor));
            if(cpu_line == 0)
            {
                sprintf(buffer,
                        " CPU%d: %s: %d/%d/%d MHz",
                        cpu,
                        have_governor ? governor : "Min/Cur/Max freq",
                        min_freq > 0 ? min_freq/1000 : -1,
                        cur_freq > 0 ? cur_freq/1000 : -1,
                        max_freq > 0 ? max_freq/1000 : -1);
            }
            else
            {
                cpustatetimes_linux(cpu, states, ARRAYLEN(states));
                snprintf(buffer, buffer_len, "   %ld %ld",
                            states[cpu_line-1].frequency,
                            states[cpu_line-1].time);
            }
            return buffer;
        }
    }
    sprintf(buffer, "%s: %ld.%02lds (+ %ld.%02ld)", text,
                    time / us.hz, time % us.hz,
                    diff / us.hz, diff % us.hz);
    return buffer;
}

static int cpuinfo_cb(int action, struct gui_synclist *lists)
{
    (void)lists;
    if (action == ACTION_NONE)
        action = ACTION_REDRAW;
    return action;
}

static bool dbg_cpuinfo(void)
{
    struct simplelist_info info;
    int cpu_count = MAX(cpucount_linux(), 1);
    int state_count = cpustatetimes_linux(0, states, ARRAYLEN(states));
    printf("%s(): %d %d\n", __func__, cpu_count, state_count);
    simplelist_info_init(&info, "CPU info:", 5 + cpu_count*(state_count+1), &state_count);
    info.get_name = get_cpuinfo;
    info.action_callback = cpuinfo_cb;
    info.timeout = HZ;
    info.scroll_all = true;
    return simplelist_show_list(&info);
}

#endif

static unsigned int ticks, freq_sum;
static unsigned int boost_ticks;

static void dbg_audio_task(void)
{
#ifdef CPUFREQ_NORMAL
    if(FREQ > CPUFREQ_NORMAL)
        boost_ticks++;
    freq_sum += FREQ/1000000; /* in MHz */
#endif
    ticks++;
}

/* A reading of all the counters above at one instant. The tick task keeps
 * running once started, so differencing the live counters against a snapshot
 * gives the rates over any chosen window. The window that matters is the one
 * between leaving this screen and returning to it, since that is the only way
 * to measure a screen you cannot be sitting in while you read the numbers. */
struct cpu_sample
{
    unsigned int ticks;
    unsigned int boost_ticks;
    unsigned int freq_sum;
    unsigned int flushes;
    unsigned int render_usec;
    unsigned int flush_usec;
};

static struct cpu_sample sample_on_close;

static void cpu_sample_take(struct cpu_sample *s)
{
    s->ticks = ticks;
    s->boost_ticks = boost_ticks;
    s->freq_sum = freq_sum;
    s->flushes = skin_flush_count();
    s->render_usec = skin_render_usec();
    s->flush_usec = skin_flush_usec();
}

static bool dbg_buffering_thread(void)
{
    int button;
    int line;
    bool done = false;
    size_t bufused;
    size_t bufsize = pcmbuf_get_bufsize();
    int pcmbufdescs = pcmbuf_descs();
    struct buffering_debug d;
    size_t filebuflen = audio_get_filebuflen();
    static bool task_running = false;
    struct cpu_sample sample_on_open;
    unsigned int away_ticks, away_frames = 0;
    int away_boost = 0, away_clock = 0, away_flush = 0;
    int away_render_ms = 0, away_flush_ms = 0;
    /* This is a size_t, but call it a long so it puts a - when it's bad. */
    #define STR_DATAREM "data_rem"
    const char * const fmt_used = "%s: %6ld/%ld";

    /* Added once and never removed: the counters have to keep running while
     * this screen is closed, or there is nothing to report on return. */
    if (!task_running)
    {
        tick_add_task(dbg_audio_task);
        task_running = true;
    }

    cpu_sample_take(&sample_on_open);

    /* Fixed for the whole visit: what happened while the screen was shut. */
    away_ticks = sample_on_open.ticks - sample_on_close.ticks;
    if (away_ticks > 0)
    {
        away_boost = (sample_on_open.boost_ticks - sample_on_close.boost_ticks)
                     * 1000 / away_ticks;                  /* in 0.1 % */
        away_clock = (sample_on_open.freq_sum - sample_on_close.freq_sum)
                     * 10 / away_ticks;                    /* in 100 kHz */
        away_flush = (sample_on_open.flushes - sample_on_close.flushes)
                     * 10 * HZ / away_ticks;               /* in 0.1 /s */
    }

    /* Averaged per frame rather than per second, so the split does not move
     * just because the screen was busier: what matters is where the time in
     * one redraw goes. Both in 0.1 ms. */
    away_frames = sample_on_open.flushes - sample_on_close.flushes;
    if (away_frames > 0)
    {
        away_render_ms = (sample_on_open.render_usec
                          - sample_on_close.render_usec) / away_frames / 100;
        away_flush_ms = (sample_on_open.flush_usec
                         - sample_on_close.flush_usec) / away_frames / 100;
    }

    FOR_NB_SCREENS(i)
        screens[i].setfont(FONT_SYSFIXED);

    while(!done)
    {
        button = get_action(CONTEXT_STD,HZ/5);
        switch(button)
        {
            case ACTION_STD_NEXT:
                audio_next();
                break;
            case ACTION_STD_PREV:
                audio_prev();
                break;
            case ACTION_STD_CANCEL:
                done = true;
                break;
        }

        buffering_get_debugdata(&d);
        bufused = bufsize - pcmbuf_free();

        FOR_NB_SCREENS(i)
        {
            line = 0;
            screens[i].clear_display();


            screens[i].putsf(0, line++, fmt_used, "pcm", (long) bufused, (long) bufsize);

            gui_scrollbar_draw(&screens[i],0, line*SYSFONT_HEIGHT, screens[i].lcdwidth, 6,
                               bufsize, 0, bufused, HORIZONTAL);
            line++;

            screens[i].putsf(0, line++, fmt_used, "alloc", audio_filebufused(),
                            (long) filebuflen);

            if (screens[i].lcdheight > 80)
            {
                gui_scrollbar_draw(&screens[i],0, line*SYSFONT_HEIGHT, screens[i].lcdwidth, 6,
                                   filebuflen, 0, audio_filebufused(), HORIZONTAL);
                line++;

                screens[i].putsf(0, line++, fmt_used, "real", (long)d.buffered_data,
                                (long)filebuflen);

                gui_scrollbar_draw(&screens[i],0, line*SYSFONT_HEIGHT, screens[i].lcdwidth, 6,
                                   filebuflen, 0, (long)d.buffered_data, HORIZONTAL);
                line++;
            }

            screens[i].putsf(0, line++, fmt_used, "usefl", (long)(d.useful_data),
                                                       (long)filebuflen);

            if (screens[i].lcdheight > 80)
            {
                gui_scrollbar_draw(&screens[i],0, line*SYSFONT_HEIGHT, screens[i].lcdwidth, 6,
                                   filebuflen, 0, d.useful_data, HORIZONTAL);
                line++;
            }

            screens[i].putsf(0, line++, "%s: %ld", STR_DATAREM, (long)d.data_rem);

            screens[i].putsf(0, line++, "track count: %2u", audio_track_count());

            screens[i].putsf(0, line++, "handle count: %d", (int)d.num_handles);

            screens[i].putsf(0, line++, "cpu freq: %3dMHz",
                             (int)((FREQ + 500000) / 1000000));

            unsigned int here_ticks = ticks - sample_on_open.ticks;
            if (here_ticks > 0)
            {
                /* in 100 kHz */
                int avgclock   = (freq_sum - sample_on_open.freq_sum)
                                 * 10 / here_ticks;
                /* in 0.1 % */
                int boostquota = (boost_ticks - sample_on_open.boost_ticks)
                                 * 1000 / here_ticks;
                screens[i].putsf(0, line++, "boost:%3d.%d%% (%d.%dMHz)",
                                 boostquota/10, boostquota%10, avgclock/10, avgclock%10);
            }
            if (away_ticks > 0)
            {
                screens[i].putsf(0, line++, "away:%3d.%d%% %d.%dMHz %d.%dfl/s",
                                 away_boost/10, away_boost%10,
                                 away_clock/10, away_clock%10,
                                 away_flush/10, away_flush%10);
            }
            if (away_frames > 0)
            {
                screens[i].putsf(0, line++, "frame: rnd%d.%dms fls%d.%dms",
                                 away_render_ms/10, away_render_ms%10,
                                 away_flush_ms/10, away_flush_ms%10);
            }

            screens[i].putsf(0, line++, "pcmbufdesc: %2d/%2d",
                             pcmbuf_used_descs(), pcmbufdescs);
            screens[i].putsf(0, line++, "watermark: %6d",
                             (int)(d.watermark));

            screens[i].update();
        }
    }

    /* Mark the point of departure rather than stopping the counters, so the
     * next visit can report on whatever screen was used in between. */
    cpu_sample_take(&sample_on_close);

    FOR_NB_SCREENS(i)
        screens[i].setfont(FONT_UI);

    return false;
#undef STR_DATAREM
}

#ifdef BUFLIB_DEBUG_PRINT
static const char* bf_getname(int selected_item, void *data,
                                   char *buffer, size_t buffer_len)
{
    (void)data;
    core_print_block_at(selected_item, buffer, buffer_len);
    return buffer;
}

static int bf_action_cb(int action, struct gui_synclist* list)
{
    if (action == ACTION_STD_OK)
    {
        if (gui_synclist_get_sel_pos(list) == 0 && core_test_free())
        {
            splash(HZ, "Freed test handle. New alloc should trigger compact");
        }
        else
        {
            splash(HZ/1, "Attempting a 64k allocation");
            int handle = core_alloc(64<<10);
            splash(HZ/2, (handle > 0) ? "Success":"Fail");
            /* for some reason simplelist doesn't allow adding items here if
             * info.get_name is given, so use normal list api */
            gui_synclist_set_nb_items(list, core_get_num_blocks());
            core_free(handle);
        }
        action = ACTION_REDRAW;
    }
    return action;
}

static bool dbg_buflib_allocs(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, "mem allocs", core_get_num_blocks(), NULL);
    info.get_name = bf_getname;
    info.action_callback = bf_action_cb;
    info.timeout = HZ;
    return simplelist_show_list(&info);
}
#endif /* BUFLIB_DEBUG_PRINT */

static const char* dbg_partitions_getname(int selected_item, void *data,
                                          char *buffer, size_t buffer_len)
{
    (void)data;
    int partition = selected_item/2;

    struct partinfo p;
    if (!disk_partinfo(partition, &p))
        return buffer;

    /* XXX fix this up to use logical sector size */
    /* XXX and if mounted, show free info... */
    if (selected_item%2)
    {
        snprintf(buffer, buffer_len, "   T:%x %lu MB", p.type,
                 (unsigned long)(p.size / ( 2048 / ( SECTOR_SIZE / 512 ))));
    }
    else
    {
        snprintf(buffer, buffer_len, "P%d: S:%llx", partition, (unsigned long long)p.start);
    }
    return buffer;
}

static bool dbg_partitions(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, "Partition Info", NUM_DRIVES * MAX_PARTITIONS_PER_DRIVE, NULL);
    info.selection_size = 2;
    info.scroll_all = true;
    info.get_name = dbg_partitions_getname;
    return simplelist_show_list(&info);
}


#if CONFIG_RTC == RTC_PCF50605
static bool dbg_pcf(void)
{
    int line;

    lcd_setfont(FONT_SYSFIXED);
    lcd_clear_display();

    while(1)
    {
        line = 0;

        lcd_putsf(0, line++, "DCDC1:  %02x", pcf50605_read(0x1b));
        lcd_putsf(0, line++, "DCDC2:  %02x", pcf50605_read(0x1c));
        lcd_putsf(0, line++, "DCDC3:  %02x", pcf50605_read(0x1d));
        lcd_putsf(0, line++, "DCDC4:  %02x", pcf50605_read(0x1e));
        lcd_putsf(0, line++, "DCDEC1: %02x", pcf50605_read(0x1f));
        lcd_putsf(0, line++, "DCDEC2: %02x", pcf50605_read(0x20));
        lcd_putsf(0, line++, "DCUDC1: %02x", pcf50605_read(0x21));
        lcd_putsf(0, line++, "DCUDC2: %02x", pcf50605_read(0x22));
        lcd_putsf(0, line++, "IOREGC: %02x", pcf50605_read(0x23));
        lcd_putsf(0, line++, "D1REGC: %02x", pcf50605_read(0x24));
        lcd_putsf(0, line++, "D2REGC: %02x", pcf50605_read(0x25));
        lcd_putsf(0, line++, "D3REGC: %02x", pcf50605_read(0x26));
        lcd_putsf(0, line++, "LPREG1: %02x", pcf50605_read(0x27));
        lcd_update();
        if (action_userabort(HZ/10))
        {
            lcd_setfont(FONT_UI);
            return false;
        }
    }

    lcd_setfont(FONT_UI);
    return false;
}
#endif

static bool dbg_cpufreq(void)
{
    int line;
    int button;
    int x = 0;
    bool done = false;

    lcd_setfont(FONT_SYSFIXED);
    lcd_clear_display();

    while(!done)
    {
        line = 0;

        int temp = FREQ / 1000;
        lcd_putsf(x, line++, "Frequency: %ld.%ld MHz", temp / 1000, temp % 1000);
        lcd_putsf(x, line++, "boost_counter: %d", get_cpu_boost_counter());


        lcd_update();
        button = get_action(CONTEXT_STD,HZ/10);

        switch(button)
        {
            case ACTION_STD_PREV:
                cpu_boost(true);
                break;

            case ACTION_STD_NEXT:
                cpu_boost(false);
                break;
            case ACTION_STD_MENU:
                x--;
                break;
            case ACTION_STD_OK:
                x = 0;
                while (get_cpu_boost_counter() > 0)
                    cpu_boost(false);
                set_cpu_frequency(CPUFREQ_DEFAULT);
                break;

            case ACTION_STD_CANCEL:
                done = true;;
        }
        lcd_clear_display();
    }
    lcd_setfont(FONT_UI);
    return false;
}

#if (CONFIG_BATTERY_MEASURE != 0)
/*
 * view_battery() shows a automatically scaled graph of the battery voltage
 * over time. Usable for estimating battery life / charging rate.
 * The power_history array is updated in power_thread of powermgmt.c.
 */

#define BAT_LAST_VAL  MIN(LCD_WIDTH, POWER_HISTORY_LEN)
#define BAT_TSPACE    20
#define BAT_YSPACE    (LCD_HEIGHT - BAT_TSPACE)


static bool view_battery(void)
{
    extern struct battery_tables_t device_battery_tables; /* powermgmt.c */
    unsigned short *power_history = device_battery_tables.history;
    int view = 0;
    int i, x, y, z, y1, y2, grid, graph;
    unsigned short maxv, minv;
    lcd_setfont(FONT_SYSFIXED);

    while(1)
    {
        lcd_clear_display();
        switch (view) {
            case 0: /* voltage history graph */
                /* Find maximum and minimum voltage for scaling */
                minv = power_history[0];
                maxv = minv + 1;
                for (i = 1; i < BAT_LAST_VAL && power_history[i]; i++) {
                    if (power_history[i] > maxv)
                        maxv = power_history[i];
                    if (power_history[i] < minv)
                        minv = power_history[i];
                }
                /* print header */
                /* adjust grid scale */
                if ((maxv - minv) > 50)
                    grid = 50;
                else
                    grid = 5;

                lcd_putsf(0, 0, "%s %d.%03dV", "Battery", power_history[0] / 1000,
                         power_history[0] % 1000);
                lcd_putsf(0, 1, "%d.%03d-%d.%03dV (%2dmV)",
                          minv / 1000, minv % 1000, maxv / 1000, maxv % 1000,
                          grid);

                i = 1;
                while ((y = (minv - (minv % grid)+i*grid)) < maxv)
                {
                    graph = ((y-minv)*BAT_YSPACE)/(maxv-minv);
                    graph = LCD_HEIGHT-1 - graph;

                    /* draw dotted horizontal grid line */
                    for (x=0; x<LCD_WIDTH;x=x+2)
                        lcd_drawpixel(x,graph);

                    i++;
                }

                x = 0;
                /* draw plot of power history
                 * skip empty entries
                 */
                for (i = BAT_LAST_VAL - 1; i > 0; i--)
                {
                    if (power_history[i] && power_history[i-1])
                    {
                        y1 = (power_history[i] - minv) * BAT_YSPACE /
                            (maxv - minv);
                        y1 = MIN(MAX(LCD_HEIGHT-1 - y1, BAT_TSPACE),
                                 LCD_HEIGHT-1);
                        y2 = (power_history[i-1] - minv) * BAT_YSPACE /
                            (maxv - minv);
                        y2 = MIN(MAX(LCD_HEIGHT-1 - y2, BAT_TSPACE),
                                 LCD_HEIGHT-1);

                        lcd_set_drawmode(DRMODE_SOLID);

                        /* make line thicker */
                        lcd_drawline(((x*LCD_WIDTH)/(BAT_LAST_VAL)),
                                     y1,
                                     (((x+1)*LCD_WIDTH)/(BAT_LAST_VAL)),
                                     y2);
                        lcd_drawline(((x*LCD_WIDTH)/(BAT_LAST_VAL))+1,
                                     y1+1,
                                     (((x+1)*LCD_WIDTH)/(BAT_LAST_VAL))+1,
                                     y2+1);
                        x++;
                    }
                }
                break;

            case 1: /* status: */
                lcd_putsf(0, 0, "Pwr status: %s",
                         charging_state() ? "charging" : "discharging");
                battery_read_info(&y, &z);
                if (y > 0)
                    lcd_putsf(0, 1, "%s: %d.%03d V (%d %%)", "Battery", y / 1000, y % 1000, z);
                else if (z > 0)
                    lcd_putsf(0, 1, "%s: %d %%", "Battery", z);
#if defined IPOD_VIDEO
                int usb_pwr  = (GPIOL_INPUT_VAL & 0x10)?true:false;
                int ext_pwr  = (GPIOL_INPUT_VAL & 0x08)?false:true;
                int dock     = (GPIOA_INPUT_VAL & 0x10)?true:false;
                int charging = (GPIOB_INPUT_VAL & 0x01)?false:true;
                int headphone= (GPIOA_INPUT_VAL & 0x80)?true:false;

                lcd_putsf(0, 3, "USB pwr:   %s",
                            usb_pwr ? "present" : "absent");
                lcd_putsf(0, 4, "EXT pwr:   %s",
                            ext_pwr ? "present" : "absent");
                lcd_putsf(0, 5, "%s:   %s", "Battery",
                    charging ? "charging" : (usb_pwr||ext_pwr) ? "charged" : "discharging");
                lcd_putsf(0, 6, "Dock mode: %s",
                         dock    ? "enabled" : "disabled");
                lcd_putsf(0, 7, "Headphone: %s",
                         headphone ? "connected" : "disconnected");
#ifdef IPOD_VIDEO
                if(probed_ramsize == 64)
                    x = (adc_read(ADC_4066_ISTAT) * 2400) / (1024 * 2);
                else
#endif
                    x = (adc_read(ADC_4066_ISTAT) * 2400) / (1024 * 3);
                lcd_putsf(0, 8, "Ibat: %d mA", x);
                lcd_putsf(0, 9, "Vbat * Ibat: %d mW", x * y / 1000);
#else
                lcd_putsf(0, 3, "Charger: %s",
                         charger_inserted() ? "present" : "absent");
                lcd_putsf(0, 4, "USB current limit: %d mA",
                          usb_charging_maxcurrent());
#endif /* target type */
                break;
            case 2: /* voltage deltas: */
                lcd_puts(0, 0, "Voltage deltas:");
                for (i = 0; i < POWER_HISTORY_LEN-1; i++) {
                    y = power_history[i] - power_history[i+1];
                    lcd_putsf(0, i+1, "-%d min: %c%d.%03d V", i,
                             (y < 0) ? '-' : ' ', ((y < 0) ? y * -1 : y) / 1000,
                             ((y < 0) ? y * -1 : y ) % 1000);
                }
                break;

            case 3: /* remaining time estimation: */

                lcd_putsf(0, 5, "Last PwrHist: %d.%03dV",
                    power_history[0] / 1000,
                    power_history[0] % 1000);

                lcd_putsf(0, 6, "%s level: %d%%", "Battery", battery_level());

                int time_left = battery_time();
                if (time_left >= 0)
                    lcd_putsf(0, 7, "Est. remain: %d m", time_left);
                else
                    lcd_puts(0, 7, "Estimation n/a");

#if (CONFIG_BATTERY_MEASURE & CURRENT_MEASURE)
                lcd_putsf(0, 8, "%s current: %d mA", "Battery", battery_current());
#endif
                break;
        }

        lcd_update();

        switch(get_action(CONTEXT_STD,HZ/2))
        {
            case ACTION_STD_PREV:
                if (view)
                    view--;
                break;

            case ACTION_STD_NEXT:
                if (view < 3)
                    view++;
                break;

            case ACTION_STD_CANCEL:
                lcd_setfont(FONT_UI);
                return false;
        }
    }
    lcd_setfont(FONT_UI);
    return false;
}

#endif /* (CONFIG_BATTERY_MEASURE != 0)  */

static int disk_callback(int btn, struct gui_synclist *lists)
{
    static const char atanums[] = { " 0 1 2 3 4 5 6" };

    (void)lists;
    int i;
    char buf[128];
    unsigned short* identify_info = ata_get_identify();
    bool timing_info_present = false;
    (void)btn;

    simplelist_reset_lines();

    for (i=0; i < 20; i++)
        ((unsigned short*)buf)[i]=htobe16(identify_info[i+27]);
    buf[40]=0;
    /* kill trailing space */
    for (i=39; i && buf[i]==' '; i--)
        buf[i] = 0;
    simplelist_addline("Model: %s", buf);
    for (i=0; i < 10; i++)
        ((unsigned short*)buf)[i]=htobe16(identify_info[i+10]);
    buf[20]=0;
    /* kill trailing space */
    for (i=19; i && buf[i]==' '; i--)
        buf[i] = 0;
    simplelist_addline("Serial number: %s", buf);
    for (i=0; i < 4; i++)
        ((unsigned short*)buf)[i]=htobe16(identify_info[i+23]);
    buf[8]=0;
    simplelist_addline(
             "Firmware: %s", buf);

    uint64_t total_sectors = (identify_info[61] << 16) | identify_info[60];
    if (identify_info[83] & 0x0400 && total_sectors == 0x0FFFFFFF)
        total_sectors = ((uint64_t)identify_info[103] << 48) |
                ((uint64_t)identify_info[102] << 32) |
                ((uint64_t)identify_info[101] << 16) |
                identify_info[100];

    uint32_t sector_size;

    /* Logical sector size > 512B ? */
    if ((identify_info[106] & 0xd000) == 0x5000) /* B14, B12 */
        sector_size = (identify_info[117] | (identify_info[118] << 16)) * 2;
    else
        sector_size = 512;

    total_sectors *= sector_size;   /* Convert to bytes */
    total_sectors /= (1024 * 1024); /* Convert to MB */

    simplelist_addline("Size: %lu MB", (unsigned long)total_sectors);
    simplelist_addline("Logical sector size: %lu B", sector_size);
#ifdef MAX_VIRT_SECTOR_SIZE
    simplelist_addline("Sector multiplier: %u", disk_get_sector_multiplier());
#endif

    if((identify_info[106] & 0xe000) == 0x6000) /* B14, B13 */
        sector_size *= BIT_N(identify_info[106] & 0x000f);
    simplelist_addline(
            "Physical sector size: %lu B", sector_size);


    simplelist_addline("SSD detected: %s", ata_disk_isssd() ? "yes" : "no");
    simplelist_addline(
             "Spinup time: %d ms", storage_spinup_time() * (1000/HZ));
    i = identify_info[82] & (1<<3);
    simplelist_addline(
             "Power mgmt: %s", i ? "enabled" : "unsupported");
    i = identify_info[83] & (1<<3);
    simplelist_addline(
             "Adv Power mgmt: %s", i ? "enabled" : "unsupported");
    i = identify_info[83] & (1<<9);
    simplelist_addline(
             "Noise mgmt: %s", i ? "enabled" : "unsupported");
    i = identify_info[85] & (1<<0);
    simplelist_addline(
             "SMART: %s", i ? "enabled" : "unsupported");
    simplelist_addline(
             "Flush cache: %s", identify_info[83] & (1<<13) ? "extended" : identify_info[83] & (1<<12) ? "standard" : identify_info[80] >= (1<<5) ? "ATA-5" : "unsupported");
    i = identify_info[82] & (1<<6);
    simplelist_addline(
             "Read-ahead: %s", i ? "enabled" : "unsupported");
    timing_info_present = identify_info[53] & (1<<1);
    if(timing_info_present) {
        simplelist_addline(
                 "PIO modes: 0 1 2%.*s%.*s",
                 (identify_info[64] & (1<<0)) << 1, &atanums[3*2],
                 (identify_info[64] & (1<<1))     , &atanums[4*2]);
    }
    else {
        simplelist_setline(
                 "No PIO mode info");
    }
    timing_info_present = identify_info[53] & (1<<1);
    if(timing_info_present) {
        simplelist_addline(
                 "Cycle times %dns/%dns",
                 identify_info[67],
                 identify_info[68] );
    } else {
        simplelist_setline(
                 "No timing info");
    }

    if (identify_info[63] & (1<<0)) {
        simplelist_addline(
                 "MDMA modes:%.*s%.*s%.*s",
                  (identify_info[63] & (1<<0)) << 1, &atanums[0*2],
                  (identify_info[63] & (1<<1))     , &atanums[1*2],
                  (identify_info[63] & (1<<2)) >> 1, &atanums[2*2]);
        simplelist_addline(
                 "MDMA Cycle times %dns/%dns",
                 identify_info[65],
                 identify_info[66] );
    }
    else {
        simplelist_setline(
                "No MDMA mode info");
    }
    if (identify_info[53] & (1<<2)) {
        simplelist_addline(
                 "UDMA modes:%.*s%.*s%.*s%.*s%.*s%.*s%.*s",
                 (identify_info[88] & (1<<0)) << 1, &atanums[0*2],
                 (identify_info[88] & (1<<1))     , &atanums[1*2],
                 (identify_info[88] & (1<<2)) >> 1, &atanums[2*2],
                 (identify_info[88] & (1<<3)) >> 2, &atanums[3*2],
                 (identify_info[88] & (1<<4)) >> 3, &atanums[4*2],
                 (identify_info[88] & (1<<5)) >> 4, &atanums[5*2],
                 (identify_info[88] & (1<<6)) >> 5, &atanums[6*2]);
    }
    else {
        simplelist_setline("No UDMA mode info");
    }
    timing_info_present = identify_info[53] & (1<<1);
    if(timing_info_present) {
        i = identify_info[49] & (1<<11);
        simplelist_addline(
            "IORDY support: %s", i ? "yes" : "no");
        i = identify_info[49] & (1<<10);
        simplelist_addline(
                "IORDY disable: %s", i ? "yes" : "no");
    } else {
        simplelist_setline("No timing info");
    }
    simplelist_addline(
             "Cluster size: %d bytes", volume_get_cluster_size(IF_MV(0)));
    i = ata_get_dma_mode();
    if (i == 0) {
        simplelist_setline("DMA not enabled");
    } else if (i == 0xff) {
        simplelist_setline("CE-ATA mode");
    } else {
        simplelist_addline(
                 "DMA mode: %s %c",
                (i & 0x40) ? "UDMA" : "MDMA",
                '0' + (i & 7));
    }
    i = identify_info[83] & (1 << 2);
    simplelist_addline(
            "CFA compatible: %s", i ? "yes" : "no");
    i = identify_info[0] & (1 << 6);
    simplelist_addline(
            "Fixed device: %s", i ? "yes" : "no");
    i = identify_info[0] & (1 << 7);
    simplelist_addline(
            "Removeable media: %s", i ? "yes" : "no");

    return btn;
}

static struct ata_smart_values smart_data STORAGE_ALIGN_ATTR;

static const char * ata_smart_get_attr_name(unsigned char id)
{
    if (id == 1)    return "Raw Read Error Rate";
    if (id == 2)    return "Throughput Performance";
    if (id == 3)    return "Spin-Up Time";
    if (id == 4)    return "Start/Stop Count";
    if (id == 5)    return "Reallocated Sector Count";
    if (id == 7)    return "Seek Error Rate";
    if (id == 8)    return "Seek Time Performance";
    if (id == 9)    return "Power-On Hours Count";
    if (id == 10)   return "Spin-Up Retry Count";
    if (id == 12)   return "Power Cycle Count";
    if (id == 191)  return "G-Sense Error Rate";
    if (id == 192)  return "Power-Off Retract Count";
    if (id == 193)  return "Load/Unload Cycle Count";
    if (id == 194)  return "HDA Temperature";
    if (id == 195)  return "Hardware ECC Recovered";
    if (id == 196)  return "Reallocated Event Count";
    if (id == 197)  return "Current Pending Sector Count";
    if (id == 198)  return "Uncorrectable Sector Count";
    if (id == 199)  return "UDMA CRC Error Count";
    if (id == 200)  return "Write Error Rate";
    if (id == 201)  return "TA Counter Detected";
    if (id == 220)  return "Disk Shift";
    if (id == 222)  return "Loaded Hours";
    if (id == 223)  return "Load/Unload Retry Count";
    if (id == 224)  return "Load Friction";
    if (id == 225)  return "Load Cycle Count";
    if (id == 226)  return "Load-In Time";
    if (id == 240)  return "Transfer Error Rate";   /* Fujitsu */
    return "Unknown Attribute";
};

static int ata_smart_get_attr_rawfmt(unsigned char id)
{
    if (id == 3)      /* Spin-up time */
        return RAWFMT_RAW16_OPT_AVG16;

    if (id == 5 ||    /* Reallocated sector count */
        id == 196)    /* Reallocated event count */
        return RAWFMT_RAW16_OPT_RAW16;

    if (id == 190 ||  /* Airflow Temperature */
        id == 194)    /* HDA Temperature */
        return RAWFMT_TEMPMINMAX;

    return RAWFMT_RAW48;
};

static int ata_smart_attr_to_string(
                struct ata_smart_attribute *attr, char *str, int size)
{
    uint16_t w[3]; /* 3 words to store 6 bytes of raw data */
    char buf[size]; /* temp string to store attribute data */
    int len, slen;
    int id = attr->id;

    if (id == 0)
        return 0; /* null attribute */

    /* align and convert raw data */
    memcpy(w, attr->raw, 6);
    w[0] = letoh16(w[0]);
    w[1] = letoh16(w[1]);
    w[2] = letoh16(w[2]);

    len = snprintf(buf, size, ": %u,%u ", attr->current, attr->worst);

    switch (ata_smart_get_attr_rawfmt(id))
    {
        case RAWFMT_RAW16_OPT_RAW16:
            len += snprintf(buf+len, size-len, "%u", w[0]);
            if ((w[1] || w[2]) && (len < size))
                len += snprintf(buf+len, size-len, " %u %u", w[1],w[2]);
            break;

        case RAWFMT_RAW16_OPT_AVG16:
            len += snprintf(buf+len, size-len, "%u", w[0]);
            if (w[1] && (len < size))
                len += snprintf(buf+len, size-len, " Avg: %u", w[1]);
            break;

        case RAWFMT_TEMPMINMAX:
            len += snprintf(buf+len, size-len, "%u -/+: %u/%u", w[0],w[1],w[2]);
            break;

        case RAWFMT_RAW48:
        default: {
            uint32_t tmp;
            memcpy(&tmp, w, sizeof(tmp));
            /* shows first 4 bytes of raw data as uint32 LE,
               and the ramaining 2 bytes as uint16 LE */
            len += snprintf(buf+len, size-len, "%lu", letoh32(tmp));
            if (w[2] && (len < size))
                len += snprintf(buf+len, size-len, " %u", w[2]);
            break;
        }
    }
    /* ignore trailing \0 when truncated */
    if (len >= size) len = size-1;

    /* fill return string; when max. size is exceded: first truncate
       attribute name, then attribute data and finally attribute id */
    slen = snprintf(str, size, "%d ", id);
    if (slen < size) {
        /* maximum space disponible for attribute name,
           including initial space separator */
        int name_sz = size - (slen + len);
        if (name_sz > 1) {
            len = snprintf(str+slen, name_sz, " %s",
                           ata_smart_get_attr_name(id));
            if (len >= name_sz) len = name_sz-1;
            slen += len;
        }

        strmemccpy(str+slen, buf, size-slen);
    }

    return 1; /* ok */
}

static bool ata_smart_dump(void)
{
    int fd;

    fd = creat("/smart_data.bin", 0666);
    if(fd >= 0)
    {
        write(fd, &smart_data, sizeof(struct ata_smart_values));
        close(fd);
    }

    fd = creat("/smart_data.txt", 0666);
    if(fd >= 0)
    {
        int i;
        char buf[128];
        for (i = 0; i < NUMBER_ATA_SMART_ATTRIBUTES; i++)
        {
            if (ata_smart_attr_to_string(
                    &smart_data.vendor_attributes[i], buf, sizeof(buf)))
            {
                write(fd, buf, strlen(buf));
                write(fd, "\n", 1);
            }
        }
        close(fd);
    }

    return false;
}

static int ata_smart_callback(int btn, struct gui_synclist *lists)
{
    (void)lists;
    static bool read_done = false;

    if (btn == ACTION_STD_CANCEL)
    {
        read_done = false;
        return btn;
    }

    /* read S.M.A.R.T. data only on first redraw */
    if (!read_done)
    {
        int rc;
        memset(&smart_data, 0, sizeof(struct ata_smart_values));
        rc = ata_read_smart(&smart_data, ATA_SMART_READ_DATA);
        simplelist_reset_lines();
        if (rc == 0)
        {
            int i;
            char buf[SIMPLELIST_MAX_LINELENGTH];
            simplelist_setline("Id  Name:  Current,Worst  Raw");
            for (i = 0; i < NUMBER_ATA_SMART_ATTRIBUTES; i++)
            {
                if (ata_smart_attr_to_string(
                        &smart_data.vendor_attributes[i], buf, sizeof(buf)))
                {
                    simplelist_addline(buf);
                }
            }
        }
        else
        {
            simplelist_addline("ATA SMART error: %#x", rc);
        }
        read_done = true;
    }

    if (btn == ACTION_STD_CONTEXT)
    {
        splash(0, "Dumping data...");
        ata_smart_dump();
        splash(HZ, "SMART data dumped");
    }

    return btn;
}

static bool dbg_ata_smart(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, "S.M.A.R.T. Data [CONTEXT to dump]", 1, NULL);
    info.action_callback = ata_smart_callback;
    info.scroll_all = true;
    return simplelist_show_list(&info);
}

static bool dbg_identify_info(void)
{
    int fd = creat("/identify_info.bin", 0666);
    if(fd >= 0)
    {
        const unsigned short *identify_info = ata_get_identify();
        /* this is a pointer to a driver buffer so we can't modify it */
        for (int i = 0; i < ATA_IDENTIFY_WORDS; ++i)
        {
            unsigned short word = swap16(identify_info[i]);
            write(fd, &word, 2);
        }
        close(fd);
    }
    return false;
}

static bool dbg_disk_info(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, "Disk Info", 1, NULL);
    info.action_callback = disk_callback;
    info.scroll_all = true;
    return simplelist_show_list(&info);
}

static int dircache_callback(int btn, struct gui_synclist *lists)
{
    (void)lists;
    struct dircache_info info;
    dircache_get_info(&info);

    if (global_settings.dircache)
    {
        switch (btn)
        {
        case ACTION_STD_CONTEXT:
            splash(HZ/2, "Rebuilding cache");
            dircache_suspend();
            *(int *)lists->data = dircache_resume();
            /* Fallthrough */
        case ACTION_UNKNOWN:
            btn = ACTION_NONE;
            break;
    #ifdef DIRCACHE_DUMPSTER
        case ACTION_STD_OK:
            splash(0, "Dumping cache");
            dircache_dump();
            btn = ACTION_NONE;
            break;
    #endif /* DIRCACHE_DUMPSTER */
        case ACTION_STD_CANCEL:
            if (*(int *)lists->data > 0 && info.status == DIRCACHE_SCANNING)
            {
                splash(HZ, ID2P(LANG_SCANNING_DISK));
                btn = ACTION_NONE;
            }
            break;
        }
    }

    simplelist_reset_lines();

    simplelist_addline("Cache status: %s", info.statusdesc);
    simplelist_addline("Last size: %zu B", info.last_size);
    simplelist_addline("Size: %zu B", info.size);
    unsigned int utilized = info.size ? 1000ull*info.sizeused / info.size : 0;
    simplelist_addline("Used: %zu B (%u.%u%%)", info.sizeused,
                       utilized / 10, utilized % 10);
    simplelist_addline("Limit: %zu B", info.size_limit);
    simplelist_addline("Reserve: %zu/%zu B", info.reserve_used, info.reserve);
    long ticks = ALIGN_UP(info.build_ticks, HZ / 10);
    simplelist_addline("Scanning took: %ld.%ld s",
                       ticks / HZ, (ticks*10 / HZ) % 10);
    simplelist_addline("Entry count: %u", info.entry_count);

    return btn;
}

static bool dbg_dircache_info(void)
{
    struct simplelist_info info;
    int syncbuild = 0;
    simplelist_info_init(&info, "Dircache Info", 0, &syncbuild);
    info.action_callback = dircache_callback;
    info.scroll_all = true;
    return simplelist_show_list(&info);
}


static int database_callback(int btn, struct gui_synclist *lists)
{
    (void)lists;
    struct tagcache_stat *stat = tagcache_get_stat();
    static bool synced = false;
    static int update_entries = 0;

    simplelist_reset_lines();

    simplelist_addline("Initialized: %s",
             stat->initialized ? "Yes" : "No");
    simplelist_addline("DB %s: %s", "Ready",
             stat->ready ? "Yes" : "No");
    simplelist_addline("DB Path: %s", stat->db_path);
    simplelist_addline("RAM Cache: %s",
             stat->ramcache ? "Yes" : "No");
    simplelist_addline("RAM: %d/%d B",
             stat->ramcache_used, stat->ramcache_allocated);
    simplelist_addline("Total entries: %d",
                       stat->total_entries);
    simplelist_setline("Progress:");
    simplelist_addline(" %d%% (%d entries)",
                       stat->progress, stat->processed_entries);
    simplelist_setline("Curfile:");
    simplelist_addline(" %s", stat->curentry ? stat->curentry : "---");
    simplelist_addline("Commit step: %d",
             stat->commit_step);
    simplelist_addline("Commit delayed: %s",
             stat->commit_delayed ? "Yes" : "No");

    simplelist_addline("Queue length: %d",
             stat->queue_length);

    if (synced)
    {
        synced = false;
        tagcache_screensync_event();
    }

    if (!btn && stat->curentry)
    {
        synced = true;
        if (update_entries <= stat->processed_entries)
        {
            update_entries = stat->processed_entries + 100;
            return ACTION_REDRAW;
        }
        return ACTION_NONE;
    }

    if (btn == ACTION_STD_CANCEL)
    {
        update_entries = 0;
        tagcache_screensync_enable(false);
    }
    return btn;
}
/* What the last cable insertion did. Reads the record firmware/usb.c keeps;
 * see struct usb_insert_record for why each field is here. */
static int usb_info_callback(int btn, struct gui_synclist *lists)
{
    (void)lists;
    const struct usb_insert_record *r = usb_get_insert_record();

    simplelist_reset_lines();

    simplelist_addline("Last insert: %ld tick", r->tick);
    simplelist_addline("Mode wanted: %s",
                       r->usb_mode == USB_MODE_MASS_STORAGE ? "Mass storage"
                                                            : "Charge");
    simplelist_addline("Buttons: %08x (mask %08x)",
                       (unsigned)r->btn_status, (unsigned)r->btn_ignore);
    simplelist_addline("  -> inverted: %s",
                       (r->btn_status & r->btn_ignore) ? "YES" : "no");
    simplelist_addline("Charge only: %s", r->power_only ? "YES" : "no");
    simplelist_addline("Host present: %s", usb_host_is_present() ? "Yes" : "No");

    simplelist_setline("Enumeration:");
    simplelist_addline("  bus resets: %d", r->bus_resets);
    simplelist_addline("  setups: %d (last req %02x)",
                       r->setups, (unsigned)r->last_setup);
    simplelist_addline("  set addr: %d", r->set_addr);
    simplelist_addline("  set config: %d (cfg %d)",
                       r->set_config, r->last_config);
    simplelist_addline("  drivers active: %02x err %02x",
                       (unsigned)r->drv_active, (unsigned)r->drv_error);
    simplelist_addline("   (storage=%02x hid=%02x audio=%02x)",
                       1 << USB_DRIVER_MASS_STORAGE,
#ifdef USB_ENABLE_HID
                       1 << USB_DRIVER_HID,
#else
                       0,
#endif
#ifdef USB_ENABLE_AUDIO
                       1 << USB_DRIVER_AUDIO
#else
                       0
#endif
                       );
    simplelist_addline("  wants disk: %s",
                       r->exclusive_required ? "Yes" : "No");

    simplelist_setline("Handover:");
    if (r->acks_expected < 0)
        simplelist_addline("  no broadcast sent");
    else
    {
        simplelist_addline("  acks expected: %d", r->acks_expected);
        simplelist_addline("  acks left: %d", r->acks_remaining);
        simplelist_addline("  broadcast at: %ld tick", r->broadcast_tick);
    }
    simplelist_addline("Storage given up: %s",
                       r->storage_handed_over ? "Yes" : "No");
    simplelist_addline("Now: %ld tick", current_tick);

    return btn;
}

static bool dbg_usb_info(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, "USB Info", 0, NULL);
    info.action_callback = usb_info_callback;
    info.scroll_all = true;
    info.timeout = HZ/2;
    return simplelist_show_list(&info);
}

static bool dbg_tagcache_info(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, "Database Info", 0, NULL);
    info.action_callback = database_callback;
    info.scroll_all = true;

    /* Don't do nonblock here, must give enough processing time
       for tagcache thread. */
    /* info.timeout = TIMEOUT_NOBLOCK; */
    info.timeout = 1;
    tagcache_screensync_enable(true);
    return simplelist_show_list(&info);
}

#if defined(CPU_PP) && !(CONFIG_STORAGE & STORAGE_SD)
static bool dbg_save_roms(void)
{
    int fd = creat("/internal_rom_000000-0FFFFF.bin", 0666);
    if(fd >= 0)
    {
        write(fd, (void *)0x20000000, FLASH_SIZE);
        close(fd);
    }

    return false;
}
#endif /* CPU */


extern bool do_screendump_instead_of_usb;

static bool dbg_screendump(void)
{
    do_screendump_instead_of_usb = !do_screendump_instead_of_usb;
    splashf(HZ, "Screendump %sabled", do_screendump_instead_of_usb?"en":"dis");
    return false;
}

extern bool write_metadata_log;

static bool dbg_metadatalog(void)
{
    write_metadata_log = !write_metadata_log;
    splashf(HZ, "Metadata log %sabled", write_metadata_log ? "en" : "dis");
    return false;
}


#ifdef CPU_BOOST_LOGGING
static bool cpu_boost_log(void)
{
    int count = cpu_boost_log_getcount();
    char *str = cpu_boost_log_getlog_first();
    bool done;
    lcd_setfont(FONT_SYSFIXED);
    for (int i = 0; i < count ;)
    {
        lcd_clear_display();
        for(int j=0; j<LCD_HEIGHT/SYSFONT_HEIGHT; j++,i++)
        {
            if (!str)
                str = cpu_boost_log_getlog_next();
            if (str)
            {
                if(strlen(str) > LCD_WIDTH/SYSFONT_WIDTH)
                    lcd_puts_scroll(0, j, str);
                else
                    lcd_puts(0, j,str);
            }
            str = NULL;
        }
        lcd_update();
        done = false;
        while (!done)
        {
            switch(get_action(CONTEXT_STD,TIMEOUT_BLOCK))
            {
                case ACTION_STD_OK:
                case ACTION_STD_PREV:
                case ACTION_STD_NEXT:
                    done = true;
                break;
                case ACTION_STD_CANCEL:
                    i = count;
                    done = true;
                break;
            }
        }
    }
    lcd_scroll_stop();
    get_action(CONTEXT_STD,TIMEOUT_BLOCK);
    lcd_setfont(FONT_UI);
    return false;
}

static bool cpu_boost_log_dump(void)
{
    int fd;
    int count = cpu_boost_log_getcount();
    char *str = cpu_boost_log_getlog_first();

    splashf(HZ, "Boost Log File Dumped");

    /* nothing to print ? */
    if(count == 0)
        return false;

    char fname[MAX_PATH];
    struct tm *nowtm = get_time();
    fd = open_pathfmt(fname, sizeof(fname), O_CREAT|O_WRONLY|O_TRUNC,
                      "%s/boostlog_%04d%02d%02d%02d%02d%02d.txt", ROCKBOX_DIR,
                      nowtm->tm_year + 1900, nowtm->tm_mon + 1, nowtm->tm_mday,
                      nowtm->tm_hour, nowtm->tm_min, nowtm->tm_sec);
    if(-1 != fd) {
        for (int i = 0; i < count; i++)
        {
            if (!str)
                str = cpu_boost_log_getlog_next();
            if (str)
            {
               fdprintf(fd, "%s\n", str);
               str = NULL;
            }
        }

        close(fd);
        return true;
    }

    return false;
}
#endif

extern bool wheel_is_touched;
extern int old_wheel_value;
extern int new_wheel_value;
extern int wheel_delta;
extern unsigned int accumulated_wheel_delta;
extern unsigned int wheel_velocity;

static bool dbg_scrollwheel(void)
{
    lcd_setfont(FONT_SYSFIXED);

    while (1)
    {
        if (action_userabort(HZ/10))
            break;

        lcd_clear_display();

        /* show internal variables of scrollwheel driver */
        lcd_putsf(0, 0, "wheel touched: %s", (wheel_is_touched) ? "true" : "false");
        lcd_putsf(0, 1, "new position: %2d", new_wheel_value);
        lcd_putsf(0, 2, "old position: %2d", old_wheel_value);
        lcd_putsf(0, 3, "wheel delta: %2d", wheel_delta);
        lcd_putsf(0, 4, "accumulated delta: %2d", accumulated_wheel_delta);
        lcd_putsf(0, 5, "velo [deg/s]: %4d", (int)wheel_velocity);

        /* show effective accelerated scrollspeed */
        lcd_putsf(0, 6, "accel. speed: %4d",
                button_apply_acceleration((1<<31)|(1<<24)|wheel_velocity) );

        lcd_update();
    }
    lcd_setfont(FONT_UI);
    return false;
}

static bool dbg_talk(void)
{
    struct simplelist_info list;
    struct talk_debug_data data;
    talk_get_debug_data(&data);

    simplelist_info_init(&list, "Voice Information:", 0, NULL);

    list.scroll_all = true;
    list.timeout = HZ;

    simplelist_reset_lines();

    simplelist_setline("Current voice file:");
    if (data.status != TALK_STATUS_ERR_NOFILE)
        simplelist_addline(" %s", data.voicefile);
    else
        simplelist_setline(" No voice information available");

    if (data.status != TALK_STATUS_OK)
    {
        simplelist_addline("Talk Status: ERR (%i)",
                    data.status);
        return simplelist_show_list(&list);
    }
    else
        simplelist_setline("Talk Status: OK");
    simplelist_setline("Number of (empty) clips in voice file:");
    simplelist_addline(" (%d) %d", data.num_empty_clips, data.num_clips);
    simplelist_setline("Min/Avg/Max size of clips:");
    simplelist_addline(" %d / %d / %d",
                    data.min_clipsize, data.avg_clipsize, data.max_clipsize);
    simplelist_setline("Memory allocated:");
    simplelist_addline(" %ld.%02ld KB",
                    data.memory_allocated / 1024, data.memory_allocated % 1024);
    simplelist_addline("Memory used:");
    simplelist_addline(" %ld.%02ld KB",
                       data.memory_used / 1024, data.memory_used % 1024);
    simplelist_setline("Number of clips in cache:");
    simplelist_addline(" %d", data.cached_clips);
    simplelist_setline("Cache hits / misses:");
    simplelist_addline("%d / %d", data.cache_hits, data.cache_misses);

    return simplelist_show_list(&list);
}


#ifdef USB_ENABLE_AUDIO
static int dbg_usb_audio_cb(int action, struct gui_synclist *lists)
{
    (void)lists;
    simplelist_reset_lines();
    simplelist_addline("%sabled", usb_core_driver_enabled(USB_DRIVER_AUDIO)?"En":"Dis");
    simplelist_addline("%sPlaying", usb_audio_get_playing()?"":"Not ");
    simplelist_addline("iface: %d alt: %d", usb_audio_get_main_intf(), usb_audio_get_alt_intf());
    simplelist_addline("out ep: 0x%X in ep: 0x%X", usb_audio_get_out_ep(), usb_audio_get_in_ep());
    simplelist_addline("Volume: %d", usb_audio_get_cur_volume());
    simplelist_addline("Sink Frequency: %lu", usb_audio_get_playback_sampling_frequency());
    simplelist_addline("Frames dropped: %d", usb_audio_get_frames_dropped());
    simplelist_addline("Buffers filled: %f", (double)usb_audio_get_prebuffering_avg()/(1<<16)); /* convert from 16.16 fixed to float */
    simplelist_addline("Min: %d / Max: %d", usb_audio_get_prebuffering_maxmin(false), usb_audio_get_prebuffering_maxmin(true));
    simplelist_addline("Samples used per Frame: %f", (double)usb_audio_get_samplesperframe()/(1<<16)); /* convert from 16.16 fixed to float */
    simplelist_addline("Samples received per frame: %f", (double)usb_audio_get_samples_rx_perframe()/(1<<16)); /* convert from 16.16 fixed to float */
    simplelist_addline("Samples diff: %f", (double)(usb_audio_get_samplesperframe()-usb_audio_get_samples_rx_perframe())/(1<<16)); /* convert from 16.16 fixed to float */
    simplelist_addline("%s", usb_audio_get_underflow()?"UNDERFLOW!":" ");
    simplelist_addline("%s", usb_audio_get_overflow()?"OVERFLOW!":" ");
    simplelist_addline("%s", usb_audio_get_alloc_failed()?"ALLOC FAILED!":" ");
    if (action == ACTION_NONE)
    {
        action = ACTION_REDRAW;
    }
    return action;
}
static bool dbg_usb_audio(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, "USB Audio", 0, NULL);
    info.scroll_all = true;
    info.action_callback = dbg_usb_audio_cb;
    return simplelist_show_list(&info);
}
#endif /* USB_ENABLE_AUDIO */



#if defined(IPOD_6G)
static bool dbg_syscfg(void) {
    struct simplelist_info info;
    struct SysCfg syscfg;

    simplelist_info_init(&info, "SysCfg NOR contents", 0, NULL);
    simplelist_reset_lines();

    const ssize_t result = syscfg_read(&syscfg);

    if (result == -1) {
        simplelist_setline("SCfg magic not found");
        return simplelist_show_list(&info);
    }

    simplelist_addline("Total size: %lu bytes, %lu entries", syscfg.header.size, syscfg.header.num_entries);

    if (result > 0) {
        simplelist_addline("Wrong size: expected %ld, got %lu", result, syscfg.header.size);
        return simplelist_show_list(&info);
    }

    if (syscfg.header.num_entries > SYSCFG_MAX_ENTRIES) {
        simplelist_addline("Too many entries, showing only first %u", SYSCFG_MAX_ENTRIES);
    }

    const size_t syscfg_num_entries = MIN(syscfg.header.num_entries, SYSCFG_MAX_ENTRIES);

    for (size_t i = 0; i < syscfg_num_entries; i++) {
        const struct SysCfgEntry* entry = &syscfg.entries[i];
        const char* tag = (char *)&entry->tag;
        const uint32_t* data32 = (uint32_t *)entry->data;

        switch (entry->tag) {
            case SYSCFG_TAG_SRNM:
                simplelist_addline("Serial number (SrNm): %s", entry->data);
                break;
            case SYSCFG_TAG_FWID:
                simplelist_addline("Firmware ID (FwId): %07lX", data32[1] & 0x0FFFFFFF);
                break;
            case SYSCFG_TAG_HWID:
                simplelist_addline("Hardware ID (HwId): %08lX", data32[0]);
                break;
            case SYSCFG_TAG_HWVR:
                simplelist_addline("Hardware version (HwVr): %06lX", data32[1]);
                break;
            case SYSCFG_TAG_CODC:
                simplelist_addline("Codec (Codc): %s", entry->data);
                break;
            case SYSCFG_TAG_SWVR:
                simplelist_addline("Software version (SwVr): %s", entry->data);
                break;
            case SYSCFG_TAG_MLBN:
                simplelist_addline("Logic board serial number (MLBN): %s", entry->data);
                break;
            case SYSCFG_TAG_MODN:
                simplelist_addline("Model number (Mod#): %s", entry->data);
                break;
            case SYSCFG_TAG_REGN:
                simplelist_addline("Sales region (Regn): %08lX %08lX", data32[0], data32[1]);
                break;
            default:
                simplelist_addline("%c%c%c%c: %08lX %08lX %08lX %08lX",
                    tag[3], tag[2], tag[1], tag[0],
                    data32[0], data32[1], data32[2], data32[3]
                );
                break;
        }
    }

    return simplelist_show_list(&info);
}

#define FLASH_PAGES (FLASH_SIZE >> 12)
#define FLASH_PAGE_SIZE (FLASH_SIZE >> 8)

static bool dbg_bootflash_dump(void) {
    splashf(HZ, "Please wait...");

    int fd;

    fd = creat("/bootflash.bin", 0666);

    if (fd < 0)
    {
        splashf(HZ * 3, "Error opening file");
        return false;
    }

    uint8_t page[FLASH_PAGE_SIZE];
    bootflash_init(SPI_PORT);

    for (int i = 0; i < FLASH_PAGES; i++) {
        bootflash_read(SPI_PORT, i << 12, FLASH_PAGE_SIZE, page);
        write(fd, page, FLASH_PAGE_SIZE);
    }

    bootflash_close(SPI_PORT);
    close(fd);
    splashf(HZ * 3, "Dump saved to /bootflash.bin");

    return false;
}
#endif


/** The menu **/
static const struct {
    unsigned char *desc; /* string or ID */
    bool (*function) (void); /* return true if USB was connected */
} menuitems[] = {
#if defined(CPU_PP) && !(CONFIG_STORAGE & STORAGE_SD)
        { "Dump ROM contents", dbg_save_roms },
#endif
#if defined(CPU_PP) || defined(CPU_S5L87XX)
        { "View I/O ports", dbg_ports },
#endif
#if CONFIG_RTC == RTC_PCF50605
        { "View PCF registers", dbg_pcf },
#endif
        { "CPU frequency", dbg_cpufreq },
        { "View OS stacks", dbg_os },
#ifdef __linux__
        { "View CPU stats", dbg_cpuinfo },
#endif
#if (CONFIG_BATTERY_MEASURE != 0)
        { "View battery", view_battery },
#endif
        { "Screendump", dbg_screendump },
        { "Skin Engine RAM usage", dbg_skin_engine },
        { "View HW info", dbg_hw_info },
        { "View partitions", dbg_partitions },
        { "View disk info", dbg_disk_info },
        { "Dump ATA identify info", dbg_identify_info},
        { "View/Dump S.M.A.R.T. data", dbg_ata_smart},
        { "Metadata log", dbg_metadatalog },
        { "View dircache info", dbg_dircache_info },
        { "View database info", dbg_tagcache_info },
        { "View USB info", dbg_usb_info },
        { "View buffering thread", dbg_buffering_thread },
#ifdef PM_DEBUG
        { "pm histogram", peak_meter_histogram},
#endif /* PM_DEBUG */
#ifdef BUFLIB_DEBUG_PRINT
        { "View buflib allocs", dbg_buflib_allocs },
#endif
#ifdef ROCKBOX_HAS_LOGF
        {"Show Log File", log_viewer_show },
        {"Dump Log File", log_viewer_dump },
#endif
#if defined(USB_ENABLE_AUDIO)
        {"USB-DAC", dbg_usb_audio},
#endif
#ifdef CPU_BOOST_LOGGING
        {"Show cpu_boost log",cpu_boost_log},
        {"Dump cpu_boost log",cpu_boost_log_dump},
#endif
        {"Debug scrollwheel", dbg_scrollwheel },
        {"Debug IAP", dbg_iap },
        {"Talk engine stats", dbg_talk },

#if defined(IPOD_6G)
        {"View SysCfg", dbg_syscfg },
        {"Dump bootflash to file", dbg_bootflash_dump },
#endif
};

static int menu_action_callback(int btn, struct gui_synclist *lists)
{
    int selection = gui_synclist_get_sel_pos(lists);
    if (btn == ACTION_STD_OK)
    {
        FOR_NB_SCREENS(i)
           viewportmanager_theme_enable(i, false, NULL);
        menuitems[selection].function();
        btn = ACTION_REDRAW;
        FOR_NB_SCREENS(i)
            viewportmanager_theme_undo(i, false);
    }
    else if (btn == ACTION_STD_CONTEXT)
    {
        MENUITEM_STRINGLIST(menu_items, "Debug", NULL, ID2P(LANG_ADD_TO_FAVES));
        if (do_menu(&menu_items, NULL, NULL, false) == 0)
            shortcuts_add(SHORTCUT_DEBUGITEM, menuitems[selection].desc);
        return ACTION_STD_CANCEL;
    }
    return btn;
}

static const char* menu_get_name(int item, void * data,
                                    char *buffer, size_t buffer_len)
{
    (void)data; (void)buffer; (void)buffer_len;
    return menuitems[item].desc;
}

static int menu_get_talk(int item, void *data)
{
    (void)data;
    if (global_settings.talk_menu && menuitems[item].desc)
    {
        talk_number(item + 1, true);
        talk_id(VOICE_PAUSE, true);
        talk_spell(menuitems[item].desc, true);
     }
    return 0;
}

int debug_menu(void)
{
    struct simplelist_info info;

    simplelist_info_init(&info, "Debug", ARRAYLEN(menuitems), NULL);
    info.action_callback = menu_action_callback;
    info.get_name        = menu_get_name;
    info.get_talk        = menu_get_talk;
    return (simplelist_show_list(&info)) ? 1 : 0;
}

bool run_debug_screen(char* screen)
{
    for (unsigned i=0; i<ARRAYLEN(menuitems); i++)
        if (!strcasecmp(screen, menuitems[i].desc))
        {
            FOR_NB_SCREENS(j)
               viewportmanager_theme_enable(j, false, NULL);
            menuitems[i].function();
            FOR_NB_SCREENS(j)
                viewportmanager_theme_undo(j, false);
            return true;
        }

    return false;
}
