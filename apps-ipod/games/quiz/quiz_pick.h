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

struct quiz_round
{
    char path[MAX_PATH];        /* the track that plays */
    unsigned long length;       /* ms */
    char title[QUIZ_CHOICES][QUIZ_TITLE_MAX];
    int right;                  /* which title[] is the track's */
};

#define QUIZ_PICK_OK        0
#define QUIZ_PICK_NO_DB    -1   /* the database would not answer */
#define QUIZ_PICK_TOO_FEW  -2   /* not enough music to make ten rounds */
#define QUIZ_PICK_NO_MEM   -3

/* Fill all QUIZ_ROUNDS of 'rounds'. A QUIZ_PICK_* code.
 *
 * Music only, never spoken word, and nothing under a minute and a half. The
 * wrong titles close in on the right one as the rounds go: by how they sound
 * where the Sound Index has measured the track, by genre and decade where it
 * has not. */
int quiz_pick(struct quiz_round *rounds);

#endif /* _QUIZ_PICK_H */
