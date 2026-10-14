/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * See debug_log.h. A file is opened per write rather than held open: the
 * writers run on background threads across USB connects and unmounts, and a
 * handle kept open over one of those stops the disk being released.
 *
 * Parts: the table of logs and their own header lines; the file (rotation,
 * the common header); the held lines and their flush; the switches; the
 * count towards switching off.
 ****************************************************************************/
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "config.h"
#include "system/library_files.h"
#include "system.h"
#include "file.h"
#include "kernel.h"
#include "rbpaths.h"
#include "timefuncs.h"
#include "version.h"
#include "powermgmt.h"
#include "power.h"
#include "ata_idle_notify.h"
#include "dircache.h"
#include "settings/settings.h"
#include "lang.h"
#include "widgets/splash.h"
#include "system/appevents.h"
#include "database/tagcache.h"
#include "system/usb_log_file.h"
#include "system/battery_log.h"
#include "debug_log.h"

/* ---- the table ---------------------------------------------------------- */

static void tagcache_state(int fd, const char *m)
{
    const struct tagcache_stat *st = tagcache_get_stat();

    fdprintf(fd, "%s %d entries, %s, ramcache %s (%dKB), dircache %s\n", m,
             st->total_entries, st->ready ? "ready" : "not ready",
             st->ramcache ? "loaded" : "not loaded", st->ramcache_used / 1024,
             !global_settings.dircache ? "off"
             : dircache_is_ready() ? "built" : "not built");
    fdprintf(fd, "%s scan at startup %d, after USB %d, audiobooks apart %d, "
             "layout %d%s\n", m, (int)global_settings.tagcache_scan_on_startup,
             (int)global_settings.tagcache_scan_on_eject,
             (int)global_settings.segregate_audiobooks, LIBRARY_FORMAT,
             library_files_need_upgrade() ? ", format.dat older" : "");
    fdprintf(fd, "%s scan paths %s\n", m, global_settings.tagcache_scan_paths);
}

static void art_state(int fd, const char *m)
{
    fdprintf(fd, "%s album art %d, artist art %d, book art %d, author art %d, "
             "fast build %d, album source %d, car artwork %d\n", m,
             (int)global_settings.db_albumart,
             (int)global_settings.db_artistart,
             (int)global_settings.db_bookart,
             (int)global_settings.db_authorart,
             (int)global_settings.art_cache_fast_build,
             global_settings.art_cache_album_source,
             global_settings.car_artwork);
}

#if defined(HAVE_MIKEY_REMOTE) && !defined(SIMULATOR)
static void mikey_state(int fd, const char *m)
{
    fdprintf(fd, "%s count centre clicks %d\n", m,
             (int)global_settings.remote_track_skip);
}
#endif

/* The upstream metadata parser's switch, from the debug menu; not saved */
extern bool write_metadata_log;

/* Held lines: enough for some fifty, so a scan reaches the disk every fifty
 * files rather than every one. A hang loses what is held. */
#define HELD_SIZE 4096

static char tagcache_held[HELD_SIZE];
static char art_held[HELD_SIZE];
static char playback_held[HELD_SIZE];

static const struct debug_log_info {
    const char *path;
    const char *name;
    const char *mark;           /* starts every header line */
    bool *setting;              /* NULL: written only while a screen is open */
    void (*state)(int fd, const char *mark);   /* the log's own header */
    void (*switched)(bool on);  /* the owner's, after the module's part */
    char *held;                 /* NULL: written through */
} log_info[DEBUG_LOG_COUNT] = {
    [DEBUG_LOG_TAGCACHE] = { LIB_TAGCACHE_LOG, "tagcache", "==",
                             &global_settings.debug_log_tagcache,
                             tagcache_state, NULL, tagcache_held },
    [DEBUG_LOG_ARTCACHE] = { LIB_ART_LOG, "art cache", "==",
                             &global_settings.debug_log_artcache,
                             art_state, NULL, art_held },
#ifdef HAVE_USBSTACK
    [DEBUG_LOG_USB]      = { LIB_USB_LOG, "USB", "==",
                             &global_settings.debug_log_usb,
                             usb_log_file_state, usb_log_file_enable, NULL },
#endif
    [DEBUG_LOG_BATTERY]  = { LIB_BATTERY_LOG, "battery", "#",
                             &global_settings.debug_log_battery,
                             battery_log_state, battery_log_enable, NULL },
    [DEBUG_LOG_PLAYBACK] = { LIB_PLAYBACK_ERRORS_LOG, "playback errors", "==",
                             &global_settings.debug_log_playback,
                             NULL, NULL, playback_held },
#if defined(HAVE_MIKEY_REMOTE) && !defined(SIMULATOR)
    [DEBUG_LOG_MIKEY]    = { LIB_LOGS_DIR "/mikey.log", "Mikey remote", "==",
                             NULL, mikey_state, NULL, NULL },
#endif
    /* Written by the parser itself: the header is all that comes from here */
    [DEBUG_LOG_METADATA] = { LIB_METADATA_LOG, "metadata", "==",
                             &write_metadata_log, NULL, NULL, NULL },
};

static struct {
    struct mutex lock;
    size_t used;                /* of held */
    unsigned long lost;         /* held lines dropped */
    bool on;                    /* the setting, as last applied */
} logs[DEBUG_LOG_COUNT];

static bool ready;              /* debug_log_init() has run */

/* Minutes on, by Switch Off After's index; 0 never */
static const unsigned short expiry_minutes[] = { 60, 6*60, 24*60, 7*24*60, 0 };

static int expiry_limit(void)
{
    unsigned i = global_settings.debug_log_expiry;

    return i < ARRAYLEN(expiry_minutes) ? expiry_minutes[i] : 0;
}

bool debug_log_any_on(void)
{
    for (int id = 0; id < DEBUG_LOG_COUNT; id++)
        if (debug_log_enabled(id))
            return true;
    return false;
}

int debug_log_minutes_left(void)
{
    int limit = expiry_limit();

    if (!limit || !debug_log_any_on())
        return -1;
    return MAX(limit - global_status.debug_log_minutes, 0);
}

bool debug_log_enabled(enum debug_log_id id)
{
    return id < DEBUG_LOG_COUNT && log_info[id].setting
           && *log_info[id].setting;
}

/* ---- the file ----------------------------------------------------------- */

static void write_header(int fd, enum debug_log_id id, const char *what)
{
    const struct debug_log_info *li = &log_info[id];
    const char *m = li->mark;
    struct tm *tm = get_time();
    long up = current_tick / HZ;

    fdprintf(fd, "%s %s log %s: %s %s\n", m, li->name, what, MODEL_NAME,
             rbversion);
    /* A 6G whose clock reset reads 2000-01-01, which passes for a date */
    fdprintf(fd, "%s %04d-%02d-%02d %02d:%02d:%02d%s, tick %ld, up %ld:%02ld\n",
             m, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
             tm->tm_min, tm->tm_sec,
             tm->tm_year + 1900 < 2020 ? " (clock not set)" : "",
             (long)current_tick, up / 3600, up / 60 % 60);
    if (debug_log_enabled(id))
    {
        int left = debug_log_minutes_left();

        if (left >= 0)
            fdprintf(fd, "%s switches itself off in %d:%02d\n", m,
                     left / 60, left % 60);
        else
            fdprintf(fd, "%s does not switch itself off\n", m);
    }
    fdprintf(fd, "%s battery %d%% %d mV, %s\n", m, battery_level(),
             battery_voltage(),
             charging_state() ? "charging"
             : charger_inserted() ? "on the charger" : "on battery");
    if (li->state)
        li->state(fd, m);
}

/* <name>.1.<ext> beside the file */
static void old_path(enum debug_log_id id, char *buf, size_t size)
{
    const char *path = log_info[id].path;
    const char *dot = strrchr(path, '.');

    snprintf(buf, size, "%.*s.1%s", (int)(dot - path), path, dot);
}

static int open_locked(enum debug_log_id id)
{
    int fd = open(log_info[id].path, O_WRONLY | O_CREAT | O_APPEND, 0666);

    if (fd >= 0 && ffilesize(fd) >= DEBUG_LOG_CAP)
    {
        char old[MAX_PATH];

        close(fd);
        old_path(id, old, sizeof old);
        remove(old);
        rename(log_info[id].path, old);
        fd = open(log_info[id].path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0)
            write_header(fd, id, "rotated");
    }
    return fd;
}

int debug_log_open(enum debug_log_id id)
{
    int fd;

    if (!ready || id >= DEBUG_LOG_COUNT || !log_info[id].path)
        return -1;
    mutex_lock(&logs[id].lock);
    fd = open_locked(id);
    if (fd < 0)
        mutex_unlock(&logs[id].lock);
    return fd;
}

void debug_log_close(enum debug_log_id id, int fd)
{
    close(fd);
    mutex_unlock(&logs[id].lock);
}

void debug_log_lost(enum debug_log_id id, int fd, unsigned long n,
                    const char *why)
{
    fdprintf(fd, "%s %lu lines lost%s%s\n", log_info[id].mark, n,
             why ? ": " : "", why ? why : "");
}

/* ---- the held lines ----------------------------------------------------- */

/* With the log's lock held. Lines stay held if the file will not open --
 * the disk is with a USB host -- until the next try. */
static void flush_into(enum debug_log_id id, int fd)
{
    if (logs[id].lost)
    {
        debug_log_lost(id, fd, logs[id].lost, "the buffer filled");
        logs[id].lost = 0;
    }
    if (logs[id].used)
    {
        write(fd, log_info[id].held, logs[id].used);
        logs[id].used = 0;
    }
}

static void flush_locked(enum debug_log_id id)
{
    int fd;

    if (!logs[id].used && !logs[id].lost)
        return;
    fd = open_locked(id);
    if (fd < 0)
        return;
    flush_into(id, fd);
    close(fd);
}

static void flush_all(void)
{
    for (int id = 0; id < DEBUG_LOG_COUNT; id++)
    {
        if (!log_info[id].held || !logs[id].used)
            continue;
        mutex_lock(&logs[id].lock);
        flush_locked(id);
        mutex_unlock(&logs[id].lock);
    }
}

/* DISK_EVENT_SPINUP is sent as the disk is about to sleep, despite its name,
 * and at shutdown and USB connect */
static void disk_idle_cb(unsigned short id, void *data)
{
    (void)id;
    (void)data;
    flush_all();
}

void debug_log(enum debug_log_id id, const char *fmt, ...)
{
    char line[256];
    int len, n;
    va_list ap;

    if (!ready || !debug_log_enabled(id) || !log_info[id].held)
        return;

    /* Ticks since boot rather than the clock: what matters here is how long
     * something took and in what order, and every header gives the tick
     * against the clock. */
    len = snprintf(line, sizeof line, "[%8ld] ", (long)current_tick);
    va_start(ap, fmt);
    n = vsnprintf(line + len, sizeof line - len, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    len += MIN(n, (int)(sizeof line - len - 1));
    line[len++] = '\n';

    mutex_lock(&logs[id].lock);
    if (logs[id].used + len > HELD_SIZE)
        flush_locked(id);
    if (logs[id].used + len > HELD_SIZE)
        logs[id].lost++;
    else
    {
        memcpy(log_info[id].held + logs[id].used, line, len);
        logs[id].used += len;
        if (logs[id].used >= HELD_SIZE * 3 / 4)
            flush_locked(id);
    }
    mutex_unlock(&logs[id].lock);
}

void debug_log_header(enum debug_log_id id, const char *what)
{
    int fd = debug_log_open(id);

    if (fd < 0)
        return;
    /* Lines held from before go above it */
    if (log_info[id].held)
        flush_into(id, fd);
    write_header(fd, id, what);
    debug_log_close(id, fd);
}

/* ---- the switches ------------------------------------------------------- */

/* Why the next log to go off went off, for its last line */
static const char *off_reason = "";

void debug_log_apply(void)
{
    if (!ready)
        return;
    for (int id = 0; id < DEBUG_LOG_COUNT; id++)
    {
        bool on = debug_log_enabled(id);
        int fd;

        if (!log_info[id].setting || on == logs[id].on)
            continue;
        logs[id].on = on;
        if (on)
        {
            /* A log switched on late in another's time gets its own */
            global_status.debug_log_minutes = 0;
            status_save(false);
            debug_log_header(id, "switched on");
            if (log_info[id].switched)
                log_info[id].switched(true);
            continue;
        }
        /* Off: what it was still holding, then the line saying so */
        if (log_info[id].switched)
            log_info[id].switched(false);
        fd = debug_log_open(id);
        if (fd < 0)
            continue;
        if (log_info[id].held)
            flush_into(id, fd);
        fdprintf(fd, "%s %s log switched off%s\n", log_info[id].mark,
                 log_info[id].name, off_reason);
        debug_log_close(id, fd);
    }
    off_reason = "";
}

static void all_off(const char *reason)
{
    off_reason = reason;
    for (int id = 0; id < DEBUG_LOG_COUNT; id++)
        if (log_info[id].setting)
            *log_info[id].setting = false;
    settings_save();
    debug_log_apply();
}

void debug_log_all_off(void)
{
    all_off(" from the menu");
}

/* ---- the count towards switching off ------------------------------------ */

static struct timeout minute_tmo;
static volatile bool expire_due;
static bool expired_notice;     /* a splash is owed */
static int minutes_saved;

/* In the tick interrupt: counts and flags, and leaves the switching to the UI
 * thread */
static int minute_cb(struct timeout *tmo)
{
    int limit = expiry_limit();

    (void)tmo;
    if (limit && debug_log_any_on()
        && ++global_status.debug_log_minutes >= limit)
        expire_due = true;
    return 60*HZ;
}

/* Every get_action(), so a log expires within a moment of its time even on a
 * screen left open for hours */
static void action_cb(unsigned short id, void *data)
{
    (void)id;
    (void)data;
    if (expire_due)
    {
        static char reason[40];
        int limit = expiry_limit();

        expire_due = false;
        if (limit && debug_log_any_on())
        {
            snprintf(reason, sizeof reason, ", expired after %d:%02d on",
                     limit / 60, limit % 60);
            all_off(reason);
            expired_notice = true;
        }
    }
    /* The count survives a reboot to within ten minutes, without a write
     * every minute */
    if (global_status.debug_log_minutes - minutes_saved >= 10
        || global_status.debug_log_minutes < minutes_saved)
    {
        minutes_saved = global_status.debug_log_minutes;
        status_save(false);
    }
}

void debug_log_notice(bool boot)
{
    if (expired_notice)
    {
        expired_notice = false;
        splash(2*HZ, ID2P(LANG_DEBUG_LOG_EXPIRED));
    }
    else if (boot && debug_log_any_on())
    {
        int left = debug_log_minutes_left();

        if (left >= 0)
            splashf(2*HZ, str(LANG_DEBUG_LOG_BOOT), left / 60, left % 60);
        else
            splash(2*HZ, ID2P(LANG_DEBUG_LOG_BOOT_NEVER));
    }
}

unsigned long debug_log_files_size(void)
{
    unsigned long total = 0;
    char old[MAX_PATH];

    for (int id = 0; id < DEBUG_LOG_COUNT; id++)
    {
        const char *paths[2] = { log_info[id].path, old };

        if (!log_info[id].path)
            continue;
        old_path(id, old, sizeof old);
        for (int i = 0; i < 2; i++)
        {
            int fd = open(paths[i], O_RDONLY);

            if (fd >= 0)
            {
                total += ffilesize(fd);
                close(fd);
            }
        }
    }
    return total;
}

void debug_log_init(void)
{
    for (int id = 0; id < DEBUG_LOG_COUNT; id++)
    {
        mutex_init(&logs[id].lock);
        logs[id].on = debug_log_enabled(id);
    }
    minutes_saved = global_status.debug_log_minutes;
    add_event(DISK_EVENT_SPINUP, disk_idle_cb);
    add_event(GUI_EVENT_ACTIONUPDATE, action_cb);
    timeout_register(&minute_tmo, minute_cb, 60*HZ, 0);
    ready = true;
}
