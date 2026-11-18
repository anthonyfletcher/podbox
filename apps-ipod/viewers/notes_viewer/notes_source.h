/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to notes_source.c: a page's Markdown, written or made up, with
 * its {{pieces}} filled in.
 ****************************************************************************/

#ifndef _NOTES_SOURCE_H_
#define _NOTES_SOURCE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "notes_links.h"

struct name_index;

/* What a page is about */
struct notes_doc
{
    int kind;                   /* NOTES_PAGE_*, or 0 for a name's page */
    int page;                   /* the written page, or -1 */
    int album;                  /* the library album, or -1 */
    long artist_seek;           /* the library album artist, or -1 */
    uint32_t artist;            /* the album artist's name key: links on the
                                   page prefer their albums */
    uint32_t key;               /* the page's own name key */
    char title[128];            /* its name, set on the banner */
};

/* The Markdown of the page `t` reaches into `out` (`size` bytes, NUL
 * ended), using `scratch` for the written file and the template; what the
 * page is about into `d`. `ix` is the names index for {{mentions}}, or NULL.
 * The bytes written; 0 when there is no such page. */
size_t notes_source(const struct notes_target *t, struct notes_doc *d,
                    const struct name_index *ix, char *out, size_t size,
                    char *scratch, size_t scratch_size);

#endif /* _NOTES_SOURCE_H_ */
