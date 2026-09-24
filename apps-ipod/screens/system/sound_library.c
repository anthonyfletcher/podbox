/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * What the analysis found across the whole library.
 *
 * screens/playback/sound_props.c says what one track sounds like. This says
 * what the index holds as a whole, which is where three questions are
 * answered that a single track cannot answer: how much of the library has
 * been measured at all, why the moods naming a speed offer the number of
 * tracks they do, and whether the words on the per-track screen are this
 * library's percentiles or the shipped ones.
 *
 * A report, not a settings screen, so it sits here beside art_health.c rather
 * than under screens/settings/. Nothing is measured and nothing is written:
 * one sequential pass over the index when the screen opens, and the counts
 * live only as long as it is up.
 *
 * Every test here is the engine's own, called rather than restated. A track
 * counts as having a tempo exactly when a mood that names a speed would
 * offer it, and as having a key exactly when the per-track read-out would
 * print one -- a second copy of either rule would drift from the first and
 * report a library the rest of the engine does not agree it has.
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "lang.h"
#include "settings/settings.h"
#include "database/sound_cal.h"
#include "database/sound_index.h"
#include "database/sound_mix.h"
#include "database/tagcache.h"
#include "widgets/list.h"
#include "widgets/splash.h"
#include "screens/system/sound_library.h"

enum lib_row
{
    ROW_MEASURED = 0,
    ROW_UNREADABLE,
    ROW_TEMPO,
    ROW_KEY,
    ROW_COMPARED,
    ROW_COUNT
};

static struct lib_counts
{
    int records;    /* in the index, measured or not */
    int usable;     /* of those, actually measured */
    int tempo;      /* with a tempo the engine trusts */
    int keyed;      /* with a key the read-out would print */
    int major;      /* of those, major rather than minor */
    int library;    /* tracks the tag database holds */
    bool calibrated;
} lc;

static int lib_pct(int n, int of)
{
    return of > 0 ? n * 100 / of : 0;
}

/* One pass, filling lc. False where there is no index to read, which the menu
 * row's own gate should already have caught. */
static bool lib_scan(void)
{
    struct sound_index_reader rd;
    struct sound_record r;
    struct sound_axes a;
    int i;

    memset(&lc, 0, sizeof (lc));

    /* Before the pass, because it may run one of its own to build the file
     * and the splash covering this covers that too. */
    sound_cal_ensure();
    lc.calibrated = sound_cal_at(CAL_ENERGY, 500) >= 0;

    if (sound_index_reader_open(&rd) != SOUND_OK)
        return false;

    lc.records = rd.count;

    for (i = 0; i < rd.count; i++)
    {
        if (!sound_index_read(&rd, i, &r))
            break;

        /* A failed decode is written zeroed, so it is in the index and is not
         * a measurement. The difference between these two counts is the
         * Unreadable row. */
        if (!sound_record_usable(&r))
            continue;

        lc.usable++;
        sound_mix_axes(&r, &a);

        if (a.speed >= 0)
            lc.tempo++;

        if (a.mode >= 0 && r.tonic <= 11)
        {
            lc.keyed++;

            if (r.mode == 0)
                lc.major++;
        }
    }

    sound_index_reader_close(&rd);

    lc.library = tagcache_get_stat()->total_entries;

    return lc.records > 0;
}

static int lib_label(int row)
{
    switch (row)
    {
        case ROW_MEASURED:   return LANG_SOUND_LIB_MEASURED;
        case ROW_UNREADABLE: return LANG_SOUND_LIB_UNREADABLE;
        case ROW_TEMPO:      return LANG_SOUND_LIB_TEMPO;
        case ROW_KEY:        return LANG_SOUND_LIB_KEY;
        default:             return LANG_SOUND_LIB_COMPARED;
    }
}

static void lib_value(int row, char *buf, size_t len)
{
    switch (row)
    {
        case ROW_MEASURED:
            /* Against the tag database rather than against the index: what
             * the reader wants to know is whether anything is still waiting
             * to be measured, and the index cannot say what it has not
             * reached. A library the database has not counted says only what
             * was measured. */
            if (lc.library > 0)
                snprintf(buf, len, "%d of %d", lc.usable, lc.library);
            else
                snprintf(buf, len, "%d", lc.usable);
            break;

        case ROW_UNREADABLE:
            snprintf(buf, len, "%d", lc.records - lc.usable);
            break;

        case ROW_TEMPO:
            snprintf(buf, len, "%d (%d%%)", lc.tempo,
                     lib_pct(lc.tempo, lc.usable));
            break;

        case ROW_KEY:
            snprintf(buf, len, "%d (%d%%), %d %s", lc.keyed,
                     lib_pct(lc.keyed, lc.usable), lc.major,
                     str(LANG_SOUND_KEY_MAJOR));
            break;

        default:
            snprintf(buf, len, "%s",
                     str(lc.calibrated ? LANG_SOUND_LIB_OWN
                                       : LANG_SOUND_LIB_SHIPPED));
            break;
    }
}

static const char *lib_get_name(int row, void *data, char *buf, size_t len)
{
    char value[64];

    (void)data;

    lib_value(row, value, sizeof (value));
    snprintf(buf, len, "%s: %s", str(lib_label(row)), value);

    return buf;
}

bool sound_library_screen(void)
{
    struct simplelist_info info;
    bool ok;

    /* The pass is a whole-index read and the disk may be asleep, so say so
     * rather than appearing to have hung. */
    splash(0, ID2P(LANG_WAIT));
    ok = lib_scan();

    if (!ok)
    {
        splash(HZ * 2, ID2P(LANG_SOUND_MIX_NO_INDEX));
        return false;
    }

    simplelist_info_init(&info, str(LANG_SOUND_LIBRARY), ROW_COUNT, NULL);
    info.get_name = lib_get_name;

    return simplelist_show_list(&info);
}
