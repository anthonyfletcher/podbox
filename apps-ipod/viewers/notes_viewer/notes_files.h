/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to notes_files.c: the page index, and the written pages of a
 * track's album and artist.
 ****************************************************************************/

#ifndef _NOTES_FILES_H_
#define _NOTES_FILES_H_

#include <stdbool.h>
#include <stddef.h>
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
    uint32_t path;      /* where it is among the saved paths */
    uint32_t hash;      /* name_index_path_hash() of the path */
    uint32_t stamp;     /* the file's size and time, as one number */
    uint8_t  kind;
    uint8_t  rank;      /* 2 for .md, 1 for .txt */
    uint16_t pad;
};

/* The pages, 0 to notes_page_count() - 1, in the order the walk found
 * them: none while the setting is off or no walk has written the index.
 * notes_page_get() is NULL for an index out of range, and for every page
 * while a completed walk's index replaces the last one. A page number or
 * pointer held across a yield is good only while notes_pages_generation()
 * is unchanged. */
int notes_page_count(void);
const struct notes_page *notes_page_get(int i);
/* The page's path into `buf`, read from the saved index; false, and `buf`
 * empty, when it cannot be read */
bool notes_page_path(const struct notes_page *pg, char *buf, size_t size);
/* Whether `a` is to be read over `b`, a page of the same name (or NULL):
 * one in /Notes over one beside the music, then .md over .txt */
bool notes_page_better(const struct notes_page *a, const struct notes_page *b);
/* Whether page `page` replaces `best` (-1 for none) as the page to read:
 * better, or as good and first */
bool notes_page_wins(int page, int best);

/* The pages whose key, alt or part is `value`, one a call: `*at` is -1 at
 * first, and -1 is returned when there are no more. They come in page
 * order, so taking each that notes_page_wins() chooses the page a look at
 * every page would. 0 finds nothing. */
#define NOTES_BY_KEY  0
#define NOTES_BY_ALT  1
#define NOTES_BY_PART 2
int notes_pages_by(int field, uint32_t value, int *at);
/* The same for the pages beside the music in the folder path_key() `dir` */
int notes_pages_in(uint64_t dir, int *at);
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
/* Changes whenever the index is loaded or replaced, renumbering the pages:
 * a page number kept from before is found again by its keys */
unsigned int notes_pages_generation(void);

/* The written pages a track has, as a mask */
#define NOTES_BIO   1
#define NOTES_ALBUM 2

struct notes_pages
{
    int have;                   /* NOTES_BIO and NOTES_ALBUM */
    int bio, album;             /* their pages, -1 for none */
};

/* The bio of the album artist (or artist) and the notes of the album, into
 * `p`; their mask. In /Notes by name, else beside the music: notes.md in
 * the folder of `track`, a path of one of the album's tracks or NULL, and
 * bio.md in the folder above it when that is named for the artist. Reads
 * no file. */
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

/* Front matter is a line of exactly --- opening the page, to the next ---
 * or ... line, for as long as its lines are YAML: blank, indented, a list's
 * or a comment's, or a key of at most 32 bytes and its colon. Whether the
 * line at `s` is. */
bool notes_yaml_line(const char *s);

/* The value of `key` in the front matter `text` starts with, a `key: value`
 * line, into `buf` without its quotes; its length, 0 for none. A page of an
 * album's has `album: Artist - Album`. */
#define NOTES_FRONT_ALBUM "album"
size_t notes_front_value(const char *text, const char *key, char *buf,
                         size_t size);

#endif /* _NOTES_FILES_H_ */
