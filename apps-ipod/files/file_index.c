/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * An index of the documents and images on the player.
 *
 * Nothing else indexes them. The tag database knows about audio and nothing
 * but audio, so a flat "every document on the device" list has no existing
 * source to read -- which is why this walks the disk itself.
 *
 * One walk produces both lists. Classifying a file is a lookup on its
 * extension (filetype_get_attr()), so having found a file it is as cheap to
 * ask both questions as one; walking twice would double the only expensive
 * part.
 *
 * The lists are plain text, one path per line, written to a .tmp and renamed
 * only on a completed walk -- so an interrupted one leaves the previous list
 * standing rather than a partial one that reads as the whole truth.
 *
 * Why this is not a normal bg_task
 * --------------------------------
 * bg_task's staleness rule is the database's entry count (see bg_task.c), and
 * that does not move when a .txt or a .jpg appears. So this uses the
 * .request-only shape -- the same one the tag database itself uses -- and
 * supplies its own triggers: the two menu entries, a USB session (the one
 * moment files reliably change), and having no index at all at startup.
 *
 * Parts, in order:
 *   - the lists on disk
 *   - the walk
 *   - the thread, and what wakes it
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "rbpaths.h"
#include "usb.h"
#include "core_alloc.h"
#include "database/tagcache.h"
#include "system/bg_task.h"
#include "files/filetypes.h"
#include "files/path_list.h"
#include "file_index.h"

#define FILE_INDEX_DOCS   ROCKBOX_DIR "/docs.lst"
#define FILE_INDEX_IMAGES ROCKBOX_DIR "/images.lst"

/* Deepest nesting the walk will follow. Recursion here costs stack, and the
 * thread's is modest; anything filed deeper than this is almost certainly not
 * a document someone means to browse. */
#define FILE_INDEX_MAX_DEPTH 12

#define FI_STACK_SIZE (DEFAULT_STACK_SIZE * 2)
static long fi_stack[FI_STACK_SIZE / sizeof(long)];
static const char fi_thread_name[] = "fileidx";
static struct event_queue fi_queue;

/* Set when something has happened that the lists would not reflect. */
static volatile bool fi_wants_scan;
static volatile bool fi_abort;
static volatile bool fi_busy;

/* The walk's open output files, and how many lines each has taken. */
static int fi_docs_fd = -1;
static int fi_images_fd = -1;
static int fi_docs_count;
static int fi_images_count;

/* One path buffer, reused down the recursion rather than one per level:
 * MAX_PATH per frame times the depth limit is far more stack than the thread
 * has. The walk appends to it and truncates on the way back out. */
static char fi_path[MAX_PATH];

bool file_index_is_busy(void)
{
    return fi_busy;
}

const char *file_index_list(bool images)
{
    return images ? FILE_INDEX_IMAGES : FILE_INDEX_DOCS;
}

/* ---- the walk ---------------------------------------------------------- */

/* Give up promptly for a USB session or a shutdown -- this holds no lock, but
 * it does hold the disk, and a walk can run for minutes. */
static bool fi_check_abort(void)
{
    struct queue_event ev;

    if (fi_abort)
        return true;

    /* Trap: the queue alone is too late for a host. With the USB stack the
     * broadcast comes from the mass-storage driver at SET_CONFIGURATION, so
     * nothing arrives here until enumeration is nearly done -- by which point a
     * walk still hammering the disk has already cost the host SET_ADDRESS.
     * usb_host_is_present() is true from the moment the cable goes in.
     *
     * The same test already gates the *start* of a scan in fi_thread(); this is
     * the one that stops a walk already under way. */
    if (usb_host_is_present())
    {
        fi_abort = true;
        return true;
    }

    queue_wait_w_tmo(&fi_queue, &ev, 0);
    switch (ev.id)
    {
        case SYS_USB_CONNECTED:
        case SYS_POWEROFF:
        case SYS_REBOOT:
            /* Put it back for the thread loop to deal with properly; this only
             * needs to know that the walk must stop. */
            queue_post(&fi_queue, ev.id, ev.data);
            fi_abort = true;
            return true;
    }
    return false;
}

/* Cover art rather than a picture someone put here to be looked at.
 *
 * These lists are for images filed deliberately, and a music library holds far
 * more album art than anything else -- unfiltered, the art buries the rest.
 * The names matched are the conventional ones the artwork search itself looks
 * for (see search_albumart_files() in metadata/albumart.c): cover.* and
 * folder.*, any extension.
 *
 * It cannot catch everything. Album art is also commonly named after the album
 * or the track, which is indistinguishable from any other filename without
 * asking the database what the album is called -- and this walk deliberately
 * knows nothing about the database. Getting the two conventional names is what
 * makes the difference in practice. */
static bool fi_is_cover_art(const char *name)
{
    static const char * const art_names[] = { "cover", "folder" };
    const char *dot = strrchr(name, '.');
    size_t stem = dot ? (size_t)(dot - name) : strlen(name);
    unsigned i;

    for (i = 0; i < ARRAYLEN(art_names); i++)
    {
        if (stem == strlen(art_names[i])
            && !strncasecmp(name, art_names[i], stem))
            return true;
    }
    return false;
}

static void fi_record(int fd, int *count, const char *path)
{
    if (fd < 0 || *count >= PATH_LIST_MAX)
        return;
    fdprintf(fd, "%s\n", path);
    (*count)++;
}

/* Both lists full: nothing more can be recorded, so stop walking. */
static bool fi_lists_full(void)
{
    return fi_docs_count >= PATH_LIST_MAX
        && fi_images_count >= PATH_LIST_MAX;
}

/* Walk 'fi_path' (a directory) and everything under it. */
static void fi_walk(int depth)
{
    DIR *dir;
    struct dirent *entry;
    size_t len = strlen(fi_path);

    if (depth > FILE_INDEX_MAX_DEPTH)
        return;

    dir = opendir(fi_path);
    if (!dir)
        return;

    while ((entry = readdir(dir)))
    {
        struct dirinfo info;

        if (fi_check_abort() || fi_lists_full())
            break;

        /* "." and "..", and by the same test every dot-directory: none of
         * them holds anything a person filed there to read, and /.rockbox in
         * particular is full of files that would classify as documents. */
        if (entry->d_name[0] == '.')
            continue;

        /* Rebuild the path at this level each time round; the previous
         * entry's name is simply overwritten. */
        if (len + 1 + strlen((char *)entry->d_name) >= sizeof(fi_path))
            continue;
        snprintf(fi_path + len, sizeof(fi_path) - len, "%s%s",
                 len > 1 ? "/" : "", (char *)entry->d_name);

        info = dir_get_info(dir, entry);
        if (info.attribute & ATTR_DIRECTORY)
        {
            fi_walk(depth + 1);
        }
        else
        {
            switch (filetype_get_attr(fi_path) & FILE_ATTR_MASK)
            {
                case FILE_ATTR_TXT:
                    fi_record(fi_docs_fd, &fi_docs_count, fi_path);
                    break;
                case FILE_ATTR_IMG:
                    if (!fi_is_cover_art((char *)entry->d_name))
                        fi_record(fi_images_fd, &fi_images_count, fi_path);
                    break;
            }
        }

        /* Back to the directory this level is walking. */
        fi_path[len] = '\0';
        yield();
    }

    closedir(dir);
}

/* One complete pass. */
static void fi_run_scan(void)
{
    bool completed;

    fi_abort = false;
    fi_busy = true;
    fi_docs_count = fi_images_count = 0;

    fi_docs_fd = open(FILE_INDEX_DOCS ".tmp", O_CREAT|O_WRONLY|O_TRUNC, 0666);
    fi_images_fd = open(FILE_INDEX_IMAGES ".tmp",
                        O_CREAT|O_WRONLY|O_TRUNC, 0666);

    strcpy(fi_path, "/");
    fi_walk(0);

    if (fi_docs_fd >= 0)
        close(fi_docs_fd);
    if (fi_images_fd >= 0)
        close(fi_images_fd);
    fi_docs_fd = fi_images_fd = -1;

    completed = !fi_abort;
    if (completed)
    {
        remove(FILE_INDEX_DOCS);
        rename(FILE_INDEX_DOCS ".tmp", FILE_INDEX_DOCS);
        remove(FILE_INDEX_IMAGES);
        rename(FILE_INDEX_IMAGES ".tmp", FILE_INDEX_IMAGES);
    }
    else
    {
        remove(FILE_INDEX_DOCS ".tmp");
        remove(FILE_INDEX_IMAGES ".tmp");
    }

    fi_busy = false;
    fi_wants_scan = !completed;   /* interrupted: try again later */
}

/* ---- the thread -------------------------------------------------------- */

static void fi_thread(void)
{
    struct queue_event ev;

    while (1)
    {
        queue_wait_w_tmo(&fi_queue, &ev, HZ * 5);

        switch (ev.id)
        {
            case SYS_USB_CONNECTED:
                usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
                usb_wait_for_disconnect(&fi_queue);
                /* The host has had the disk. This is the one moment files
                 * reliably appear and disappear without anyone telling us. */
                fi_wants_scan = true;
                break;

            case SYS_TIMEOUT:
                /* Only while the database is leaving the disk alone: both
                 * walk it, and two at once makes each slower than the pair
                 * run in turn.
                 *
                 * And never while a host has hold of us: the disk is about to
                 * be taken back. The run is gated rather than the arming, so
                 * the request stays pending and a real extraction still gets
                 * its scan however early the question is asked. */
                if (fi_wants_scan && !tagcache_is_busy()
                    && !usb_host_is_present())
                {
                    fi_wants_scan = false;
                    fi_run_scan();
                }
                break;
        }
    }
}

/* bg_task.request: both triggers do the same thing here. There is nothing to
 * keep between passes -- the walk rewrites both lists from scratch -- so a
 * rebuild and an update are the same walk. */
static void fi_request(bool rebuild)
{
    (void)rebuild;
    fi_wants_scan = true;
}

struct bg_task file_index_task =
{
    .request = fi_request,
};

void file_index_init(void)
{
    queue_init(&fi_queue, true);
    bg_task_init(&file_index_task);

    /* Nothing to read on a first boot, or after the lists were deleted. */
    if (!file_exists(FILE_INDEX_DOCS) && !file_exists(FILE_INDEX_IMAGES))
        fi_wants_scan = true;

    create_thread(fi_thread, fi_stack, sizeof(fi_stack), 0,
                  fi_thread_name IF_PRIO(, PRIORITY_BACKGROUND)
                  IF_COP(, CPU));
}
