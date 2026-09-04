/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to spike_score.c: the best run there has been, and the tracks
 * it was played over.
 ****************************************************************************/

#ifndef SPIKE_SCORE_H
#define SPIKE_SCORE_H

#include <stdbool.h>

/* Tracks a run lists. A long run is an evening, and the list is read rather
 * than counted, so it stops rather than growing without a bound. The count
 * in the header keeps rising past it. */
#define SPK_LOG_MAX      200

/* Room a name and a genre are given on the way to the file. The screen shows
 * about thirty characters of a name, so this is generous and the truncation
 * is never what the reader sees. */
#define SPK_NAME_MAX     64
#define SPK_GENRE_MAX    32

/* What a run was worth. One record, because there is one kind of run: the
 * numbers are gathered as it goes and the track list is written as it goes,
 * so nothing here is held in RAM for the length of an evening. */
struct spk_run
{
    long score;
    long beats;
    long secs;          /* how long it lasted */
    int  tracks;        /* ...and over how many */
    int  bpm10;         /* mean track tempo, in tenths */
};

/* Which list a reader wants: the run that has just ended, or the record. The
 * two are the same file until a run beats the record and the log becomes
 * one. */
enum spk_log
{
    SPK_LOG_RUN,
    SPK_LOG_BEST
};

/* The record. False where there has been none, in which case the numbers
 * are zeroed. */
bool spk_score_best(struct spk_run *out);

/* A run beginning: whatever the last one logged is dropped. */
void spk_score_begin(void);

/* One track, as it starts playing. Written straight to the log rather than
 * kept, because a run is however many tracks the player has patience for
 * and the numbers are the only part of it worth holding. */
void spk_score_played(const char *name, const char *genre);

/* The run just ended. True where it beat the record, in which case its log
 * has become the record's. */
bool spk_score_end(const struct spk_run *r);

/* The list, for the screen. A page is cached, so scrolling costs one file
 * read a screenful rather than one a row. */
int spk_score_tracks(enum spk_log which);
void spk_score_track(enum spk_log which, int n, char *name, int nsize,
                     char *genre, int gsize);

#endif /* SPIKE_SCORE_H */
