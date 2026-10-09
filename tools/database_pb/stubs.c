/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Stands in for the firmware, as tools/soundscan/stubs.c does for the
 * analysis.
 *
 * tagcache.c is the player's, and reaches for things only a running player
 * has. The tool calls tagcache_tool_run() alone, on one thread, with no RAM
 * copy, no dircache, no playback and no USB host, so most of what is here is
 * the answer a player in that state would give. The allocator is the one
 * real piece: a pool of the tool's own under the player's buflib, so a commit
 * borrows, pins and frees its buffers exactly as it does on the player.
 *
 * Parts, in order:
 *   - progress
 *   - memory
 *   - the kernel
 *   - the rest of the player
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include "config.h"
#include "kernel.h"
#include "thread.h"
#include "buflib.h"
#include "core_alloc.h"
#include "dircache.h"
#include "ata_idle_notify.h"
#include "usb.h"
#include "panic.h"
#include "lang.h"
#include "settings/settings.h"
#include "system/debug_log.h"
#include "database/tagcache.h"
#include "database/sound_index.h"
#include "database_pb.h"

struct user_settings global_settings;

/* tagcache.c reads one phrase, on a path of the thread this tool replaces */
unsigned char *language_strings[LANG_LAST_INDEX_IN_ARRAY];

/* ---- progress ---------------------------------------------------------- *
 *
 * The scan yields every few files on the player, which is where this tool
 * says how far it has got: one line, redrawn in place a few times a second,
 * with the stage, the counts so far and the folder being read. */
#define LINE_WIDTH 78

volatile long current_tick;
static long last_shown_ms;
static bool shown;
static bool scan_seen;
static char folder[MAX_PATH];

static long now_ms(void)
{
    return (long)(clock() * 1000 / CLOCKS_PER_SEC);
}

/* tagcache.c yields on a tick deadline, so the tick has to move between
 * yields as well as at them */
static void tick(void)
{
    current_tick = now_ms() * HZ / 1000;
}

/* The tail of 'path' that fits in 'width', marked as cut */
static const char *tail(const char *path, int width)
{
    static char buf[MAX_PATH];
    int len = strlen(path);

    if (len <= width)
        return path;
    snprintf(buf, sizeof(buf), "...%s", path + len - (width - 3));
    return buf;
}

void yield(void)
{
    const struct tagcache_stat *s;
    char line[LINE_WIDTH + 1];
    long now;

    tick();
    now = now_ms();
    if (shown && now - last_shown_ms < 250)
        return;
    last_shown_ms = now;

    s = tagcache_get_stat();
    if (s->commit_step > 0)
        snprintf(line, sizeof(line), "Writing the database: step %d of %d",
                 s->commit_step, tagcache_get_max_commit_step());
    else if (s->scanning)
    {
        int n;
        const char *cur = (const char *)s->curentry;

        scan_seen = true;
        if (cur != NULL)
        {
            const char *slash = strrchr(cur, '/');
            int len = slash != NULL && slash != cur ? slash - cur : 1;

            snprintf(folder, sizeof(folder), "%.*s", len, cur);
        }
        n = snprintf(line, sizeof(line), "Reading: %d checked, %d new, "
                     "%d changed  ", s->processed_entries, s->scan_added,
                     s->scan_changed);
        if (n < LINE_WIDTH)
            snprintf(line + n, sizeof(line) - n, "%s",
                     tail(folder, LINE_WIDTH - n));
    }
    else if (scan_seen)
        snprintf(line, sizeof(line), "Finishing");
    else if (database_pb_rebuild)
        snprintf(line, sizeof(line), "Saving play counts");
    else
        snprintf(line, sizeof(line), "Checking the database");

    printf("\r%-*s", LINE_WIDTH, line);
    shown = true;
}

/* tagcache.c sleeps a tick at a time to let the player breathe; here that
 * would only be wasted time */
unsigned sleep(unsigned ticks)
{
    (void)ticks;
    yield();
    return 0;
}

void database_pb_progress_end(void)
{
    if (shown)
        printf("\r%-*s\r", LINE_WIDTH, "");
    shown = false;
}

/* ---- memory ------------------------------------------------------------ *
 *
 * The RAM copy of the database and the commit's merge buffer both come from
 * here, and the commit takes all it can get, so the pool is generous: a
 * desktop has it to spare, and a bigger merge buffer is a faster commit. */
#define POOL_SIZE (256 * 1024 * 1024)

struct buflib_context core_ctx;

static void pool_init(void)
{
    static bool done;
    void *pool;

    if (done)
        return;
    pool = malloc(POOL_SIZE);
    if (pool == NULL)
    {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    buflib_init(&core_ctx, pool, POOL_SIZE);
    done = true;
}

int core_alloc_ex(size_t size, struct buflib_callbacks *ops)
{
    pool_init();
    return buflib_alloc_ex(&core_ctx, size, ops);
}

int core_alloc(size_t size)
{
    return core_alloc_ex(size, NULL);
}

int core_free(int handle)
{
    return buflib_free(&core_ctx, handle);
}

void core_pin(int handle)
{
    buflib_pin(&core_ctx, handle);
}

void core_unpin(int handle)
{
    buflib_unpin(&core_ctx, handle);
}

size_t core_available(void)
{
    pool_init();
    return buflib_available(&core_ctx);
}

size_t core_allocatable(void)
{
    pool_init();
    return buflib_allocatable(&core_ctx);
}

/* No audio buffer is laid out: the reserve tagcache keeps for one is the
 * pool's to spend */
size_t audio_buffer_size(void)
{
    return 0;
}

/* ---- the kernel -------------------------------------------------------- *
 *
 * One thread, so nothing waits and nothing is waited for. The database
 * thread is never started: tagcache_tool_run() does its work instead, and a
 * post to its queue -- the import after a fresh build -- is done there too. */
void mutex_init(struct mutex *m)
{
    (void)m;
}

void mutex_lock(struct mutex *m)
{
    (void)m;
}

void mutex_unlock(struct mutex *m)
{
    (void)m;
}

void queue_init(struct event_queue *q, bool register_queue)
{
    (void)q; (void)register_queue;
}

void queue_post(struct event_queue *q, long id, intptr_t data)
{
    (void)q; (void)id; (void)data;
}

bool queue_peek(struct event_queue *q, struct queue_event *ev)
{
    (void)q; (void)ev;
    return false;
}

bool queue_peek_ex(struct event_queue *q, struct queue_event *ev,
                   unsigned int flags, const long (*filters)[2])
{
    (void)q; (void)ev; (void)flags; (void)filters;
    return false;
}

void queue_wait_w_tmo(struct event_queue *q, struct queue_event *ev,
                      int ticks)
{
    (void)q; (void)ticks;
    ev->id = SYS_TIMEOUT;
    ev->data = 0;
}

unsigned int create_thread(void (*function)(void), void *stack,
                           size_t stack_size, unsigned flags,
                           const char *name IF_PRIO(, int priority)
                           IF_COP(, unsigned int core))
{
    (void)function; (void)stack; (void)stack_size; (void)flags; (void)name;
    return 0;
}

/* ---- the rest of the player -------------------------------------------- */

static void say(const char *kind, const char *what)
{
    database_pb_progress_end();
    printf("  %-11s%s\n", kind, what);
}

/* The player's tracing goes to its own log files; here it is -v's, cut down
 * to the files that changed something. The scan names every file before it
 * says what it did with one, so the name is held until then. */
void debug_log(enum debug_log_id id, const char *fmt, ...)
{
    static char file[MAX_PATH];
    char msg[MAX_PATH + 64];
    va_list ap;

    (void)id;
    if (!database_pb_verbose)
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (!strncmp(msg, "file: ", 6))
        snprintf(file, sizeof(file), "%s", msg + 6);
    else if (!strcmp(msg, "info: added"))
        say("new", file);
    else if (!strcmp(msg, "info: re-adding"))
        say("changed", file);
    else if (!strcmp(msg, "error: get_metadata failed"))
        say("unreadable", file);
    else if (!strncmp(msg, "refs: gone ", 11))
        say("gone", msg + 11);
    else if (strncmp(msg, "del ", 4) && strncmp(msg, "walk: ", 6)
             && strncmp(msg, "refs: done", 10))
        say("", msg);
}

void debug_log_restart(enum debug_log_id id)
{
    (void)id;
}

/* A broken invariant in buflib or the commit: stop before writing more */
void panicf(const char *fmt, ...)
{
    va_list ap;

    database_pb_progress_end();
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(3);
}

/* The metadata readers' chatter, one line per file */
void debugf(const char *fmt, ...)
{
    (void)fmt;
}

void splashf(int ticks, const char *fmt, ...)
{
    va_list ap;

    (void)ticks;
    database_pb_progress_end();
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* No dircache: every lookup reads the disk */
void dircache_wait(void)
{
}

int dircache_resume(void)
{
    return 0;
}

void dircache_free_buffer(void)
{
}

int dircache_search(unsigned int flags, struct dircache_fileref *dcfrefp,
                    const char *path)
{
    (void)flags; (void)dcfrefp; (void)path;
    return -1;
}

void dircache_get_info(struct dircache_info *info)
{
    memset(info, 0, sizeof(*info));
}

/* The player is the one mounted; nothing is plugged into it. The scan asks
 * this for every file, which makes it the place the tick moves between
 * yields. */
bool usb_host_is_present(void)
{
    tick();
    return false;
}

void usb_acknowledge(long id, intptr_t seqnum)
{
    (void)id; (void)seqnum;
}

void usb_wait_for_disconnect(struct event_queue *q)
{
    (void)q;
}

void register_storage_idle_func(void (*function)(void))
{
    (void)function;
}

/* The player's disk is visible as a volume, so a scan of "/" walks it as
 * "/<HDD0>" and every stored path carries that prefix. Art thumbnails are
 * keyed on those paths, and a resurrected track is matched on its filename
 * first. Trap: the simulator hides the volume and stores bare paths, so a
 * simulator-built database is no test of this. */
bool ns_volume_is_visible(IF_MV_NONVOID(int volume))
{
    return IF_MV_VOL(volume) == 0;
}

/* The RAM copy's album tables key art by folder; there is no RAM copy */
unsigned int art_cache_dir_hash(const char *dir)
{
    (void)dir;
    return 0;
}

/* Set by tagcache_init(), which the tool does not call */
bool (*sound_index_genre_of)(uint64_t key, char *buf, size_t size);

/* A file's time as the player stores it, which every unchanged track is
 * matched on. The player reads a FAT entry's local time and makes it a number
 * as if it were UTC. Windows turns that local time into UTC with the offset
 * in force now -- not the one on the file's date -- so adding that offset
 * back gives the player's number exactly. These two are what the simulator's
 * filesystem layer calls on each directory entry; nothing else in the tool
 * does. Trap: the C library's localtime and mktime instead come out an hour
 * off in summer time, and every track then looks changed. */
#ifdef _WIN32
static time_t utc_offset_now(void)
{
    time_t now = time(NULL);
    struct tm local = *localtime(&now);

    return _mkgmtime(&local) - now;
}

struct tm *localtime_r(const time_t *t, struct tm *out)
{
    time_t shifted = *t + utc_offset_now();
    struct tm *tmp = gmtime(&shifted);

    if (tmp == NULL)
        return NULL;
    *out = *tmp;
    return out;
}

time_t mktime(struct tm *tm)
{
    return _mkgmtime(tm);
}
#endif
