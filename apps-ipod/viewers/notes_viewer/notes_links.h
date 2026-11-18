/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to notes_links.c: what a [Name] in a page links to.
 ****************************************************************************/

#ifndef _NOTES_LINKS_H_
#define _NOTES_LINKS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What a link reaches */
#define NOTES_TO_NONE   0       /* nothing: the name is plain text */
#define NOTES_TO_PAGE   1       /* a written page; `album` too when it is a
                                   library album's */
#define NOTES_TO_ARTIST 2       /* a library album artist's made-up page */
#define NOTES_TO_ALBUM  3       /* a library album's made-up page */
#define NOTES_TO_TRACK  4       /* a track, on its album's page */
#define NOTES_TO_NAME   5       /* a name two or more pages mention */
#define NOTES_TO_SECTION 6      /* a heading of the page the link is on,
                                   whose name key is `key` */

struct notes_target
{
    int to;                     /* NOTES_TO_* */
    int page;                   /* the page index's, or -1 */
    int album;                  /* a row of the database's album table, or -1 */
    long artist_seek;           /* NOTES_TO_ARTIST's, else -1 */
    int track;                  /* NOTES_TO_TRACK's idx_id, else -1 */
    uint32_t key;               /* the name's key */
};

/* The library's albums as links match them, worked out into `buf` once
 * for a screen: false when the database's RAM copy is not loaded (links
 * then reach written pages alone) or `size` is too small. Bytes needed for
 * the library as it is. */
struct name_index;
size_t notes_links_need(void);
/* `ix` is the names index, which a name two or more pages give is found in,
 * or NULL */
bool notes_links_begin(void *buf, size_t size, const struct name_index *ix);
void notes_links_end(void);

/* Where the link written `name` (its `len` bytes, qualifier and all) goes,
 * from a page about the album artist whose key is `artist` and the album
 * `album` (a row, or -1). False for plain text. */
bool notes_link_find(const char *name, size_t len, uint32_t artist,
                     int album, struct notes_target *t);

/* The library album a written album page is about: by its folder beside
 * the music, else by its artist and title. -1 for none. */
int notes_page_album(int page);

/* The name of a library album row and its album artist, through Trim
 * Titles when it is on */
bool notes_album_title(int album, char *buf, size_t size);
bool notes_album_artist(int album, char *buf, size_t size);

#endif /* _NOTES_LINKS_H_ */
