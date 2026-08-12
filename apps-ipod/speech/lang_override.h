/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to lang_override.c.
 ****************************************************************************/

#ifndef __LANG_OVERRIDE_H
#define __LANG_OVERRIDE_H

/* Apply /.rockbox/langs/overrides.cfg over the strings in use. Must be called
 * after every lang_core_load(), which resets them all. Silent if the file is
 * absent. */
void lang_override_load(void);

#endif /* __LANG_OVERRIDE_H */
