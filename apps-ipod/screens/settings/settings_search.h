/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to settings_search.c.
 ****************************************************************************/

#ifndef _SETTINGS_SEARCH_H_
#define _SETTINGS_SEARCH_H_

/* Type, pick a setting, change it, and come back to the results. Returns 0 to
 * stay in the calling menu, or a GO_TO_* code when something else has to
 * happen first (USB). */
int settings_search_run(void);

#endif /* _SETTINGS_SEARCH_H_ */
