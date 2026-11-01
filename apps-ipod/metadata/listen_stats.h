/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to listen_stats.c.
 ****************************************************************************/
#ifndef _LISTEN_STATS_H_
#define _LISTEN_STATS_H_

#include <stdbool.h>
#include <stdint.h>

/* One book's or show's figures. Days count from 1999-12-31; 0 unknown. */
struct listen_stats
{
    uint64_t key;               /* book_resume_key() of its id */
    uint32_t seconds;           /* listened, at the book's own pace */
    uint32_t longest;           /* the longest one play, seconds */
    uint16_t started, last, finished;
    uint16_t days;              /* days listened on */
    uint16_t streak, best_streak;
    uint8_t  kind;              /* 1 a book, 2 a show */
    uint8_t  pad[3];
};

/* The record of 'key', zeroed when there is none (false) */
bool listen_stats_get(uint64_t key, struct listen_stats *out);

/* Every record of 'kind' together: the time and days summed, the longest
 * and best streak the most of any, started the earliest; how many there are
 * and how many finished */
void listen_stats_sum(int kind, struct listen_stats *all, int *items,
                      int *finished);

/* Write what has been heard since the last call. Called where a book's place
 * is saved, never on the audio thread. */
void listen_stats_flush(void);

/* Once, at boot */
void listen_stats_init(void);

#endif /* _LISTEN_STATS_H_ */
