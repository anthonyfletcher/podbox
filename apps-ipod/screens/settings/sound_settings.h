/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to sound_settings.c.
 ****************************************************************************/

#ifndef _SOUND_SETTINGS_H_
#define _SOUND_SETTINGS_H_

#include "settings/settings_list.h"

/* Volume Limit as a number range, in place of its custom settings entry. */
const struct settings_list *volume_limit_setting(void);

#endif /* _SOUND_SETTINGS_H_ */
