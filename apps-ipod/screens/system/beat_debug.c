/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Shows what beat_analyse.c is making of the music, so the detector can
 * be judged against real tracks before any game is built on it.
 *
 * How to read it. The dashed vertical line is **now** -- the audio leaving
 * the DAC this instant. Everything right of it has been analysed but not yet
 * heard, and slides left; everything left of it has already played. A tick
 * above a column is an onset, so the question the screen exists to answer is
 * whether a tick crosses the playhead at the moment you hear that hit.
 *
 * Strips are ordered as a spectrogram is, treble at the top and bass at the
 * bottom, and each says what it listens to. Bass is the one to watch first:
 * it should mark the kick drum and little else.
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include "config.h"
#include "lcd.h"
#include "font.h"
#include "kernel.h"
#include "input/action.h"
#include "system/shutdown.h"   /* default_event_handler */
#include "audio/beat_analyse.h"
#include "audio/beat_track.h"
#include "screens/system/beat_debug.h"

/* Where now sits: the trace arrives from the right and is met here. */
#define PLAYHEAD_X      48

/* Room above each strip for the onset ticks, and between strips. */
#define TICK_HEIGHT     5
#define STRIP_GAP       4

/* Top to bottom, treble first. The order is the display's, not the
 * analyser's, so this table is the only place that knows about it. */
static const struct
{
    int         group;
    const char *label;
} strips[BEAT_GROUPS] =
{
    { BEAT_HIGH, "HI  hat/cym" },
    { BEAT_MID,  "MID voc/snr" },
    { BEAT_LOW,  "LOW kick   " },
};

/* Audio time to screen x. Left of the playhead is the recent past, which is
 * what makes the moment of arrival watchable rather than something that
 * happens at the edge of the screen and is gone. */
static int time_to_x(uint32_t time_ms, unsigned long play_ms)
{
    long dt = (long)time_ms - (long)play_ms;

    return PLAYHEAD_X
           + (int)((dt * (LCD_WIDTH - PLAYHEAD_X)) / BEAT_VIEW_MS);
}

/* Audio either side of now that counts as "being heard", for the firing
 * indicator. Wide enough to survive a 20Hz redraw -- a 50ms frame must not
 * step over the onset it is meant to show. */
#define NOW_WINDOW_MS   40

static void draw_strip(int group, int top, int height, unsigned long play_ms)
{
    int base = top + height - 1;
    int span = height - TICK_HEIGHT - 1;
    bool firing = false;
    int back;

    /* Columns are placed by the audio time they cover, not by the order they
     * arrived. The codec commits in bursts, so hops arrive in bursts too;
     * drawing newest-at-the-right would make the trace lurch and would put
     * now wherever the buffer happened to be. */
    for (back = 0; back < BEAT_HISTORY; back++)
    {
        const struct beat_window *w = beat_analyse_window(back);
        int level, x;

        if (w == NULL)
            break;

        x = time_to_x(w->time_ms, play_ms);

        if (x >= LCD_WIDTH)
            continue;   /* Further ahead than the trace shows */
        if (x < 0)
            break;      /* Off the left, and everything older is too */

        level = w->level[group] * span / 100;
        if (level > 0)
            lcd_vline(x, base - level, base);

        if (w->onset & (1 << group))
        {
            long dt = (long)w->time_ms - (long)play_ms;

            lcd_vline(x, top, top + TICK_HEIGHT - 2);

            if (dt <= NOW_WINDOW_MS && dt >= -NOW_WINDOW_MS)
                firing = true;
        }
    }

    /* The playhead goes solid across this strip while the group is firing
     * there. Something that thumps on the beat in one fixed place is what an
     * ear can check; a tick moving through a scrolling trace is not. */
    if (firing)
        lcd_fillrect(PLAYHEAD_X - 1, top, 3, height - 1);

    lcd_hline(0, LCD_WIDTH - 1, base + 1);
}

/* The projected beat grid, drawn through every strip so it can be compared
 * against the onset ticks. Dotted at a different pitch from the playhead so
 * the two do not read as the same kind of mark. */
static void draw_beat_grid(unsigned long play_ms, int top)
{
    unsigned long from = play_ms > 400 ? play_ms - 400 : 0;
    int n;

    for (n = 0; n < 16; n++)
    {
        unsigned long beat_ms;
        int in_bar, x, y;

        if (!beat_track_next(from, n, &beat_ms, &in_bar))
            return;

        x = time_to_x((uint32_t)beat_ms, play_ms);
        if (x < 0)
            continue;
        if (x >= LCD_WIDTH)
            return;

        /* The first beat of each group of four gets a solid line, so the
         * grouping is visible without counting dots. */
        if (in_bar == 0)
            lcd_vline(x, top, LCD_HEIGHT - 1);
        else
            for (y = top; y < LCD_HEIGHT; y += 6)
                lcd_vline(x, y, y + 1);
    }
}

/* Four boxes, the current beat of the bar filled. Easier to read at a glance
 * than a digit, and it is the count the ear is being asked to check.
 *
 * Counted at the playhead, not at the analyser: the box has to change when
 * the beat is *heard*, or it leads the music by the whole look-ahead. */
static void draw_bar_count(const struct beat_track *beat,
                           unsigned long play_ms, int y, int height)
{
    int in_bar = -1, i;

    /* Signed, for the same reason beat_track_next() is: the anchor sits a
     * look-ahead ahead of the playhead, so this difference is normally
     * negative and an unsigned test never fires. */
    if (beat->locked && beat->period_ms > 0)
    {
        long d = (long)play_ms - (long)beat->last_ms;
        long idx = d / (long)beat->period_ms;

        if (idx * (long)beat->period_ms > d)
            idx--;

        in_bar = (int)(((idx % 4) + 4) % 4);
    }

    for (i = 0; i < 4; i++)
    {
        int x = LCD_WIDTH - 4 - (4 - i) * (height + 3);

        if (i == in_bar)
            lcd_fillrect(x, y, height, height);
        else
            lcd_drawrect(x, y, height, height);
    }
}

static void draw_screen(void)
{
    struct beat_status st;
    struct beat_track beat;
    int th, top, height, i, y;

    beat_analyse_status(&st);
    beat_track_get(&beat);

    lcd_clear_display();

    th = font_get(FONT_SYSFIXED)->height;

    lcd_putsf(0, 0, "la%4u lead%4u sr%5u rs%u",
              st.lookahead_ms, st.lead_ms, st.samplerate, st.reseeds);

    if (beat.locked)
        lcd_putsf(0, 1, "BPM %3u  conf %2u", beat.bpm, beat.confidence);
    else
        lcd_putsf(0, 1, "BPM  --  conf %2u", beat.confidence);

    draw_bar_count(&beat, st.play_ms, th + 1, th - 2);

    for (i = 0; i < BEAT_GROUPS; i++)
    {
        int g = strips[i].group;

        lcd_putsf(0, i + 2, "%s f%6d t%6d %2u.%u/s",
                  strips[i].label, st.flux[g], st.threshold[g],
                  st.rate10[g] / 10, st.rate10[g] % 10);
    }

    top = (BEAT_GROUPS + 2) * th + STRIP_GAP;
    height = (LCD_HEIGHT - top - (BEAT_GROUPS - 1) * STRIP_GAP)
             / BEAT_GROUPS;

    if (beat.locked)
        draw_beat_grid(st.play_ms, top);

    for (i = 0; i < BEAT_GROUPS; i++)
    {
        draw_strip(strips[i].group, top + i * (height + STRIP_GAP), height,
                   st.play_ms);
    }

    /* The playhead last, so it reads over the traces rather than under them,
     * and dashed so it does not hide the column it marks. */
    for (y = top; y < LCD_HEIGHT; y += 4)
        lcd_vline(PLAYHEAD_X, y, y + 1);

    lcd_update();
}

bool beat_debug_screen(void)
{
    bool to_root = false;

    beat_analyse_start();
    lcd_setfont(FONT_SYSFIXED);

    while (1)
    {
        /* The timeout is what paces both the redraw and the analyser: a
         * poll covers up to ~370ms of audio, so at 20Hz the cursor keeps up
         * with the codec even when a redraw runs long. */
        int button = get_action(CONTEXT_STD, HZ / 20);

        if (button == ACTION_STD_CANCEL)
            break;

        if (button == ACTION_STD_MENU)
        {
            to_root = true;
            break;
        }

        default_event_handler(button);

        beat_analyse_poll();
        draw_screen();
    }

    beat_analyse_stop();
    lcd_setfont(FONT_UI);

    return to_root;
}
