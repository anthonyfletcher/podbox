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
 * section's heading as a string, and the byte offset of the name in the
 * file's text as the Markdown back end gives it */
struct name_mention
{
    uint32_t key;
    uint16_t file;
    uint16_t section;           /* into the strings, or NAME_NONE */
    uint32_t at;
};
#define NAME_NONE 0xffff

/* A page the index read: its number in the page index (notes_files.h) and
 * name_index_path_hash() of its path, which says whether that number is
 * still the same page */
struct name_file
{
    uint32_t hash;
    uint16_t page, pad;
};

/* The buffer name_index_load() needs for the largest index the task
 * writes */
#define NAME_INDEX_LOAD_BYTES (168 * 1024)

/* The index, loaded whole into a caller's buffer by name_index_load() */
struct name_index
{
    const struct name_mention *m;
    int count;                  /* mentions, sorted by key, then file, then at */
    const struct name_file *files;
    int nfiles;
    const char *strings;        /* NUL-separated */
    const uint32_t *str_off;    /* each string's offset into `strings` */
    int nstrings;
    const uint32_t *name_key;   /* each name key, and the string of its name */
    const uint16_t *name_str;   /* as first written */
    int nnames;
};

/* The background task's registration; called before bg_task_start() */
void name_index_init(void);

/* The file index's walk has listed the pages again: read them if they
 * changed */
void name_index_pages_changed(void);

/* Reads the index into `buf`. False when there is none, or it does not fit. */
bool name_index_load(void *buf, size_t size, struct name_index *ix);

/* The hash a page's path is recorded by */
uint32_t name_index_path_hash(const char *path);

/* The name the key `key` was first written as, or NULL */
const char *name_index_name(const struct name_index *ix, uint32_t key);

/* Whether anyone with the name key `key` or `alt` is named anywhere,
 * without loading the index: for a menu deciding whether to offer a row */
bool name_index_named(uint32_t key, uint32_t alt);

#endif /* _NAME_INDEX_H_ */
