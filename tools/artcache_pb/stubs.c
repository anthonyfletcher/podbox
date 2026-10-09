/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Stands in for the firmware, as tools/database_pb/stubs.c does for the
 * database.
 *
 * art_cache.c and tagcache.c are the player's, and reach for things only a
 * running player has. The tool runs one pass alone, on one thread, with no
 * dircache, no playback and no USB host, so most of what is here is the
 * answer a player in that state would give. The allocator is a pool of the
 * tool's own under the player's buflib. File times and the volume in a path
 * are the player's, not the simulator's: thumbnails are keyed on paths, and
 * stamps on images' sizes and times.
 *
 * Parts, in order:
 *   - progress
 *   - memory
 *   - the kernel and the background task
 *   - the rest of the player
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
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
#include "system/bg_task.h"
#include "database/tagcache.h"
#include "database/sound_index.h"
#include "metadata/art_cache.h"
#include "artcache_pb.h"

struct user_settings global_settings;

/* tagcache.c reads one phrase, on a path of the thread this tool replaces */
unsigned char *language_strings[LANG_LAST_INDEX_IN_ARRAY];

/* ---- progress ---------------------------------------------------------- *
 *
 * One line, redrawn in place a few times a second: albums and artists
 * visited, thumbnails written, and the folder being worked on. Drawn from
 * yield() and from the stop check the pass makes for every track. */
#define LINE_WIDTH 78

volatile long current_tick;
static long last_shown_ms;
static bool shown;
static unsigned int gen_start;
static bool gen_taken;

static long now_ms(void)
{
    return (long)(clock() * 1000 / CLOCKS_PER_SEC);
}

/* The pass and the database yield on a tick deadline, so the tick has to
 * move between yields as well as at them */
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

/* A path without its volume, for showing */
static const char *shown_path(const char *path)
{
    const char *slash;

    if (path[0] == '/' && path[1] == '<' && (slash = strchr(path + 1, '/')))
        return slash;
    return path;
}

static void progress(void)
{
    struct art_cache_counts c;
    char line[LINE_WIDTH + 1];
    const char *folder;
    long now;
    int n;

    tick();
    now = now_ms();
    if (shown && now - last_shown_ms < 250)
        return;
    last_shown_ms = now;
    if (!gen_taken)
    {
        gen_start = art_cache_generation();
        gen_taken = true;
    }

    art_cache_get_counts(&c);
    folder = art_cache_activity();
    n = snprintf(line, sizeof(line), "%d albums, %d artists, %u written  ",
                 c.albums, c.artists, art_cache_generation() - gen_start);
    if (n < LINE_WIDTH && folder != NULL)
        snprintf(line + n, sizeof(line) - n, "%s",
                 tail(shown_path(folder), LINE_WIDTH - n));

    printf("\r%-*s", LINE_WIDTH, line);
    shown = true;
}

void yield(void)
{
    progress();
}

/* The player sleeps a tick at a time to let other threads run; here that
 * would only be wasted time */
unsigned sleep(unsigned ticks)
{
    (void)ticks;
    progress();
    return 0;
}

void artcache_pb_progress_end(void)
{
    if (shown)
        printf("\r%-*s\r", LINE_WIDTH, "");
    shown = false;
}

/* ---- memory ------------------------------------------------------------ *
 *
 * The RAM copy of the database, the pass's folder table and its decode
 * buffer all come from here. */
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

/* ---- the kernel and the background task -------------------------------- *
 *
 * One thread, so nothing waits and nothing is waited for. The pass is run
 * directly, not by the background task, which would only decide when. */
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

/* Asked for every track: nothing outranks the only pass there is */
bool bg_task_should_stop(const struct bg_task *task)
{
    (void)task;
    progress();
    return false;
}

void bg_task_init(struct bg_task *task)
{
    (void)task;
}

void bg_task_post(struct bg_task *task, long id)
{
    (void)task; (void)id;
}

void bg_task_update(struct bg_task *task)
{
    (void)task;
}

/* A purge says the cache covers nothing, on disk as in RAM */
void bg_task_forget(struct bg_task *task)
{
    task->write_marks(NULL);
    task->done_marks.entries = -1;
    task->done_marks.commitid = -1;
    task->done_marks.deleted = -1;
}

/* The shared buffer a screen claims; the no-art lists hold it while they are
 * written */
#define APP_BUFFER_SIZE (4 * 1024 * 1024)

void *app_claim_buffer(size_t *buffer_size, const char *owner)
{
    static void *buf;

    (void)owner;
    if (buf == NULL && (buf = malloc(APP_BUFFER_SIZE)) == NULL)
    {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    *buffer_size = APP_BUFFER_SIZE;
    return buf;
}

void app_release_buffer(const char *owner)
{
    (void)owner;
}

/* Nothing plays, so no track offers its art */
struct mp3entry *audio_current_track(void)
{
    return NULL;
}

bool add_event(unsigned short id,
               void (*handler)(unsigned short id, void *event_data))
{
    (void)id; (void)handler;
    return true;
}

/* ---- the rest of the player -------------------------------------------- */

static void say(const char *kind, const char *what)
{
    artcache_pb_progress_end();
    printf("  %-9s%s\n", kind, shown_path(what));
}

/* The player's tracing goes to its own log files; here -v shows the art
 * cache's, and the database's stay quiet */
void debug_log(enum debug_log_id id, const char *fmt, ...)
{
    char msg[MAX_PATH + 64];
    va_list ap;

    if (!artcache_pb_verbose || id != DEBUG_LOG_ARTCACHE)
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (!strncmp(msg, "art changed: ", 13))
        say("changed", msg + 13);
    else if (strcmp(msg, "starting pass") && strncmp(msg, "pass ", 5))
        say("", msg);
}

void debug_log_restart(enum debug_log_id id)
{
    (void)id;
}

/* A broken invariant in buflib or the pass: stop before writing more */
void panicf(const char *fmt, ...)
{
    va_list ap;

    artcache_pb_progress_end();
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
    artcache_pb_progress_end();
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* No dircache to keep, and none to trust: a pass reads every folder's image
 * stamp from the disk, as the player does when its directory cache is up --
 * so an image replaced on the computer is picked up. */
bool dircache_is_ready(void)
{
    return true;
}

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

/* The player is the one mounted; nothing is plugged into it */
bool usb_host_is_present(void)
{
    progress();
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

/* The player's disk is visible as a volume, so the database's paths carry
 * "/<HDD0>" -- and the thumbnails are keyed on those paths */
bool ns_volume_is_visible(IF_MV_NONVOID(int volume))
{
    return IF_MV_VOL(volume) == 0;
}

/* Set by tagcache_init(), which the tool does not call */
bool (*sound_index_genre_of)(uint64_t key, char *buf, size_t size);

/* A file's time as the player reads it: a FAT entry's local time made a
 * number as if it were UTC. Windows turns that local time into UTC with the
 * offset in force now, so adding that offset back gives the player's number
 * exactly. Image stamps are made from these, and the simulator's filesystem
 * layer is the only caller. Trap: the C library's localtime and mktime come
 * out an hour off in summer time, and every image then looks replaced. */
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
