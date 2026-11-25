/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A book's place in its series, from whichever of SERIES-PART, MVIN and
 * MP4's movement number the file carries first; audiobook tools write one or
 * the other. The number keeps two decimal places, for a novella at 2.5.
 ****************************************************************************/

#include <stdbool.h>
#include "platform.h"
#include "strcasecmp.h"
#include "metadata.h"
#include "series_part.h"

static void set(struct mp3entry *id3, unsigned long hundredths)
{
    if (id3->series_part == 0)
        id3->series_part = hundredths < SERIES_PART_MAX
                           ? hundredths + 1 : SERIES_PART_MAX;
}

void series_part_number(unsigned long n, struct mp3entry *id3)
{
    set(id3, n < SERIES_PART_MAX ? n * 100 : SERIES_PART_MAX);
}

void series_part_text(const char *value, struct mp3entry *id3)
{
    unsigned long whole = 0, frac = 0;
    const char *p = value;

    while (*p == ' ')
        p++;
    if (*p < '0' || *p > '9')
        return;
    for (; *p >= '0' && *p <= '9'; p++)
        if (whole < SERIES_PART_MAX)
            whole = whole * 10 + (*p - '0');
    /* A comma is the decimal point in much of Europe */
    if ((*p == '.' || *p == ',') && p[1] >= '0' && p[1] <= '9')
    {
        frac = (p[1] - '0') * 10;
        if (p[2] >= '0' && p[2] <= '9')
            frac += p[2] - '0';
    }
    set(id3, whole < SERIES_PART_MAX ? whole * 100 + frac : SERIES_PART_MAX);
}

bool series_part_user_tag(const char *key, const char *value,
                          struct mp3entry *id3)
{
    if (strcasecmp(key, "series-part"))
        return false;
    series_part_text(value, id3);
    return true;
}
