/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to dialog_prose.c: a dialog whose body is prose, not a question.
 ****************************************************************************/

#ifndef _GUI_DIALOG_PROSE_H_
#define _GUI_DIALOG_PROSE_H_

#include <stdbool.h>

/* Ask for something the user needs to read first.
 *
 * The yes-no dialog fits a question on two or three lines and puts the wheel
 * on the button pair. This one is for the other case: an explanation long
 * enough to need scrolling, where the decision is easy once it has been read.
 * So the wheel scrolls the text -- which is what the reader wants it for --
 * and the two buttons take fixed keys instead: SELECT accepts, MENU cancels.
 *
 * 'body' is one string; blank lines in it separate paragraphs. It is wrapped
 * to the box, and a marker appears while there is more below.
 *
 * True if the user accepted. */
bool dialog_prose_confirm(const char *title, const char *body,
                          const char *accept_label,
                          const char *cancel_label);

#endif /* _GUI_DIALOG_PROSE_H_ */
