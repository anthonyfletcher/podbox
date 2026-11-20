/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to notes_files.c: the page index, and the written pages of a
 * track's album and artist.
 ****************************************************************************/

#ifndef _NOTES_FILES_H_
#define _NOTES_FILES_H_

#include <stdbool.h>
#include <stdint.h>
#include "file.h"           /* MAX_PATH */

#define NOTES_DIR "/Notes"

/* A page of the index. Its keys are art_name.h's: */
#define NOTES_PAGE_PERSON 1     /* /Notes/People, or bio.* beside the music */
#define NOTES_PAGE_ALBUM  2     /* /Notes/Albums, or notes.* */
#define NOTES_PAGE_OTHER  3     /* any other folder of /Notes, or its root */

struct notes_page
{
    uint32_t key;       /* its whole name; an album's artist and title as a
                           work, art_name_work_hash() */
    uint32_t alt;       /* the name read "Surname, First" the other way */
    uint32_t part;      /* an album's title alone; else the name without
                           its qualifier, 0 when it has none */
    uint32_t by;        /* an album's artist; another page's folder */
    uint64_t dir;       /* beside the music, path_key() of its folder */
    uint32_t path;
    uint32_t size, mtime;
    uint8_t  kind;
    uint8_t  rank;      /* 2 for .md, 1 for .txt */
    uint16_t pad;
};

/* The pages, 0 to notes_page_count() - 1: none while the setting is off or
 * no walk has written the index. notes_page_get() is NULL for an index out
 * of range, and for every page while a completed walk's index replaces the
 * last one: a caller holding an index across a yield checks it. */
int notes_page_count(void);
const struct notes_page *notes_page_get(int i);
const char *notes_page_path(const struct notes_page *pg);
/* Whether `a` is to be read over `b`, a page of the same name (or NULL):
 * one in /Notes over one beside the music, then .md over .txt */
bool notes_page_better(const struct notes_page *a, const struct notes_page *b);
/* The name a page's file gives it: its file name without the extension, or
 * beside the music its folder's name */
void notes_page_name(const struct notes_page *pg, char *name, size_t size);
/* `len` bytes of `name` without a trailing " (qualifier)" */
size_t notes_bare_len(const char *name, size_t len);

/* The page index, kept in memory from the file index's walk: read at boot
 * (false with none yet, which a walk is to write); then a walk's begin,
 * each file it passes, and its end. The last index answers until a walk
 * completes, which replaces and saves it; an interrupted walk leaves it. */
bool notes_where_load(void);
void notes_where_begin(void);
void notes_where_found(const char *path, uint32_t size, uint32_t mtime);
void notes_where_end(bool completed);

/* The written pages a track has, as a mask */
#define NOTES_BIO   1
#define NOTES_ALBUM 2

struct notes_pages
{
    int have;                   /* NOTES_BIO and NOTES_ALBUM */
    char bio[MAX_PATH];         /* the bio's file */
    char person[MAX_PATH];      /* who it is about, as its file is named */
    char album[MAX_PATH];       /* the notes' file */
};

/* The bio of the album artist (or artist) and the notes of the album, into
 * `p`; their mask. In /Notes by name, else beside the music: notes.md in
 * the folder of `track`, a path of one of the album's tracks or NULL, and
 * bio.md in the folder above it when that is named for the artist. */
int notes_find_names(const char *albumartist, const char *artist,
                     const char *album, const char *track,
                     struct notes_pages *p);

/* An album the screen opens without its music playing: its names, and a
 * path of one of its tracks for the files beside the music */
struct notes_album
{
    char albumartist[128], artist[128], album[128], year[8];
    char path[MAX_PATH];
};

/* The first track a search on tag_filename finds, and its album's names.
 * The caller finishes the search. */
struct tagcache_search;
bool notes_album_read(struct tagcache_search *tcs, struct notes_album *a);

/* The same for the album `album_seek` by the album artist `artist_seek`
 * (a seek, a split artist's handle, or -1 for any). False when the database
 * has none. */
bool notes_album_of(long album_seek, long artist_seek, struct notes_album *a);

/* notes_find_names() of it */
int notes_find_album(const struct notes_album *a, struct notes_pages *p);

/* Whether there is a written page of the person called `name`, in /Notes
 * or beside the music */
bool notes_has_bio(const char *name);

#endif /* _NOTES_FILES_H_ */
