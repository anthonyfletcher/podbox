/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Screen Transitions: the next screen slides in over the one it replaces.
 ****************************************************************************/
#ifndef TRANSITION_H
#define TRANSITION_H

enum transition_dir
{
    TRANSITION_FORWARD,     /* into a menu or a deeper list: from the right */
    TRANSITION_BACK,        /* back out of one: from the left */
};

/* Called just before the code that draws the next screen: keeps what the
 * screen shows and holds the panel while the next one draws. Does nothing
 * with the setting off, or with a slide already begun and held, whose
 * picture is the one the panel shows. */
void transition_begin(enum transition_dir dir);
/* Slides the whole screen, not the list area, for the slide begun: for a
 * change to or from a screen with no list, such as Now Playing. */
void transition_whole_screen(void);
/* Runs the slide begun, once the next screen has drawn; get_action() calls
 * it as the screen waits for a button. */
void transition_run(void);
/* Drops a slide begun and shows the screen as it is: for anything that
 * must show at once, such as a splash. */
void transition_cancel(void);

#endif /* TRANSITION_H */
