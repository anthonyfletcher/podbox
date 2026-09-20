/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The sound read-out: what the Playlist Engine measured about one track, in
 * words rather than numbers.
 *
 * Every line is a band of one axis from sound_mix_axes(), and every band edge
 * is a percentile of a real 3439-record library rather than a round number on
 * the 0-1000 scale. That is the same rule sound_mood.c states above its own
 * table and for the same reason: "Bright" has to mean brighter than music
 * that exists, or the word lands on nothing. The percentile behind each edge
 * is named beside it.
 *
 * Nothing is measured here and nothing is written. A record already on disk
 * is read once per track and turned into phrases.
 *
 * Parts, in order:
 *   - the bands
 *   - the record, and the one-track cache over it
 *   - the moods a track sits in
 *   - the rows
 *   - the screen
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "lang.h"
#include "string-extra.h"
#include "settings/settings.h"
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

/* A band runs up to 'upto' exclusive, and the last one is open. */
struct band
{
    int16_t upto;
    int16_t lang;
};

#define BAND_TOP  (SOUND_AX + 1)

/* p25 436, p50 551, p75 647. */
static const struct band bd_energy[] = {
    { 440, LANG_SOUND_ENERGY_LOW },
    { 560, LANG_SOUND_ENERGY_MODERATE },
    { 650, LANG_SOUND_ENERGY_HIGH },
    { BAND_TOP, LANG_SOUND_ENERGY_VERY_HIGH } };

/* Spectral balance, not treble level -- see band_rel() in sound_mix.c. A
 * quarter of the library reads 0 here, so the first edge sits at p25 125 and
 * Dark names a quarter of the music rather than a corner of it. p50 291,
 * p75 458, p90 583. */
static const struct band bd_tone[] = {
    { 130, LANG_SOUND_TONE_DARK },
    { 300, LANG_SOUND_TONE_WARM },
    { 460, LANG_SOUND_TONE_BALANCED },
    { 590, LANG_SOUND_TONE_BRIGHT },
    { BAND_TOP, LANG_SOUND_TONE_VERY_BRIGHT } };

/* p25 388, p50 555, p75 700. */
static const struct band bd_activity[] = {
    { 390, LANG_SOUND_ACTIVITY_SPARSE },
    { 560, LANG_SOUND_ACTIVITY_OPEN },
    { 700, LANG_SOUND_ACTIVITY_BUSY },
    { BAND_TOP, LANG_SOUND_ACTIVITY_VERY_BUSY } };

/* p25 254, p50 381, p75 509. */
static const struct band bd_attack[] = {
    { 255, LANG_SOUND_ATTACK_SMOOTH },
    { 380, LANG_SOUND_ATTACK_EVEN },
    { 510, LANG_SOUND_ATTACK_PUNCHY },
    { BAND_TOP, LANG_SOUND_ATTACK_VERY_PUNCHY } };

/* p25 291, p50 400, p75 566. */
static const struct band bd_texture[] = {
    { 290, LANG_SOUND_TEXTURE_NOISY },
    { 400, LANG_SOUND_TEXTURE_BLENDED },
    { 570, LANG_SOUND_TEXTURE_CLEAR },
    { BAND_TOP, LANG_SOUND_TEXTURE_VERY_CLEAR } };

/* Width is the one axis whose interesting values are its ends, so the edges
 * are p10 40 and p90 630 with p50 210 between them: a tenth of the library at
 * either extreme, where being told is worth something, and the broad middle
 * split in two. */
static const struct band bd_space[] = {
    { 40, LANG_SOUND_SPACE_MONO },
    { 210, LANG_SOUND_SPACE_NARROW },
    { 630, LANG_SOUND_SPACE_WIDE },
    { BAND_TOP, LANG_SOUND_SPACE_VERY_WIDE } };

/* Trap: the crest axis is inverted against its name, as sound_mood.c also
 * warns -- it is SOUND_AX minus the measured crest factor, so a high value is
 * a compressed master and a low one a dynamic master. The words below run in
 * that order and not in the one the axis name suggests. p10 334, p25 500,
 * p50 667, p75 834. */
static const struct band bd_master[] = {
    { 340, LANG_SOUND_MASTER_VERY_DYNAMIC },
    { 500, LANG_SOUND_MASTER_DYNAMIC },
    { 670, LANG_SOUND_MASTER_EVEN },
    { 840, LANG_SOUND_MASTER_COMPRESSED },
    { BAND_TOP, LANG_SOUND_MASTER_VERY_COMPRESSED } };

/* p25 416, p50 500, p75 583. */
static const struct band bd_movement[] = {
    { 420, LANG_SOUND_MOVEMENT_STATIC },
    { 500, LANG_SOUND_MOVEMENT_GENTLE },
    { 590, LANG_SOUND_MOVEMENT_SHIFTING },
    { BAND_TOP, LANG_SOUND_MOVEMENT_RESTLESS } };

/* Beats per minute, so these are absolute and owe the library nothing: a
 * listener who knows what 90 BPM feels like is right about it whatever else
 * is on the player. */
static const struct band bd_pace[] = {
    { 70, LANG_SOUND_PACE_VERY_SLOW },
    { 90, LANG_SOUND_PACE_SLOW },
    { 110, LANG_SOUND_PACE_RELAXED },
    { 130, LANG_SOUND_PACE_MODERATE },
    { 150, LANG_SOUND_PACE_BRISK },
    { 170, LANG_SOUND_PACE_FAST },
    { 32767, LANG_SOUND_PACE_VERY_FAST } };

/* The last band of every table is open, so this always lands on one. */
static int band_of(const struct band *b, int v)
{
    while (v >= b->upto)
        b++;

    return b->lang;
}

/* The low or middle band leads where it sits in the top quarter of the
 * library and above the other -- p75 is 464 and 450. One or neither, never
 * both: two directions named at once describe nothing. */
static int tone_lead(const struct sound_axes *a)
{
    if (a->low >= 464 && a->low > a->mid)
        return LANG_SOUND_TONE_BASS_LED;

    if (a->mid >= 450 && a->mid > a->low)
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

static bool props_load(const char *path)
{
    struct sound_index_reader r;
    uint64_t key;

    if (path == NULL || path[0] == '\0')
        return false;

    if (props_have && strcmp(props_path, path) == 0)
        return true;

    props_have = false;
    strmemccpy(props_path, path, sizeof (props_path));

    if (!global_settings.playlist_engine || !sound_index_exists())
        return false;

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

    if (props_rec.period_ms == 0)
    {
        props_band_text(buf, len, LANG_SOUND_PACE_NONE, say_it);
        return;
    }

    bpm = 60000 / props_rec.period_ms;
    named = props_ax.tempo >= 0;
    tight = props_rec.tempo_spread <= SOUND_TEMPO_PHASE_MS;
    id = band_of(bd_pace, bpm);

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
            props_band_text(buf, len, band_of(bd_energy, props_ax.energy),
                            say_it);
            break;

        case ROW_PACE:
            props_pace(buf, len, say_it);
            break;

        case ROW_TONE:
        {
            int lead = tone_lead(&props_ax);
            int id = band_of(bd_tone, props_ax.bright);

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
            props_band_text(buf, len, band_of(bd_activity, props_ax.dens),
                            say_it);
            break;

        case ROW_ATTACK:
            props_band_text(buf, len, band_of(bd_attack, props_ax.peak),
                            say_it);
            break;

        case ROW_TEXTURE:
            props_band_text(buf, len, band_of(bd_texture, props_ax.clarity),
                            say_it);
            break;

        case ROW_SPACE:
            props_band_text(buf, len, band_of(bd_space, props_ax.width),
                            say_it);
            break;

        case ROW_MASTER:
            props_band_text(buf, len, band_of(bd_master, props_ax.crest),
                            say_it);
            break;

        case ROW_MOVEMENT:
            props_band_text(buf, len, band_of(bd_movement, props_ax.change),
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
                 str(band_of(bd_energy, props_ax.energy)),
                 str(band_of(bd_tone, props_ax.bright)));

    return true;
}

bool sound_props_screen(const char *path)
{
    struct gui_synclist lists;
    bool leave = false;
    int key;

    /* Read the record again rather than trusting the cache the row filled.
     * A rescan between the two is the one thing that changes the answer for a
     * path that has not changed, and looking at a track to see what a rescan
     * did to it is the reason to open this. */
    props_have = false;

    if (!props_load(path))
        return false;

    /* The activity Track Info pushes, and deliberately not one of its own: a
     * theme that gives Track Info a viewport is describing a read-out of a
     * single track, which is what this is. A second activity would leave
     * every such theme styling one of the two and not the other. */
    push_current_activity(ACTIVITY_ID3SCREEN);

    gui_synclist_init(&lists, &props_get_name, NULL, false, 1, NULL);

    if (global_settings.talk_menu)
        gui_synclist_set_voice_callback(&lists, props_speak);

    gui_synclist_set_nb_items(&lists, ROW_COUNT);
    gui_synclist_set_title(&lists, str(LANG_SOUND_PROPERTIES), NOICON);
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

            gui_synclist_set_title(&lists, str(LANG_SOUND_PROPERTIES), NOICON);
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
