/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Measures every track in the library and writes the sound index.
 *
 * The whole of this screen is one loop: walk the database, decode a window of
 * each track that has no current record, and append what it measured. It runs
 * for hours and its design follows from that rather than from anything about
 * the analysis.
 *
 * Three rules fall out of the duration. It runs only on the charger, and asks
 * for one *after* explaining itself rather than before -- someone who does not
 * yet know what this is cannot be expected to have plugged in for it. Losing
 * the charger pauses it rather than ending it: a player running unpowered with
 * the disk awake is the worst state it can be in, but so is a message the user
 * cannot act on before the screen closes. And stopping never loses anything --
 * the index is appended to as it goes, so there are two endings and both keep
 * what was measured.
 *
 * While it runs the player charges without mounting (USB_MODE_CHARGE): a cable
 * found on a desk is nearly always power, and handing the disk to a laptop
 * a quarter of the way through loses the run for a connection nobody asked
 * for.
 *
 * Parts, in order:
 *   - progress and the display
 *   - the per-track callbacks the decoder polls
 *   - one track
 *   - the run
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "core_alloc.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "power.h"
#include "powermgmt.h"
#include "backlight.h"
#include "audio.h"
#include "usb.h"
#include "file.h"
#include "metadata.h"
#include "string-extra.h"
#include "settings/settings.h"
#include "audio/beat_probe.h"
#include "audio/track_decode.h"
#include "database/sound_index.h"
#include "database/tagcache.h"
#include "screens/system/sound_scan.h"
#include "draw/screen_access.h"
#include "widgets/splash.h"
#include "widgets/yesno.h"
#include "widgets/dialog.h"
#include "widgets/dialog_prose.h"
#include "draw/viewport.h"
#include "font.h"
#include "input/action.h"
#include "api/misc.h"
#include "system/format_time.h"

/* The window, from the specification: past the intro, and long enough for the
 * tempo envelope to fill and then hold.
 *
 * The whole window is taken. Stopping as soon as beat_probe_settled() is true
 * ended the decode at around eleven seconds, and that is a tempo criterion
 * ending every other measurement with it -- loudness, the band levels, the
 * onset rates and the chroma all stop being sampled because the *tempo* has
 * converged. Measured over thirty tracks against a forty-second window, the
 * eleven-second one disagreed about the nearest track two times in three,
 * changed its mind about whether to trust the tempo on thirteen of them and
 * about major-versus-minor on twelve. Those axes carry most of the weight in
 * a match, so the early stop was buying scan time with the thing the scan is
 * for. */
#define SS_START_PCT     15
#define SS_START_MIN_MS  5000
#define SS_START_MAX_MS  30000
#define SS_WINDOW_MS     40000

/* The codec's view of the file. Must exceed the largest contiguous read any
 * codec asks for, or track_decode.c re-centres its window on every request. */
#define SS_WINDOW_BYTES  (256 * 1024)

/* Tracks too short to measure. The tempo envelope alone wants seventeen
 * seconds and the window starts after the intro. */
#define SS_MIN_LENGTH_MS 25000

static int           ss_handle;

static int           ss_total;
static int           ss_done;      /* Measured this run */
static int           ss_skipped;   /* Already current */
static int           ss_failed;

static long          ss_work;      /* Ticks spent measuring */
static unsigned long ss_audio_ms;  /* Audio measured, for the rate */

static char          ss_now[64];

static bool          ss_stop;      /* The user asked */
static bool          ss_unplugged;


/** Progress **/

/* What the run is asking for, before it starts.
 *
 * Long enough to need scrolling, which is the point: three of these four
 * paragraphs are things a user would be annoyed to discover afterwards, and
 * the fourth is the one that makes the other three acceptable. */
static const char ss_explain[] =
    "This measures how every track sounds -- its tempo, loudness, energy "
    "and key -- and stores the result, so playlists can be built from how "
    "the music feels rather than from its tags.\n"
    "\n"
    "It takes hours. A few thousand tracks is most of a night.\n"
    "\n"
    "The player has to stay plugged in, and stops if the charger is "
    "removed. Nothing else runs while it works: the disk stays awake "
    "throughout and the processor runs flat out, so the player will be warm "
    "and will do nothing else until it has finished.\n"
    "\n"
    "Stopping costs nothing -- you can update the analysis, which will carry "
    "on from where it left off.";

static const char ss_depth_explain[] =
    "Thorough listens to forty seconds of each track. Quick stops as soon as "
    "it has worked out the tempo, around eleven seconds, and gets through the "
    "library about three and a half times faster.\n"
    "\n"
    "Quick is faster than it is accurate. Working out the tempo is what stops "
    "it, and that ends the measurement of loudness, tone and key along with "
    "it -- measured against a full analysis, Quick picked a different closest "
    "track two times in three.\n"
    "\n"
    "Choose Quick if a full analysis on the player is too long to sit "
    "through. The desktop tool does a whole library thoroughly in minutes.";

/* Asked once the run has been agreed rather than before it: this is a
 * question about how long to spend, which only matters to somebody who has
 * decided to spend it. Remembered, so the answer also settles what a later
 * update does. */
static void ss_ask_depth(void)
{
    global_settings.analysis_depth =
        dialog_prose_confirm("Analysis depth", ss_depth_explain,
                             "Thorough", "Quick")
        ? ANALYSIS_THOROUGH : ANALYSIS_QUICK;

    settings_save();
}

/* The running display: a framed box with the figures and one button.
 *
 * A box rather than the whole screen because that is what it is -- one modal
 * thing happening over the player, with one way out. Drawn with the shared
 * dialog frame so it follows the theme, but not through dialog_run(): the
 * loop here belongs to the scan, which spends ten seconds inside a codec at
 * a time and polls for input from in there. */
/* Four lines and a button, in that order: what it is doing, what it is doing
 * it to, how long that leaves, and the bar. The pause takes the same four so
 * the box does not resize when the charger comes out. */
static void ss_draw(const char *paused)
{
    struct screen *lcd = &screens[SCREEN_MAIN];
    struct dialog_style style = *dialog_get_default_style();
    struct viewport vp, content, *last_vp;
    char line[64];
    int fh, bh;
    int y, w, bar;

    viewport_set_defaults(&vp, lcd->screen_type);
    last_vp = lcd->set_viewport(&vp);

    fh = font_get(vp.font)->height;
    bh = fh + 8;

    vp.width  = lcd->lcdwidth * 92 / 100;
    vp.height = fh * 4 + fh / 2 + bh + style.box_margin * 2 + 8;
    if (vp.height > lcd->lcdheight)
        vp.height = lcd->lcdheight;
    vp.x = (lcd->lcdwidth - vp.width) / 2;
    vp.y = (lcd->lcdheight - vp.height) / 2;

    dialog_frame_box(lcd, &vp, &style, &content);
    lcd->set_viewport(&content);
    lcd->set_drawmode(DRMODE_FG);

    /* No heading. The user chose this a moment ago and read a screen of prose
     * about it; a line saying what it is would be the only thing here that
     * tells them nothing. */
    y = 0;

    if (paused != NULL)
    {
        lcd->putsxy(0, y, "Paused");
        y += fh;
        lcd->putsxy(0, y, paused);
        y += fh;

        /* The third line stays empty: there is no rate while nothing is
         * being measured, and an estimate left over from before the pause
         * would say the run was still going. */
        y += fh;
    }
    else
    {
        snprintf(line, sizeof (line), "Analysing %d of %d",
                 ss_done + ss_skipped, ss_total);
        lcd->putsxy(0, y, line);
        y += fh;

        /* Clipped rather than wrapped -- the line below it is load bearing
         * and a two-line name would push it out of the box. */
        lcd->putsxy(0, y, ss_now);
        y += fh;

        /* Only once a track has finished: before that the estimate is made
         * of one sample and jumps about. */
        if (ss_done > 0 && ss_work > 0)
        {
            int left = ss_total - ss_done - ss_skipped;
            long eta = (ss_work / ss_done) * (left > 0 ? left : 0) / HZ;

            if (eta >= 3600)
                snprintf(line, sizeof (line), "About %ldh %ldm left",
                         eta / 3600, (eta % 3600) / 60);
            else
                snprintf(line, sizeof (line), "About %ldm left",
                         (eta + 59) / 60);

            lcd->putsxy(0, y, line);
        }
        else
        {
            lcd->putsxy(0, y, "Working out how long...");
        }

        y += fh;
    }

    y += fh / 4;

    /* The bar is the one thing readable at arm's length, and it keeps
     * running through a pause -- what has been done is still done. */
    w = content.width;
    bar = ss_total > 0 ? (ss_done + ss_skipped) * w / ss_total : 0;
    lcd->set_drawmode(DRMODE_SOLID);
    lcd->drawrect(0, y, w, fh - 2);
    lcd->fillrect(1, y + 1, bar > 2 ? bar - 2 : 0, fh - 4);

    /* One button, and it is the only thing this screen does. */
    dialog_draw_button(lcd, &style, content.width / 4,
                       content.height - bh, content.width / 2, bh,
                       "Stop", true);

    lcd->set_viewport(&vp);
    lcd->update_viewport();
    lcd->set_viewport(last_vp);
}


/** What the decoder polls **/

/* Called from inside the codec, often. The charger is checked here rather
 * than once a track: a slow track takes tens of seconds, and that is a long
 * time to keep running unpowered with the disk awake. */
static bool ss_abort(void)
{
    int button;

    if (!charger_inserted())
    {
        ss_unplugged = true;
        return true;
    }

    button = get_action(CONTEXT_STD, TIMEOUT_NOBLOCK);

    /* SELECT as well as the two back keys: the screen draws one button and
     * draws it selected, so pressing select has to be what it looks like. */
    if (button == ACTION_STD_OK || button == ACTION_STD_CANCEL ||
        button == ACTION_STD_MENU)
    {
        ss_stop = true;
        return true;
    }

    if (button == SYS_USB_CONNECTED)
    {
        ss_stop = true;
        return true;
    }

    return false;
}


/** One track **/

/* The file's length, for the staleness check. The decoder reports this too,
 * but the point of the check is to decide whether to decode at all. */
static uint32_t ss_file_size(const char *path)
{
    int fd = open(path, O_RDONLY);
    off_t n;

    if (fd < 0)
        return 0;

    n = filesize(fd);
    close(fd);

    return n > 0 ? (uint32_t)n : 0;
}

/* Measure 'path' and append it. False only if the run should end. */
static bool ss_measure(const char *path, uint32_t mtime, uint32_t genre_key,
                       int year, unsigned long length_ms, void *buf)
{
    struct track_sound s;
    struct sound_record rec;
    unsigned long start_ms, analysed = 0;
    uint64_t key = sound_index_key(path);
    uint32_t size = ss_file_size(path);
    long t0;
    int rc;

    if (sound_index_done(key, mtime, size))
    {
        ss_skipped++;
        return true;
    }

    start_ms = length_ms / 100 * SS_START_PCT;
    if (start_ms < SS_START_MIN_MS)
        start_ms = SS_START_MIN_MS;
    if (start_ms > SS_START_MAX_MS)
        start_ms = SS_START_MAX_MS;
    if (length_ms < start_ms + 5000)
        start_ms = 0;

    beat_probe_start();

    t0 = current_tick;
    /* Quick hands the decode a reason to stop early; Thorough gives it none
     * and takes the whole window. */
    rc = track_decode_run(path, start_ms, SS_WINDOW_MS, buf, SS_WINDOW_BYTES,
                          beat_probe_sink,
                          global_settings.analysis_depth == ANALYSIS_QUICK
                          ? beat_probe_settled : NULL,
                          ss_abort, &analysed);
    ss_work += current_tick - t0;

    if (ss_stop || ss_unplugged)
        return false;

    beat_probe_result(&s);
    sound_index_fill(&rec, key, mtime, size, genre_key, year, &s, rc);

    /* A file that will not decode still gets a record, so the next run does
     * not spend the same seconds discovering the same thing. */
    if (rc == TRACK_DECODE_FAILED || rc == TRACK_DECODE_NO_CODEC ||
        rc == TRACK_DECODE_NO_FILE)
    {
        ss_failed++;
    }

    if (analysed < SS_MIN_LENGTH_MS / 2)
        rec.flags |= SOUND_F_SHORT;

    ss_audio_ms += analysed;
    ss_done++;

    return sound_index_add(&rec);
}


/** The run **/

/* Wait for power, with the screen still up, rather than giving up on the run.
 *
 * Saying "Connect to charger" and then closing asks the user to go and find a
 * cable and afterwards start again from the menu -- and the message is gone
 * by the time they have. Waiting costs nothing and is the same answer to both
 * cases: no charger at the start, and a charger pulled out half way through.
 * The second is a pause, not an abandonment; the run has lost nothing but the
 * track it was in the middle of.
 *
 * False if the user stopped instead of plugging in. */
static bool ss_wait_for_charger(void)
{
    while (!charger_inserted())
    {
        int button;

        ss_draw("Connect the charger to continue");
        reset_poweroff_timer();

        button = get_action(CONTEXT_STD, HZ / 2);

        if (button == ACTION_STD_OK || button == ACTION_STD_CANCEL ||
            button == ACTION_STD_MENU)
        {
            ss_stop = true;
            return false;
        }

        if (default_event_handler(button) == SYS_USB_CONNECTED)
        {
            ss_stop = true;
            return false;
        }
    }

    ss_unplugged = false;

    return true;
}

/* Whether a finished index is already there to be thrown away. */
static bool ss_have_index(void)
{
    struct sound_index_reader r;

    if (sound_index_reader_open(&r) != SOUND_OK)
        return false;

    sound_index_reader_close(&r);

    return true;
}

static bool ss_gate(bool *fresh)
{
    int partial = 0;

    if (!tagcache_is_usable())
    {
        splash(HZ * 3, "Needs the database");
        return false;
    }

    /* A part-finished run outranks whichever row was chosen. Discarding it is
     * a decision made here, in front of the number, rather than a side-effect
     * of picking the rebuild row.
     *
     * yesno_pop() rather than yesno_pop_confirm(): the latter puts "Are you
     * sure?" on a line of its own above whatever it is given, which reads as
     * two questions when the thing below is already one. */
    if (sound_index_partial(&partial))
    {
        char q[64];

        snprintf(q, sizeof (q),
                 "An analysis is part finished (%d done). Carry on with it?",
                 partial);

        if (yesno_pop(q))
        {
            *fresh = false;
            return true;
        }

        if (!yesno_pop("Start again from the beginning?"))
            return false;

        *fresh = true;
        return true;
    }

    if (!dialog_prose_confirm("Analyse library", ss_explain,
                              "Start", "Not now"))
    {
        return false;
    }

    ss_ask_depth();

    /* Asked last, and only when it is a real question: there is a finished
     * index and the row picked was the one that throws it away. Measuring a
     * library again is a night, so the cheaper answer is offered rather than
     * just refused. */
    if (*fresh && ss_have_index())
    {
        if (yesno_pop("You already have an index. Rebuild it from scratch?"))
            return true;

        if (!yesno_pop("Update it instead?"))
            return false;

        *fresh = false;
    }

    return true;
}

bool sound_scan_screen(bool rebuild)
{
    struct tagcache_search tcs;
    char path[MAX_PATH];
    bool fresh = rebuild;
    int written;
    int rc;

    if (!ss_gate(&fresh))
        return false;

    audio_stop();

    /* Charge without mounting, for the length of the run. A cable found on a
     * desk is nearly always power, and a scan that gets a quarter of the way
     * through and then hands the disk to a laptop has lost the run for the
     * sake of a connection nobody asked for. Restored on the way out. */
    usb_set_mode(USB_MODE_CHARGE);

    /* Nothing else clears what the explanation dialog left behind: the run
     * draws a box, and a box leaves whatever was around it. */
    FOR_NB_SCREENS(rc)
    {
        screens[rc].clear_display();
        screens[rc].update();
    }

    ss_total = tagcache_get_stat()->total_entries;
    ss_done = ss_skipped = ss_failed = 0;
    ss_work = 0;
    ss_audio_ms = 0;
    ss_stop = ss_unplugged = false;
    strlcpy(ss_now, "", sizeof (ss_now));

    ss_handle = core_alloc(SS_WINDOW_BYTES);
    if (ss_handle <= 0)
    {
        splash(HZ * 3, "Not enough memory");
        return true;
    }

    rc = sound_index_begin(ss_total + 1, fresh);
    if (rc != SOUND_OK)
    {
        core_free(ss_handle);
        splash(HZ * 3, "Could not open the index");
        return true;
    }

    /* Asked for after the explanation, not before it: someone who does not
     * yet know what this is cannot be expected to have plugged in for it. The
     * screen is already up, so the request has somewhere to live and the run
     * starts the moment the cable goes in. */
    if (!ss_wait_for_charger())
    {
        sound_index_close();
        core_free(ss_handle);
        usb_set_mode(global_settings.usb_mode);
        return true;
    }

    ss_draw(NULL);

    if (!tagcache_search(&tcs, tag_filename))
    {
        sound_index_close();
        core_free(ss_handle);
        splash(HZ * 3, "Database busy");
        return true;
    }

    while (tagcache_get_next(&tcs, path, sizeof (path)))
    {
        char genre[48];
        unsigned long length_ms;
        uint32_t mtime;
        int year;

        /* No button is pressed for hours, so idle poweroff would otherwise
         * fire in the middle of the run. */
        reset_poweroff_timer();

        length_ms = (unsigned long)tagcache_get_numeric(&tcs, tag_length);
        mtime = (uint32_t)tagcache_get_numeric(&tcs, tag_mtime);
        year = (int)tagcache_get_numeric(&tcs, tag_year);

        genre[0] = '\0';
        tagcache_retrieve(&tcs, tcs.idx_id, tag_genre, genre, sizeof (genre));

        strlcpy(ss_now, strrchr(path, '/') ? strrchr(path, '/') + 1 : path,
                sizeof (ss_now));

        if (length_ms < SS_MIN_LENGTH_MS)
        {
            ss_skipped++;
        }
        else
        {
            /* Retried rather than skipped when the charger goes: the track
             * was abandoned mid-decode and measured nothing, and the walk has
             * already moved past it. Waiting and measuring it again is the
             * only way it gets done in this run. */
            while (!ss_measure(path, mtime, sound_index_genre_key(genre),
                               year, length_ms, core_get_data(ss_handle)))
            {
                if (ss_stop || !ss_unplugged)
                    break;

                if (!ss_wait_for_charger())
                    break;
            }
        }

        if (ss_stop)
            break;

        /* Once a track, not on a timer: at seconds a track there is nothing
         * to animate, and the backlight is off for most of the run anyway. */
        ss_draw(NULL);
    }

    tagcache_search_finish(&tcs);
    core_free(ss_handle);
    usb_set_mode(global_settings.usb_mode);

    /* Before either of the calls below: closing and finishing both drop the
     * index's table, and the count goes with it. */
    written = sound_index_count();

    /* Losing the charger is no longer an ending -- the run waits for it and
     * carries on -- so there are two: the user stopped, or it finished. */
    if (ss_stop)
    {
        sound_index_close();
        splashf(HZ * 4, "Stopped. %d of %d done", written, ss_total);
    }
    else if (sound_index_finish() == SOUND_OK)
    {
        splashf(HZ * 4, "Done. %d measured, %d unreadable",
                written, ss_failed);
    }
    else
    {
        sound_index_close();
        splash(HZ * 4, "Could not write the index");
    }

    return true;
}
