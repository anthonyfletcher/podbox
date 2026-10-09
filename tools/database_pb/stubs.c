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
 * says how far it has got. */
volatile long current_tick;
static time_t last_shown;
static bool shown;

void yield(void)
{
    const struct tagcache_stat *s = tagcache_get_stat();
    time_t now = time(NULL);

    current_tick = (long)(clock() * HZ / CLOCKS_PER_SEC);
    if (now == last_shown)
        return;
    last_shown = now;
    shown = true;
    if (s->commit_step > 0)
        printf("\r  committing, step %d of %d        ", s->commit_step,
               tagcache_get_max_commit_step());
    else
        printf("\r  %d files read", s->processed_entries);
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
        printf("\n");
    shown = false;
}

/* ---- memory ------------------------------------------------------------ *
 *
 * The commit takes all it can get for its merge, so the pool is generous: a
 * desktop has it to spare, and a bigger merge buffer is a faster commit. */
#define POOL_SIZE (64 * 1024 * 1024)

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

/* The player's tracing goes to its own log files; here it is -v's */
void debug_log(enum debug_log_id id, const char *fmt, ...)
{
    va_list ap;

    (void)id;
    if (!database_pb_verbose)
        return;
    database_pb_progress_end();
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
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

/* The player is the one mounted; nothing is plugged into it */
bool usb_host_is_present(void)
{
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

/* The root is scanned as "/", as on the player, whose one volume is not
 * named in its paths */
bool ns_volume_is_visible(IF_MV_NONVOID(int volume))
{
    IF_MV((void)volume;)
    return false;
}

/* The RAM copy's album tables key art by folder; there is no RAM copy */
unsigned int art_cache_dir_hash(const char *dir)
{
    (void)dir;
    return 0;
}

/* Set by tagcache_init(), which the tool does not call */
bool (*sound_index_genre_of)(uint64_t key, char *buf, size_t size);

/* MinGW has no localtime_r, and the simulator's filesystem layer wants one
 * for directory timestamps. One thread, so the copy is safe. */
#ifdef _WIN32
struct tm *localtime_r(const time_t *t, struct tm *out)
{
    struct tm *tmp = localtime(t);

    if (tmp == NULL)
        return NULL;
    *out = *tmp;
    return out;
}
#endif
