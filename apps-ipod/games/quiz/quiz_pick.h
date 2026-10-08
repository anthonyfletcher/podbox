/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to quiz_pick.c: choosing the Music Quiz's tracks.
 ****************************************************************************/
#ifndef _QUIZ_PICK_H
#define _QUIZ_PICK_H

#include "file.h"               /* MAX_PATH */

#define QUIZ_ROUNDS     10
#define QUIZ_CHOICES    5
#define QUIZ_TITLE_MAX  96

/* What a round asks for. Each is a setting; a round takes one of those that
 * are on and that its track has an answer for. */
enum quiz_kind
{
    QUIZ_KIND_TITLE,
    QUIZ_KIND_ARTIST,
    QUIZ_KIND_ALBUM,
    QUIZ_KIND_YEAR,
    QUIZ_KINDS
};

struct quiz_round
{
    char path[MAX_PATH];        /* the track that plays */
    unsigned long length;       /* ms */
    enum quiz_kind kind;
    char choice[QUIZ_CHOICES][QUIZ_TITLE_MAX];
    int right;                  /* which choice[] is the track's */
};

#define QUIZ_PICK_OK        0
#define QUIZ_PICK_NO_DB    -1   /* the database would not answer */
#define QUIZ_PICK_TOO_FEW  -2   /* not enough music to make ten rounds */
#define QUIZ_PICK_NO_MEM   -3
#define QUIZ_PICK_STOPPED  -4   /* USB or a shutdown, in *event */

/* Fill all QUIZ_ROUNDS of 'rounds'. A QUIZ_PICK_* code.
 *
 * Music only, never spoken word, and nothing under a minute and a half. The
 * wrong answers close in on the right one as the rounds go: by how they sound
 * where the Sound Index has measured the track, by genre and decade where it
 * has not, and for a year by how many years apart. A kind that cannot make a
 * round gives it to another kind that is on; QUIZ_PICK_TOO_FEW means none
 * could. QUIZ_PICK_STOPPED leaves the event in *event for the caller to
 * pass to default_event_handler(). */
int quiz_pick(struct quiz_round *rounds, long *event);

#endif /* _QUIZ_PICK_H */
