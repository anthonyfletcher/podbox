/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * See battery_log.h.
 *
 * Parts: the minute being counted (every tick, so every field is a share of
 * the minute's ticks rather than a guess from one look); the ring of finished
 * minutes; the writer, run as the disk is about to sleep; the setting's hooks.
 ****************************************************************************/
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "lcd.h"
#include "audio.h"
#include "storage.h"
#include "backlight.h"
#include "powermgmt.h"
#include "ata_idle_notify.h"
#include "settings/settings.h"
#include "skin/skin_engine.h"
#include "system/activity.h"
#include "system/debug_log.h"
#include "battery_log.h"

#define SAMPLE_TICKS (60*HZ)
/* Four hours: far longer than the disk ever goes without sleeping while
 * something plays, and a USB session long enough to fill it costs only the
 * minutes it overran, which the file says. */
#define RING_SIZE 240

struct battery_sample
{
    unsigned short minute;
    unsigned short mv;
    signed char level;
    bool charging;
    unsigned char activity;
    /* Ticks out of SAMPLE_TICKS */
    unsigned short playing, paused, boost, backlight, lcd, disk;
    unsigned short mhz;         /* average */
    unsigned short spinups;
    unsigned short frames;      /* skin flushes */
};

/* ---- the minute being counted ------------------------------------------- */

static struct
{
    unsigned int ticks;
    unsigned int playing, paused, boost, backlight, lcd, disk, spinups;
    unsigned long mhz_sum;
    unsigned int frames_at_start;
} acc;
static bool disk_was;
static unsigned short minute;

static void acc_reset(void)
{
    memset(&acc, 0, sizeof acc);
    acc.frames_at_start = skin_flush_count();
}

/* ---- the ring ----------------------------------------------------------- */

static struct battery_sample ring[RING_SIZE];
static volatile unsigned int ring_head;     /* written by the tick */
static volatile unsigned int ring_tail;     /* written by the writer */
static volatile unsigned int dropped;

static void close_minute(void)
{
    unsigned int next = (ring_head + 1) % RING_SIZE;

    if (next == ring_tail)
        dropped++;
    else
    {
        struct battery_sample *s = &ring[ring_head];

        s->minute = minute;
        s->mv = battery_voltage();
        s->level = battery_level();
#if CONFIG_CHARGING >= CHARGING_MONITOR
        s->charging = charge_state > DISCHARGING;
#else
        s->charging = false;
#endif
        s->activity = get_current_activity();
        s->playing = acc.playing;
        s->paused = acc.paused;
        s->boost = acc.boost;
        s->backlight = acc.backlight;
        s->lcd = acc.lcd;
        s->disk = acc.disk;
        s->mhz = acc.mhz_sum / SAMPLE_TICKS;
        s->spinups = acc.spinups;
        s->frames = skin_flush_count() - acc.frames_at_start;
        ring_head = next;
    }

    minute++;
    acc_reset();
}

static struct timeout tick_tmo;

/* A repeating timeout rather than a tick task: the tick task table is eight
 * slots and panics when full. */
static int battery_log_tick(struct timeout *tmo)
{
    unsigned int status = audio_status();
    bool disk = storage_disk_is_active();

    if (status & AUDIO_STATUS_PAUSE)
        acc.paused++;
    else if (status & AUDIO_STATUS_PLAY)
        acc.playing++;
#ifdef CPUFREQ_NORMAL
    if (FREQ > CPUFREQ_NORMAL)
        acc.boost++;
    acc.mhz_sum += FREQ / 1000000;
#endif
    if (is_backlight_on(true))
        acc.backlight++;
    if (lcd_active())
        acc.lcd++;
    if (disk)
    {
        acc.disk++;
        if (!disk_was)
            acc.spinups++;
    }
    disk_was = disk;

    if (++acc.ticks >= SAMPLE_TICKS)
        close_minute();

    (void)tmo;
    return 1;           /* again next tick */
}

/* ---- the writer --------------------------------------------------------- */

static bool started;            /* battery_log_init() has run */
static bool running;

static int pct(unsigned int ticks)
{
    return ticks * 100 / SAMPLE_TICKS;
}

static void write_minutes(void)
{
    unsigned int lost = dropped;
    int fd;

    if (ring_tail == ring_head && lost == 0)
        return;

    fd = debug_log_open(DEBUG_LOG_BATTERY);
    if (fd >= 0)
    {
        if (lost)
        {
            int level;

            debug_log_lost(DEBUG_LOG_BATTERY, fd, lost,
                           "the disk did not sleep");
            /* the tick counts on meanwhile */
            level = disable_irq_save();
            dropped -= lost;
            restore_irq(level);
        }
        while (ring_tail != ring_head)
        {
            const struct battery_sample *s = &ring[ring_tail];

            fdprintf(fd, "%u,%u,%d,%d,%d,%d,%d,%u,%d,%d,%d,%u,%u,%u\n",
                     s->minute, s->mv, s->level, s->charging,
                     pct(s->playing), pct(s->paused), pct(s->boost), s->mhz,
                     pct(s->backlight), pct(s->lcd), pct(s->disk),
                     s->spinups, s->frames, s->activity);
            ring_tail = (ring_tail + 1) % RING_SIZE;
        }
        debug_log_close(DEBUG_LOG_BATTERY, fd);
    }
}

/* DISK_EVENT_SPINUP is sent as the disk is about to sleep, despite its name */
static void disk_idle_cb(unsigned short id, void *data)
{
    (void)id;
    (void)data;
    write_minutes();
}

/* What the columns measure the effect of, then the columns, so a rotated
 * file reads as a CSV on its own */
void battery_log_state(int fd, const char *mark)
{
    fdprintf(fd, "%s storage %s, dynamic colors %d, theme %s\n", mark,
             storage_get_ssd_mode() ? "ssd" : "hdd",
             (int)global_settings.dynamic_colors, global_settings.wps_file);
#if BATTERY_CAPACITY_INC > 0
    fdprintf(fd, "%s capacity %d mAh\n", mark,
             global_settings.battery_capacity);
#endif
    fdprintf(fd, "%s backlight %d s, %d s plugged, brightness %d\n", mark,
             global_settings.backlight_timeout,
             global_settings.backlight_timeout_plugged,
             global_settings.brightness);
    fdprintf(fd, "minute,mV,level,charging,playing%%,paused%%,boost%%,"
                 "MHz,backlight%%,lcd%%,disk%%,spinups,frames,"
                 "activity\n");
}

/* ---- the setting's hooks ------------------------------------------------ */

static void start(void)
{
    if (running)
        return;
    minute = 0;
    acc_reset();
    disk_was = storage_disk_is_active();
    add_event(DISK_EVENT_SPINUP, disk_idle_cb);
    timeout_register(&tick_tmo, battery_log_tick, 1, 0);
    running = true;
}

static void stop(void)
{
    if (!running)
        return;
    timeout_cancel(&tick_tmo);
    remove_event(DISK_EVENT_SPINUP, disk_idle_cb);
    running = false;
    write_minutes();
}

void battery_log_enable(bool on)
{
    if (!started)
        return;
    if (on)
        start();
    else
        stop();
}

void battery_log_init(void)
{
    started = true;
    if (global_settings.debug_log_battery)
    {
        debug_log_header(DEBUG_LOG_BATTERY, "started");
        start();
    }
}
