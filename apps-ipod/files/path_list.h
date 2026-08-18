/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to path_list.c.
 ****************************************************************************/
#ifndef _PATH_LIST_H
#define _PATH_LIST_H

#include <stdbool.h>

/* Hard ceiling on lines held, whatever a caller asks for. These lists are
 * meant to be read by a person; past a few hundred the screen is not the
 * right tool and the offset table is not worth the memory. */
#define PATH_LIST_MAX 500

struct path_list {
    bool  held;                    /* the scratch buffer is ours */
    char *text;                    /* the file, newlines turned into NULs */
    int   line[PATH_LIST_MAX];     /* offset of each path into text */
    int   count;
    bool  truncated;               /* more lines, or more bytes, than were kept */
};

/* Read 'file' and index its lines. False when there is nothing to show -- no
 * such file, or empty -- having claimed nothing. On success the caller must
 * path_list_free() when done: every pointer handed out points into that
 * memory.
 *
 * The lines live in the linker-reserved scratch buffer (system/app_buffer.h),
 * not in a core_alloc(). Three reasons, and the first is the one users notice:
 *
 *  - core_alloc() from a screen makes the audio buffer shrink and the current
 *    track rebuffer, so opening a list interrupted playback;
 *  - the handle had to be pinned for as long as the list was on screen, and a
 *    pinned block is one buflib cannot compact across -- so a viewer opened
 *    from the list got a smaller contiguous pool than the same viewer opened
 *    from anywhere else;
 *  - the scratch buffer has a fixed size, which bounds the read: only the
 *    first PATH_LIST_MAX lines are kept, whatever the file's size.
 *
 * Only one list exists at a time, and holding this buffer means nothing else
 * may ask for it -- so a caller that opens another screen (browser_flat opens
 * a viewer) must path_list_free() first and load again afterwards. */
bool path_list_load(struct path_list *pl, const char *file, int max_entries);

/* Hands the scratch buffer back. Safe to call on a list that never loaded, and
 * safe to call twice. */
void path_list_free(struct path_list *pl);

/* The whole path, and just its last component. Both return "" out of range,
 * so a list callback can hand the result straight back. */
const char *path_list_get(const struct path_list *pl, int index);
const char *path_list_leaf(const struct path_list *pl, int index);

/* Writing a list.
 *
 * Both producers -- the file index and the artwork cache's "found nothing"
 * lists -- publish the same way: lines go to a ".tmp" beside the real file,
 * which replaces it only when the pass that wrote it finished. An interrupted
 * pass leaves the previous list standing.
 *
 * Trap, and the reason this lives here rather than in each producer: renaming
 * a ".tmp" that was never created still removes the published file first, so a
 * writer that skips the open() check destroys the previous list and puts
 * nothing in its place. Both producers had that bug independently. _open()
 * reports failure and _close() re-checks, so neither can have it again.
 *
 * A producer writing a pair of lists should open both before recording
 * anything and publish neither unless both opened -- see fi_run_scan(). */
struct path_list_writer {
    int   fd;          /* < 0 when the open failed; nothing is published */
    const char *path;  /* the published name; ".tmp" is derived from it */
    int   count;
};

bool path_list_write_open(struct path_list_writer *w, const char *file);
void path_list_write_record(struct path_list_writer *w, const char *line);
void path_list_write_close(struct path_list_writer *w, bool completed);

/* True once the writer has taken all the lines a reader could show, so a walk
 * feeding it can stop early. */
bool path_list_write_full(const struct path_list_writer *w);

#endif /* _PATH_LIST_H */
