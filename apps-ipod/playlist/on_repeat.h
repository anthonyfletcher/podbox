/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to on_repeat.c.
 ****************************************************************************/

#ifndef _ON_REPEAT_H
#define _ON_REPEAT_H

#include <stdbool.h>
#include <stdint.h>

/* The longest On Repeat Length allows */
#define ON_REPEAT_MAX 100

/* The most played tracks of the span the On Repeat setting names, as
 * path_key()s of tracks the database holds, at most 'max' of them, in a new
 * shuffled order each call -- chained by sound when the playlist engine is
 * on. Leaves out books and podcasts kept apart from the music. Reads the
 * playback log, so seconds on a cold disk; callable from any thread, and a
 * second caller waits for the first. The read ends early once *stop turns
 * true; stop may be NULL. Returns how many, 0 without reading while the
 * database is not in RAM, or -1 when the clock is not set and so there is no
 * span. */
int on_repeat_keys(uint64_t *keys, int max, const volatile bool *stop);

/* Whether on_repeat_offered() would answer without reading the log */
bool on_repeat_known(void);

/* Work out, and remember, whether On Repeat has anything to play, unless that
 * is already known. For a thread that can read while a list waits; the read
 * ends early once *stop turns true, and stop may be NULL. */
void on_repeat_refresh(const volatile bool *stop);

/* Whether On Repeat has anything to play, for a list deciding whether to
 * offer it. Remembered for as long as the answer cannot have changed, so a
 * list opened again reads nothing. Without 'may_read' it never waits: an
 * answer not remembered, or one being worked out, is taken as yes. */
bool on_repeat_offered(bool may_read);

/* Build the On Repeat playlist and start it, asking first before replacing a
 * playlist built by hand. Says on screen why when it cannot. Returns the
 * GO_TO_* the playlist catalogue should take. */
int on_repeat_play(void);

#endif /* _ON_REPEAT_H */
