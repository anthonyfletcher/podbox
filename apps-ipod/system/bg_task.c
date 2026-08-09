/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The shared body of a background task's thread. See bg_task.h for what a
 * task is and why the threads stayed where they were.
 *
 * Parts, in order:
 *   - the marker file (what the last completed pass covered)
 *   - registration and ranking
 *   - the triggers
 *   - the tick, which is where all the policy is
 *   - the tag database's forwarding-only task
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include "config.h"
#include "kernel.h"
#include "file.h"
#include "usb.h"
#include "panic.h"
#include "database/tagcache.h"
#include "bg_task.h"

/* How long a task sleeps between checks. This is all deferred work with
 * nobody waiting on it, so the tick is slow deliberately. */
#define BG_TICK_PERIOD (HZ * 5)

/* How long to wait after a pass that did not finish. A pass fails either
 * because something interrupted it (USB, a database commit) or because the
 * memory it wanted was not free, and both are worth sitting out more than one
 * tick. The second especially: a failed core_alloc() is not a cheap no-op, it
 * compacts the whole pool and asks the audio buffer to shrink on the way. */
#define BG_RETRY_DELAY (HZ * 15)

/* How many times a stale task may hold off one it outranks before giving way.
 * Without a limit, a task whose pass can never succeed -- not enough memory
 * for it, ever -- would starve everything below it for the whole session. */
#define BG_MAX_PREEMPT_FAILS 3

/* Registered tasks, kept only so ranks can be compared. This must be no
 * *smaller* than the number of ticked tasks, which is what bg_task_init()
 * panics about: a task that did not fit is invisible to bg_task_preempted(),
 * so ranking silently stops working for everything below it. */
#define BG_MAX_TASKS 4
static struct bg_task *bg_tasks[BG_MAX_TASKS];
static int bg_tasks_count;

/* ---------------------------------------------------------------------------
 * The marker file
 * ------------------------------------------------------------------------ */

/* Marks that match nothing, so a task holding them is stale. */
static void bg_marks_none(struct bg_marks *m)
{
    m->entries = -1;
    m->commitid = -1;
    m->deleted = -1;
}

static bool bg_marks_equal(const struct bg_marks *a, const struct bg_marks *b)
{
    return a->entries == b->entries
        && a->commitid == b->commitid
        && a->deleted == b->deleted;
}

/* What the library looks like right now, as this task last understood it. */
static void bg_marks_now(const struct bg_task *task, struct bg_marks *m)
{
    struct tagcache_marks tm;

    tagcache_get_marks(&tm);
    m->entries = tagcache_get_stat()->total_entries;
    m->commitid = tm.commitid;

    /* -1 is "could not count", not a count, and the two must not be compared
     * as if they were. The database is only countable while it is held in
     * RAM, and a USB session drops it until something reloads it -- so a tick
     * landing in that window reads -1, which against a stored count reads as
     * a change and runs a pass; the next tick, with the cache back, reads the
     * real number and runs another. Carry the last real answer forward
     * instead. Never having had one (the cache is switched off) leaves this
     * -1 throughout, and deletions go unnoticed, which is the truth. */
    m->deleted = tm.deleted_ct < 0 ? task->done_marks.deleted : tm.deleted_ct;
}

/* The marks of the last completed pass, or none if there wasn't one.
 *
 * A file written before the marks existed holds a single number, so the parse
 * falls short and the task reads as never having run -- one pass each after
 * the firmware update, which is the right answer anyway since nothing before
 * this could see a deletion. */
static void bg_read_done(const struct bg_task *task, struct bg_marks *m)
{
    char buf[48];
    int fd = open(task->done_file, O_RDONLY);

    bg_marks_none(m);

    if (fd >= 0)
    {
        int n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0)
        {
            buf[n] = '\0';
            if (sscanf(buf, "%d %d %d",
                       &m->entries, &m->commitid, &m->deleted) != 3)
                bg_marks_none(m);
        }
        close(fd);
    }
}

static void bg_write_done(const struct bg_task *task,
                          const struct bg_marks *m)
{
    char buf[48];
    int fd = open(task->done_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (fd >= 0)
    {
        int n = snprintf(buf, sizeof(buf), "%d %d %d\n",
                         m->entries, m->commitid, m->deleted);
        write(fd, buf, n);
        close(fd);
    }
}

/* Forget that any pass ever completed, so the next tick runs one. The file
 * goes as well as the copy in RAM: a rebuild interrupted half way through
 * must not leave a marker behind claiming the library is covered, and a pass
 * that purges its own artifacts partway through must not leave the copy in
 * RAM saying they are still there. */
void bg_task_forget(struct bg_task *task)
{
    remove(task->done_file);
    bg_marks_none(&task->done_marks);
    bg_marks_none(&task->prev_marks);
    task->retry_at = 0;
    task->fails = 0;
    task->verified = false;
}

/* ---------------------------------------------------------------------------
 * Registration and ranking
 * ------------------------------------------------------------------------ */

void bg_task_init(struct bg_task *task)
{
    /* A .request-only task keeps none of this. It is never ticked, so it has
     * no marker to read and no pass to be preempted for -- registering it
     * would only put an entry in the rank table whose `running`/`wants_run`
     * can never become true. It also has no done_file, and reading one would
     * mean handing open() a NULL path. */
    if (task->request)
        return;

    bg_read_done(task, &task->done_marks);
    bg_marks_none(&task->prev_marks);

    if (bg_tasks_count >= BG_MAX_TASKS)
        panicf("bg_task: more than %d tasks", BG_MAX_TASKS);

    bg_tasks[bg_tasks_count++] = task;
}

bool bg_task_preempted(const struct bg_task *task)
{
    int i;

    for (i = 0; i < bg_tasks_count; i++)
    {
        const struct bg_task *other = bg_tasks[i];

        if (other == task || other->rank >= task->rank)
            continue;

        /* Waiting counts as much as running. A task that has found itself
         * stale but cannot start yet is exactly the one this pass has to get
         * out of the way of. */
        if (other->running || other->wants_run)
            return true;
    }
    return false;
}

const char *bg_task_state(const struct bg_task *task)
{
    if (task->running)
        return "Running";
    /* Stale and would start, but something it does not outrank is still in
     * the way -- another task's pass, or a database that is busy. */
    if (task->wants_run)
        return "Waiting";
    if (task->retry_at != 0)
        return "Backing off";
    return "Idle";
}

/* ---------------------------------------------------------------------------
 * The triggers
 * ------------------------------------------------------------------------ */

void bg_task_rebuild(struct bg_task *task)
{
    if (task->request)
    {
        task->request(true);
        return;
    }
    task->rebuild_req = true;
}

void bg_task_update(struct bg_task *task)
{
    if (task->request)
    {
        task->request(false);
        return;
    }
    task->update_req = true;
}

/* ---------------------------------------------------------------------------
 * The tick
 * ------------------------------------------------------------------------ */

void bg_task_tick(struct bg_task *task, struct event_queue *queue)
{
    struct queue_event ev;
    struct bg_marks now;
    bool finished;

    queue_wait_w_tmo(queue, &ev, BG_TICK_PERIOD);

    switch (ev.id)
    {
        case SYS_USB_CONNECTED:
            usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
            usb_wait_for_disconnect(queue);
            /* The library may have changed while we were a disk, and so may
             * the artifacts -- this is the one way they go missing without a
             * trigger, so it is also the one place worth re-checking. */
            bg_marks_none(&task->prev_marks);
            task->verified = false;
            return;

        case SYS_TIMEOUT:
            break;

        default:
            if (task->handle_event)
                task->handle_event(&ev);
            return;
    }

    /* A rebuild throws the artifacts away first; an update keeps them and
     * lets the pass fill in what is missing. Either way the task is stale
     * afterwards, which is what actually starts the work below. */
    if (task->rebuild_req)
    {
        task->rebuild_req = false;
        task->update_req = false;
        if (task->purge)
            task->purge();
        bg_task_forget(task);
    }
    else if (task->update_req)
    {
        task->update_req = false;
        bg_task_forget(task);
    }

    if (task->retry_at != 0)
    {
        if (!TIME_AFTER(current_tick, task->retry_at))
            return;
        task->retry_at = 0;
    }

    /* Never begin anything while a host has hold of us, and ahead of the gates
     * below, which reach the disk to answer.
     *
     * Trap: acknowledging the connect is not the end of it. A host that
     * unconfigures us mid-session broadcasts a disconnect before re-requesting
     * the disk, so the wait above returns with the cable still in. */
    if (usb_host_is_present())
        return;

    /* Only ever work against a database that is readable and holding still. */
    if (!tagcache_is_usable() || tagcache_is_busy())
    {
        bg_marks_none(&task->prev_marks);
        return;
    }

    /* Require the marks to be stable across two consecutive ticks, so a scan
     * still in flight is never mistaken for a settled library. */
    bg_marks_now(task, &now);
    if (!bg_marks_equal(&now, &task->prev_marks))
    {
        task->prev_marks = now;
        return;
    }

    if (bg_marks_equal(&now, &task->done_marks))
    {
        /* Already answered once, and only a USB session or a trigger can have
         * changed the answer -- both of which clear `verified`. Re-asking every
         * tick means a disk access every tick, forever, which on a player
         * without dircache is enough to keep ATA awake and hold off idle
         * poweroff for as long as the device is switched on. */
        if (task->verified)
            return;

        if (!task->artifact_ok || task->artifact_ok())
        {
            task->verified = true;
            task->wants_run = false;
            return;
        }
    }

    /* Stale. Announce that before the rank check rather than after: a task
     * kept waiting has to be visible to the pass it is waiting on, and that
     * visibility is the whole mechanism by which the other pass stands down. */
    task->wants_run = true;

    if (bg_task_preempted(task))
        return;

    task->running = true;
    finished = task->run();
    task->running = false;

    if (finished)
    {
        task->done_marks = now;
        task->prev_marks = now;
        task->wants_run = false;
        task->fails = 0;
        bg_write_done(task, &now);
        return;
    }

    /* Keep announcing the intent while the failure might still be someone
     * else's doing -- the likeliest reason a pass fails outright is that it
     * wanted memory a lower-ranked task has pinned, and that task only lets go
     * once it can see this. Give way after a few tries so a pass that can
     * never succeed does not starve everything below it. */
    if (++task->fails >= BG_MAX_PREEMPT_FAILS)
    {
        task->wants_run = false;
        task->fails = 0;
    }
    task->retry_at = current_tick + BG_RETRY_DELAY;
}

/* ---------------------------------------------------------------------------
 * The tag database
 *
 * It has no marker, no rank and no pass here, and nothing ticks it: it owns a
 * thread and a command queue of its own and needs neither. All it takes from
 * this file is the shape of the two triggers, so that a settings menu can
 * drive all three background tasks the same way.
 * ------------------------------------------------------------------------ */

static void tagcache_request(bool rebuild)
{
    if (rebuild)
        tagcache_rebuild();
    else
        tagcache_update();
}

struct bg_task tagcache_task =
{
    .request = tagcache_request,
};
