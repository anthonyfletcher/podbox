/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to quiz.c: the Music Quiz.
 ****************************************************************************/
#ifndef _QUIZ_H
#define _QUIZ_H

/* The quiz, from the first round to leaving. Stops whatever is playing,
 * after asking, and puts the playlist back as it was on the way out -- ready
 * to resume, not playing. Returns a GO_TO_* code. */
int music_quiz_screen(void);

#endif /* _QUIZ_H */
