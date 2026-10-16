/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The music files the metadata parser refused, so a walk parses each one once
 * per change rather than at every update, and the owner can see which they
 * are.
 *
 * Two files in the library's cache folder: unreadable.dat, the path key and
 * modification time of each refusal, which the walk reads; and unreadable.txt,
 * their paths, which the owner reads. A refusal stands until the file changes,
 * a rebuild forgets them all, or a different firmware is running -- its parser
 * may read what this one could not.
 *
 * Parts, in order:
 *   - the table
 *   - a walk: begin, the two questions, end
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "file.h"
#include "crc32.h"
#include "system/library_files.h"
#include "system/strutil.h"     /* read_line */
#include "database/libfile.h"
#include "database/path_key.h"
#include "database/unreadable.h"
#ifndef DBTOOL
#include "version.h"
#endif

#define UNREADABLE_DAT   LIB_UNREADABLE_FILE
#define UNREADABLE_TXT   LIB_UNREADABLE_LIST

/* Past this many, a refusal is not remembered and the file is parsed again
 * at the next walk, as every file was before. */
#define UNREADABLE_MAX 256

/* ------------------------------------------------------------------ *
 * the table                                                          *
 * ------------------------------------------------------------------ */

struct unreadable_rec
{
    uint64_t key;
    uint32_t mtime;
    uint32_t seen;              /* met by this walk; never written as set */
};

static struct unreadable_rec table[UNREADABLE_MAX];
static int count;
static int list_fd = -1;        /* unreadable.txt.new, while a walk runs */

/* Which firmware made the file. The date is in rbversion, so every build
 * differs and each tries the refusals again once. The desktop tool has no
 * version, and marks its file as no firmware's. */
static int32_t firmware_mark(void)
{
#ifdef DBTOOL
    return 0;
#else
    return (int32_t)crc_32(rbversion, strlen(rbversion), 0xffffffff);
#endif
}

static struct unreadable_rec *find(uint64_t key)
{
    for (int i = 0; i < count; i++)
        if (table[i].key == key)
            return &table[i];
    return NULL;
}

static void list_line(const char *line)
{
    if (list_fd >= 0)
    {
        write(list_fd, line, strlen(line));
        write(list_fd, "\n", 1);
    }
}

/* Without the player's volume prefix, which means nothing to a reader */
static void list_path(const char *path)
{
    list_line(path_key_strip(path));
}

/* ------------------------------------------------------------------ *
 * a walk                                                             *
 * ------------------------------------------------------------------ */

void unreadable_begin(void)
{
    struct libfile_header h;
    int fd;

    count = 0;
    fd = libfile_open(UNREADABLE_DAT, LIB_UNREADABLE_MAGIC,
                      LIB_UNREADABLE_VERSION, sizeof(struct unreadable_rec), &h,
                      NULL);
    if (fd >= 0)
    {
        if (h.marks.commitid == firmware_mark())
        {
            while (count < UNREADABLE_MAX && (uint32_t)count < h.count
                   && read(fd, &table[count], sizeof(table[0]))
                      == (ssize_t)sizeof(table[0]))
                table[count++].seen = 0;
        }
        close(fd);
    }

    list_fd = open(UNREADABLE_TXT ".new", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    list_line("# Music files the database could not read. Each is tried "
              "again when it changes.");
}

bool unreadable_known(const char *path, unsigned long mtime)
{
    struct unreadable_rec *r = find(path_key(path));

    if (r == NULL || r->mtime != (uint32_t)mtime)
        return false;
    r->seen = 1;
    list_path(path);
    return true;
}

void unreadable_add(const char *path, unsigned long mtime)
{
    uint64_t key = path_key(path);
    struct unreadable_rec *r = find(key);

    if (r == NULL)
    {
        if (count >= UNREADABLE_MAX)
            return;
        r = &table[count++];
        r->key = key;
    }
    r->mtime = mtime;
    r->seen = 1;
    list_path(path);
}

/* A walk that missed folders keeps their refusals, and their lines from the
 * last list: they were not tried, so nothing about them has changed. */
static void list_unseen(void)
{
    char line[MAX_PATH];
    int fd = open(UNREADABLE_TXT, O_RDONLY);

    if (fd < 0)
        return;
    while (read_line(fd, line, sizeof(line)) > 0)
    {
        struct unreadable_rec *r;

        if (line[0] == '#')
            continue;
        r = find(path_key(line));
        if (r != NULL && !r->seen)
            list_line(line);
    }
    close(fd);
}

void unreadable_end(bool complete)
{
    struct libfile_writer w;
    struct libfile_marks m;
    bool begun, ok;
    int kept = 0;

    if (!complete)
        list_unseen();

    libfile_no_marks(&m);
    m.commitid = firmware_mark();
    begun = libfile_begin(&w, UNREADABLE_DAT, LIB_UNREADABLE_MAGIC,
                          LIB_UNREADABLE_VERSION, sizeof(struct unreadable_rec),
                          &m);
    ok = begun;
    for (int i = 0; ok && i < count; i++)
    {
        struct unreadable_rec r = table[i];

        if (!r.seen && complete)
            continue;
        r.seen = 0;
        ok = libfile_write(&w, &r, sizeof(r), 1);
        kept++;
    }
    /* A write that failed leaves the last walk's file as it was */
    if (begun)
    {
        libfile_finish(&w, ok && kept > 0);
        if (ok && kept == 0)
            remove(UNREADABLE_DAT);
    }

    /* The list follows the records: left as it was when they could not be
     * written, removed only when there are none to list */
    if (list_fd >= 0)
    {
        close(list_fd);
        list_fd = -1;
        if (!ok || kept == 0
            || rename(UNREADABLE_TXT ".new", UNREADABLE_TXT) < 0)
        {
            remove(UNREADABLE_TXT ".new");
            if (ok && kept == 0)
                remove(UNREADABLE_TXT);
        }
    }
    count = 0;
}

void unreadable_forget(void)
{
    remove(UNREADABLE_DAT);
    remove(UNREADABLE_TXT);
}
