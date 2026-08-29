/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to spike_score.c: what the best has been.
 ****************************************************************************/

#ifndef SPIKE_SCORE_H
#define SPIKE_SCORE_H

#include <stdbool.h>

/* Track bests kept. The file is held in score order, so this drops the
 * lowest -- which is the right rule for a table that is read as a ranking,
 * and it means the file itself is the leaderboard rather than something a
 * screen has to sort. */
#define SPK_SCORE_TRACKS    64

/* One screenful, and how much of a track's name it can show. */
#define SPK_SCORE_PAGE      12
#define SPK_SCORE_NAME      44

/* The best run there has been, and how far it went. Zero where there has
 * been none. */
void spk_score_run(long *score, long *beats);

/* The best against one track, by its path. Zero where there has been none;
 * zero as well for a track with no path, which is what a database browse
 * hands over until the file is opened. */
long spk_score_track(const char *path);

/* Record one, and say whether it beat what was there. A run that did not
 * beat it writes nothing, so the file is only touched when something
 * happened. */
bool spk_score_put_run(long score, long beats);
bool spk_score_put_track(const char *path, long score);

/* The table as a list: row 0 is the run, and the tracks are ranked under it.
 * Rows are formatted here rather than by the screen, because what a row is
 * -- rank, score, and the track's own name without its directory or its
 * extension -- is a property of the table and not of how it is drawn.
 *
 * A page is cached, so walking the list draws one file read per screenful
 * rather than one per row. */
int spk_score_rows(void);
void spk_score_row(int n, char *buf, int size);

#endif /* SPIKE_SCORE_H */
