/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * The year card, its week bar, and the drill-down behind them.
 *
 * A year is shown as its Monday-to-Sunday weeks, one pill each, coloured by
 * how much listening each holds. Past a full day of music in one week the
 * pill changes metal -- gold, ice, violet, ember, white as the hours climb --
 * which is the whole point of the card: a year of listening has a shape, and
 * the weeks that got away from you are visible in it.
 *
 * Weeks are derived from the day array rather than binned during the parse.
 * A week's listening is the sum of its days', so it costs one pass over a few
 * hundred days instead of a bin per entry, and nothing upstream has to know
 * that weeks exist.
 *
 * The per-week recap is the exception, and the expensive one. "Top artist in
 * week 12" cannot be answered from period aggregates at all, so it re-reads
 * the log filtered to those seven days. That is a whole-family read per week
 * opened; the byte-offset-per-day index in the spec removes it, and until
 * then it is why this screen boosts the CPU and puts a splash up.
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <string-extra.h>
#include "config.h"
#include "kernel.h"
#include "system.h"          /* cpu_boost */
#include "button.h"
#include "lcd.h"
#include "font.h"
#include "timefuncs.h"
#include "pv_paint.h"
#include "pv_log.h"
#include "pv_names.h"
#include "pv_stats.h"
#include "pv_cards.h"
#include "pv_year.h"

#define PV_W LCD_WIDTH
#define PV_H LCD_HEIGHT

/* A year touches at most 54 Monday-aligned weeks, counting the partial ones
 * at each end. */
#define PV_N_WEEKS 54

/* A week turns from "quiet" to "active" here; everything above is a tier. */
#define WEEK_ACTIVE_SECS (30 * 60)

/* The ladder, in seconds. Index 0 is quiet; the rest are the named tiers. */
static const unsigned tier_secs[] =
{
    0,
    1440u * 60,      /* SUPERWEEK: a full day of music in seven */
    2880u * 60,      /* ULTRAWEEK */
    4320u * 60,      /* HYPERWEEK */
    5760u * 60,      /* GIGAWEEK  */
    7200u * 60       /* OMEGAWEEK */
};
#define PV_TIERS ((int)(sizeof(tier_secs) / sizeof(tier_secs[0])))

static const unsigned tier_col[PV_TIERS] =
{
    0,
    LCD_RGBPACK(255, 185,  50),   /* gold   */
    LCD_RGBPACK(120, 220, 255),   /* ice    */
    LCD_RGBPACK(200, 135, 255),   /* violet */
    LCD_RGBPACK(255, 120,  85),   /* ember  */
    LCD_RGBPACK(255, 255, 255)    /* white  */
};

static const unsigned tier_hi[PV_TIERS] =
{
    0,
    LCD_RGBPACK(255, 240, 170),
    LCD_RGBPACK(235, 252, 255),
    LCD_RGBPACK(240, 225, 255),
    LCD_RGBPACK(255, 220, 195),
    LCD_RGBPACK(190, 245, 255)
};

/* Week bar geometry. */
#define WB_PITCH 5
#define WB_SEGW  4
#define WB_H     12
#define WB_Y     137
#define WB_X     ((PV_W - (n_weeks * WB_PITCH - (WB_PITCH - WB_SEGW))) / 2)

/* The two lines under the bar. The card writes its year summary here and the
 * browser writes the selected week's over the top, so both use these and the
 * band below covers exactly the pair. */
#define PV_YEAR_L0   162
#define PV_YEAR_L1   180
#define PV_YEAR_HINT 208
#define PV_YEAR_LY   (PV_YEAR_L0 - 4)
#define PV_YEAR_LH   (PV_YEAR_HINT + 18 - PV_YEAR_LY)

static int      display_year;
static long     display_day0;     /* unix day of the Monday on/before Jan 1 */
static int      n_weeks;
static unsigned week_secs[PV_N_WEEKS];

static int week_tier(unsigned secs)
{
    int t = PV_TIERS - 1;

    while (t > 0 && secs < tier_secs[t])
        t--;
    return t;
}

/* Work out which year to show and bin its weeks.
 *
 * The year is whichever one the most recent listening falls in -- with no
 * picker, that is the only answer that does not need asking about, and for
 * anyone still listening it is the current year. */
static void year_init(const struct pv_totals *t)
{
    const struct pv_day *days;
    int day_n;

    memset(week_secs, 0, sizeof(week_secs));
    display_year = 0;
    display_day0 = 0x7fffffffL;   /* matches no day */
    n_weeks = 52;

    if (!t->ts_max)
        return;

    pv_civil_from_days((long)(t->ts_max / 86400), &display_year, NULL, NULL);

    /* Weeks are real Monday-to-Sunday ones, so the bar starts on the Monday
     * on or before Jan 1 and the first and last pills may be partial. Unix
     * day 4 was Monday 1970-01-05. */
    {
        long jan1 = pv_days_from_civil(display_year, 1, 1);

        display_day0 = jan1 - (((jan1 - 4) % 7) + 7) % 7;
        n_weeks = (int)((pv_days_from_civil(display_year + 1, 1, 1)
                         - display_day0 + 6) / 7);
        if (n_weeks > PV_N_WEEKS)
            n_weeks = PV_N_WEEKS;
    }

    days = pv_stats_days(&day_n);
    for (int i = 0; i < day_n; i++)
    {
        long rel = days[i].day - display_day0;

        if (rel < 0 || rel >= 7L * n_weeks)
            continue;
        week_secs[rel / 7] += days[i].secs;
    }
}

bool pv_year_available(void)
{
    return display_year != 0;
}

/* Days of the displayed year that have happened.
 *
 * Rockbox's tm_yday comes from a month*30 approximation that runs up to two
 * days fast, so the day of the year is derived from the civil date instead. */
static int year_elapsed_days(void)
{
    struct tm *now = get_time();
    int nowy = now ? now->tm_year + 1900 : 0;
    long ylen = pv_days_from_civil(display_year + 1, 1, 1)
              - pv_days_from_civil(display_year, 1, 1);

    if (nowy > display_year)
        return (int)ylen;
    if (nowy < display_year)
        return 0;
    return (int)(pv_days_from_civil(nowy, now->tm_mon + 1, now->tm_mday)
                 - pv_days_from_civil(nowy, 1, 1)) + 1;
}

/* Which pill "now" falls in, so the bar can dim the weeks still to come. */
static int year_cur_week(void)
{
    int doy, off, w;

    if (!display_year)
        return -1;

    doy = year_elapsed_days();
    if (doy <= 0)
        return -1;

    off = (int)(pv_days_from_civil(display_year, 1, 1) - display_day0);
    w = (off + doy - 1) / 7;
    if (w < 0)
        return -1;
    return (w >= n_weeks) ? n_weeks - 1 : w;
}

/* The bar. A tiered week gets a pill in its metal with a sparkle above it,
 * an active one gets the accent, and a quiet or future one gets a wash --
 * lighter for weeks already passed than for weeks not yet reached. */
static void draw_week_bar(const struct pv_theme *th, int cursor)
{
    int cur_week = year_cur_week();

    pv_band(th, WB_Y - 8, WB_H + 16);
    lcd_set_drawmode(DRMODE_SOLID);

    for (int i = 0; i < n_weeks; i++)
    {
        int x = WB_X + i * WB_PITCH;
        int t = week_tier(week_secs[i]);
        int cx = x + WB_SEGW / 2;

        if (t >= 2)
        {
            pv_capsule(th, cx, WB_Y - 3, cx, WB_Y + WB_H + 2, 2, tier_col[t]);
            pv_capsule(th, cx, WB_Y, cx, WB_Y + WB_H - 1, 1, tier_hi[t]);
            lcd_set_drawmode(DRMODE_SOLID);
            lcd_set_foreground(LCD_RGBPACK(255, 255, 255));
            lcd_drawpixel(cx + 2, WB_Y - 5);
            lcd_drawpixel(cx + 1, WB_Y - 6);
            lcd_drawpixel(cx + 3, WB_Y - 6);
            lcd_drawpixel(cx + 2, WB_Y - 7);
            if (t >= 3)
            {
                lcd_drawpixel(cx - 2, WB_Y + WB_H + 3);
                lcd_drawpixel(cx - 3, WB_Y + WB_H + 4);
                lcd_drawpixel(cx - 1, WB_Y + WB_H + 4);
                lcd_drawpixel(cx - 2, WB_Y + WB_H + 5);
            }
        }
        else if (t == 1)
        {
            pv_capsule(th, cx, WB_Y - 2, cx, WB_Y + WB_H + 1, 2, tier_col[1]);
            pv_capsule(th, cx, WB_Y + 1, cx, WB_Y + WB_H - 2, 1, tier_hi[1]);
            lcd_set_drawmode(DRMODE_SOLID);
        }
        else
        {
            unsigned c;

            if (week_secs[i] >= WEEK_ACTIVE_SECS)
                c = th->accent;
            else if (cur_week >= 0 && i <= cur_week)
                c = pv_blend(pv_grad_at(th, WB_Y),
                             LCD_RGBPACK(255, 255, 255), 60);
            else
                c = pv_blend(pv_grad_at(th, WB_Y),
                             LCD_RGBPACK(255, 255, 255), 22);
            lcd_set_foreground(c);
            lcd_fillrect(x, WB_Y, WB_SEGW, WB_H);
        }

        if (i == cursor)
        {
            lcd_set_foreground(LCD_RGBPACK(255, 255, 255));
            lcd_drawrect(x - 2, WB_Y - 5, WB_SEGW + 4, WB_H + 10);
        }
    }
}

/* "Jun 22 - Jun 28", clamped to the year at either end. */
static void week_range(int wi, char *out, size_t sz)
{
    int y1, m1, d1, y2, m2, d2;
    long s = display_day0 + (long)wi * 7;
    long e = s + 6;
    long jan1 = pv_days_from_civil(display_year, 1, 1);
    long last = pv_days_from_civil(display_year + 1, 1, 1) - 1;

    if (s < jan1)
        s = jan1;
    if (e > last)
        e = last;

    pv_civil_from_days(s, &y1, &m1, &d1);
    pv_civil_from_days(e, &y2, &m2, &d2);
    snprintf(out, sz, "%s %d - %s %d",
             pv_month_abbr[m1], d1, pv_month_abbr[m2], d2);
}

/* ------------------------------------------------------ one week's recap */

/* Distinct names seen in a week. A week holds a few hundred plays and rather
 * fewer distinct artists, so a linear table is both small enough to keep and
 * quick enough to search. */
#define WK_SLOTS 48

struct wk_named
{
    char name[PV_NAME_MAX];
    int  c;
};

struct week_info
{
    long plays, skips, mins;
    char top_artist[PV_NAME_MAX], top_track[PV_NAME_MAX];
    int  top_artist_n, top_track_n;
};

static struct wk_named wk_art[WK_SLOTS], wk_trk[WK_SLOTS];
static int wk_art_n, wk_trk_n;

/* What the scan below is accumulating, and the window it accepts. */
struct week_scan_ctx
{
    struct week_info *w;
    long  d_lo, d_hi;
    unsigned long secs;
};

static void wk_add(struct wk_named *arr, int *n, const char *name)
{
    for (int i = 0; i < *n; i++)
    {
        if (strcmp(arr[i].name, name) == 0)
        {
            arr[i].c++;
            return;
        }
    }
    if (*n < WK_SLOTS)
    {
        strlcpy(arr[*n].name, name, PV_NAME_MAX);
        arr[*n].c = 1;
        (*n)++;
    }
}

static void week_entry(const struct pv_entry *e, void *ctx)
{
    struct week_scan_ctx *c = ctx;
    char artist[PV_NAME_MAX], title[PV_NAME_MAX], album[PV_NAME_MAX];
    long d;

    if (!e->valid_ts)
        return;

    d = (long)(e->ts / 86400UL);
    if (d < c->d_lo || d >= c->d_hi)
        return;

    /* A browsing tap is no more an opinion here than anywhere else. */
    if (!e->listened && !e->skipped)
        return;

    if (!e->listened)
    {
        c->w->skips++;
        return;
    }

    c->w->plays++;
    c->secs += e->elapsed_ms / 1000;

    /* Names are resolved only for entries inside the week, which is what
     * keeps this affordable: the read is the whole log, the naming is one
     * week of it. */
    if (e->artist)
    {
        wk_add(wk_art, &wk_art_n, e->artist);
        wk_add(wk_trk, &wk_trk_n, e->title);
    }
    else
    {
        pv_names_resolve(e->path, artist, title, album);
        wk_add(wk_art, &wk_art_n, artist);
        wk_add(wk_trk, &wk_trk_n, title);
    }
}

/* The byte range of the log covering days [d_lo, d_hi).
 *
 * The day array carries the offset of each day's first entry, so the range is
 * "where the first day in the window starts" to "where the first day after it
 * starts" -- turning a megabyte read into about fifteen kilobytes.
 *
 * This assumes a day's entries sit together in the log, which append-only
 * writing guarantees for everything except a clock being moved backwards
 * mid-history. A day split that way loses its later half from this one
 * week's recap; nothing else, and no total, is affected. */
static bool week_byte_range(long d_lo, long d_hi,
                            unsigned long *from, unsigned long *to)
{
    const struct pv_day *days;
    int day_n;
    bool found = false;

    *from = 0;
    *to = 0;

    days = pv_stats_days(&day_n);
    if (!days)
        return false;

    /* Reported separately from the offset itself, because 0 is a perfectly
     * good offset -- it is where the very first line of the log sits. */
    for (int i = 0; i < day_n; i++)
    {
        if (!found && days[i].day >= d_lo && days[i].day < d_hi)
        {
            *from = days[i].offset;
            found = true;
        }
        if (days[i].day >= d_hi)
        {
            *to = days[i].offset;
            break;
        }
    }

    return found;
}

static void week_scan(int wi, const struct pv_totals *t, struct week_info *w)
{
    struct week_scan_ctx ctx;
    unsigned long from, to;
    int bi;

    memset(w, 0, sizeof(*w));
    wk_art_n = wk_trk_n = 0;

    ctx.w = w;
    ctx.d_lo = display_day0 + (long)wi * 7;
    ctx.d_hi = ctx.d_lo + 7;
    ctx.secs = 0;

    if (!week_byte_range(ctx.d_lo, ctx.d_hi, &from, &to))
        return;                 /* no days recorded in this week at all */

    pv_log_read_range(t->source, from, to, week_entry, &ctx);
    w->mins = (long)(ctx.secs / 60);

    bi = -1;
    for (int i = 0; i < wk_art_n; i++)
    {
        if (bi < 0 || wk_art[i].c > wk_art[bi].c)
            bi = i;
    }
    if (bi >= 0)
    {
        strlcpy(w->top_artist, wk_art[bi].name, PV_NAME_MAX);
        w->top_artist_n = wk_art[bi].c;
    }

    bi = -1;
    for (int i = 0; i < wk_trk_n; i++)
    {
        if (bi < 0 || wk_trk[i].c > wk_trk[bi].c)
            bi = i;
    }
    if (bi >= 0)
    {
        strlcpy(w->top_track, wk_trk[bi].name, PV_NAME_MAX);
        w->top_track_n = wk_trk[bi].c;
    }
}

static int draw_week_card(const struct pv_theme *th, int wi,
                          const struct pv_totals *t, int dir)
{
    static const char *const tag[PV_TIERS] =
    {
        NULL,
        "SUPERWEEK - a full day of music",
        "ULTRAWEEK - it never left your hands",
        "HYPERWEEK - it plays in your sleep",
        "GIGAWEEK - when did it even charge?",
        "OMEGAWEEK - silence is a myth"
    };
    struct week_info w;
    char buf2[80], rng[40], fb[64];
    int tier;

    week_scan(wi, t, &w);
    tier = week_tier(week_secs[wi]);

    pv_fill(th);
    snprintf(buf2, sizeof(buf2), "WEEK %d", wi + 1);
    pv_kicker(22, buf2, tier ? tier_col[tier] : th->accent);
    pv_underline(th, 43);
    week_range(wi, rng, sizeof(rng));
    pv_text_centre(50, rng, PV_TEXT_DIM);

    if (w.plays == 0)
    {
        pv_text_centre(112, "nothing logged this week", PV_TEXT_LIGHT);
        pv_present(dir);
        return 0;
    }

    pv_text_centre(142, "minutes this week", th->accent);

    snprintf(buf2, sizeof(buf2), "top artist: %s", w.top_artist);
    pv_fit_text(buf2, PV_W - 24, fb, sizeof(fb));
    pv_text_centre(166, fb, PV_TEXT_LIGHT);

    snprintf(buf2, sizeof(buf2), "top song: %s", w.top_track);
    pv_fit_text(buf2, PV_W - 24, fb, sizeof(fb));
    pv_text_centre(188, fb, PV_TEXT_LIGHT);

    snprintf(buf2, sizeof(buf2), "%ld plays  -  %ld skips", w.plays, w.skips);
    pv_text_centre(210, buf2, PV_TEXT_DIM);

    if (tier)
        pv_text_centre(224, tag[tier], tier_col[tier]);

    pv_present(dir);
    return pv_animate_count(th, PV_W / 2, 80, 80, 46 + 8 + 6, 30, 46, 8, w.mins);
}

/* ------------------------------------------------------------- the card */

int pv_year_card(const struct pv_theme *th, int idx, int dir,
                 const struct pv_totals *t)
{
    char buf2[64];
    long ylen;
    int doy, pct, active = 0;

    year_init(t);

    pv_fill(th);
    pv_kicker(22, "YOUR YEAR SO FAR", th->accent);
    pv_underline(th, 43);

    if (!display_year)
    {
        pv_text_centre(104, "no clock, no calendar", PV_TEXT_LIGHT);
        pv_text_centre(126, "set the time & date to unlock this card",
                       PV_TEXT_DIM);
        pv_page_dots(th, idx, PV_CARD_COUNT);
        pv_present(dir);
        return 0;
    }

    ylen = pv_days_from_civil(display_year + 1, 1, 1)
         - pv_days_from_civil(display_year, 1, 1);
    doy = year_elapsed_days();
    pct = (int)((long)doy * 100 / ylen);

    for (int i = 0; i < n_weeks; i++)
    {
        if (week_secs[i] >= WEEK_ACTIVE_SECS)
            active++;
    }

    draw_week_bar(th, -1);

    /* PV_YEAR_L0/L1 rather than literals: the browser overdraws these two
     * lines with its own, and the two layouts have to agree or the old text
     * shows through from under the new. */
    snprintf(buf2, sizeof(buf2), "%d active weeks", active);
    pv_text_centre(PV_YEAR_L0, buf2, PV_TEXT_LIGHT);
    snprintf(buf2, sizeof(buf2), "longest daily streak: %d days", t->streak);
    pv_text_centre(PV_YEAR_L1, buf2, PV_TEXT_DIM);

    pv_text_centre(PV_YEAR_HINT, "SELECT", PV_TEXT_DIM);

    pv_page_dots(th, idx, PV_CARD_COUNT);
    pv_present(dir);

    /* 62, not 64: the percentage animates last and repaints a band down to
     * y+66, and the bar's own band starts at WB_Y - 8. At 64 the two overlap
     * by two rows and every frame clips the tip off the sparkles that sit
     * above a tiered week. */
    return pv_animate_percent(th, 62, pct);
}

/* ----------------------------------------------------------- the browser */

int pv_year_browse(const struct pv_theme *th, const struct pv_totals *t)
{
    int cursor = year_cur_week();
    int pending = 0;

    if (!display_year)
        return 0;
    if (cursor < 0)
        cursor = 0;

    for (;;)
    {
        char rng[40], buf2[64];
        long m;
        int wt, btn, base;

        draw_week_bar(th, cursor);
        pv_band(th, PV_YEAR_LY, PV_YEAR_LH);

        week_range(cursor, rng, sizeof(rng));
        snprintf(buf2, sizeof(buf2), "week %d: %s", cursor + 1, rng);
        pv_text_centre(PV_YEAR_L0, buf2, PV_TEXT_LIGHT);

        m = week_secs[cursor] / 60;
        wt = week_tier(week_secs[cursor]);
        if (wt)
        {
            static const char *const nm[PV_TIERS] =
            {
                NULL, "SUPERWEEK", "ULTRAWEEK ?!", "HYPERWEEK ?!?",
                "GIGAWEEK ?!?!", "OMEGAWEEK ?!?!?"
            };
            snprintf(buf2, sizeof(buf2), "%ld min - %s", m, nm[wt]);
            pv_text_centre(PV_YEAR_L1, buf2, tier_col[wt]);
        }
        else
        {
            snprintf(buf2, sizeof(buf2), "%ld min", m);
            pv_text_centre(PV_YEAR_L1, buf2, PV_TEXT_DIM);
        }

        /* The card's hint is now wrong -- in here SELECT opens the week and
         * Menu is the way back out -- and it sits inside the band above, so
         * it has to be rewritten every pass rather than once on the way in. */
        pv_text_centre(PV_YEAR_HINT, "MENU", PV_TEXT_DIM);

        lcd_update_rect(0, WB_Y - 8, PV_W, WB_H + 16);
        lcd_update_rect(0, PV_YEAR_LY, PV_W, PV_YEAR_LH);

        btn = pending ? pending : button_get(true);
        pending = 0;

        if (btn == SYS_USB_CONNECTED)
            return btn;

        base = btn & ~BUTTON_REPEAT;
        if (base == BUTTON_SCROLL_FWD || base == BUTTON_RIGHT)
        {
            if (cursor < n_weeks - 1)
                cursor++;
        }
        else if (base == BUTTON_SCROLL_BACK || base == BUTTON_LEFT)
        {
            if (cursor > 0)
                cursor--;
        }
        else if (base == BUTTON_SELECT)
        {
            int wdir = 0;

            for (;;)
            {
                int b, bb;

                /* Each week card re-reads the log, so this is the one place
                 * in the deck that is genuinely slow rather than merely
                 * animated. */
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
                cpu_boost(true);
#endif
                b = draw_week_card(th, cursor, t, wdir);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
                cpu_boost(false);
#endif
                if (!b)
                    b = button_get(true);
                if (b == SYS_USB_CONNECTED)
                    return b;

                bb = b & ~BUTTON_REPEAT;
                if ((bb == BUTTON_SCROLL_FWD || bb == BUTTON_RIGHT)
                    && cursor < n_weeks - 1)
                {
                    cursor++;
                    wdir = +1;
                }
                else if ((bb == BUTTON_SCROLL_BACK || bb == BUTTON_LEFT)
                         && cursor > 0)
                {
                    cursor--;
                    wdir = -1;
                }
                else if (bb == BUTTON_MENU || bb == BUTTON_SELECT)
                {
                    break;
                }
            }

            /* Put the year card back underneath and carry on browsing. */
            pending = pv_year_card(th, PV_CARD_YEAR, 0, t);
        }
        else if (base == BUTTON_MENU)
        {
            return 0;
        }
    }
}
