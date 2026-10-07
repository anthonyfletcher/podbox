/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The first load of a new layout: library_files.h says where each file
 * lives, and this moves the files of an older layout there, once.
 *
 * library/format.txt holds the layout's version. A firmware that expects a
 * higher one runs the steps after it at boot, before anything reads the
 * files and before USB is answered. Every step skips what is already done,
 * so a boot cut short by a flat battery simply finishes the rest next time;
 * format.txt is written only once every step has succeeded. Each move is a
 * rename on the same volume, so nothing is copied. What it did is appended
 * to logs/upgrade.log.
 *
 * Parts, in order:
 *   - the layout's version and the log
 *   - moving one file
 *   - the steps: folders, files, numbered playback logs, a database kept
 *     elsewhere
 *   - converting the files whose format changed, into libfiles
 *   - dead names
 *   - library_files_init()
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include "config.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "string-extra.h"
#include "rbpaths.h"
#include "system/strutil.h"
#include "system/library_files.h"
#include "database/libfile.h"
#include "metadata/art_cache.h"     /* ART_CACHE_FORMAT_VERSION */

/* The layout these sources expect. A change to it adds steps and bumps this. */
#define LIBRARY_FORMAT 1
#define FORMAT_FILE    LIB_DIR "/format.txt"

/* ------------------------------------------------------------------ *
 * the layout's version and the log                                   *
 * ------------------------------------------------------------------ */

static int read_format(void)
{
    char buf[16];
    int fd = open(FORMAT_FILE, O_RDONLY);
    int n;

    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return atoi(buf);
}

static bool write_format(void)
{
    int fd = open(FORMAT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (fd < 0)
        return false;
    bool ok = fdprintf(fd, "%d\n", LIBRARY_FORMAT) > 0;
    close(fd);
    return ok;
}

static void upgrade_log(const char *fmt, ...)
{
    char line[2 * MAX_PATH + 32];
    va_list ap;
    int fd = open(LIB_UPGRADE_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);

    if (fd < 0)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fdprintf(fd, "[%8ld] %s\n", current_tick, line);
    close(fd);
}

/* ------------------------------------------------------------------ *
 * moving one file                                                    *
 * ------------------------------------------------------------------ */

static bool exists(const char *path)
{
    return file_exists(path) || dir_exists(path);
}

/* Moves from to to. Nothing to move, or something already at to, counts as
 * done: a file at the new name is newer than any left at the old one, which
 * is kept for the owner to look at. False only when a rename failed. */
static bool move_file(const char *from, const char *to)
{
    if (!exists(from))
        return true;
    if (exists(to))
    {
        upgrade_log("kept %s: %s already there", from, to);
        return true;
    }
    if (rename(from, to) < 0)
    {
        upgrade_log("FAILED %s -> %s (%d)", from, to, errno);
        return false;
    }
    upgrade_log("moved %s -> %s", from, to);
    return true;
}

/* ------------------------------------------------------------------ *
 * the steps                                                          *
 * ------------------------------------------------------------------ */

static const char *const folders[] = {
    LIB_DIR, LIB_USER_DIR, LIB_CACHE_DIR, LIB_LOGS_DIR,
};

static const struct move {
    const char *from;
    const char *to;
} moves[] = {
    { ROCKBOX_DIR "/db_sound.dat",           LIB_SOUND_FILE },
    { ROCKBOX_DIR "/db_sound.part",          LIB_SOUND_PART },
    { ROCKBOX_DIR "/db_sound.cal",           LIB_SOUND_CAL_FILE },
    { ROCKBOX_DIR "/database_changelog.txt", LIB_PLAYS_FILE },
    { ROCKBOX_DIR "/playback.log",           LIB_PLAYBACK_LOG },
    { ROCKBOX_DIR "/audiobooks.resume",      LIB_AUDIOBOOKS_FILE },
    { ROCKBOX_DIR "/pv_badges.dat",          LIB_BADGES_FILE },
    { ROCKBOX_DIR "/musicquiz.scores",       LIB_QUIZ_FILE },
    { ROCKBOX_DIR "/spike.scores",           LIB_SPIKE_FILE },
    { ROCKBOX_DIR "/known_artists.txt",      LIB_KNOWN_ARTISTS_FILE },
    { ROCKBOX_DIR "/playername.txt",         LIB_PLAYER_NAME_FILE },
    { ROCKBOX_DIR "/db_summary.dat",         LIB_ALBUMS_FILE },
    { ROCKBOX_DIR "/db_summary.plays",       LIB_ALBUM_PLAYS_FILE },
    { ROCKBOX_DIR "/album_covers.cfg",       LIB_COVERS_FILE },
    { ROCKBOX_DIR "/pv_index.dat",           LIB_REPORT_INDEX_FILE },
    { ROCKBOX_DIR "/pv_moves.dat",           LIB_REPORT_MOVES_FILE },
    { ROCKBOX_DIR "/docs.lst",               LIB_DOCUMENTS_FILE },
    { ROCKBOX_DIR "/images.lst",             LIB_IMAGES_FILE },
    { ROCKBOX_DIR "/spike.run",              LIB_SPIKE_RUN_FILE },
    /* The thumbnails go as one folder (see move_folder()), and their lists
     * after them */
    { ROCKBOX_DIR "/thumbcache",             LIB_ART_DIR },
    { LIB_ART_DIR "/noart_albums.lst",  LIB_ART_DIR "/no_art_albums.txt" },
    { LIB_ART_DIR "/noart_artists.lst", LIB_ART_DIR "/no_art_artists.txt" },
    { ROCKBOX_DIR "/carousel/emptyslide.pfraw", LIB_COVERS_EMPTY_FILE },
    { ROCKBOX_DIR "/tagcache.log",           LIB_TAGCACHE_LOG },
    { ROCKBOX_DIR "/artcache.log",           LIB_ART_LOG },
    { ROCKBOX_DIR "/usb-log.txt",            LIB_USB_LOG },
    { ROCKBOX_DIR "/buffer-damage.log",      LIB_BUFFER_LOG },
};

/* Files nothing reads any more, and the half-written copies an older
 * firmware may have left. The art cache's markers go only after
 * convert_stamps() has read them. */
static const char *const dead[] = {
    ROCKBOX_DIR "/pv_names.dat",
    ROCKBOX_DIR "/db_summary.done",
    LIB_ART_DIR "/done.txt",
    LIB_ART_DIR "/format.txt",
    LIB_ART_DIR "/coverflow/_fallback.aat",
    LIB_ART_DIR "/list/_fallback.aat",
    LIB_ART_DIR "/wps/_fallback.aat",
    ROCKBOX_DIR "/database_state.tcd",
    ROCKBOX_DIR "/stage0.log",
    ROCKBOX_DIR "/audiobooks.resume.tmp",
    ROCKBOX_DIR "/spike.scores.tmp",
    ROCKBOX_DIR "/db_summary.tmp",
    ROCKBOX_DIR "/pv_index.new",
    ROCKBOX_DIR "/db_sound.dat.new",
    ROCKBOX_DIR "/docs.lst.tmp",
    ROCKBOX_DIR "/images.lst.tmp",
    ROCKBOX_DIR "/database_changelog.txt.new",
    LIB_ART_DIR "/stamps.dat.tmp",
    LIB_ART_DIR "/noart_albums.lst.tmp",
    LIB_ART_DIR "/noart_artists.lst.tmp",
};

/* A folder to a new name, or into the folder already there. That one is
 * newer -- something wrote it after an earlier boot failed to move this --
 * so where both hold a file, the old copy is dropped. Only caches are moved
 * this way. */
static bool move_folder(const char *from, const char *to)
{
    bool moved;

    if (!dir_exists(from))
        return true;
    if (!exists(to))
        return move_file(from, to);

    upgrade_log("merging %s into %s", from, to);
    do
    {
        DIR *dir = opendir(from);
        struct dirent *de;

        if (!dir)
            return false;
        moved = false;
        while ((de = readdir(dir)) != NULL)
        {
            char src[MAX_PATH], dst[MAX_PATH];

            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            snprintf(src, sizeof(src), "%s/%s", from, de->d_name);
            snprintf(dst, sizeof(dst), "%s/%s", to, de->d_name);
            if (dir_get_info(dir, de).attribute & ATTR_DIRECTORY)
            {
                if (!move_folder(src, dst))
                {
                    closedir(dir);
                    return false;
                }
            }
            else if (exists(dst) ? remove(src) < 0 : rename(src, dst) < 0)
            {
                /* Stop rather than scan the folder again for ever */
                closedir(dir);
                upgrade_log("FAILED %s -> %s (%d)", src, dst, errno);
                return false;
            }
            moved = true;
        }
        closedir(dir);
    } while (moved);    /* entries removed under the reader can be skipped */

    return rmdir(from) == 0 || !dir_exists(from);
}

/* playback_0001.log and on. Names are gathered before any is moved: renaming
 * out of a folder while reading it can skip entries. */
static bool move_playback_logs(void)
{
    char names[16][32];
    int n;

    do
    {
        DIR *dir = opendir(ROCKBOX_DIR);
        struct dirent *de;

        if (!dir)
            return true;
        n = 0;
        while (n < (int)ARRAYLEN(names) && (de = readdir(dir)) != NULL)
        {
            size_t len = strlen(de->d_name);
            if (len < sizeof(names[0]) && len > 13
                && !strncasecmp(de->d_name, "playback_", 9)
                && !strcasecmp(de->d_name + len - 4, ".log"))
                strmemccpy(names[n++], de->d_name, sizeof(names[0]));
        }
        closedir(dir);

        for (int i = 0; i < n; i++)
        {
            char from[MAX_PATH], to[MAX_PATH];
            snprintf(from, sizeof(from), ROCKBOX_DIR "/%s", names[i]);
            snprintf(to, sizeof(to), LIB_USER_DIR "/%s", names[i]);
            /* One that stays behind would be found again for ever */
            if (!move_file(from, to) || exists(from))
                return false;
        }
    } while (n == (int)ARRAYLEN(names));

    return true;
}

/* The folder the removed Database Directory setting named, if it is not
 * /.rockbox. Read from config.cfg, which still holds the line. */
static bool old_database_dir(char *out, size_t size)
{
    char line[MAX_PATH + 32];
    char *name, *value;
    bool found = false;
    int fd = open(CONFIGFILE, O_RDONLY);

    if (fd < 0)
        return false;
    while (read_line(fd, line, sizeof(line)) > 0)
    {
        if (settings_parseline(line, &name, &value)
            && !strcmp(name, "database path"))
        {
            strmemccpy(out, value, size);
            found = true;
        }
    }
    close(fd);
    if (!found)
        return false;

    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/')
        out[--len] = '\0';
    return len > 0 && strcasecmp(out, ROCKBOX_DIR);
}

/* A database kept elsewhere is the one that was in use, so it replaces
 * whatever /.rockbox holds, which is older. */
static bool move_database(void)
{
    char dir[MAX_PATH];
    char from[MAX_PATH], to[MAX_PATH];
    bool ok = true;

    if (!old_database_dir(dir, sizeof(dir)))
        return true;
    snprintf(from, sizeof(from), "%s/database_idx.tcd", dir);
    snprintf(to, sizeof(to), "%s/database_tmp.tcd", dir);
    if (!file_exists(from) && !file_exists(to))
        return true;

    upgrade_log("database in %s", dir);
    for (int i = -2; i < 32; i++)
    {
        char name[32];

        if (i == -2)
            strcpy(name, "database_idx.tcd");
        else if (i == -1)
            strcpy(name, "database_tmp.tcd");
        else
            snprintf(name, sizeof(name), "database_%d.tcd", i);
        snprintf(from, sizeof(from), "%s/%s", dir, name);
        snprintf(to, sizeof(to), ROCKBOX_DIR "/%s", name);
        if (!file_exists(from))
        {
            /* A stale file /.rockbox has and the moved one does not */
            if (file_exists(to))
                remove(to);
            continue;
        }
        remove(to);
        ok &= move_file(from, to);
    }

    snprintf(from, sizeof(from), "%s/database_changelog.txt", dir);
    if (file_exists(from))
    {
        remove(LIB_PLAYS_FILE);
        ok &= move_file(from, LIB_PLAYS_FILE);
    }
    return ok;
}

/* ------------------------------------------------------------------ *
 * converting the files whose format changed                          *
 * ------------------------------------------------------------------ */

/* The magic an old file starts with, or 0 */
static uint32_t first_word(const char *path)
{
    uint32_t w = 0;
    int fd = open(path, O_RDONLY);

    if (fd >= 0)
    {
        if (read(fd, &w, sizeof(w)) != (ssize_t)sizeof(w))
            w = 0;
        close(fd);
    }
    return w;
}

/* Up to three numbers from a small text file; how many were read */
static int read_numbers(const char *path, int *v)
{
    char buf[48];
    int fd = open(path, O_RDONLY);
    int n;

    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return sscanf(buf, "%d %d %d", &v[0], &v[1], &v[2]);
}

/* stamps.dat: "AST1", then 8-byte records. Its format.txt becomes the
 * version, and done.txt the marks, so a cache that was current stays so. */
static bool convert_stamps(void)
{
    static const char path[] = LIB_ART_DIR "/stamps.dat";
    struct libfile_writer w;
    struct libfile_marks marks;
    unsigned char buf[256];
    int done[3];
    int fd, version, n;
    bool ok = true;

    if (first_word(path) != 0x31545341u)        /* "AST1" */
        return true;

    version = read_numbers(LIB_ART_DIR "/format.txt", done) >= 1 ? done[0] : 0;
    libfile_no_marks(&marks);
    if (read_numbers(LIB_ART_DIR "/done.txt", done) == 3)
    {
        marks.entries = done[0];
        marks.commitid = done[1];
        marks.deleted = done[2];
    }

    fd = open(path, O_RDONLY);
    if (fd < 0 || lseek(fd, 4, SEEK_SET) != 4
        || !libfile_begin(&w, path, LIB_STAMPS_MAGIC, version, 8, &marks))
    {
        if (fd >= 0)
            close(fd);
        return false;
    }
    while (ok && (n = read(fd, buf, sizeof(buf))) >= 8)
        ok = libfile_write(&w, buf, n & ~7, n / 8);
    close(fd);
    ok = libfile_finish(&w, ok);
    upgrade_log("%s %s", ok ? "converted" : "FAILED to convert", path);
    return ok;
}

/* album_plays.dat: bare 12-byte records */
static bool convert_album_plays(void)
{
    struct libfile_writer w;
    struct libfile_header h;
    unsigned char buf[240];
    int fd, n;
    bool ok = true;

    if (!file_exists(LIB_ALBUM_PLAYS_FILE)
        || libfile_peek(LIB_ALBUM_PLAYS_FILE, LIB_PLAYS_MAGIC,
                        LIB_PLAYS_VERSION, &h))
        return true;

    fd = open(LIB_ALBUM_PLAYS_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    if (ffilesize(fd) % 12 != 0)
    {
        /* Misaligned: no record in it can be trusted */
        close(fd);
        remove(LIB_ALBUM_PLAYS_FILE);
        upgrade_log("dropped misaligned %s", LIB_ALBUM_PLAYS_FILE);
        return true;
    }
    if (!libfile_begin(&w, LIB_ALBUM_PLAYS_FILE, LIB_PLAYS_MAGIC,
                       LIB_PLAYS_VERSION, 12, NULL))
    {
        close(fd);
        return false;
    }
    while (ok && (n = read(fd, buf, sizeof(buf))) > 0)
        ok = libfile_write(&w, buf, n, n / 12);
    close(fd);
    ok = libfile_finish(&w, ok);
    upgrade_log("%s %s", ok ? "converted" : "FAILED to convert",
                LIB_ALBUM_PLAYS_FILE);
    return ok;
}

/* report_badges.dat: "PVB1", the row count, a bit per row, then a 32-bit
 * time per row. Each row becomes { when, seen, pad[3] }. */
static bool convert_badges(void)
{
    struct libfile_writer w;
    uint32_t hdr[2];
    unsigned char seen[128];
    uint32_t when;
    int fd;
    bool ok;

    if (first_word(LIB_BADGES_FILE) != 0x50564231u)     /* "PVB1" */
        return true;

    fd = open(LIB_BADGES_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    ok = read(fd, hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)
         && hdr[1] <= sizeof(seen) * 8
         && read(fd, seen, (hdr[1] + 7) / 8) == (ssize_t)((hdr[1] + 7) / 8);
    if (!ok)
    {
        /* Too short to hold what it claims: no earned date is trustworthy */
        close(fd);
        remove(LIB_BADGES_FILE);
        upgrade_log("dropped unreadable %s", LIB_BADGES_FILE);
        return true;
    }
    if (!libfile_begin(&w, LIB_BADGES_FILE, LIB_BADGES_MAGIC,
                       LIB_BADGES_VERSION, 8, NULL))
    {
        close(fd);
        return false;
    }
    for (uint32_t i = 0; ok && i < hdr[1]; i++)
    {
        unsigned char rec[8] = { 0 };

        ok = read(fd, &when, sizeof(when)) == (ssize_t)sizeof(when);
        memcpy(rec, &when, sizeof(when));
        rec[4] = (seen[i / 8] >> (i % 8)) & 1;
        ok = ok && libfile_write(&w, rec, sizeof(rec), 1);
    }
    close(fd);
    ok = libfile_finish(&w, ok);
    upgrade_log("%s %s", ok ? "converted" : "FAILED to convert",
                LIB_BADGES_FILE);
    return ok;
}

/* Rebuildable, and rebuilt when the report next opens */
static void drop_old_moves(void)
{
    if (first_word(LIB_REPORT_MOVES_FILE) == 0x50564d31u)   /* "PVM1" */
    {
        remove(LIB_REPORT_MOVES_FILE);
        upgrade_log("removed old %s", LIB_REPORT_MOVES_FILE);
    }
}

/* ------------------------------------------------------------------ *
 * dead names                                                         *
 * ------------------------------------------------------------------ */

static void remove_dead(void)
{
    for (unsigned i = 0; i < ARRAYLEN(dead); i++)
    {
        if (file_exists(dead[i]) && remove(dead[i]) == 0)
            upgrade_log("removed %s", dead[i]);
    }
    rmdir(ROCKBOX_DIR "/carousel");     /* only if now empty */
}

/* ------------------------------------------------------------------ *
 * library_files_init()                                               *
 * ------------------------------------------------------------------ */

bool library_files_need_upgrade(void)
{
    return read_format() < LIBRARY_FORMAT;
}

void library_files_init(void (*progress)(int done, int total))
{
    const int total = ARRAYLEN(moves) + 4;
    int done = 0;
    bool ok = true;

    for (unsigned i = 0; i < ARRAYLEN(folders); i++)
        mkdir(folders[i]);

    if (!library_files_need_upgrade())
        return;

    upgrade_log("upgrade from layout %d to %d", read_format(), LIBRARY_FORMAT);

    for (unsigned i = 0; i < ARRAYLEN(moves); i++)
    {
        if (dir_exists(moves[i].from))
            ok &= move_folder(moves[i].from, moves[i].to);
        else
            ok &= move_file(moves[i].from, moves[i].to);
        if (progress)
            progress(++done, total);
    }

    ok &= move_playback_logs();
    if (progress)
        progress(++done, total);

    ok &= move_database();
    if (progress)
        progress(++done, total);

    ok &= convert_stamps() & convert_album_plays() & convert_badges();
    drop_old_moves();
    if (progress)
        progress(++done, total);

    /* Only once the markers have been read into what replaced them */
    if (ok)
        remove_dead();
    if (progress)
        progress(++done, total);

    if (ok && write_format())
        upgrade_log("done");
    else
        upgrade_log("incomplete: the rest at the next boot");
}
