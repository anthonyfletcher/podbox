/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The saved aggregates: the file, its header, and the watermark that says
 * whether it still describes the log.
 *
 * Measured on a 3,500-track library with 9,672 entries: reading and parsing
 * the log costs 850 ms and aggregating it another 670 ms, boosted. This
 * removes the second entirely and most of the first, because a record is read
 * rather than derived.
 *
 * What it does NOT do is take responsibility for the numbers. The log is the
 * record; this is a cache of what the log adds up to. Every check below fails
 * towards a full rebuild, and none of them tries to repair anything.
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <file.h>
#include "config.h"
#include "rbpaths.h"
#include "pv_index.h"

#define PV_INDEX_PATH ROCKBOX_DIR "/pv_index.dat"

/* Written to a temporary name and renamed into place, so a write interrupted
 * by a flat battery leaves the previous index rather than a truncated one. */
#define PV_INDEX_TMP  ROCKBOX_DIR "/pv_index.new"

#define PV_INDEX_MAGIC   0x50564931UL   /* "PVI1" */
/* Bumped whenever the model's CONTENT changes shape, not just its layout.
 *
 * The size check below catches a changed struct; it cannot catch a changed
 * meaning. When pv_names stopped reading a leading track number as an artist,
 * every field kept its type and a saved index went on serving the old, wrong
 * rows -- so the fix looked like no fix at all. A naming or aggregation change
 * belongs here. */
#define PV_INDEX_VERSION 2

/* Bytes of the log kept verbatim, ending at the watermark. */
#define PV_INDEX_TAIL 64

struct pv_index_hdr
{
    unsigned long magic;
    unsigned long version;
    struct pv_index_id id;
    unsigned long covered;                 /* log bytes accounted for */
    unsigned char tail[PV_INDEX_TAIL];     /* the bytes just before that */
};

static int rd_fd = -1;
static int wr_fd = -1;

bool pv_index_read_begin(const struct pv_index_id *id, unsigned long log_size,
                         unsigned long *covered)
{
    struct pv_index_hdr hdr;
    unsigned char now[PV_INDEX_TAIL];
    int fd;

    fd = open(PV_INDEX_PATH, O_RDONLY);
    if (fd < 0)
        return false;

    if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)
        || hdr.magic != PV_INDEX_MAGIC
        || hdr.version != PV_INDEX_VERSION
        || memcmp(&hdr.id, id, sizeof(*id)) != 0)
    {
        close(fd);
        return false;
    }

    /* The log cannot have shrunk. If it has, it is not the same log. */
    if (hdr.covered > log_size)
    {
        close(fd);
        return false;
    }

    /* And the bytes where the index stopped must still be those bytes. This
     * is what distinguishes "the log grew" from "the log was replaced". */
    if (hdr.covered >= PV_INDEX_TAIL)
    {
        if (pv_log_peek((enum pv_source)hdr.id.source, hdr.covered,
                        now, PV_INDEX_TAIL) != PV_INDEX_TAIL
            || memcmp(now, hdr.tail, PV_INDEX_TAIL) != 0)
        {
            close(fd);
            return false;
        }
    }

    rd_fd = fd;
    if (covered)
        *covered = hdr.covered;
    return true;
}

bool pv_index_read(void *dst, size_t bytes)
{
    if (rd_fd < 0)
        return false;
    if (bytes == 0)
        return true;
    return read(rd_fd, dst, bytes) == (ssize_t)bytes;
}

void pv_index_read_end(void)
{
    if (rd_fd >= 0)
        close(rd_fd);
    rd_fd = -1;
}

bool pv_index_write_begin(const struct pv_index_id *id,
                          unsigned long covered, enum pv_source src)
{
    struct pv_index_hdr hdr;

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic   = PV_INDEX_MAGIC;
    hdr.version = PV_INDEX_VERSION;
    hdr.id      = *id;
    hdr.covered = covered;

    if (covered >= PV_INDEX_TAIL)
    {
        /* A short read here is not fatal: the tail simply will not match next
         * time and the index is rebuilt, which is the safe direction. */
        pv_log_peek(src, covered, hdr.tail, PV_INDEX_TAIL);
    }

    wr_fd = open(PV_INDEX_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (wr_fd < 0)
        return false;

    if (write(wr_fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr))
    {
        pv_index_write_abort();
        return false;
    }
    return true;
}

bool pv_index_write(const void *src, size_t bytes)
{
    if (wr_fd < 0)
        return false;
    if (bytes == 0)
        return true;
    if (write(wr_fd, src, bytes) != (ssize_t)bytes)
    {
        pv_index_write_abort();
        return false;
    }
    return true;
}

bool pv_index_write_end(void)
{
    if (wr_fd < 0)
        return false;

    close(wr_fd);
    wr_fd = -1;

    /* Only now does the new index become the index. */
    remove(PV_INDEX_PATH);
    return rename(PV_INDEX_TMP, PV_INDEX_PATH) == 0;
}

void pv_index_write_abort(void)
{
    if (wr_fd >= 0)
        close(wr_fd);
    wr_fd = -1;
    remove(PV_INDEX_TMP);
}

void pv_index_discard(void)
{
    remove(PV_INDEX_PATH);
}
