/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to text_viewer.c.
 ****************************************************************************/
#ifndef _TEXT_VIEWER_H_
#define _TEXT_VIEWER_H_

#include <stdbool.h>
#include <stddef.h>

/* Core-linked document viewer, built on the ts_* extraction engine (see
 * txt_source.h). Opens `file` -- plain text, markdown, html, rtf, fb2, epub,
 * docx or pdf -- and pages through its text. Returns a GO_TO_* code.
 *
 * Where the reader got to is remembered per document and restored on open, so
 * reopening a file resumes it with no help from the caller. */
int text_viewer(const char *file);

/* Whether a book with more than one chapter is open, for the reader's menu */
bool text_viewer_has_contents(void);

/* The open book's contents as a list; picking an entry goes to that chapter.
 * Returns 1 after a pick, which closes the menu it was opened from, and
 * MENU_ATTACHED_USB after USB. */
int text_viewer_contents(void);

/* Path of the most recently read document, for a caller that wants to reopen
 * it. False if nothing has been read, or the record is unusable. The document
 * may since have been deleted; the caller checks. */
bool text_viewer_last_document(char *buf, size_t bufsize);

/* Writes the reading places an older firmware kept as text into their
 * libfile, at the first boot of a new layout */
bool text_viewer_convert_places(const char *text_file);

/* text_viewer_colour_mode; new ones go at the end, after the cfg names */
enum
{
    TV_COLOUR_THEME = 0,
    TV_COLOUR_INVERTED,
    TV_COLOUR_BOW,           /* black on white */
    TV_COLOUR_WOB,           /* white on black */
    TV_COLOUR_CREAM,
    TV_COLOUR_PEACH,
    TV_COLOUR_YELLOW,
    TV_COLOUR_SEPIA,
    TV_COLOUR_NIGHT,
    TV_COLOUR_CUSTOM,        /* text_viewer_fg on text_viewer_bg */
};

/* The reading places, newest first: each document's path, how far through
 * it the page left on ends in percent (-1 when not known) and whether it was
 * read to its end, until `fn` returns false */
typedef bool (*text_viewer_place_fn)(const char *path, int pct,
                                     bool finished, void *data);
void text_viewer_each_place(text_viewer_place_fn fn, void *data);

/* Whether `path` was read to its end, or marked Finished */
bool text_viewer_finished(const char *path);

/* Marks `path` as the Books shelf's Mark as does: In progress keeps its
 * place (or gives it one at its start), Finished as well and flags it,
 * Not started forgets it. False if the file could not be written. */
enum { TV_MARK_IN_PROGRESS, TV_MARK_NOT_STARTED, TV_MARK_FINISHED };
bool text_viewer_mark_place(const char *path, int state);

/* text_viewer_footer */
enum
{
    TV_FOOTER_OFF = 0,
    TV_FOOTER_PAGE,
    TV_FOOTER_CHAPTER,       /* chapter number and pages left in it */
    TV_FOOTER_BOOK,          /* how far through the book */
};

/* text_viewer_orientation */
enum
{
    TV_ORIENT_UPRIGHT = 0,
    TV_ORIENT_RIGHT,         /* sideways, the top of the player to the left */
    TV_ORIENT_LEFT,          /* sideways, the top to the right */
};

/* text_viewer_align */
enum
{
    TV_ALIGN_LEFT = 0,
    TV_ALIGN_JUSTIFIED,
};

/* text_viewer_read_aloud */
enum
{
    TV_READ_OFF = 0,
    TV_READ_PAGE,
    TV_READ_CONTINUOUS,
};

#endif /* _TEXT_VIEWER_H_ */
