/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to bg_task_info.c: a live view of the three background tasks.
 ****************************************************************************/

#ifndef _BG_TASK_INFO_H_
#define _BG_TASK_INFO_H_

#include <stdbool.h>

/* Auto-refreshing status list: state and current activity for the database,
 * the album index and the artwork cache. Runs its own loop until the user
 * leaves. Returns simplelist_show_list()'s result (true if USB was attached). */
bool bg_task_info_screen(void);

#endif /* _BG_TASK_INFO_H_ */
