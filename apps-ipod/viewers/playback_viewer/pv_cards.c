/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * The cards: one screen each, drawn from the aggregates.
 *
 * Every card is the same three moves -- fill the gradient, put the small
 * heading and captions down, then animate the one figure that is the point of
 * it -- so what differs between them is a palette, a label and which question
 * the model is asked. That is why the palettes are a table rather than
 * fourteen functions' worth of constants.
 *
 * Animations poll the button queue and give up the moment anything is
 * pressed, handing the code back so the deck can act on it. A card is a
 * flourish, never a delay: holding the wheel flips through the whole deck at
 * the speed of the wheel, not the speed of the animations.
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "kernel.h"
#include "system.h"          /* cpu_boost */
#include "button.h"
#include "lcd.h"
#include "font.h"
#include "pv_paint.h"
#include "pv_art.h"
#include "pv_stats.h"
#include "pv_cards.h"
#include "pv_badges_ui.h"
#include "pv_year.h"

#define PV_W LCD_WIDTH
#define PV_H LCD_HEIGHT

/* Vivid, Wrapped-ish palettes, one per card, in enum order. */
static const struct pv_theme themes[PV_CARD_COUNT] =
{
    /* intro   */ { LCD_RGBPACK( 45, 22, 80), LCD_RGBPACK(120, 30,110),
                    LCD_RGBPACK(235,215,255), LCD_RGBPACK(255,180,235),
                    LCD_RGBPACK(220,185,255) },
    /* minutes */ { LCD_RGBPACK(  8, 95, 92), LCD_RGBPACK( 22,158, 96),
                    LCD_RGBPACK(225,255,235), LCD_RGBPACK(130,245,175),
                    LCD_RGBPACK(180,255,210) },
    /* plays   */ { LCD_RGBPACK( 78, 42,158), LCD_RGBPACK( 42, 78,188),
                    LCD_RGBPACK(240,235,255), LCD_RGBPACK(175,205,255),
                    LCD_RGBPACK(210,200,255) },
    /* artists */ { LCD_RGBPACK(190, 42, 96), LCD_RGBPACK(150, 28, 62),
                    LCD_RGBPACK(255,235,242), LCD_RGBPACK(255,170,195),
                    LCD_RGBPACK(255,200,215) },
    /* songs   */ { LCD_RGBPACK( 28, 82,178), LCD_RGBPACK( 20,150,190),
                    LCD_RGBPACK(235,246,255), LCD_RGBPACK(160,215,255),
                    LCD_RGBPACK(190,225,255) },
    /* albums  */ { LCD_RGBPACK(190, 96, 18), LCD_RGBPACK(176,138, 20),
                    LCD_RGBPACK(255,244,225), LCD_RGBPACK(255,205,140),
                    LCD_RGBPACK(255,225,170) },
    /* clock   */ { LCD_RGBPACK( 18, 84,104), LCD_RGBPACK( 20, 40, 84),
                    LCD_RGBPACK(225,250,252), LCD_RGBPACK(130,225,232),
                    LCD_RGBPACK(150,230,235) },
    /* night   */ { LCD_RGBPACK( 20, 18, 70), LCD_RGBPACK(  6,  6, 26),
                    LCD_RGBPACK(255,250,225), LCD_RGBPACK(215,205,255),
                    LCD_RGBPACK(255,225,140) },
    /* skips   */ { LCD_RGBPACK(180, 52, 52), LCD_RGBPACK(112, 28, 40),
                    LCD_RGBPACK(255,238,235), LCD_RGBPACK(255,175,165),
                    LCD_RGBPACK(255,190,180) },
    /* loyal   */ { LCD_RGBPACK(140, 20, 90), LCD_RGBPACK( 60, 12, 50),
                    LCD_RGBPACK(255,235,245), LCD_RGBPACK(255,160,205),
                    LCD_RGBPACK(255,140,200) },
    /* redis   */ { LCD_RGBPACK( 98, 76,140), LCD_RGBPACK( 44, 36, 78),
                    LCD_RGBPACK(245,240,255), LCD_RGBPACK(200,180,250),
                    LCD_RGBPACK(215,190,255) },
    /* year    */ { LCD_RGBPACK( 16, 64, 96), LCD_RGBPACK( 12, 34, 44),
                    LCD_RGBPACK(230,255,240), LCD_RGBPACK(120,245,165),
                    LCD_RGBPACK( 95,235,140) },
    /* ach     */ { LCD_RGBPACK( 52, 38, 12), LCD_RGBPACK( 18, 11,  6),
                    LCD_RGBPACK(255,244,200), LCD_RGBPACK(255,196, 70),
                    LCD_RGBPACK(255,185, 50) },
    /* heat    */ { LCD_RGBPACK( 14, 58, 34), LCD_RGBPACK(  6, 22, 14),
                    LCD_RGBPACK(230,255,238), LCD_RGBPACK(120,240,160),
                    LCD_RGBPACK(110,235,150) },
    /* type    */ { LCD_RGBPACK(158, 20,104), LCD_RGBPACK( 54, 12, 92),
                    LCD_RGBPACK(255,235,248), LCD_RGBPACK(255,160,215),
                    LCD_RGBPACK(255,150,210) },
    /* outro   */ { LCD_RGBPACK(120, 30,110), LCD_RGBPACK( 45, 22, 80),
                    LCD_RGBPACK(235,215,255), LCD_RGBPACK(255,180,235),
                    LCD_RGBPACK(220,185,255) },
};

/* Where the hero figure sits, and how tall a strip has to be repainted to
 * redraw it. Set per card before an animation runs. */
static int hero_y, hero_h;

/* --------------------------------------------------------------- helpers */

/* One row of a top-five list: rank badge, name, value, and a bar scaled to
 * the leader. 'frac' grows the bar during the reveal. */
static void list_row(const struct pv_theme *th, int y, int rank,
                     const char *name, int value, int maxval, int frac,
                     const struct bitmap *art)
{
    char num[8], vb[16], nb[48];
    int bx = 26, by = y + 12, br = 12;
    int w, h, valx, barx, bary, barw_full, bw;

    if (art)
    {
        /* Cover in place of the badge. Centred in the badge's space so the
         * text beside it does not shift depending on whether a row happens
         * to have artwork. */
        int ix = 10 + (PV_ART_PX - art->width) / 2;
        int iy = y + 1 + (PV_ART_PX - art->height) / 2;

        lcd_set_drawmode(DRMODE_SOLID);
        lcd_bitmap((const fb_data *)art->data, ix, iy,
                   art->width, art->height);
    }
    else
    {
        pv_disc(th, bx, by, br, th->accent, 256);

        snprintf(num, sizeof(num), "%d", rank);
        lcd_setfont(pv_body_font());
        lcd_set_drawmode(DRMODE_FG);
        lcd_getstringsize(num, &w, &h);
        lcd_set_foreground(th->bg1);
        lcd_putsxy(bx - w / 2, by - h / 2, num);
    }

    /* Both branches above leave the drawmode somewhere of their own choosing
     * -- the bitmap needs SOLID -- so the text below sets its own. In SOLID
     * mode lcd_putsxy() fills each character cell with the background colour,
     * and nothing here ever sets one, so every label would come out on a slab
     * of whatever the UI last used. */
    lcd_setfont(pv_body_font());
    lcd_set_drawmode(DRMODE_FG);

    pv_commafmt(value, vb, sizeof(vb));
    lcd_getstringsize(vb, &w, &h);
    valx = PV_W - 14 - w;
    lcd_set_foreground(th->accent);
    lcd_putsxy(valx, y + 1, vb);

    pv_fit_text(name, valx - 48 - 8, nb, sizeof(nb));
    lcd_set_foreground(PV_TEXT_LIGHT);
    lcd_putsxy(48, y + 1, nb);

    barx = 48;
    bary = y + 21;
    barw_full = valx - 8 - barx;
    bw = maxval ? (int)((long)barw_full * value / maxval) : 0;
    bw = bw * frac / 256;
    if (bw >= 4)
        pv_capsule(th, barx + 2, bary, barx + bw - 2, bary, 2,
                   pv_blend(pv_grad_at(th, bary), th->accent, 210));
}

/* A whole top-five card: the rows grow in one after another. */
static int draw_top_list(const struct pv_theme *th, enum pv_table table,
                         enum pv_rank rank, const char *kick, int idx,
                         int dir,
                         bool with_art, const char *empty)
{
    const struct pv_agg *list[5];
    const struct bitmap *art[5] = { NULL, NULL, NULL, NULL, NULL };
    /* Five rows at this pitch end just above the page dots. Widening either
     * the pitch or the start pushes the reveal's band over them, and since
     * the band is repainted every frame the dots would be drawn once and then
     * wiped -- they are put down before the animation starts. */
    int n, maxv, y0 = 58, rh = 33, base = 8, frames;

    pv_fill(th);
    pv_kicker(22, kick, th->accent);
    pv_underline(th, 43);

    n = pv_stats_top(table, rank, list, 5);
    if (n == 0)
    {
        pv_text_centre(112, empty, PV_TEXT_LIGHT);
        pv_page_dots(th, idx, PV_CARD_COUNT);
        pv_present(dir);
        return 0;
    }

    /* The leader sets the bar scale, so the top row is always full width. */
    maxv = 1;
    for (int i = 0; i < n; i++)
    {
        const struct pv_agg *a = list[i];
        int v = (rank == PV_RANK_SKIPS) ? a->skips : a->count;
        if (i == 0 && v > 0)
            maxv = v;
    }

    pv_page_dots(th, idx, PV_CARD_COUNT);
    pv_present(dir);      /* reveal the card, then grow the rows into it */

    /* Artwork once per card, not once per frame: the rows below are redrawn
     * every frame of the reveal. Only the artist and album lists have
     * folders behind them -- a title is not a folder. */
    if (with_art)
    {
        pv_art_reset();
        for (int i = 0; i < n; i++)
            art[i] = pv_art_get(list[i]->art_hash, i);
    }

    frames = (n - 1) * 2 + base;
    for (int fr = (pv_exporting() ? frames : 0); fr <= frames; fr++)
    {
        int btn;

        pv_band(th, y0 - 2, n * rh + 4);
        for (int i = 0; i < n; i++)
        {
            int lp = fr - i * 2;      /* each row starts two frames later */
            int frac, v;

            if (lp <= 0)
                continue;
            frac = (lp >= base) ? 256 : lp * 256 / base;
            v = (rank == PV_RANK_SKIPS) ? list[i]->skips : list[i]->count;
            list_row(th, y0 + i * rh, i + 1, list[i]->name, v, maxv, frac,
                     art[i]);
        }
        lcd_update_rect(0, y0 - 2, PV_W, n * rh + 4);

        btn = button_get_w_tmo(HZ / 60);
        if (pv_nav_button(btn))
        {
            pv_band(th, y0 - 2, n * rh + 4);
            for (int i = 0; i < n; i++)
            {
                int v = (rank == PV_RANK_SKIPS) ? list[i]->skips
                                                : list[i]->count;
                list_row(th, y0 + i * rh, i + 1, list[i]->name, v, maxv, 256,
                         art[i]);
            }
            lcd_update_rect(0, y0 - 2, PV_W, n * rh + 4);
            return btn;
        }
    }
    return 0;
}

/* ----------------------------------------------------------------- cards */

static int card_intro(const struct pv_theme *th, const struct pv_totals *t)
{
    char buf[40], buf2[64];

    pv_fill(th);
    pv_disc(th, 40, 50, 46, LCD_RGBPACK(255, 255, 255), 26);
    pv_disc(th, 290, 210, 60, LCD_RGBPACK(255, 255, 255), 22);
    pv_kicker(34, "MY YEAR IN MUSIC", th->accent);

    if (t->ts_max)
    {
        int y, m, d;
        pv_civil_from_days((long)(t->ts_max / 86400), &y, &m, &d);
        snprintf(buf, sizeof(buf), "%d", y);
        pv_number(th, PV_W / 2, 78, buf, 38, 64, 11);
    }

    if (t->ts_min)
    {
        int y1, m1, d1, y2, m2, d2;
        pv_civil_from_days((long)(t->ts_min / 86400), &y1, &m1, &d1);
        pv_civil_from_days((long)(t->ts_max / 86400), &y2, &m2, &d2);
        snprintf(buf2, sizeof(buf2), "%s %d  -  %s %d %d",
                 pv_month_abbr[m1], d1, pv_month_abbr[m2], d2, y2);
        pv_text_centre(178, buf2, PV_TEXT_DIM);
    }

    return 0;
}

static int card_minutes(const struct pv_theme *th, const struct pv_totals *t,
                        int idx, int dir)
{
    char buf2[64];
    long mins = t->seconds / 60;

    pv_fill(th);
    pv_kicker(22, "YOU LISTENED FOR", th->accent);
    pv_underline(th, 43);
    pv_text_centre(150, "minutes", th->accent);
    snprintf(buf2, sizeof(buf2), "about %ld hours with your music", mins / 60);
    pv_text_centre(188, buf2, PV_TEXT_DIM);
    pv_page_dots(th, idx, PV_CARD_COUNT);
    pv_present(dir);

    return pv_animate_count(th, PV_W / 2, hero_y, hero_y, hero_h,
                            34, 56, 10, mins);
}

static int card_plays(const struct pv_theme *th, const struct pv_totals *t,
                      int idx, int dir)
{
    char buf2[64];

    pv_fill(th);
    pv_kicker(22, "TRACKS PLAYED", th->accent);
    pv_underline(th, 43);
    pv_text_centre(150, "tracks", th->accent);
    snprintf(buf2, sizeof(buf2), "%d different songs", t->titles);
    pv_text_centre(188, buf2, PV_TEXT_DIM);
    pv_page_dots(th, idx, PV_CARD_COUNT);
    pv_present(dir);

    return pv_animate_count(th, PV_W / 2, hero_y, hero_y, hero_h,
                            34, 56, 10, t->plays);
}

static int card_night(const struct pv_theme *th, const struct pv_totals *t,
                      int idx, int dir)
{
    const struct pv_agg *na[1];
    char buf2[64], fb[64];

    pv_fill(th);
    pv_kicker(22, "THE 3AM CLUB", th->accent);
    pv_underline(th, 43);

    if (t->night == 0)
    {
        pv_text_centre(112, "Your nights are quiet. Respect.", PV_TEXT_LIGHT);
        pv_page_dots(th, idx, PV_CARD_COUNT);
        pv_present(dir);
        return 0;
    }

    pv_text_centre(150, "plays between midnight and 5am", th->accent);

    if (pv_stats_top(PV_T_ARTIST, PV_RANK_NIGHT, na, 1) == 1)
    {
        snprintf(buf2, sizeof(buf2), "usually %s", na[0]->name);
        pv_fit_text(buf2, PV_W - 24, fb, sizeof(fb));
        pv_text_centre(178, fb, PV_TEXT_LIGHT);
    }
    if (pv_stats_top(PV_T_TITLE, PV_RANK_NIGHT, na, 1) == 1)
    {
        snprintf(buf2, sizeof(buf2), "on repeat: %s", na[0]->name);
        pv_fit_text(buf2, PV_W - 24, fb, sizeof(fb));
        pv_text_centre(198, fb, PV_TEXT_DIM);
    }

    pv_page_dots(th, idx, PV_CARD_COUNT);
    pv_present(dir);
    return pv_animate_count(th, PV_W / 2, hero_y - 5, hero_y - 5, hero_h,
                            34, 56, 10, t->night);
}

static int card_clock(const struct pv_theme *th, const struct pv_totals *t,
                      int idx, int dir)
{
    const int bx = 16, base = 178, maxh = 100, F = 14;
    int bw = (PV_W - 32) / 24;
    int peak = 0, maxv = 1, ret = 0;
    char buf2[64];

    pv_fill(th);
    pv_kicker(22, "YOUR LISTENING CLOCK", th->accent);
    pv_underline(th, 43);

    for (int i = 1; i < 24; i++)
    {
        if (t->hour_hist[i] > t->hour_hist[peak])
            peak = i;
    }
    for (int i = 0; i < 24; i++)
    {
        if (t->hour_hist[i] > maxv)
            maxv = t->hour_hist[i];
    }

    snprintf(buf2, sizeof(buf2), "you peak around %02d:00", peak);
    pv_text_centre(204, buf2, th->accent);
    pv_page_dots(th, idx, PV_CARD_COUNT);
    pv_present(dir);

    /* Runs to fr == F and draws that frame before testing the queue, so the
     * chart is always finished on the way out. The bars grow from nothing, so
     * bailing straight out of an early frame -- which is the normal case, the
     * wheel event that turned the card is still queued behind it -- would
     * leave the card with no bars on it at all. The shared counters snap to
     * their final value for the same reason. */
    for (int fr = (pv_exporting() ? F : 0); ; fr++)
    {
        int frac = fr * 256 / F;
        int btn;

        pv_band(th, base - maxh - 2, maxh + 22);
        for (int i = 0; i < 24; i++)
        {
            int hh = (t->hour_hist[i] * maxh) / maxv;
            unsigned c;

            hh = hh * frac / 256;
            if (hh <= 0)
                continue;

            c = (i == peak) ? th->num1
                            : pv_blend(pv_grad_at(th, base),
                                       LCD_RGBPACK(255, 255, 255), 90);
            lcd_set_drawmode(DRMODE_SOLID);
            lcd_set_foreground(c);
            lcd_fillrect(bx + i * bw, base - hh, bw - 1, hh);
        }

        lcd_set_drawmode(DRMODE_FG);
        lcd_set_foreground(PV_TEXT_DIM);
        lcd_putsxy(bx, base + 4, "0");
        lcd_putsxy(bx + 12 * bw - 4, base + 4, "12");
        lcd_putsxy(bx + 23 * bw - 8, base + 4, "23");
        lcd_update_rect(0, base - maxh - 2, PV_W, maxh + 22);

        if (ret || fr >= F)
            break;

        btn = button_get_w_tmo(HZ / 60);
        if (pv_nav_button(btn))
        {
            /* Keep the button, but take one more pass at full height on the
             * way past rather than leaving from here. */
            ret = btn;
            fr = F - 1;
        }
    }
    return ret;
}

static int card_skips(const struct pv_theme *th, const struct pv_totals *t,
                      int idx, int dir)
{
    const struct pv_agg *slist[1];
    long total = t->plays + t->skips;
    int pct = total ? (int)((t->skips * 100) / total) : 0;
    char nb[48];

    pv_fill(th);
    pv_kicker(22, "THE SKIP REPORT", th->accent);
    pv_underline(th, 43);
    pv_text_centre(150, "of plays were skips", th->accent);

    if (pv_stats_top(PV_T_TITLE, PV_RANK_SKIPS, slist, 1) == 1)
    {
        pv_text_centre(178, "most skipped", PV_TEXT_DIM);
        pv_fit_text(slist[0]->name, PV_W - 24, nb, sizeof(nb));
        pv_text_centre(198, nb, PV_TEXT_LIGHT);
    }

    pv_page_dots(th, idx, PV_CARD_COUNT);
    pv_present(dir);
    return pv_animate_percent(th, 70, pct);
}

/* The heatmap, GitHub-style: one cell per day, columns are Monday-aligned
 * weeks. Laid out over the log's own span rather than a calendar year, and
 * showing the most recent weeks that fit when the history is longer. */
static int card_heat(const struct pv_theme *th,
                     int idx, int dir)
{
    const int pitch = 5, cell = 4, gy0 = 88;
    const int maxw = (PV_W - 24) / pitch;      /* weeks that fit across */
    const struct pv_day *days;
    int day_n, nweeks, active = 0;
    long first_day, last_day, day0;
    unsigned best = 0;
    long best_day = 0;
    int gx0;
    char buf2[64];

    pv_fill(th);
    pv_kicker(22, "EVERY DAY OF IT", th->accent);
    pv_underline(th, 43);

    days = pv_stats_days(&day_n);
    if (!days || day_n == 0)
    {
        pv_text_centre(112, "nothing on the calendar yet", PV_TEXT_LIGHT);
        pv_page_dots(th, idx, PV_CARD_COUNT);
        pv_present(dir);
        return 0;
    }

    first_day = days[0].day;
    last_day  = days[day_n - 1].day;

    /* Start the grid on a Monday, and if the span is longer than the screen
     * holds, show the most recent weeks rather than the oldest. */
    day0 = first_day - (((first_day - 4) % 7) + 7) % 7;
    nweeks = (int)((last_day - day0) / 7) + 1;
    if (nweeks > maxw)
    {
        day0 += (long)(nweeks - maxw) * 7;
        nweeks = maxw;
    }

    gx0 = (PV_W - nweeks * pitch + (pitch - cell)) / 2;

    lcd_set_drawmode(DRMODE_SOLID);
    for (int w = 0; w < nweeks; w++)
    {
        for (int d = 0; d < 7; d++)
        {
            int y = gy0 + d * pitch;
            /* Empty is a faint wash rather than nothing, so the shape of the
             * grid is visible even where the listening is not. */
            unsigned c = pv_blend(pv_grad_at(th, y),
                                  LCD_RGBPACK(255, 255, 255), 22);
            lcd_set_foreground(c);
            lcd_fillrect(gx0 + w * pitch, y, cell, cell);
        }
    }

    for (int i = 0; i < day_n; i++)
    {
        long rel = days[i].day - day0;
        unsigned m = days[i].secs / 60;
        int y, tt;
        unsigned c;

        if (rel < 0 || rel >= 7L * nweeks)
            continue;

        active++;
        if (m > best)
        {
            best = m;
            best_day = days[i].day;
        }

        y = gy0 + (int)(rel % 7) * pitch;
        tt = (m == 0) ? 0
           : (m <  15) ? 70
           : (m <  60) ? 130
           : (m < 180) ? 190 : 256;
        c = (tt == 0) ? pv_blend(pv_grad_at(th, y),
                                 LCD_RGBPACK(255, 255, 255), 22)
                      : pv_blend(pv_grad_at(th, y), th->accent, tt);
        lcd_set_foreground(c);
        lcd_fillrect(gx0 + (int)(rel / 7) * pitch, y, cell, cell);
    }

    snprintf(buf2, sizeof(buf2), "%d of %d days had music",
             active, nweeks * 7);
    pv_text_centre(154, buf2, th->accent);

    if (best)
    {
        int by, bm, bd;
        pv_civil_from_days(best_day, &by, &bm, &bd);
        snprintf(buf2, sizeof(buf2), "best day: %uh %02um on %s %d",
                 best / 60, best % 60, pv_month_abbr[bm], bd);
        pv_text_centre(180, buf2, PV_TEXT_LIGHT);
    }

    pv_page_dots(th, idx, PV_CARD_COUNT);
    pv_present(dir);
    return 0;
}

/* Four axes, each a slider between two poles, giving one of sixteen types. */
static int card_type(const struct pv_theme *th, const struct pv_totals *t,
                     int idx, int dir)
{
    static const struct
    {
        const char *lo, *hi;
        char lc, hc;
        int  mid;         /* at or above this, the high pole wins */
    } axis[4] =
    {
        { "roamer",   "devotee",   'R', 'D', 45 },
        { "explorer", "repeater",  'E', 'F', 55 },
        { "sampler",  "committed", 'S', 'C', 78 },
        { "daylight", "night owl", 'L', 'N',  8 },
    };
    /* Indexed by (devotee<<3)|(repeater<<2)|(committed<<1)|nightowl. */
    static const char *const type_name[16] =
    {
        "The Free Spirit",     "The Night Scout",
        "The Adventurer",      "The Midnight Wanderer",
        "The Channel Hopper",  "The Midnight Zapper",
        "The Comfort Curator", "The Night Ritualist",
        "The Catalog Sifter",  "The Night Miner",
        "The Completionist",   "The Deep Diver",
        "The Picky Superfan",  "The Restless Fan",
        "The True Believer",   "The Midnight Devotee",
    };
    const int TX0 = 104, TX1 = 216, ROWY = 62, ROWH = 30, F = 14;
    const struct pv_agg *a1[1];
    long total = t->plays + t->skips;
    int val[4], pos[4], tt = 0, ret = 0;
    char code[8];

    pv_fill(th);
    pv_kicker(22, "YOUR LISTENING TYPE", th->accent);
    pv_underline(th, 43);

    val[0] = (t->plays && pv_stats_top(PV_T_ARTIST, PV_RANK_PLAYS, a1, 1) == 1)
             ? (int)((long)a1[0]->count * 100 / t->plays) : 0;
    val[1] = t->plays ? (int)((t->plays - t->titles) * 100 / t->plays) : 0;
    val[2] = total ? (int)(t->plays * 100 / total) : 100;
    val[3] = t->plays ? (int)(t->night * 100 / t->plays) : 0;

    for (int i = 0; i < 4; i++)
    {
        bool hi = val[i] >= axis[i].mid;

        if (hi)
            tt |= 8 >> i;
        code[i * 2]     = hi ? axis[i].hc : axis[i].lc;
        code[i * 2 + 1] = (i < 3) ? ' ' : '\0';

        /* Bent so each axis's tipping point sits at the middle of its track,
         * whatever value it actually is -- otherwise every marker would
         * cluster at one end. */
        pos[i] = (val[i] <= axis[i].mid)
               ? val[i] * 50 / axis[i].mid
               : 50 + (val[i] - axis[i].mid) * 50 / (100 - axis[i].mid);
    }

    /* The verdict is drawn before the markers move, not after they land: it
     * is what the card is about, and it reads as an afterthought if it turns
     * up once the animation has finished. It sits clear of the band the loop
     * repaints, so one draw survives every frame. */
    pv_kicker(176, code, th->accent);
    pv_text_centre(200, type_name[tt], PV_TEXT_LIGHT);

    for (int fr = (pv_exporting() ? F : 0); fr <= F; fr++)
    {
        int btn;

        pv_band(th, ROWY - 10, 4 * ROWH);
        for (int i = 0; i < 4; i++)
        {
            int yr = ROWY + i * ROWH;
            bool hi = val[i] >= axis[i].mid;
            unsigned bg = pv_grad_at(th, yr);
            int cur, mx, lw, lh;

            pv_capsule(th, TX0, yr, TX1, yr, 3,
                       pv_blend(bg, LCD_RGBPACK(255, 255, 255), 55));
            pv_capsule(th, (TX0 + TX1) / 2, yr - 5, (TX0 + TX1) / 2, yr + 5, 1,
                       pv_blend(bg, LCD_RGBPACK(255, 255, 255), 95));

            cur = 50 + (int)pv_ease(pos[i] - 50, fr, F);
            mx = TX0 + (TX1 - TX0) * cur / 100;
            pv_disc(th, mx, yr, 5, th->num1, 256);

            lcd_setfont(pv_body_font());
            lcd_set_drawmode(DRMODE_FG);
            lcd_set_foreground(hi ? PV_TEXT_DIM : PV_TEXT_LIGHT);
            lcd_putsxy(14, yr - 6, axis[i].lo);
            lcd_set_foreground(hi ? PV_TEXT_LIGHT : PV_TEXT_DIM);
            lcd_getstringsize(axis[i].hi, &lw, &lh);
            lcd_putsxy(PV_W - 14 - lw, yr - 6, axis[i].hi);
        }

        if (fr == 0 || pv_exporting())
        {
            /* Every marker starts centred, so the first frame is the card as
             * a whole arriving; the rest are just the markers moving.
             *
             * When exporting there is no first frame -- the loop starts at
             * the last one -- so that single frame has to do both jobs, and
             * must not then skip the rest of the body. */
            pv_page_dots(th, idx, PV_CARD_COUNT);
            pv_present(dir);
            if (!pv_exporting())
                continue;
        }

        lcd_update_rect(0, ROWY - 10, PV_W, 4 * ROWH);
        btn = button_get_w_tmo(HZ / 50);
        if (pv_nav_button(btn))
        {
            ret = btn;
            break;
        }
    }

    return ret;
}

static int card_outro(const struct pv_theme *th, const struct pv_totals *t)
{
    static const long stones[] =
        { 1000, 2500, 5000, 10000, 25000, 50000, 100000 };
    const struct pv_agg *a[1];
    long mins = t->seconds / 60;
    long next = 0;
    char buf2[64], fb[64];

    pv_fill(th);
    pv_disc(th, 285, 40, 50, LCD_RGBPACK(255, 255, 255), 24);
    pv_disc(th, 35, 205, 56, LCD_RGBPACK(255, 255, 255), 20);
    pv_kicker(40, "THAT'S A WRAP", th->accent);
    pv_underline(th, 60);

    /* Three summary lines evenly spaced, then the milestone set apart from
     * them -- it is about what comes next rather than what happened. */
    snprintf(buf2, sizeof(buf2), "%ld minutes listened", mins);
    pv_text_centre(87, buf2, PV_TEXT_LIGHT);

    snprintf(buf2, sizeof(buf2), "%ld plays  -  %d songs", t->plays, t->titles);
    pv_text_centre(117, buf2, PV_TEXT_LIGHT);

    if (pv_stats_top(PV_T_ARTIST, PV_RANK_PLAYS, a, 1) == 1)
    {
        snprintf(buf2, sizeof(buf2), "#1 artist: %s", a[0]->name);
        pv_fit_text(buf2, PV_W - 24, fb, sizeof(fb));
        pv_text_centre(147, fb, th->accent);
    }

    for (unsigned i = 0; i < sizeof(stones) / sizeof(stones[0]); i++)
    {
        if (mins < stones[i])
        {
            next = stones[i];
            break;
        }
    }

    if (next)
    {
        char nb[16];
        int bx0 = 70, bx1 = PV_W - 70, bary = 212, fill;

        pv_commafmt(next, nb, sizeof(nb));
        snprintf(buf2, sizeof(buf2), "next milestone: %s minutes", nb);
        pv_text_centre(184, buf2, PV_TEXT_DIM);

        pv_capsule(th, bx0, bary, bx1, bary, 3,
                   pv_blend(pv_grad_at(th, bary),
                            LCD_RGBPACK(255, 255, 255), 55));
        fill = (int)((long)(bx1 - bx0) * mins / next);
        if (fill > 3)
            pv_capsule(th, bx0, bary, bx0 + fill, bary, 3, th->accent);
    }
    else
    {
        pv_text_centre(184, "100,000+ minutes. you broke the scale.",
                       th->accent);
    }

    return 0;
}

/* ------------------------------------------------------------------ entry */

static int draw_body(int idx, int dir, const struct pv_totals *t)
{
    const struct pv_theme *th;
    int ret = 0;

    if (idx < 0 || idx >= PV_CARD_COUNT)
        return 0;

    th = &themes[idx];

    /* Where the counted figure sits on the cards that have one. */
    hero_y = 74;
    hero_h = 56 + 10 + 6;

    switch (idx)
    {
    case PV_CARD_INTRO:
        ret = card_intro(th, t);
        break;
    case PV_CARD_MINUTES:
        return card_minutes(th, t, idx, dir);
    case PV_CARD_PLAYS:
        return card_plays(th, t, idx, dir);
    case PV_CARD_ARTISTS:
        return draw_top_list(th, PV_T_ARTIST, PV_RANK_PLAYS, "TOP ARTISTS",
                             idx, dir, true, "No plays logged yet");
    case PV_CARD_SONGS:
        return draw_top_list(th, PV_T_TITLE, PV_RANK_PLAYS, "TOP SONGS",
                             idx, dir, false, "No plays logged yet");
    case PV_CARD_ALBUMS:
        return draw_top_list(th, PV_T_ALBUM, PV_RANK_PLAYS, "TOP ALBUMS",
                             idx, dir, true, "No plays logged yet");
    case PV_CARD_CLOCK:
        return card_clock(th, t, idx, dir);
    case PV_CARD_NIGHT:
        return card_night(th, t, idx, dir);
    case PV_CARD_SKIPS:
        return card_skips(th, t, idx, dir);
    case PV_CARD_LOYAL:
        return draw_top_list(th, PV_T_TITLE, PV_RANK_LOYAL, "NEVER SKIPPED",
                             idx, dir, false, "You skip everything?!");
    case PV_CARD_REDIS:
        return draw_top_list(th, PV_T_TITLE, PV_RANK_REDIS, "REMEMBER THESE?",
                             idx, dir, false, "No forgotten favourites yet");
    case PV_CARD_YEAR:
        return pv_year_card(th, idx, dir, t);
    case PV_CARD_ACH:
        return pv_badges_card(th, idx, dir, t);
    case PV_CARD_HEAT:
        return card_heat(th, idx, dir);
    case PV_CARD_TYPE:
        return card_type(th, t, idx, dir);
    case PV_CARD_OUTRO:
        ret = card_outro(th, t);
        break;
    default:
        pv_fill(th);
        break;
    }

    pv_page_dots(th, idx, PV_CARD_COUNT);
    pv_present(dir);
    return ret;
}

const struct pv_theme *pv_cards_theme(int idx)
{
    if (idx < 0 || idx >= PV_CARD_COUNT)
        idx = 0;
    return &themes[idx];
}

int pv_cards_draw(int idx, int dir, const struct pv_totals *t)
{
    int ret;

    /* A card is gradient fills and per-pixel blends edge to edge, and these
     * targets idle at a third to a quarter of their top clock -- 30 MHz of
     * 80 on the 5G, 54 of 216 on the 6G. Unboosted, the animations do not
     * merely run slowly, they visibly stutter. */
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    cpu_boost(true);
#endif

    ret = draw_body(idx, dir, t);

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    cpu_boost(false);
#endif

    return ret;
}
