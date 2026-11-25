/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A book's place in its series, read into struct mp3entry's series_part:
 * from a SERIES-PART user tag in any format, ID3's MVIN frame or MP4's
 * movement number atom.
 ****************************************************************************/
#ifndef _SERIES_PART_H
#define _SERIES_PART_H

#include <stdbool.h>

struct mp3entry;

/* series_part holds hundredths plus one, so Book 0 is 1 and 0 is none, and
 * never more than this, so the database keeps it in 16 bits */
#define SERIES_PART_MAX 0xffff

/* Takes a user tag named SERIES-PART, any case, and says whether it was */
bool series_part_user_tag(const char *key, const char *value,
                          struct mp3entry *id3);

/* From text such as "2", "02", "2.5" or "2/7" */
void series_part_text(const char *value, struct mp3entry *id3);

/* From a whole number */
void series_part_number(unsigned long n, struct mp3entry *id3);

#endif /* _SERIES_PART_H */
