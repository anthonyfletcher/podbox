/***************************************************************************
 * Public entry point for the core-linked Properties screen
 * (apps/viewers/properties.c), ported from the properties plugin.
 * GNU General Public License (version 2+)
 *
 * Interface to properties.c.
 ****************************************************************************/

#ifndef _PROPERTIES_H_
#define _PROPERTIES_H_

/* Show File/Directory/Track properties for `file` (a filesystem path, or the
 * database-browser activity string for multi-track database selections).
 * Returns a GO_TO_* code. */
int properties(const char *file);

#endif /* _PROPERTIES_H_ */
