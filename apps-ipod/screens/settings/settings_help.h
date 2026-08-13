/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to settings_help.c: the on-device explanations.
 ****************************************************************************/

#ifndef _SETTINGS_HELP_H_
#define _SETTINGS_HELP_H_

#include <stdbool.h>
#include <stddef.h>

/* Copy the explanation for `key` into `buf`, returning false if there is none.
 *
 * The text lives in /.rockbox/docs/settings-help.txt, keyed by cfg name, one
 * stanza each:
 *
 *     [backlight timeout]
 *     How long the backlight stays on after the last button press. The
 *     single biggest lever on battery life.
 *
 * Not lang strings. There are around 250 settings and a second phrase for each
 * would be 250 entries in every .lang file, growing the binary and the .lng for
 * something read a handful of times a year. The cost is that it is English
 * only, which is the same choice the shipped guide makes.
 *
 * The file is read on demand and nothing is cached: this runs when a person
 * asks for it, and the alternative is holding 30KB for the rest of the session.
 * Deliberately never read to decide whether to *offer* Explain -- see the
 * comment where it is offered. */
bool settings_help_lookup(const char *key, char *buf, size_t bufsz);

#endif /* _SETTINGS_HELP_H_ */
