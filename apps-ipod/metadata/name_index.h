/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to name_index.c: the people named in the bios and liner notes
 * in /Notes, and where each is named.
 ****************************************************************************/

#ifndef _NAME_INDEX_H_
#define _NAME_INDEX_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/* One place a person is named: their name key (art_name.h), the file, the
 * section's heading as an offset into that file's headings, and the byte
 * offset of the name in the file's text as the Markdown back end gives
 * it */
struct name_mention
{
    uint32_t key;
    uint16_t file;
    uint16_t section;           /* see name_index_section(), or NAME_NONE */
    uint32_t at;
};
#define NAME_NONE 0xffff
#define NAME_NO_STRING 0xffffffffu

/* A page the index read: its number in the page index (notes_files.h),
 * name_index_path_hash() of its path, which says whether that number is
 * still the same page, its front matter's album as a string, and where its
 * headings start among the strings */
struct name_file
{
    uint32_t hash;
    uint32_t album;             /* or NAME_NO_STRING */
    uint32_t heads;
    uint16_t page, pad;
};

/* The index, open. It is read a block at a time into a cache in the
 * buffer given to name_index_open(), so any buffer of a block or more
 * opens it, and a larger one reads the disk less. */
struct ni_slot;
struct name_index
{
    int fd;
    uint32_t size;
    uint32_t count;             /* mentions, sorted by key, then file, then at */
    uint32_t nfiles, nalbums, nnames;
    uint32_t mentions, mfence, nfence, files, albums, names, pool, pool_end;
    struct ni_slot *slots;
    char *data;
    int nslots;
    uint32_t tick;
};

/* The background task's registration; called before bg_task_start() */
void name_index_init(void);

/* The file index's walk has listed the pages again: read them if they
 * changed */
void name_index_pages_changed(void);

/* Opens the index with its cache in `buf`, which stays the index's until
 * name_index_close(). False when there is none, it is damaged, or `buf` is
 * smaller than one block. */
bool name_index_open(void *buf, size_t size, struct name_index *ix);
void name_index_close(struct name_index *ix);

/* The first mention whose key is `key` or above, ix->count when none, and
 * mention `i`; false past the end or when it cannot be read */
uint32_t name_index_first(struct name_index *ix, uint32_t key);
bool name_index_mention(struct name_index *ix, uint32_t i,
                        struct name_mention *m);

/* Whether file `file` names `key` */
bool name_index_names_in(struct name_index *ix, uint32_t key,
                         uint32_t file);

/* File `file`, and the file number of the `i`th of the ix->nalbums files
 * whose front matter names an album (-1 when none) */
bool name_index_file(struct name_index *ix, uint32_t file,
                     struct name_file *f);
int name_index_album_file(struct name_index *ix, uint32_t i);

/* The string at `off`, or the heading of `m` in its file `f`, into `buf`;
 * false, and `buf` empty, when there is none */
bool name_index_string(struct name_index *ix, uint32_t off, char *buf,
                       size_t size);
bool name_index_section(struct name_index *ix, const struct name_file *f,
                        const struct name_mention *m, char *buf, size_t size);

/* The name the key `key` was first written as; false when none */
bool name_index_name(struct name_index *ix, uint32_t key, char *buf,
                     size_t size);

/* How many files name `key`, at most 0xffff */
int name_index_files_naming(struct name_index *ix, uint32_t key);

/* The hash a page's path is recorded by */
uint32_t name_index_path_hash(const char *path);

/* Whether anyone with the name key `key` or `alt` is named anywhere,
 * without a buffer: for a menu deciding whether to offer a row */
bool name_index_named(uint32_t key, uint32_t alt);

#endif /* _NAME_INDEX_H_ */
