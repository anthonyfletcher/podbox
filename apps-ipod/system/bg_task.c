/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The thread every background task shares. See bg_task.h for what a task
 * is.
 *
 * Parts, in order:
 *   - the marker file (what the last completed pass covered)
 *   - registration and ranking
 *   - the triggers
 *   - the tick, which is where all the policy is
 *   - the thread
 *   - the tag database's forwarding-only task
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "kernel.h"
#include "thread.h"
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
static struct bg_task *bg_tasks[BG_MAX_TASKS];   /* kept in rank order */
static int bg_tasks_count;

/* .request-only tasks with a tick to take, in registration order. */
#define BG_MAX_STEPS 2
static struct bg_task *bg_steps[BG_MAX_STEPS];
static int bg_steps_count;

/* Sized for the deepest pass: the artwork cache's JPEG decoder and the album
 * index's builder each need DEFAULT_STACK_SIZE + 0x2000, and the file index's
 * recursive walk fits inside that. */
#define BG_STACK_SIZE (DEFAULT_STACK_SIZE + 0x2000)
static long bg_stack[BG_STACK_SIZE / sizeof(long)];
static const char bg_thread_name[] = "bgtask";
static struct event_queue bg_queue;

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
    task->gave_way = false;
    task->verified = false;
}

/* ---------------------------------------------------------------------------
 * Registration and ranking
 * ------------------------------------------------------------------------ */

void bg_task_init(struct bg_task *task)
{
    int i;

    /* A .request-only task keeps none of the marker state. It stays out of the
     * rank table, whose `running`/`wants_run` it would never set, and it has
     * no done_file -- reading one would mean handing open() a NULL path. */
    if (task->request)
    {
        if (!task->tick)
            return;
        if (bg_steps_count >= BG_MAX_STEPS)
            panicf("bg_task: more than %d steps", BG_MAX_STEPS);
        bg_steps[bg_steps_count++] = task;
        return;
    }

    bg_read_done(task, &task->done_marks);
    bg_marks_none(&task->prev_marks);

    if (bg_tasks_count >= BG_MAX_TASKS)
        panicf("bg_task: more than %d tasks", BG_MAX_TASKS);

    /* Rank order is tick order: whoever outranks runs first. */
    for (i = bg_tasks_count; i > 0 && bg_tasks[i - 1]->rank > task->rank; i--)
        bg_tasks[i] = bg_tasks[i - 1];
    bg_tasks[i] = task;
    bg_tasks_count++;
}

size_t bg_task_reserve_bytes(void)
{
    size_t most = 0;
    int i;

    for (i = 0; i < bg_tasks_count; i++)
    {
        if (bg_tasks[i]->work_bytes > most)
            most = bg_tasks[i]->work_bytes;
    }

    return most;
}

static bool bg_task_stale(struct bg_task *task);

bool bg_task_preempted(const struct bg_task *task)
{
    int i;

    for (i = 0; i < bg_tasks_count; i++)
    {
        struct bg_task *other = bg_tasks[i];

        if (other == task || other->rank >= task->rank)
            continue;

        if (other->rebuild_req || other->update_req)
            return true;

        /* Trap: asking a task that gave way whether it is stale says yes
         * again once its back-off ends, and a pass that restarts from the
         * beginning, like the file walk, is then never let finish */
        if (other->gave_way)
            continue;

        /* The other task cannot tick while this pass holds the thread, so the
         * pass asks on its behalf, no more often than its own tick would. That
         * keeps the two-tick stability check meaningful and the deleted-count
         * walk to once a period. */
        if (!other->running && TIME_AFTER(current_tick, other->next_check))
        {
            other->next_check = current_tick + BG_TICK_PERIOD;
            bg_task_stale(other);
        }

        /* Waiting counts as much as running. A task that has found itself
         * stale but cannot start yet is exactly the one this pass has to get
         * out of the way of. */
        if (other->running || other->wants_run)
            return true;
    }
    return false;
}

/* The event is peeked, never taken. The queue is in the broadcast list, so the
 * storage handover counts an acknowledgement from it and hands the host the
 * disk only once every count is in -- and the thread loop is what sends it. A
 * pass that swallowed the event would leave the handover waiting for an
 * acknowledgement nobody can still send, and the player would report an empty
 * drive for as long as it stayed plugged in. */
bool bg_task_should_stop(const struct bg_task *task)
{
    struct queue_event ev;

    if (bg_task_preempted(task))
        return true;

    /* Trap: the queue alone is too late for a host. Nothing arrives on it
     * until SET_CONFIGURATION, by which point a pass holding the CPU has
     * already cost the host SET_ADDRESS. usb_host_is_present() is true from
     * the moment the cable goes in. */
    if (usb_host_is_present())
        return true;

    if (!queue_peek(&bg_queue, &ev))
        return false;

    switch (ev.id)
    {
        case SYS_USB_CONNECTED:
        case SYS_POWEROFF:
        case SYS_REBOOT:
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

/* Whether the task has work it may start: past its back-off, the database
 * settled, and the library moved from what its last pass covered or the
 * output gone. Sets `wants_run` when so, which is what a pass it outranks
 * watches for. Asked from inside such a pass too -- see bg_task_preempted(). */
static bool bg_task_stale(struct bg_task *task)
{
    struct bg_marks now;

    if (task->retry_at != 0)
    {
        if (!TIME_AFTER(current_tick, task->retry_at))
            return false;
        task->retry_at = 0;
    }

    /* Never begin anything while a host has hold of us, and ahead of the gates
     * below, which reach the disk to answer.
     *
     * Trap: acknowledging the connect is not the end of it. A host that
     * unconfigures us mid-session broadcasts a disconnect before re-requesting
     * the disk, so the thread's wait returns with the cable still in. */
    if (usb_host_is_present())
        return false;

    /* Only ever work against a database that is readable and holding still. */
    if (!tagcache_is_usable() || tagcache_is_busy())
    {
        bg_marks_none(&task->prev_marks);
        return false;
    }

    /* Require the marks to be stable across two consecutive ticks, so a scan
     * still in flight is never mistaken for a settled library. */
    bg_marks_now(task, &now);
    if (task->gave_way && !bg_marks_equal(&now, &task->gave_way_marks))
        task->gave_way = false;
    if (!bg_marks_equal(&now, &task->prev_marks))
    {
        task->prev_marks = now;
        return false;
    }

    if (bg_marks_equal(&now, &task->done_marks))
    {
        /* Already answered once, and only a USB session or a trigger can have
         * changed the answer -- both of which clear `verified`. Re-asking every
         * tick means a disk access every tick, forever, which on a player
         * without dircache is enough to keep ATA awake and hold off idle
         * poweroff for as long as the device is switched on. */
        if (task->verified)
            return false;

        if (!task->artifact_ok || task->artifact_ok())
        {
            task->verified = true;
            task->wants_run = false;
            return false;
        }
    }

    /* Stale. Announce that before the rank check rather than after: a task
     * kept waiting has to be visible to the pass it is waiting on, and that
     * visibility is the whole mechanism by which the other pass stands down. */
    task->wants_run = true;
    return true;
}

/* One ranked task's turn on the thread. */
static void bg_task_tick(struct bg_task *task)
{
    struct bg_marks covered;
    bool finished;

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

    task->next_check = current_tick + BG_TICK_PERIOD;
    if (!bg_task_stale(task) || bg_task_preempted(task))
        return;

    /* The marks the stale check settled on, and so the ones the pass ran
     * against. */
    covered = task->prev_marks;

    task->running = true;
    finished = task->run();
    task->running = false;

    if (finished)
    {
        task->done_marks = covered;
        task->prev_marks = covered;
        task->wants_run = false;
        task->fails = 0;
        task->gave_way = false;
        bg_write_done(task, &covered);
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
        task->gave_way = true;
        task->gave_way_marks = covered;
    }
    task->retry_at = current_tick + BG_RETRY_DELAY;
}

/* ---------------------------------------------------------------------------
 * The thread
 * ------------------------------------------------------------------------ */

static void bg_thread(void)
{
    struct queue_event ev;
    int i;

    while (1)
    {
        queue_wait_w_tmo(&bg_queue, &ev, BG_TICK_PERIOD);

        switch (ev.id)
        {
            case SYS_USB_CONNECTED:
                usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
                usb_wait_for_disconnect(&bg_queue);
                /* The library may have changed while we were a disk, and so
                 * may the artifacts -- this is the one way they go missing
                 * without a trigger, so it is also the one place worth
                 * re-checking. */
                for (i = 0; i < bg_tasks_count; i++)
                {
                    bg_marks_none(&bg_tasks[i]->prev_marks);
                    bg_tasks[i]->verified = false;
                }
                for (i = 0; i < bg_steps_count; i++)
                    bg_steps[i]->request(false);
                break;

            case SYS_TIMEOUT:
                for (i = 0; i < bg_tasks_count; i++)
                    bg_task_tick(bg_tasks[i]);
                for (i = 0; i < bg_steps_count; i++)
                    bg_steps[i]->tick();
                break;

            default:
                /* bg_task_post() carries the task in the data word. A
                 * broadcast has SYS_EVENT set and is nobody's here. */
                if (!(ev.id & SYS_EVENT))
                {
                    struct bg_task *task = (struct bg_task *)ev.data;
                    task->handle_event(&ev);
                }
                break;
        }
    }
}

void bg_task_start(void)
{
    queue_init(&bg_queue, true);
    create_thread(bg_thread, bg_stack, sizeof(bg_stack), 0,
                  bg_thread_name IF_PRIO(, PRIORITY_BACKGROUND)
                  IF_COP(, CPU));
}

void bg_task_post(struct bg_task *task, long id)
{
    queue_post(&bg_queue, id, (intptr_t)task);
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
