/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to context_menu_config.c.
 ****************************************************************************/
#ifndef _CONTEXT_MENU_CONFIG_H_
#define _CONTEXT_MENU_CONFIG_H_

/* The rows of one kind of context menu -- param is its enum
 * context_menu_kind -- to turn on and off. MENU_ATTACHED_USB when the screen
 * was left for the root menu, else 0. */
int context_menu_config(void *param);

#endif
