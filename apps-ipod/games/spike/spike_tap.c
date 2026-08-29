/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Tap the beat you hear, against what the tracker made of the same music.
 *
 * The screen exists because the game cannot tell two failures apart. A grid
 * that is out of time is either cut at the wrong tempo, or cut at the right
 * one and started in the wrong place, and the fixes have nothing in common.
 * Tapping separates them: the ratio answers the first and the phase answers
 * the second.
 *
 * Both are measured against the same clock the game runs on and through the
 * same button, so what is read here is what the game would have felt.
 *
 * What the numbers mean:
 *
 *   ratio   the game's beat over yours. Near 100% and the tempo is right.
 *           Near 50 or 200 and it is an octave out, which is the one error
 *           a listener spots instantly and no tracker reliably catches --
 *           the game's pause menu has a half/double toggle for exactly it.
 *   phase   where your taps sit relative to the tracker's beats. A tight
 *           spread with the mean away from zero is a right tempo started in
 *           the wrong place, and the mean is the number to put into the
 *           audio offset. It carries your own reaction time with it, which
 *           is intended: that is what the offset is nulling.
 *   spread  how far the taps scatter. Wide means the tempo is wrong, however
 *           good the ratio looks -- a wrong period walks the phase around.
 *   drift   the phase error of the first tap against the last. Steady drift
 *           with a tight spread is a tempo close but not exact.
 *
 * Parts, in order:
 *   - the taps
 *   - the statistics
 *   - the screen
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include "config.h"
#include "system.h"           /* TIME_AFTER, TIME_BEFORE */
#include "lcd.h"
#include "font.h"
#include "kernel.h"
#include "audio.h"            /* audio_status */
#include "audio/beat_analyse.h"
#include "audio/beat_track.h"
#include "input/action.h"
#include "system/shutdown.h"  /* default_event_handler */
#include "draw/viewport.h"    /* viewportmanager_theme_enable */
#include "widgets/splash.h"
#include "games/spike/spike_clock.h"
#include "games/spike/spike_tap.h"

/* Taps kept. Enough for the drift over a couple of bars to show, few enough
 * that the median below stays a handful of comparisons. */
#define BT_TAPS         32

#define BT_FRAME_TICKS  (HZ / 25)

/* The scatter plot: half its width is half a beat either way. */
#define BT_PLOT_X       20
#define BT_PLOT_W       280
#define BT_PLOT_Y       150
#define BT_PLOT_H       40

static unsigned long tap_ms[BT_TAPS];
static short         tap_err[BT_TAPS];  /* phase against the tracker's beat */
static int           taps;              /* taps taken, saturating at BT_TAPS */
static int           tap_head;          /* next slot, so the ring is oldest-first */


/** The taps **/

static void bt_clear(void)
{
    taps = 0;
    tap_head = 0;
}

/* Fold a difference into plus or minus half a period, which is what makes a
 * tap on the beat read as zero however many beats have gone by. */
static int bt_fold(long diff, int period)
{
    long r;

    if (period <= 0)
        return 0;

    r = diff % period;
    if (r < 0)
        r += period;
    if (r > period / 2)
        r -= period;

    return (int)r;
}

static void bt_take(unsigned long at)
{
    struct beat_track bt;
    unsigned long ref;
    int in_bar, err = 0;

    beat_track_get(&bt);

    /* Folded against the tracker's own period rather than the game's: this
     * is asking where the tracker thinks the beat is, and the game's grid is
     * an exact ratio of it either way. */
    if (bt.locked && bt.period_ms > 0
        && beat_track_next(at, 0, &ref, &in_bar))
        err = bt_fold((long)at - (long)ref, (int)bt.period_ms);

    tap_ms[tap_head] = at;
    tap_err[tap_head] = (short)err;
    tap_head = (tap_head + 1) % BT_TAPS;

    if (taps < BT_TAPS)
        taps++;
}

/* Oldest first, so the drift below reads in the order they were tapped. */
static int bt_index(int i)
{
    if (taps < BT_TAPS)
        return i;

    return (tap_head + i) % BT_TAPS;
}


/** The statistics **/

/* Median rather than mean, because one missed tap doubles an interval and
 * one nervous double-tap halves it -- and either would drag a mean far
 * enough to change the octave the ratio reports. */
static int bt_period(void)
{
    int gap[BT_TAPS];
    int n = 0, i, j;

    for (i = 1; i < taps; i++)
    {
        long d = (long)tap_ms[bt_index(i)] - (long)tap_ms[bt_index(i - 1)];

        if (d > 100 && d < 3000)
            gap[n++] = (int)d;
    }

    if (n == 0)
        return 0;

    for (i = 1; i < n; i++)
    {
        int v = gap[i];

        for (j = i - 1; j >= 0 && gap[j] > v; j--)
            gap[j + 1] = gap[j];
        gap[j + 1] = v;
    }

    return gap[n / 2];
}

static void bt_phase(int *mean, int *spread, int *drift)
{
    long sum = 0;
    int lo = 0, hi = 0, i;

    *mean = *spread = *drift = 0;

    if (taps < 2)
        return;

    for (i = 0; i < taps; i++)
    {
        int e = tap_err[bt_index(i)];

        sum += e;
        if (i == 0 || e < lo)
            lo = e;
        if (i == 0 || e > hi)
            hi = e;
    }

    *mean = (int)(sum / taps);
    *spread = hi - lo;
    *drift = tap_err[bt_index(taps - 1)] - tap_err[bt_index(0)];
}

/* The relation, stated without deciding who is wrong: tapping halves or
 * eighths is as likely as the tracker choosing the wrong level, and the two
 * look identical from here. */
static const char *bt_verdict(int pct, bool locked)
{
    if (!locked)
        return "no lock";
    if (pct == 0)
        return "keep tapping";
    if (pct >= 92 && pct <= 108)
        return "matches";
    if (pct >= 190 && pct <= 210)
        return "1 game beat = 2 taps";
    if (pct >= 45 && pct <= 55)
        return "2 game beats = 1 tap";
    if (pct >= 380 && pct <= 420)
        return "1 game beat = 4 taps";
    if (pct >= 22 && pct <= 28)
        return "4 game beats = 1 tap";
    if (pct >= 140 && pct <= 160)
        return "3 against 2";
    if (pct >= 62 && pct <= 72)
        return "2 against 3";

    return "unrelated";
}


/** The screen **/

static void bt_plot(int period)
{
    int i;

    lcd_drawrect(BT_PLOT_X, BT_PLOT_Y, BT_PLOT_W, BT_PLOT_H);

    /* The centre is the tracker's beat; the edges are half a beat either
     * side. Taps clustered anywhere on this line are a tempo that fits;
     * taps spread across it are one that does not. */
    lcd_vline(BT_PLOT_X + BT_PLOT_W / 2, BT_PLOT_Y - 4,
              BT_PLOT_Y + BT_PLOT_H + 3);

    if (period < 4)
        return;

    for (i = 0; i < taps; i++)
    {
        int e = tap_err[bt_index(i)];
        int x = BT_PLOT_X + BT_PLOT_W / 2
                + (e * (BT_PLOT_W / 2)) / (period / 2);

        if (x < BT_PLOT_X + 1)
            x = BT_PLOT_X + 1;
        else if (x > BT_PLOT_X + BT_PLOT_W - 2)
            x = BT_PLOT_X + BT_PLOT_W - 2;

        /* Newest tallest, so the drift shows as a lean. */
        lcd_vline(x, BT_PLOT_Y + BT_PLOT_H - 2 - (i * (BT_PLOT_H - 4)) / taps,
                  BT_PLOT_Y + BT_PLOT_H - 2);
    }
}

static void bt_draw(bool pulse)
{
    struct beat_track bt;
    char line[64];
    int mine = bt_period();
    int game, pct = 0, mean, spread, drift;

    beat_track_get(&bt);
    game = bt.locked ? spk_octave(bt.period_ms) : 0;

    if (game > 0 && mine > 0)
        pct = (game * 100) / mine;

    bt_phase(&mean, &spread, &drift);

    lcd_clear_display();

    lcd_putsxy(2, 0, (const unsigned char *)
               "Beat tap   SELECT taps   PLAY resets");
    lcd_hline(0, LCD_WIDTH - 1, 12);

    snprintf(line, sizeof (line), "tracker  %3u BPM  %4u ms  conf %2u %s",
             bt.bpm, bt.period_ms, bt.confidence,
             bt.locked ? "LOCK" : "----");
    lcd_putsxy(2, 20, (const unsigned char *)line);

    if (game > 0)
        snprintf(line, sizeof (line), "game     %3d BPM  %4d ms",
                 60000 / game, game);
    else
        snprintf(line, sizeof (line), "game       no lock -- 500 ms");
    lcd_putsxy(2, 32, (const unsigned char *)line);

    if (mine > 0)
        snprintf(line, sizeof (line), "you      %3d BPM  %4d ms  %d taps",
                 60000 / mine, mine, taps);
    else
        snprintf(line, sizeof (line), "you        %d taps", taps);
    lcd_putsxy(2, 44, (const unsigned char *)line);

    snprintf(line, sizeof (line), "ratio    %3d%%   %s",
             pct, bt_verdict(pct, bt.locked));
    lcd_putsxy(2, 62, (const unsigned char *)line);

    snprintf(line, sizeof (line), "phase   %+4d ms  spread %3d ms",
             mean, spread);
    lcd_putsxy(2, 74, (const unsigned char *)line);

    snprintf(line, sizeof (line), "drift   %+4d ms over %d taps",
             drift, taps);
    lcd_putsxy(2, 86, (const unsigned char *)line);

    /* The tracker's beat, so the numbers can be checked against an ear:
     * a block that flashes with the music is a lock worth trusting. */
    if (pulse)
        lcd_fillrect(LCD_WIDTH / 2 - 24, 108, 48, 28);
    else
        lcd_drawrect(LCD_WIDTH / 2 - 24, 108, 48, 28);

    bt_plot(bt.period_ms > 0 ? (int)bt.period_ms : 0);

    lcd_putsxy(2, BT_PLOT_Y + BT_PLOT_H + 8, (const unsigned char *)
               "early                beat                 late");

    /* A tight cluster off centre is a fixed offset and the phase figure is
     * the correction; a smear is a tempo that does not fit, whatever the
     * ratio says. */
    if (spread > (int)bt.period_ms / 3)
        lcd_putsxy(2, BT_PLOT_Y + BT_PLOT_H + 20, (const unsigned char *)
                   "scattered -- tempo does not fit these taps");
    else if (taps > 3)
        lcd_putsxy(2, BT_PLOT_Y + BT_PLOT_H + 20, (const unsigned char *)
                   "clustered -- phase above is the offset to dial in");

    lcd_update();
}

bool spike_tap_screen(void)
{
    struct viewport vp;
    long next_frame;
    unsigned long pulse_at = 0;
    bool pulse = false;

    if (!(audio_status() & AUDIO_STATUS_PLAY))
    {
        splash(HZ * 2, "Play a track first");
        return false;
    }

    viewportmanager_theme_enable(SCREEN_MAIN, false, &vp);
    lcd_set_backdrop(NULL);
    lcd_setfont(FONT_SYSFIXED);
    lcd_set_foreground(LCD_BLACK);
    lcd_set_background(LCD_WHITE);

    bt_clear();
    spk_clock_reset();
    beat_analyse_start();
    next_frame = current_tick + BT_FRAME_TICKS;

    while (1)
    {
        long wait = next_frame - current_tick;
        struct beat_track bt;
        unsigned long ref, now;
        int in_bar, button;

        if (wait < 1)
            wait = 1;

        button = get_action(CONTEXT_SPIKE, wait);

        if (button == ACTION_SPIKE_EXIT)
            break;

        /* Timestamped against the clock rather than against the frame, and
         * through the same action the game's jump uses -- so the numbers
         * here describe the same path a real press takes. */
        if (button == ACTION_SPIKE_JUMP)
            bt_take(spk_clock_now());
        else if (button == ACTION_SPIKE_PAUSE)
            bt_clear();

        default_event_handler(button);

        if (TIME_BEFORE(current_tick, next_frame))
            continue;

        next_frame += BT_FRAME_TICKS;
        if (TIME_BEFORE(next_frame, current_tick))
            next_frame = current_tick;

        if (!spk_clock_tick())
        {
            /* Playback moved: the taps were measured against audio that is
             * no longer playing, so they say nothing about this. */
            bt_clear();
            beat_analyse_start();
        }

        beat_analyse_poll();

        now = spk_clock_ms();
        beat_track_get(&bt);

        /* Lit for the head of each projected beat. Beats are projected
         * rather than waited for, so this marks one that has not been heard
         * yet as readily as one that has. */
        if (bt.locked && bt.period_ms > 0 && now > bt.period_ms
            && beat_track_next(now - bt.period_ms, 0, &ref, &in_bar))
        {
            if (ref != pulse_at)
            {
                pulse_at = ref;
                pulse = true;
            }
            else if (now > ref + 90)
                pulse = false;
        }
        else
            pulse = false;

        bt_draw(pulse);
    }

    beat_analyse_stop();
    lcd_setfont(FONT_UI);
    viewportmanager_theme_undo(SCREEN_MAIN, true);

    return false;
}
