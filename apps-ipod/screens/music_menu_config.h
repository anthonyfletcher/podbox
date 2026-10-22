/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to music_menu_config.c.
 ****************************************************************************/
#ifndef _MUSIC_MENU_CONFIG_H
#define _MUSIC_MENU_CONFIG_H

/* The screen that turns Music menu rows on and off. MENU_ATTACHED_USB if it
 * was left for the root menu, otherwise 0. */
int music_menu_config(void);

/* Turn off one Music menu row, by its position in tagnavi.config's root menu,
 * as the Music Menu screen would. False for a row it cannot describe. */
bool music_menu_hide_row(int index);

#endif /* _MUSIC_MENU_CONFIG_H */
