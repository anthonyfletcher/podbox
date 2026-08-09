/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to edit_line.c: the click-wheel text entry model and renderer.
 ****************************************************************************/

/* One line of click-wheel-editable text: the model, the key handling and the
 * drawing, with no opinion about what encloses it.
 *
 * The caret is an insertion point *between* characters (a bar, 0..len), not a
 * block sitting on one:
 *
 *   - the wheel inserts a character at the bar and then cycles it in place, so
 *     text can be added anywhere, not just appended at the end;
 *   - Left/Right move the bar one gap, which commits the character being
 *     composed (the next spin inserts a fresh one);
 *   - holding Left/Right backspaces the character before the bar / deletes the
 *     one after it.
 *
 * The two caret shapes state what the wheel will do: a bar means "I will
 * insert a new character here", the inverse-video block means "I will keep
 * changing this one".
 *
 * What accepts, cancels or otherwise surrounds the line is the caller's:
 * keyboard.c wraps it in a dialog with a Cancel/OK row, and the database
 * search wraps it in a dialog with a results list. Both get the same editor.
 */

#ifndef _GUI_EDIT_LINE_H_
#define _GUI_EDIT_LINE_H_

#include <stdbool.h>
#include "draw/screen_access.h"
#include "rbunicode.h"

#define EDIT_LINE_MAX 512       /* absolute cap on editable characters */

struct edit_line {
    ucschar_t text[EDIT_LINE_MAX];
    int len;
    int caret;           /* insertion point, 0..len: the gap *before* text[caret] */
    int max_len;
    int scroll;          /* horizontal pixel scroll to keep the caret visible */
    long last_wheel_tick;
    long last_del_tick;  /* throttles held backspace/delete */
    bool editing;        /* the wheel is composing text[caret-1] (implies caret>0) */
};

/* Start editing `utf8` (may be NULL or empty), capped at max_len characters.
 * The bar opens at the end of the existing text, composing nothing, so the
 * first wheel spin inserts a character there. */
void edit_line_init(struct edit_line *s, const char *utf8, int max_len);

/* Re-encode the line into `out` as UTF-8, optionally trimming leading and
 * trailing spaces. Returns the number of bytes written, excluding the NUL. */
int edit_line_get(const struct edit_line *s, char *out, int outlen, bool trim);

/* Handle one ACTION_KBD_* that belongs to the text itself -- the wheel, the
 * caret taps and the held-delete pair. Returns true if the text changed, which
 * is the caller's cue to re-run whatever depends on it.
 *
 * Actions the line does not own (SELECT, DONE, ABORT) are ignored and left for
 * the caller, so they can mean different things in different enclosures. Test
 * edit_line_owns_action() to tell the two apart without guessing. */
bool edit_line_action(struct edit_line *s, int action);
bool edit_line_owns_action(int action);

/* The line's height in the active viewport's font. */
int edit_line_height(struct screen *display);

/* Draw the line into the active viewport, `width` pixels wide, at the top.
 * Clips and scrolls horizontally to keep the caret in view; leaves the screen
 * in DRMODE_SOLID. Does not clear -- the enclosure owns the background. */
void edit_line_draw(struct screen *display, struct edit_line *s, int width);

#endif /* _GUI_EDIT_LINE_H_ */
