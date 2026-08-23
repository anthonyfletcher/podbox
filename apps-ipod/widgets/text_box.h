/***************************************************************************
 * Original code from RockBox
 * was: apps/screens.h (view_text declaration)
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * Interface to text_box.c (view_text).
 ****************************************************************************/

#ifndef _TEXT_BOX_H_
#define _TEXT_BOX_H_

/* Full-screen scrollable display for a string already in memory. Not to be
 * confused with viewers/text_viewer/, which streams documents from a file.
 * Returns 1 if it was left for the root menu -- MENU, or a USB attach --
 * 0 otherwise. */
int view_text(const char *title, const char *text);

#endif /* _TEXT_BOX_H_ */
