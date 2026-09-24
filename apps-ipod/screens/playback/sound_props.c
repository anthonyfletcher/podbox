/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The sound read-out: what the Playlist Engine measured about one track, in
 * words rather than numbers.
 *
 * Every line is a band of one axis from sound_mix_axes(), and every band edge
 * is a percentile rather than a round number on the 0-1000 scale. That is the
 * same rule sound_mood.c states above its own table and for the same reason:
 * "Bright" has to mean brighter than music that exists, or the word lands on
 * nothing.
 *
 * Which music is sound_cal.c's answer -- this player's library, where it has
 * enough of one to take a percentile from. The numbers in the tables below
 * are the same percentiles of the 3439-record library they were derived on,
 * and they stand where a calibration cannot.
 *
 * Nothing is measured here and nothing is written. A record already on disk
 * is read once per track and turned into phrases.
 *
 * Parts, in order:
 *   - the bands
 *   - the record, and the one-track cache over it
 *   - the moods a track sits in
 *   - the rows
 *   - the mean of a folder, for an album read-out
 *   - the screen
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "lang.h"
#include "string-extra.h"
#include "settings/settings.h"
#include "files/filetypes.h"
#include "database/sound_cal.h"
#include "database/sound_index.h"
#include "database/sound_mix.h"
#include "database/sound_mood.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "input/action.h"
#include "speech/talk.h"
#include "system/activity.h"
#include "system/shutdown.h"
#include "widgets/list.h"
#include "widgets/text_box.h"
#include "screens/playback/sound_props.h"


/** The bands **/

/* A band runs up to 'upto' exclusive, and the last one is open.
 *
 * 'pct' is which point of the ladder that edge is, and it is the edge's real
 * definition -- 'upto' is that percentile taken on the library the table was
 * written against, kept as the answer for a player that has no calibration of
 * its own. sound_cal.c supplies the same point of this library's ladder where
 * it can. An axis is calibrated whole or not at all, so a table never mixes
 * one library's edges with another's. */
struct band
{
    int16_t upto;
    int16_t lang;
    int16_t pct;    /* per mille, or -1 for an edge the library cannot move */
};

#define BAND_TOP  (SOUND_AX + 1)

/* p25 436, p50 551, p75 647. */
static const struct band bd_energy[] = {
    { 440, LANG_SOUND_ENERGY_LOW,       250 },
    { 560, LANG_SOUND_ENERGY_MODERATE,  500 },
    { 650, LANG_SOUND_ENERGY_HIGH,      750 },
    { BAND_TOP, LANG_SOUND_ENERGY_VERY_HIGH, -1 } };

/* Spectral balance, not treble level -- see band_rel() in sound_mix.c. A
 * quarter of the library reads 0 here, so the first edge sits at p25 125 and
 * Dark names a quarter of the music rather than a corner of it. p50 291,
 * p75 458, p90 583. */
static const struct band bd_tone[] = {
    { 130, LANG_SOUND_TONE_DARK,     250 },
    { 300, LANG_SOUND_TONE_WARM,     500 },
    { 460, LANG_SOUND_TONE_BALANCED, 750 },
    { 590, LANG_SOUND_TONE_BRIGHT,   900 },
    { BAND_TOP, LANG_SOUND_TONE_VERY_BRIGHT, -1 } };

/* p25 388, p50 555, p75 700. */
static const struct band bd_activity[] = {
    { 390, LANG_SOUND_ACTIVITY_SPARSE, 250 },
    { 560, LANG_SOUND_ACTIVITY_OPEN,   500 },
    { 700, LANG_SOUND_ACTIVITY_BUSY,   750 },
    { BAND_TOP, LANG_SOUND_ACTIVITY_VERY_BUSY, -1 } };

/* p25 254, p50 381, p75 509. */
static const struct band bd_attack[] = {
    { 255, LANG_SOUND_ATTACK_SMOOTH, 250 },
    { 380, LANG_SOUND_ATTACK_EVEN,   500 },
    { 510, LANG_SOUND_ATTACK_PUNCHY, 750 },
    { BAND_TOP, LANG_SOUND_ATTACK_VERY_PUNCHY, -1 } };

/* p25 291, p50 400, p75 566. */
static const struct band bd_texture[] = {
    { 290, LANG_SOUND_TEXTURE_NOISY,   250 },
    { 400, LANG_SOUND_TEXTURE_BLENDED, 500 },
    { 570, LANG_SOUND_TEXTURE_CLEAR,   750 },
    { BAND_TOP, LANG_SOUND_TEXTURE_VERY_CLEAR, -1 } };

/* Width is the one axis whose interesting values are its ends, so the edges
 * are p10 40 and p90 630 with p50 210 between them: a tenth of the library at
 * either extreme, where being told is worth something, and the broad middle
 * split in two. */
static const struct band bd_space[] = {
    { 40,  LANG_SOUND_SPACE_MONO,   100 },
    { 210, LANG_SOUND_SPACE_NARROW, 500 },
    { 630, LANG_SOUND_SPACE_WIDE,   900 },
    { BAND_TOP, LANG_SOUND_SPACE_VERY_WIDE, -1 } };

/* Trap: the crest axis is inverted against its name, as sound_mood.c also
 * warns -- it is SOUND_AX minus the measured crest factor, so a high value is
 * a compressed master and a low one a dynamic master. The words below run in
 * that order and not in the one the axis name suggests. p10 334, p25 500,
 * p50 667, p75 834. */
static const struct band bd_master[] = {
    { 340, LANG_SOUND_MASTER_VERY_DYNAMIC, 100 },
    { 500, LANG_SOUND_MASTER_DYNAMIC,      250 },
    { 670, LANG_SOUND_MASTER_EVEN,         500 },
    { 840, LANG_SOUND_MASTER_COMPRESSED,   750 },
    { BAND_TOP, LANG_SOUND_MASTER_VERY_COMPRESSED, -1 } };

/* p25 416, p50 500, p75 583. */
static const struct band bd_movement[] = {
    { 420, LANG_SOUND_MOVEMENT_STATIC,   250 },
    { 500, LANG_SOUND_MOVEMENT_GENTLE,   500 },
    { 590, LANG_SOUND_MOVEMENT_SHIFTING, 750 },
    { BAND_TOP, LANG_SOUND_MOVEMENT_RESTLESS, -1 } };

/* Beats per minute, so these are absolute and owe the library nothing: a
 * listener who knows what 90 BPM feels like is right about it whatever else
 * is on the player. */
static const struct band bd_pace[] = {
    { 70,  LANG_SOUND_PACE_VERY_SLOW, -1 },
    { 90,  LANG_SOUND_PACE_SLOW,      -1 },
    { 110, LANG_SOUND_PACE_RELAXED,   -1 },
    { 130, LANG_SOUND_PACE_MODERATE,  -1 },
    { 150, LANG_SOUND_PACE_BRISK,     -1 },
    { 170, LANG_SOUND_PACE_FAST,      -1 },
    { 32767, LANG_SOUND_PACE_VERY_FAST, -1 } };

/* One edge, this library's where it has one.
 *
 * A calibrated ladder cannot decrease, so resolved edges stay in the order
 * the table is written in. Two that land together leave a band naming nothing
 * on this library, which is the right answer and not a fault. */
static int band_edge(const struct band *b, int axis)
{
    int cal = b->pct < 0 ? -1 : sound_cal_at(axis, b->pct);

    return cal >= 0 ? cal : b->upto;
}

/* The last band of every table is open, so this always lands on one. */
static int band_of(const struct band *b, int axis, int v)
{
    while (v >= band_edge(b, axis))
        b++;

    return b->lang;
}

/* The low or middle band leads where it sits in the top quarter of the
 * library and above the other -- p75 is 464 and 450 on the library the table
 * was written against, and this one's where sound_cal.c has it. One or
 * neither, never both: two directions named at once describe nothing. */
static int tone_lead(const struct sound_axes *a)
{
    int lo = sound_cal_at(CAL_LOW, 750);
    int md = sound_cal_at(CAL_MID, 750);

    if (lo < 0)
        lo = 464;

    if (md < 0)
        md = 450;

    if (a->low >= lo && a->low > a->mid)
        return LANG_SOUND_TONE_BASS_LED;

    if (a->mid >= md && a->mid > a->low)
        return LANG_SOUND_TONE_MID_LED;

    return -1;
}


/** The record **/

/* One track's reading, kept because the list asks for every row on every draw
 * and the answer is behind a file open and a binary search. Keyed by path,
 * which is what the caller has. */
static char props_path[MAX_PATH];
static struct sound_record props_rec;
static struct sound_axes props_ax;
static bool props_have;

/* A folder's read-out is the same rows over the mean of its tracks, so it
 * shares everything below and differs in two places only: there is no one
 * record behind it, so the pace comes from here rather than from props_rec,
 * and there is no key row at all. */
static bool props_album;
static int  props_bpm;     /* mean of the tracks with a trusted tempo, or 0 */

static bool props_load(const char *path)
{
    struct sound_index_reader r;
    uint64_t key;

    if (path == NULL || path[0] == '\0')
        return false;

    if (props_have && !props_album && strcmp(props_path, path) == 0)
        return true;

    props_have = false;
    props_album = false;
    strmemccpy(props_path, path, sizeof (props_path));

    if (!global_settings.playlist_engine || !sound_index_exists())
        return false;

    /* Before the bands are read rather than beside them: the first call is a
     * pass over the whole index, and doing it here costs one track's row
     * rather than one row's draw. */
    sound_cal_ensure();

    if (sound_index_reader_open(&r) != SOUND_OK)
        return false;

    key = sound_index_key(path);

    if (sound_index_find(&r, key, &props_rec) &&
        sound_record_usable(&props_rec))
    {
        sound_mix_axes(&props_rec, &props_ax);
        props_have = true;
    }

    sound_index_reader_close(&r);

    return props_have;
}


/** The moods **/

#define PROPS_MOODS  3

/* The moods this track would be offered by, nearest first, and how many there
 * are. MIX_MAX_DISTANCE is the ceiling sound_mix.c builds a playlist against,
 * so the row says something a listener can act on: these are the mood
 * playlists this track turns up in.
 *
 * None is an answer rather than a failure -- about one track in twelve sits
 * inside no mood at all -- so 'out[0]' is then the nearest of the sixteen and
 * the return is zero, which the caller reports as a nearest rather than as
 * somewhere the track is. */
static int props_moods(const struct sound_axes *a, uint8_t *out)
{
    int score[PROPS_MOODS];
    int held = 0;
    int near_mood = -1;
    int near_score = 0;
    int m, i, j;

    for (m = 0; m < MOOD_COUNT; m++)
    {
        int d = sound_mood_score(a, m);

        if (d < 0)
            continue;

        if (near_mood < 0 || d < near_score)
        {
            near_mood = m;
            near_score = d;
        }

        if (d > MIX_MAX_DISTANCE)
            continue;

        for (i = 0; i < held && score[i] <= d; i++)
            ;

        if (i >= PROPS_MOODS)
            continue;

        for (j = (held < PROPS_MOODS ? held : PROPS_MOODS - 1); j > i; j--)
        {
            out[j] = out[j - 1];
            score[j] = score[j - 1];
        }

        out[i] = (uint8_t)m;
        score[i] = d;

        if (held < PROPS_MOODS)
            held++;
    }

    if (held == 0 && near_mood >= 0)
        out[0] = (uint8_t)near_mood;

    return held;
}

/* The mood names as one string, and how many of them the track is actually
 * in. Zero leaves the single nearest in the buffer. */
static void props_mood_text(char *buf, size_t len, int *count, bool say_it)
{
    uint8_t pick[PROPS_MOODS];
    int n = props_moods(&props_ax, pick);
    int i;

    *count = n;
    buf[0] = '\0';

    for (i = 0; i < (n > 0 ? n : 1); i++)
    {
        int id = sound_mood_name(pick[i]);

        if (i > 0)
            strlcat(buf, ", ", len);

        strlcat(buf, str(id), len);

        if (say_it)
            talk_id(id, true);
    }
}


/** The rows **/

enum props_row
{
    ROW_MOODS = 0,
    ROW_ENERGY,
    ROW_PACE,
    ROW_TONE,
    ROW_ACTIVITY,
    ROW_ATTACK,
    ROW_TEXTURE,
    ROW_SPACE,
    ROW_MASTER,
    ROW_MOVEMENT,
    ROW_KEY,
    ROW_COUNT
};

static int props_rows(void)
{
    /* ROW_KEY is last, so a folder is simply the rows before it. */
    return props_album ? ROW_KEY : ROW_COUNT;
}

/* Note names, not phrases: the notation is the same in every language this
 * builds for. */
static const char * const props_notes[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

/* The label a row carries. Moods is the one that moves, because a track
 * inside none of them is being told something different. */
static int props_label(int row, int mood_count)
{
    switch (row)
    {
        case ROW_MOODS:
            return mood_count > 0 ? LANG_SOUND_MOODS
                                  : LANG_SOUND_NEAREST_MOOD;
        case ROW_ENERGY:   return LANG_SOUND_ENERGY;
        case ROW_PACE:     return LANG_SOUND_PACE;
        case ROW_TONE:     return LANG_SOUND_TONE;
        case ROW_ACTIVITY: return LANG_SOUND_ACTIVITY;
        case ROW_ATTACK:   return LANG_SOUND_ATTACK;
        case ROW_TEXTURE:  return LANG_SOUND_TEXTURE;
        case ROW_SPACE:    return LANG_SOUND_SPACE;
        case ROW_MASTER:   return LANG_SOUND_MASTER;
        case ROW_MOVEMENT: return LANG_SOUND_MOVEMENT;
        default:           return LANG_SOUND_KEY;
    }
}

/* One band, spoken where this is a voice pass. */
static void props_band_text(char *buf, size_t len, int id, bool say_it)
{
    strlcpy(buf, str(id), len);

    if (say_it)
        talk_id(id, true);
}

/* The tempo, against both tolerances rather than one -- see
 * SOUND_TEMPO_MATCH_PER_MILLE and SOUND_TEMPO_PHASE_MS in sound_index.h.
 *
 * The number is shown whenever there is one: it is the only thing on this
 * screen a listener can check against their own foot. The two bounds decide
 * what is said around it. Whether the tempo may be named at all is the
 * looser question and the engine has already answered it, so that is read
 * off the axis rather than tested a second time here. Whether it holds
 * firmly enough to call steady is the tighter one, and a tempo can be well
 * worth matching on while being no use to set a metronome by -- which is
 * most live playing, and nearly all jazz. */
static void props_pace(char *buf, size_t len, bool say_it)
{
    int bpm, id;
    bool named, tight;

    /* A folder reports the mean of the tracks whose tempo is trusted, and
     * nothing where none of them is. There is no steadiness to report: how
     * far one track's beat wandered says nothing about an album's pace, and
     * the word for it would read as a judgement on all of them. */
    if (props_album)
    {
        if (props_bpm == 0)
        {
            props_band_text(buf, len, LANG_SOUND_PACE_NONE, say_it);
            return;
        }

        snprintf(buf, len, "%s, %d %s",
                 str(band_of(bd_pace, -1, props_bpm)), props_bpm,
                 str(LANG_SOUND_BPM));

        if (say_it)
            talk_id(band_of(bd_pace, -1, props_bpm), true);

        return;
    }

    if (props_rec.period_ms == 0)
    {
        props_band_text(buf, len, LANG_SOUND_PACE_NONE, say_it);
        return;
    }

    bpm = 60000 / props_rec.period_ms;
    named = props_ax.tempo >= 0;
    tight = props_rec.tempo_spread <= SOUND_TEMPO_PHASE_MS;
    id = band_of(bd_pace, -1, bpm);

    if (named)
        snprintf(buf, len, "%s, %d %s", str(id), bpm, str(LANG_SOUND_BPM));
    else
        snprintf(buf, len, "%d %s", bpm, str(LANG_SOUND_BPM));

    if (!tight)
    {
        strlcat(buf, ", ", len);
        strlcat(buf, str(LANG_SOUND_PACE_UNSTEADY), len);
    }

    if (say_it)
    {
        if (named)
            talk_id(id, true);

        talk_number(bpm, true);
        talk_id(LANG_SOUND_BPM, true);

        if (!tight)
            talk_id(LANG_SOUND_PACE_UNSTEADY, true);
    }
}

static void props_key(char *buf, size_t len, bool say_it)
{
    int id;

    if (props_ax.mode < 0 || props_rec.tonic > 11)
    {
        props_band_text(buf, len, LANG_SOUND_KEY_UNCLEAR, say_it);
        return;
    }

    id = props_rec.mode ? LANG_SOUND_KEY_MINOR : LANG_SOUND_KEY_MAJOR;
    snprintf(buf, len, "%s %s", props_notes[props_rec.tonic], str(id));

    if (say_it)
    {
        talk_spell(props_notes[props_rec.tonic], true);
        talk_id(id, true);
    }
}

/* A row's value. 'count' comes back with the number of moods the track is in,
 * which only ROW_MOODS sets and only its label needs. */
static void props_value(int row, char *buf, size_t len, int *count,
                        bool say_it)
{
    *count = 0;

    if (say_it && row != ROW_MOODS)
        talk_id(props_label(row, 0), false);

    switch (row)
    {
        case ROW_MOODS:
            /* The label depends on the answer, so it is spoken after the
             * count is known rather than before it. */
            props_mood_text(buf, len, count, false);

            if (say_it)
            {
                talk_id(props_label(row, *count), false);
                props_mood_text(buf, len, count, true);
            }
            break;

        case ROW_ENERGY:
            props_band_text(buf, len,
                            band_of(bd_energy, CAL_ENERGY, props_ax.energy),
                            say_it);
            break;

        case ROW_PACE:
            props_pace(buf, len, say_it);
            break;

        case ROW_TONE:
        {
            int lead = tone_lead(&props_ax);
            int id = band_of(bd_tone, CAL_BRIGHT, props_ax.bright);

            if (lead < 0)
                props_band_text(buf, len, id, say_it);
            else
            {
                snprintf(buf, len, "%s, %s", str(id), str(lead));

                if (say_it)
                {
                    talk_id(id, true);
                    talk_id(lead, true);
                }
            }
            break;
        }

        case ROW_ACTIVITY:
            props_band_text(buf, len,
                            band_of(bd_activity, CAL_DENS, props_ax.dens),
                            say_it);
            break;

        case ROW_ATTACK:
            props_band_text(buf, len,
                            band_of(bd_attack, CAL_PEAK, props_ax.peak),
                            say_it);
            break;

        case ROW_TEXTURE:
            props_band_text(buf, len,
                            band_of(bd_texture, CAL_CLARITY, props_ax.clarity),
                            say_it);
            break;

        case ROW_SPACE:
            props_band_text(buf, len,
                            band_of(bd_space, CAL_WIDTH, props_ax.width),
                            say_it);
            break;

        case ROW_MASTER:
            props_band_text(buf, len,
                            band_of(bd_master, CAL_CREST, props_ax.crest),
                            say_it);
            break;

        case ROW_MOVEMENT:
            props_band_text(buf, len,
                            band_of(bd_movement, CAL_CHANGE, props_ax.change),
                            say_it);
            break;

        default:
            props_key(buf, len, say_it);
            break;
    }
}

static const char *props_get_name(int row, void *data, char *buf,
                                  size_t buf_len)
{
    char value[MAX_PATH];
    int count;

    (void)data;

    props_value(row, value, sizeof (value), &count, false);
    snprintf(buf, buf_len, "%s: %s", str(props_label(row, count)), value);

    return buf;
}

static int props_speak(int row, void *data)
{
    char value[MAX_PATH];
    int count;

    (void)data;
    props_value(row, value, sizeof (value), &count, true);

    return 0;
}


/** The screen **/

bool sound_props_summary(const char *path, char *buf, size_t len)
{
    int count;

    if (!props_load(path))
        return false;

    props_mood_text(buf, len, &count, false);

    /* Inside no mood, the nearest one is not a description of the track, so
     * the summary says what was measured rather than what it is near. */
    if (count == 0)
        snprintf(buf, len, "%s, %s",
                 str(band_of(bd_energy, CAL_ENERGY, props_ax.energy)),
                 str(band_of(bd_tone, CAL_BRIGHT, props_ax.bright)));

    return true;
}

static bool props_show(const char *title)
{
    struct gui_synclist lists;
    bool leave = false;
    int key;

    /* The activity Track Info pushes, and deliberately not one of its own: a
     * theme that gives Track Info a viewport is describing a read-out of a
     * single track, which is what this is. A second activity would leave
     * every such theme styling one of the two and not the other. */
    push_current_activity(ACTIVITY_ID3SCREEN);

    gui_synclist_init(&lists, &props_get_name, NULL, false, 1, NULL);

    if (global_settings.talk_menu)
        gui_synclist_set_voice_callback(&lists, props_speak);

    gui_synclist_set_nb_items(&lists, props_rows());
    gui_synclist_set_title(&lists, title, NOICON);
    gui_synclist_draw_settled(&lists);
    gui_synclist_speak_item(&lists);

    while (true)
    {
        if (list_do_action(CONTEXT_LIST, HZ / 2, &lists, &key))
            continue;

        if (key == ACTION_STD_OK)
        {
            char value[MAX_PATH];
            int row = gui_synclist_get_sel_pos(&lists);
            int count;

            /* A row mid-scroll keeps animating under the text view. */
            gui_synclist_scroll_stop(&lists);
            props_value(row, value, sizeof (value), &count, false);

            FOR_NB_SCREENS(i)
                viewportmanager_theme_enable(i, false, NULL);

            leave = view_text(str(props_label(row, count)), value) != 0;

            FOR_NB_SCREENS(i)
                viewportmanager_theme_undo(i, false);

            if (leave)
                break;

            gui_synclist_set_title(&lists, title, NOICON);
            gui_synclist_draw(&lists);
            continue;
        }

        if (key == ACTION_STD_CANCEL)
            break;

        if (key == ACTION_STD_MENU ||
            default_event_handler(key) == SYS_USB_CONNECTED)
        {
            leave = true;
            break;
        }
    }

    FOR_NB_SCREENS(i)
        screens[i].scroll_stop();

    pop_current_activity();

    return leave;
}

bool sound_props_screen(const char *path)
{
    /* Read the record again rather than trusting the cache the row filled.
     * A rescan between the two is the one thing that changes the answer for a
     * path that has not changed, and looking at a track to see what a rescan
     * did to it is the reason to open this. */
    props_have = false;

    if (!props_load(path))
        return false;

    return props_show(str(LANG_SOUND_PROPERTIES));
}


/* The mean of a set of tracks, fed in one at a time.
 *
 * One at a time because the two callers enumerate differently and neither
 * can hand over a list: the file browser has a folder to walk, and the
 * database browser walks a row's subentries through a callback of its own
 * (browser_db_subentries_do_action()). Averaging here rather than in either
 * of them is what keeps this out of both browsers' business.
 *
 * The quantities average and the categories do not. Mode is the only category
 * that reaches an axis, and it is taken as a majority or not at all: an album
 * half in one mode and half in the other is in neither, and a mood reading
 * the axis would otherwise be handed a decision the music does not support.
 *
 * Tempo is averaged over the tracks that have one rather than over all of
 * them, for the same reason the moods that name a speed only offer those
 * tracks -- an untrusted reading is not a slow one.
 *
 * The index reader is held open across the run. A track arrives per call, and
 * opening the index for each would be a file open per track. */
static struct album_acc
{
    struct sound_index_reader r;
    struct sound_axes sum;
    long bpm_sum;
    int n, n_tempo, n_steady, n_mode, minor;
    bool open;
    bool done;

    /* One of the tracks counted, for a caller that needs to know whose album
     * this is -- the mix rules key an artist off a path, and a mean has
     * none. Any of them will do; they share a folder. */
    char one[MAX_PATH];
} alb;

void sound_props_album_begin(void)
{
    memset(&alb, 0, sizeof (alb));

    props_have = false;
    props_album = true;
    props_bpm = 0;

    memset(&props_ax, 0, sizeof (props_ax));

    if (!global_settings.playlist_engine || !sound_index_exists())
        return;

    sound_cal_ensure();

    alb.open = sound_index_reader_open(&alb.r) == SOUND_OK;
}

void sound_props_album_add(const char *path)
{
    struct sound_record rec;
    struct sound_axes a;

    if (!alb.open || path == NULL || path[0] == '\0')
        return;

    if (!sound_index_find(&alb.r, sound_index_key(path), &rec) ||
        !sound_record_usable(&rec))
        return;

    sound_mix_axes(&rec, &a);

    alb.sum.loud     += a.loud;
    alb.sum.crest    += a.crest;
    alb.sum.width    += a.width;
    alb.sum.peak     += a.peak;
    alb.sum.clarity  += a.clarity;
    alb.sum.change   += a.change;
    alb.sum.dens     += a.dens;
    alb.sum.bright   += a.bright;
    alb.sum.low      += a.low;
    alb.sum.mid      += a.mid;
    alb.sum.dynamics += a.dynamics;
    alb.sum.energy   += a.energy;

    if (a.tempo >= 0)
    {
        alb.sum.tempo += a.tempo;
        alb.sum.speed += a.speed;
        alb.bpm_sum   += 60000 / rec.period_ms;
        alb.n_tempo++;
    }

    if (a.steady >= 0)
    {
        alb.sum.steady += a.steady;
        alb.n_steady++;
    }

    if (a.mode >= 0)
    {
        alb.n_mode++;

        if (a.mode != 0)
            alb.minor++;
    }

    if (alb.n == 0)
        strmemccpy(alb.one, path, sizeof (alb.one));

    alb.n++;
}

/* Turn the sums into the mean, once. Every finishing call reaches it and any
 * of them may come first, so it guards itself: run twice it would divide what
 * it has already divided.
 *
 * Deliberately separate from showing the result. props_show() answers "was
 * this left for the root menu", which is not "was there anything to show" --
 * a caller that reads one as the other reports an unmeasured album every time
 * somebody presses back. */
bool sound_props_album_ready(void)
{
    int n = alb.n;

    if (alb.open)
        sound_index_reader_close(&alb.r);

    alb.open = false;

    if (alb.done)
        return n > 0;

    alb.done = true;

    if (n == 0)
        return false;

    props_ax.loud     = alb.sum.loud / n;
    props_ax.crest    = alb.sum.crest / n;
    props_ax.width    = alb.sum.width / n;
    props_ax.peak     = alb.sum.peak / n;
    props_ax.clarity  = alb.sum.clarity / n;
    props_ax.change   = alb.sum.change / n;
    props_ax.dens     = alb.sum.dens / n;
    props_ax.bright   = alb.sum.bright / n;
    props_ax.low      = alb.sum.low / n;
    props_ax.mid      = alb.sum.mid / n;
    props_ax.dynamics = alb.sum.dynamics / n;
    props_ax.energy   = alb.sum.energy / n;

    props_ax.tempo  = alb.n_tempo  ? alb.sum.tempo / alb.n_tempo   : -1;
    props_ax.speed  = alb.n_tempo  ? alb.sum.speed / alb.n_tempo   : -1;
    props_ax.steady = alb.n_steady ? alb.sum.steady / alb.n_steady : -1;
    props_bpm       = alb.n_tempo
                      ? (int)(alb.bpm_sum / alb.n_tempo) : 0;

    /* A majority, and level is not a majority. */
    if (alb.n_mode > 0 && alb.minor * 2 != alb.n_mode)
        props_ax.mode = alb.minor * 2 > alb.n_mode ? 1 : 0;
    else
        props_ax.mode = -1;

    props_have = true;

    return true;
}

bool sound_props_album_screen(void)
{
    return props_show(str(LANG_SOUND_ALBUM));
}

bool sound_props_album_result(struct sound_axes *out, char *path, size_t len)
{
    if (!sound_props_album_ready())
        return false;

    *out = props_ax;

    if (path != NULL)
        strmemccpy(path, alb.one, len);

    return true;
}

void sound_props_album_walk(const char *dir)
{
    DIR *d;
    struct dirent *e;
    char path[MAX_PATH];

    sound_props_album_begin();

    /* A folder that will not open, or that was never named, has nothing
     * measured in it as far as this is concerned. The finishing call is still
     * the caller's to make, since it is what closes the index. */
    d = dir != NULL ? opendir(dir) : NULL;
    if (d == NULL)
        return;

    while ((e = readdir(d)) != NULL)
    {
        if (filetype_get_attr(e->d_name) != FILE_ATTR_AUDIO)
            continue;

        /* The separator only where the folder does not already end in one,
         * or the root gives "//name" -- which keys to a different record
         * from the one the scan wrote, and so reads as unmeasured. */
        if (snprintf(path, sizeof (path), "%s%s%s", dir,
                     dir[0] != '\0' && dir[strlen(dir) - 1] == '/' ? "" : "/",
                     e->d_name) >= (int)sizeof (path))
            continue;

        sound_props_album_add(path);
    }

    closedir(d);
}
