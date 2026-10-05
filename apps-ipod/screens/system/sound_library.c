/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * What the analysis found across the whole library.
 *
 * screens/playback/sound_props.c says what one track sounds like. This says
 * what the index holds as a whole, which is where questions are answered that
 * a single track cannot answer: how much of the library has been measured at
 * all, how many tracks each mood playlist can draw on, why the moods naming a
 * speed offer the number of tracks they do, and whether the words on the
 * per-track screen are this library's percentiles or the shipped ones.
 *
 * A report, not a settings screen, so it sits here beside art_health.c rather
 * than under screens/settings/. Nothing is measured and nothing is written:
 * one sequential pass over the index when the screen opens, and the counts
 * live only as long as it is up.
 *
 * Every test here is the engine's own, called rather than restated. A track
 * counts as having a tempo exactly when a mood that names a speed would
 * offer it, as having a key exactly when the per-track read-out would print
 * one, as in a mood exactly when that mood's playlist would consider it, and
 * in a Pace band by the read-out's own edges -- a second copy of any rule
 * would drift from the first and report a library the rest of the engine
 * does not agree it has.
 *
 * Each section is an action row of its own, so Explain reaches it the way it
 * reaches every other action row.
 *
 * Parts, in order:
 *   - the counts, and the pass that fills them
 *   - the sections
 *   - the menu
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "lang.h"
#include "settings/settings.h"
#include "database/sound_cal.h"
#include "database/sound_index.h"
#include "database/sound_mix.h"
#include "database/sound_mood.h"
#include "database/tagcache.h"
#include "widgets/list.h"
#include "widgets/menu.h"
#include "widgets/splash.h"
#include "screens/playback/sound_props.h"
#include "screens/system/sound_library.h"


/** The counts **/

/* 1900s to 2090s: the years sound_mix_axes() keeps. */
#define LIB_DECADES  20

/* Twelve tonics, each major and minor, at tonic * 2 + mode. */
#define LIB_KEYS     24

static struct lib_counts
{
    int records;    /* in the index, measured or not */
    int usable;     /* of those, actually measured */
    int tempo;      /* with a tempo the engine trusts */
    int keyed;      /* with a key the read-out would print */
    int major;      /* of those, major rather than minor */
    int library;    /* tracks the tag database holds */
    bool calibrated;

    int mood[MOOD_COUNT];
    int no_mood;
    int pace[SOUND_PACE_BANDS];
    int key[LIB_KEYS];
    int decade[LIB_DECADES];
    long decade_db10[LIB_DECADES];   /* loudness summed, dBFS x10 */
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
    int i, m;

    memset(&lc, 0, sizeof (lc));

    /* Before the pass, because it may run one of its own to build the file
     * and the splash covering this covers that too. */
    sound_cal_ensure();
    lc.calibrated = sound_cal_at(CAL_ENERGY, 500) >= 0;

    if (sound_index_reader_open(&rd) != SOUND_OK)
        return false;

    lc.records = rd.count;
    splash_progress_set_delay(HZ / 2);

    for (i = 0; i < rd.count; i++)
    {
        bool in_mood = false;

        /* Or the codec stops refilling while music plays, which is heard. */
        if ((i & 63) == 0)
            yield();

        splash_progress(i, rd.count, "%s", str(LANG_WAIT));
        if (!sound_index_read(&rd, i, &r))
            break;

        /* A failed decode is written zeroed, so it is in the index and is not
         * a measurement. The difference between these two counts is the
         * Unreadable row. */
        if (!sound_record_usable(&r))
            continue;

        lc.usable++;
        sound_mix_axes(&r, &a);

        /* The ceiling a mood playlist builds against, so a count is what
         * that playlist can draw on before its length and artist rules. */
        for (m = 0; m < MOOD_COUNT; m++)
        {
            int d = sound_mood_score(&a, m);

            if (d >= 0 && d <= MIX_MAX_DISTANCE)
            {
                lc.mood[m]++;
                in_mood = true;
            }
        }

        if (!in_mood)
            lc.no_mood++;

        if (a.speed >= 0)
        {
            lc.tempo++;
            lc.pace[sound_props_pace_band(60000 / r.period_ms)]++;
        }

        if (a.mode >= 0 && r.tonic <= 11)
        {
            lc.keyed++;
            lc.key[r.tonic * 2 + (r.mode ? 1 : 0)]++;

            if (r.mode == 0)
                lc.major++;
        }

        if (a.year != 0)
        {
            int d = (a.year - 1900) / 10;

            lc.decade[d]++;
            lc.decade_db10[d] += r.loudness_db10;
        }
    }

    sound_index_reader_close(&rd);

    lc.library = tagcache_get_stat()->total_entries;

    return lc.records > 0;
}


/** The sections **/

enum lib_section
{
    SEC_OVERVIEW = 0,
    SEC_MOODS,
    SEC_PACE,
    SEC_KEYS,
    SEC_DECADES,
};

enum lib_row
{
    ROW_MEASURED = 0,
    ROW_UNREADABLE,
    ROW_TEMPO,
    ROW_KEY,
    ROW_COMPARED,
    ROW_COUNT
};

/* Keys and decades list only what the library has, so their rows are an
 * index into the counts, built when the section opens. Keys go most common
 * first, decades oldest first. */
static uint8_t lib_order[LIB_KEYS > LIB_DECADES ? LIB_KEYS : LIB_DECADES];
static int lib_ordered;

static void lib_order_keys(void)
{
    int k, j;

    lib_ordered = 0;

    for (k = 0; k < LIB_KEYS; k++)
    {
        if (lc.key[k] == 0)
            continue;

        for (j = lib_ordered; j > 0 && lc.key[lib_order[j - 1]] < lc.key[k];
             j--)
            lib_order[j] = lib_order[j - 1];

        lib_order[j] = (uint8_t)k;
        lib_ordered++;
    }
}

static void lib_order_decades(void)
{
    int d;

    lib_ordered = 0;

    for (d = 0; d < LIB_DECADES; d++)
    {
        if (lc.decade[d] > 0)
            lib_order[lib_ordered++] = (uint8_t)d;
    }
}

/* A count and its share of the measured tracks, which is what every share on
 * these screens is of. */
static void lib_count(char *buf, size_t len, const char *label, int n)
{
    snprintf(buf, len, "%s: %d (%d%%)", label, n, lib_pct(n, lc.usable));
}

static void lib_overview(int row, char *buf, size_t len)
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
                snprintf(buf, len, "%s: %d of %d",
                         str(LANG_SOUND_LIB_MEASURED), lc.usable, lc.library);
            else
                snprintf(buf, len, "%s: %d",
                         str(LANG_SOUND_LIB_MEASURED), lc.usable);
            break;

        case ROW_UNREADABLE:
            snprintf(buf, len, "%s: %d", str(LANG_SOUND_LIB_UNREADABLE),
                     lc.records - lc.usable);
            break;

        case ROW_TEMPO:
            lib_count(buf, len, str(LANG_SOUND_LIB_TEMPO), lc.tempo);
            break;

        case ROW_KEY:
            snprintf(buf, len, "%s: %d (%d%%), %d %s",
                     str(LANG_SOUND_LIB_KEY), lc.keyed,
                     lib_pct(lc.keyed, lc.usable), lc.major,
                     str(LANG_SOUND_KEY_MAJOR));
            break;

        default:
            snprintf(buf, len, "%s: %s", str(LANG_SOUND_LIB_COMPARED),
                     str(lc.calibrated ? LANG_SOUND_LIB_OWN
                                       : LANG_SOUND_LIB_SHIPPED));
            break;
    }
}

/* The last row of every section but the overview is the tracks the others
 * leave out. Pace, keys and decades then add up to the measured tracks;
 * moods add up to more, because a track can be in several. */
static void lib_moods(int row, char *buf, size_t len)
{
    if (row < MOOD_COUNT)
        lib_count(buf, len, str(sound_mood_name(row)), lc.mood[row]);
    else
        lib_count(buf, len, str(LANG_SOUND_LIB_NO_MOOD), lc.no_mood);
}

static void lib_pace(int row, char *buf, size_t len)
{
    if (row < SOUND_PACE_BANDS)
        lib_count(buf, len, str(sound_props_pace_name(row)), lc.pace[row]);
    else
        lib_count(buf, len, str(LANG_SOUND_LIB_UNCLEAR),
                  lc.usable - lc.tempo);
}

static void lib_keys(int row, char *buf, size_t len)
{
    char name[32];
    int k;

    if (row >= lib_ordered)
    {
        lib_count(buf, len, str(LANG_SOUND_LIB_UNCLEAR),
                  lc.usable - lc.keyed);
        return;
    }

    k = lib_order[row];
    snprintf(name, sizeof (name), "%s %s", sound_props_note(k / 2),
             str(k & 1 ? LANG_SOUND_KEY_MINOR : LANG_SOUND_KEY_MAJOR));
    lib_count(buf, len, name, lc.key[k]);
}

/* A mean of dBFS readings, in tenths, with the track count beside it so a
 * decade of three tracks is not read as a trend. */
static void lib_decades(int row, char *buf, size_t len)
{
    int d, n;
    long mean;

    if (row >= lib_ordered)
    {
        n = lc.usable;

        for (d = 0; d < LIB_DECADES; d++)
            n -= lc.decade[d];

        lib_count(buf, len, str(LANG_SOUND_LIB_NO_YEAR), n);
        return;
    }

    d = lib_order[row];
    mean = lc.decade_db10[d] / lc.decade[d];

    snprintf(buf, len, "%ds: %s%ld.%ld dB (%d)", 1900 + d * 10,
             mean < 0 ? "-" : "", (mean < 0 ? -mean : mean) / 10,
             (mean < 0 ? -mean : mean) % 10, lc.decade[d]);
}

static const char *lib_get_name(int row, void *data, char *buf, size_t len)
{
    switch ((intptr_t)data)
    {
        case SEC_OVERVIEW: lib_overview(row, buf, len); break;
        case SEC_MOODS:    lib_moods(row, buf, len);    break;
        case SEC_PACE:     lib_pace(row, buf, len);     break;
        case SEC_KEYS:     lib_keys(row, buf, len);     break;
        default:           lib_decades(row, buf, len);  break;
    }

    return buf;
}

/* MENU_ATTACHED_USB where the list was left for the root, which
 * MENU_FUNC_CHECK_RETVAL carries out of the menu; 0 to come back to it. */
static int lib_show(void *param)
{
    struct simplelist_info info;
    intptr_t sec = (intptr_t)param;
    int rows, title;

    switch (sec)
    {
        case SEC_OVERVIEW:
            rows = ROW_COUNT;
            title = LANG_SOUND_LIB_OVERVIEW;
            break;
        case SEC_MOODS:
            rows = MOOD_COUNT + 1;
            title = LANG_SOUND_LIB_MOODS;
            break;
        case SEC_PACE:
            rows = SOUND_PACE_BANDS + 1;
            title = LANG_SOUND_LIB_PACE;
            break;
        case SEC_KEYS:
            lib_order_keys();
            rows = lib_ordered + 1;
            title = LANG_SOUND_LIB_KEYS;
            break;
        default:
            lib_order_decades();
            rows = lib_ordered + 1;
            title = LANG_SOUND_LIB_DECADES;
            break;
    }

    simplelist_info_init(&info, str(title), rows, param);
    info.get_name = lib_get_name;

    return simplelist_show_list(&info) ? MENU_ATTACHED_USB : 0;
}


/** The menu **/

#define LIB_ITEM(name, sec, lang)                                       \
    MENUITEM_FUNCTION_W_PARAM(name, MENU_FUNC_CHECK_RETVAL, ID2P(lang), \
                              lib_show, (void*)sec, NULL, Icon_NOICON)

LIB_ITEM(lib_overview_item, SEC_OVERVIEW, LANG_SOUND_LIB_OVERVIEW);
LIB_ITEM(lib_moods_item,    SEC_MOODS,    LANG_SOUND_LIB_MOODS);
LIB_ITEM(lib_pace_item,     SEC_PACE,     LANG_SOUND_LIB_PACE);
LIB_ITEM(lib_keys_item,     SEC_KEYS,     LANG_SOUND_LIB_KEYS);
LIB_ITEM(lib_decades_item,  SEC_DECADES,  LANG_SOUND_LIB_DECADES);

MAKE_MENU(lib_menu, ID2P(LANG_SOUND_LIBRARY), NULL, Icon_NOICON,
          &lib_overview_item, &lib_moods_item, &lib_pace_item,
          &lib_keys_item, &lib_decades_item);

bool sound_library_screen(void)
{
    int ret;

    /* The pass is a whole-index read and the disk may be asleep, so say so
     * rather than appearing to have hung. One pass serves every section. */
    splash(0, ID2P(LANG_WAIT));

    if (!lib_scan())
    {
        splash(HZ * 2, ID2P(LANG_SOUND_MIX_NO_INDEX));
        return false;
    }

    ret = do_menu(&lib_menu, NULL, NULL, false);

    return ret == MENU_ATTACHED_USB || ret == GO_TO_ROOT;
}
