/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Background tasks: the deferred work that keeps the database's derived
 * artifacts current.
 *
 * Three things rebuild themselves after the library changes -- the tag
 * database, the carousel's album index and the artwork thumbnail cache -- and
 * the last two are identical in everything but the pass itself: wake on a
 * timer, acknowledge USB, wait for the database to settle, compare the
 * library's marks against a marker file, run, record. That shape lives here,
 * so each task supplies only its pass.
 *
 * Every task runs on the one thread this owns, in rank order, sharing its
 * stack and queue: thread slots are few enough that a screen starting its own
 * thread can fail to get one. Each task keeps its pass, its events and its
 * abort check in its own file. What lives here is the thread and the policy:
 * when a task may run, and which task gives way to which.
 ****************************************************************************/

#ifndef _BG_TASK_H_
#define _BG_TASK_H_

#include <stdbool.h>
#include <stddef.h>
#include "config.h"
#include "kernel.h"

/* Rank decides who waits. A task *outranks* another when its rank is the
 * smaller number: it runs first, and it turns a running pass of the other
 * back. The index outranks the artwork cache because it finishes in seconds
 * where a full artwork pass takes minutes, and because the carousel blocks on
 * the index while nothing at all blocks on artwork. */
#define BG_RANK_INDEX   0
#define BG_RANK_ART     1
#define BG_RANK_FILES   2   /* last: a walk takes minutes, nothing waits on it */

/* What "the library" looked like at some moment. A task is stale when the
 * marks move, so between them these are the whole definition of "something
 * changed that the derived artifacts care about".
 *
 * Three rather than one because the entry count alone misses deletions:
 * tagcache marks a removed entry and leaves the count where it was, so a
 * library that only ever loses tracks looks untouched forever.
 *
 * The play counter (tagcache's serial) is deliberately absent. Playing a
 * track changes nothing either pass produces -- the album index carries
 * playback figures, but it keeps them current by its own means. */
struct bg_marks
{
    int entries;   /* tagcache entry count */
    int commitid;  /* commits so far -- moves when tracks are added */
    int deleted;   /* entries flagged deleted, -1 when not countable */
};

struct bg_task
{
    /* ---- supplied by the task ---- */

    /* Where the marks of the last completed pass are kept. On disk rather
     * than in RAM so an unchanged library costs nothing at startup. */
    const char *done_file;
    int rank;

    /* Peak this task's pass holds from core at once, or 0 for a task that
     * allocates nothing. The audio buffer takes all of core once playback has
     * run, so memory asked for here comes back out of it through playback's
     * shrink_callback(), which stops and rebuffers the track to get it.
     * Declaring the peak is what lets audio_reset_buffer() leave the room
     * unclaimed instead. */
    size_t work_bytes;

    /* The pass. Returns false if it did not finish -- interrupted, or the
     * memory it needed was not free. The marker is written only on true, so a
     * false is simply retried later. */
    bool (*run)(void);

    /* Optional: throw away what the task produced, for a rebuild. The marker
     * file is the helper's, so this deals only with the artifacts. */
    void (*purge)(void);

    /* Optional: false when the output is missing even though the marker
     * matches. A marker cannot notice that someone deleted the file.
     *
     * Asked once and then remembered -- see `verified`. It touches the disk,
     * and asking every tick forever would defer both ATA spindown and idle
     * poweroff on any player without dircache to answer it from RAM. */
    bool (*artifact_ok)(void);

    /* Optional: queue events other than the tick and USB, which belong to the
     * task rather than here. */
    void (*handle_event)(const struct queue_event *ev);

    /* Optional, and exclusive with everything above but `rank`: a task that
     * drives its own scanning. bg_task_rebuild()/bg_task_update() call this
     * and touch nothing else, and the task is never ticked. It exists so the
     * tag database can present the same two triggers as the others without
     * its thread being rewritten -- see tagcache_task. */
    void (*request)(bool rebuild);

    /* Optional, with `request`: the task's turn on the shared thread, taken
     * every tick after the ranked tasks have had theirs. Such a task has no
     * marks to re-check after a USB session, so it is sent request(false)
     * instead. */
    void (*tick)(void);

    /* ---- owned by bg_task.c ---- */
    volatile bool running;
    volatile bool wants_run;
    volatile bool rebuild_req;
    volatile bool update_req;
    bool verified;      /* artifact_ok() has answered yes; don't ask again */
    struct bg_marks done_marks;  /* what the last completed pass covered */
    struct bg_marks prev_marks;  /* what last tick saw (stability check) */
    int  fails;         /* consecutive unfinished passes */
    long retry_at;      /* tick before which not to try again, 0 = now */
    long next_check;    /* when a lower task's pass may next look at this */
};

/* Register the task and read its marker back. Call before bg_task_start().
 *
 * A .request-only task may be passed. It has no marker to read, and is
 * registered only if it has a tick to take. Calling this for one is harmless,
 * so a caller need not know which kind it holds. */
void bg_task_init(struct bg_task *task);

/* Start the thread every task shares. Once, after every bg_task_init(). */
void bg_task_start(void);

/* Queue an event for the task's handle_event(), to run on the thread. */
void bg_task_post(struct bg_task *task, long id);

/* The largest work_bytes any registered task declares: what the audio buffer
 * has to leave free for a background pass not to be paid for in stopped
 * playback. Zero until the tasks have registered. */
size_t bg_task_reserve_bytes(void);

/* True when a task that outranks this one is waiting to run. Asked during a
 * pass, it looks at most once a tick period at whether the other has gone
 * stale, as the other's own tick would have. */
bool bg_task_preempted(const struct bg_task *task);

/* Whether a pass should stop where it stands: preempted, a USB host arriving,
 * or a shutdown. A pass checks this as it goes and, if set, returns false --
 * it will be retried later. */
bool bg_task_should_stop(const struct bg_task *task);

/* One word for what the task is doing, for the status screen. Reads the state
 * above and nothing else, so it costs nothing to ask. */
const char *bg_task_state(const struct bg_task *task);

/* The standard triggers, and the whole vocabulary a settings menu needs.
 * Rebuild discards what is there and does the work again; update keeps it and
 * fills in only what is missing. */
void bg_task_rebuild(struct bg_task *task);
void bg_task_update(struct bg_task *task);

/* Forget that any pass ever completed, so the next tick runs one. The
 * triggers above do this for you; call it directly only from inside a pass
 * that has just discarded its own artifacts, where saying so afterwards would
 * be too late to matter. */
void bg_task_forget(struct bg_task *task);

/* The tag database wearing the same face. It scans on its own thread and has
 * its own command queue, so this forwards the two triggers and no more. */
extern struct bg_task tagcache_task;

#endif /* _BG_TASK_H_ */
