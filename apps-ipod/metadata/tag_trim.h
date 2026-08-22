/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to tag_trim.c: the shortened album and track names the now
 * playing screen shows.
 ****************************************************************************/

#ifndef _TAG_TRIM_H_
#define _TAG_TRIM_H_

/* Read the pattern file. Called once at startup, and again whenever the
 * setting is switched on so that an edited file takes effect without a
 * reboot. Touches the disk, so never call it from a draw path. */
void tag_trim_init(void);

/* The name with a trailing edition, version or guest-artist group removed,
 * written into buf. Returns name itself when nothing is trimmed, when the
 * setting is off, or when the result would not fit -- so use the pointer that
 * comes back and do not assume buf was written. */
const char *tag_trim(const char *name, char *buf, int buf_size);

#endif /* _TAG_TRIM_H_ */
