/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Background tasks: the deferred work that keeps the database's derived
 * artifacts current.
 *
 * Three things rebuild themselves after the library changes -- the tag
 * database, the carousel's album index and the artwork thumbnail cache -- and
 * the last two are identical in everything but the pass itself: wake on a
 * timer, acknowledge USB, wait for the database to settle, compare an entry
 * count against a marker file, run, record. That shape lives here, so each
 * task supplies only its pass.
 *
 * This owns no thread and is not a scheduler. Every task keeps its own
 * thread, stack and queue -- the stacks differ by a lot, and a thread holding
 * a queue has to acknowledge USB on it itself. What lives here is the policy:
 * when a task may run, and which task gives way to which.
 ****************************************************************************/

#ifndef _BG_TASK_H_
#define _BG_TASK_H_

#include <stdbool.h>
#include "config.h"
#include "kernel.h"

/* Rank decides who waits. A task *outranks* another when its rank is the
 * smaller number: it runs first, and it turns a running pass of the other
 * back. The index outranks the artwork cache because it finishes in seconds
 * where a full artwork pass takes minutes, and because the carousel blocks on
 * the index while nothing at all blocks on artwork. */
#define BG_RANK_INDEX   0
#define BG_RANK_ART     1

struct bg_task
{
    /* ---- supplied by the task ---- */

    /* Where the entry count of the last completed pass is kept. On disk
     * rather than in RAM so an unchanged library costs nothing at startup. */
    const char *done_file;
    int rank;

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

    /* Optional, and exclusive with everything above: a task that drives its
     * own scanning. bg_task_rebuild()/bg_task_update() call this and touch
     * nothing else, and the task is never ticked. It exists so the tag
     * database can present the same two triggers as the others without its
     * thread being rewritten -- see tagcache_task. */
    void (*request)(bool rebuild);

    /* ---- owned by bg_task.c ---- */
    volatile bool running;
    volatile bool wants_run;
    volatile bool rebuild_req;
    volatile bool update_req;
    bool verified;      /* artifact_ok() has answered yes; don't ask again */
    int  done_total;    /* entry count the last completed pass covered */
    int  prev_total;    /* entry count seen last tick (stability check) */
    int  fails;         /* consecutive unfinished passes */
    long retry_at;      /* tick before which not to try again, 0 = now */
};

/* Register the task and read its marker back. Call before its thread starts. */
void bg_task_init(struct bg_task *task);

/* One turn of a task's thread loop: wait on the queue with a timeout, then do
 * whatever that turn calls for. Intended to be the entire thread body. */
void bg_task_tick(struct bg_task *task, struct event_queue *queue);

/* True when a task that outranks this one is waiting to run. A pass should
 * check this as it goes and, if set, stop and return false -- it will be
 * retried once the other task is done. */
bool bg_task_preempted(const struct bg_task *task);

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
