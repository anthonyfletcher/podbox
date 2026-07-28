/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/yesno.h
 * Copyright (C) 2005 by Kevin Ferrare
 * GNU General Public License (version 2+)
 *
 * Interface to yesno.c: the text_message type and the yesno result enum.
 ****************************************************************************/

#ifndef _GUI_YESNO_H_
#define _GUI_YESNO_H_
#include <stdbool.h>

enum yesno_res
{
    YESNO_YES,
    YESNO_NO,
    YESNO_USB,
    YESNO_TMO
};

struct text_message
{
    const char **message_lines;
    int nb_lines;
};

/*
 * Runs the yesno asker :
 * it will display the 'main_message' question, and wait for user keypress
 * PLAY means yes, other keys means no
 *  - main_message : the question the user has to answer
 *  - yes_message : message displayed if answer is 'yes'
 *  - no_message : message displayed if answer is 'no'
 */
extern enum yesno_res gui_syncyesno_run(
                           const struct text_message * main_message,
                           const struct text_message * yes_message,
                           const struct text_message * no_message);

/* Sets SBS title for the screen. Title may be NULL. */
extern enum yesno_res gui_syncyesno_run_w_title(
                           const char *title,
                           const struct text_message * main_message,
                           const struct text_message * yes_message,
                           const struct text_message * no_message);

extern enum yesno_res gui_syncyesno_run_w_tmo(
                           int ticks, enum yesno_res tmo_default_res,
                           const char *title,
                           const struct text_message * main_message,
                           const struct text_message * yes_message,
                           const struct text_message * no_message);

bool yesno_pop(const char* text);
bool yesno_pop_confirm(const char* text);

#endif /* _GUI_YESNO_H_ */
