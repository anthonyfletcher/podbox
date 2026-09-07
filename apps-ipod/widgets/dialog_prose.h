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
 * 'accept_default' is which of the two is highlighted once the buttons take
 * focus. True for a question whose obvious answer is yes; false where the
 * dialog stands for a setting that is currently the other value, because a
 * dialog opening on what the setting is not reads as a proposal to change it.
 *
 * True if the user accepted. */
bool dialog_prose_confirm(const char *title, const char *body,
                          const char *accept_label,
                          const char *cancel_label,
                          bool accept_default);

#endif /* _GUI_DIALOG_PROSE_H_ */
