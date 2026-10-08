/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to quiz.c: the Music Quiz.
 ****************************************************************************/
#ifndef _QUIZ_H
#define _QUIZ_H

#include <stdbool.h>

/* The quiz, from the first round to leaving. Stops whatever is playing,
 * after asking, and puts the playlist back as it was on the way out -- ready
 * to resume, not playing. Returns a GO_TO_* code. */
int music_quiz_screen(void);

/* Whether the playlist is the quiz's own while a game runs. Anything that
 * would replace or reorder it -- a car or dock choosing what plays -- must
 * refuse until it is false again. */
bool music_quiz_has_playlist(void);

/* Rewrites the best score an older firmware kept as text; the first boot of
 * a new layout calls it. */
bool quiz_scores_convert(const char *text_file);

#endif /* _QUIZ_H */
