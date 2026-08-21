/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/achievements_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * The badge wall: the card that counts it, and the list you browse it in.
 *
 * A row shows as much as it has earned the right to. An unlocked badge shows
 * its name in its metal and the date it was first seen; the lowest unearned
 * rung of each ladder shows its name and how far along you are; everything
 * else is "? ? ?". Thirty rows reading "listen for N minutes" in ascending
 * order would be neither a surprise nor a list worth scrolling.
 *
 * Drawn with the deck's own toolkit rather than the list widget, because it
 * has to look like the deck it is reached from -- and because the tier
 * colours and the progress bars are the point of it.
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
#include "pv_paint.h"
#include "pv_badges.h"
#include "pv_badges_ui.h"
#include "pv_cards.h"
#include "pv_stats.h"

#define PV_W LCD_WIDTH
#define PV_H LCD_HEIGHT

#define ROWS 6
#define ROW_Y0 56
#define ROW_H  25

/* The wall's own palette -- gold on near-black, the same metal the first
 * tier wears. */
static const struct pv_theme wall_th =
{
    LCD_RGBPACK(52, 38, 12), LCD_RGBPACK(18, 11, 6),
    LCD_RGBPACK(255, 244, 200), LCD_RGBPACK(255, 196, 70),
    LCD_RGBPACK(255, 185, 50)
};

/* One metal per tier, and its highlight. */
static const unsigned tier_col[PV_TIER_N] =
{
    LCD_RGBPACK(255, 185,  50),   /* gold  */
    LCD_RGBPACK(120, 220, 255),   /* ice   */
    LCD_RGBPACK(200, 135, 255),   /* violet*/
    LCD_RGBPACK(255, 120,  85),   /* ember */
    LCD_RGBPACK(255, 255, 255),   /* white */
    LCD_RGBPACK(255, 145, 205)    /* pink  */
};

static const unsigned tier_hi[PV_TIER_N] =
{
    LCD_RGBPACK(255, 240, 170),
    LCD_RGBPACK(235, 252, 255),
    LCD_RGBPACK(240, 225, 255),
    LCD_RGBPACK(255, 220, 195),
    LCD_RGBPACK(190, 245, 255),
    LCD_RGBPACK(255, 222, 240)
};

static int tier_of(int i)
{
    const struct pv_badge *b = pv_badges_get(i);
    int t = b ? b->tier : PV_TIER_GOLD;

    return (t >= 0 && t < PV_TIER_N) ? t : PV_TIER_GOLD;
}

/* A whole palette from one metal: the wall's own colours are gold over a dark
 * tint of itself, and every tier is built the same way. */
static void tier_theme(struct pv_theme *th, int t)
{
    const unsigned black = LCD_RGBPACK(0, 0, 0);

    th->bg0    = pv_blend(black, tier_col[t], 52);
    th->bg1    = pv_blend(black, tier_col[t], 18);
    th->num0   = tier_hi[t];
    th->num1   = tier_col[t];
    th->accent = tier_col[t];
}

/* ------------------------------------------------------------------ rows */

static void draw_row(int y, int i, bool selected)
{
    const struct pv_theme *th = &wall_th;
    const struct pv_badge *b = pv_badges_get(i);
    enum pv_badge_vis v = pv_badges_vis(i);
    int cy = y + ROW_H / 2 - 1;
    char rtxt[36], nb[48];
    const char *name;
    unsigned ncol;
    int rw = 0, rh;

    /* The marker: filled in its metal when earned, an empty ring when not. */
    if (v == PV_BV_DONE)
    {
        int t = tier_of(i);

        pv_disc(th, 17, cy, 8, tier_col[t], 256);
        pv_disc(th, 17, cy, 4, tier_hi[t], 256);
    }
    else
    {
        pv_disc(th, 17, cy, 8, LCD_RGBPACK(255, 255, 255), 55);
        pv_disc(th, 17, cy, 6, pv_grad_at(th, cy), 256);
    }

    lcd_setfont(pv_body_font());
    lcd_set_drawmode(DRMODE_FG);

    /* The right-hand column says either "NEW" or how far along this is. */
    rtxt[0] = '\0';
    if (v == PV_BV_DONE)
    {
        if (pv_badges_is_new(i))
            strlcpy(rtxt, "NEW", sizeof(rtxt));
    }
    else if (v == PV_BV_GOAL)
    {
        char a[16], c[16];
        long thr = b->threshold;
        long val = pv_badges_value(i);

        if (val < 0)
            val = 0;
        if (val > thr)
            val = thr;
        pv_commafmt(val, a, sizeof(a));
        pv_commafmt(thr, c, sizeof(c));
        snprintf(rtxt, sizeof(rtxt), "%s/%s", a, c);
    }
    if (rtxt[0])
        lcd_getstringsize(rtxt, &rw, &rh);

    name = (v == PV_BV_HIDDEN) ? "? ? ?" : b->name;
    pv_fit_text(name, PV_W - 14 - rw - 8 - 32, nb, sizeof(nb));

    ncol = (v == PV_BV_DONE) ? tier_hi[tier_of(i)]
         : (v == PV_BV_GOAL) ? PV_TEXT_LIGHT
         : pv_blend(pv_grad_at(th, cy), LCD_RGBPACK(255, 255, 255), 90);
    lcd_set_foreground(ncol);
    lcd_putsxy(32, y + 4, nb);

    if (rtxt[0])
    {
        lcd_set_foreground(pv_badges_is_new(i) ? tier_col[tier_of(i)]
                                               : PV_TEXT_DIM);
        lcd_putsxy(PV_W - 12 - rw, y + 4, rtxt);
    }

    if (selected)
    {
        lcd_set_drawmode(DRMODE_SOLID);
        lcd_set_foreground(LCD_RGBPACK(255, 255, 255));
        lcd_drawrect(4, y, PV_W - 8, ROW_H - 1);
    }
}

static void draw_wall(int cursor, int top, int done, int newn)
{
    const struct pv_theme *th = &wall_th;
    const struct pv_badge *b;
    enum pv_badge_vis v;
    char buf[64], fb[64];
    int total = pv_badges_count();
    /* Tight against the last row on purpose: the second footer line sits a
     * whole line lower again, and any more gap runs it off the bottom. */
    int fy = ROW_Y0 + ROWS * ROW_H + 2;
    const char *desc;

    pv_fill(th);

    if (done >= total)
        pv_kicker(10, "THE WHOLE WALL", tier_col[PV_TIER_ICE]);
    else
        pv_kicker(10, "ACHIEVEMENTS", th->accent);

    if (newn > 0)
        snprintf(buf, sizeof(buf), "%d / %d unlocked  -  %d NEW",
                 done, total, newn);
    else
        snprintf(buf, sizeof(buf), "%d / %d unlocked", done, total);
    pv_text_centre(28, buf, PV_TEXT_LIGHT);
    pv_underline(th, 48);

    for (int r = 0; r < ROWS; r++)
    {
        int i = top + r;

        if (i >= total)
            break;
        draw_row(ROW_Y0 + r * ROW_H, i, i == cursor);
    }

    /* The footer belongs to the selected row: what it wants, and either how
     * far along you are or when you got there. */
    b = pv_badges_get(cursor);
    v = pv_badges_vis(cursor);

    desc = (v == PV_BV_HIDDEN) ? "keep listening to reveal this one" : b->desc;
    pv_fit_text(desc, PV_W - 20, fb, sizeof(fb));
    pv_text_centre(fy, fb, (v == PV_BV_DONE) ? tier_col[tier_of(cursor)]
                                             : PV_TEXT_DIM);

    if (v == PV_BV_DONE)
    {
        unsigned long w = pv_badges_when(cursor);

        if (w > 0)
        {
            int dy, dm, dd;

            pv_civil_from_days((long)(w / 86400UL), &dy, &dm, &dd);
            snprintf(buf, sizeof(buf), "earned %s %d %d",
                     pv_month_abbr[dm], dd, dy);
        }
        else
        {
            strlcpy(buf, "earned before records began", sizeof(buf));
        }
        pv_text_centre(fy + 17, buf, PV_TEXT_DIM);
    }
    else if (v != PV_BV_HIDDEN)
    {
        int bx0 = 50, bx1 = PV_W - 50, bary = fy + 20, fill;
        long val = pv_badges_value(cursor);
        long thr = b->threshold;

        if (val > thr)
            val = thr;
        if (val < 0)
            val = 0;

        pv_capsule(th, bx0, bary, bx1, bary, 3,
                   pv_blend(pv_grad_at(th, bary),
                            LCD_RGBPACK(255, 255, 255), 55));
        fill = (int)((long)(bx1 - bx0) * val / (thr ? thr : 1));
        if (fill > 3)
            pv_capsule(th, bx0, bary, bx0 + fill, bary, 3, th->accent);
    }

    lcd_update();
}

int pv_badges_browse(const struct pv_totals *t)
{
    int total = pv_badges_count();
    int done = pv_badges_unlocked_count();
    int newn = pv_badges_new_count();
    int cursor = 0, top = 0;

    (void)t;

    /* Classifying is the deck's job, on the way in. The wall is only ever
     * reached through the deck, so the work is already done -- and doing it
     * again here would clear the new set while the deck is still showing it. */

    for (;;)
    {
        int btn, base, step;

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
        cpu_boost(true);        /* a full gradient and six rows per frame */
#endif
        draw_wall(cursor, top, done, newn);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
        cpu_boost(false);
#endif

        btn = button_get(true);
        if (btn == SYS_USB_CONNECTED)
            return btn;

        base = btn & ~BUTTON_REPEAT;
        step = 0;
        if (base == BUTTON_SCROLL_FWD || base == BUTTON_RIGHT)
            step = +1;
        else if (base == BUTTON_SCROLL_BACK || base == BUTTON_LEFT)
            step = -1;
        else if (base == BUTTON_MENU || base == BUTTON_SELECT)
            return 0;

        if (step)
        {
            cursor += step;
            if (cursor < 0)
                cursor = 0;
            if (cursor >= total)
                cursor = total - 1;

            /* Keep the cursor on screen, moving the window only when it
             * would otherwise leave. */
            if (cursor < top)
                top = cursor;
            if (cursor >= top + ROWS)
                top = cursor - ROWS + 1;
        }
    }
}

/* -------------------------------------------------------- the crown page */

/* How many crowns are worth turning through to get to the deck.
 *
 * Everything announced in one pass carries the same timestamp -- the log can
 * only say a badge is earned now, not when it was crossed -- so "the latest
 * few" is the tail of the table, which within a ladder is its highest rungs.
 * The rest are not lost: they are on the wall, dated, and the last page says
 * how many. */
#define CROWN_MAX 5

int pv_badges_crown_count(void)
{
    int n = pv_badges_new_count();

    return n > CROWN_MAX ? CROWN_MAX : n;
}

int pv_badges_crown(int k, int n, int dir)
{
    int extra = pv_badges_new_count() - n;      /* what the cap left out */
    int i = pv_badges_new_index(k + extra);
    const struct pv_badge *b = pv_badges_get(i);
    unsigned long earned;
    struct pv_theme th;
    char buf[64], fb[64];
    int t;

    if (!b)
        return 0;

    t = tier_of(i);
    tier_theme(&th, t);

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    cpu_boost(true);            /* a full gradient and an anti-aliased shape */
#endif

    pv_fill(&th);
    pv_kicker(22, "ACHIEVEMENT UNLOCKED", tier_col[t]);
    pv_underline(&th, 43);

    pv_crown(&th, PV_W / 2, 92, 64, tier_col[t], tier_hi[t]);

    pv_fit_text(b->name, PV_W - 20, fb, sizeof(fb));
    pv_text_centre(132, fb, tier_hi[t]);

    pv_fit_text(b->desc, PV_W - 20, fb, sizeof(fb));
    pv_text_centre(158, fb, PV_TEXT_DIM);

    earned = pv_badges_when(i);
    if (earned > 0)
    {
        int dy, dm, dd;

        pv_civil_from_days((long)(earned / 86400UL), &dy, &dm, &dd);
        snprintf(buf, sizeof(buf), "earned %s %d %d",
                 pv_month_abbr[dm], dd, dy);
    }
    else
    {
        strlcpy(buf, "earned before records began", sizeof(buf));
    }
    pv_text_centre(186, buf, PV_TEXT_DIM);

    if (extra > 0 && k == n - 1)
    {
        snprintf(buf, sizeof(buf), "+%d more on the wall", extra);
        pv_text_centre(206, buf, PV_TEXT_DIM);
    }

    pv_page_dots(&th, k, n);

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    cpu_boost(false);
#endif

    pv_present(dir);
    return 0;
}

/* ------------------------------------------------------------- the card */

int pv_badges_card(const struct pv_theme *th, int idx, int dir,
                   const struct pv_totals *t)
{
    char buf[64];
    int total = t->badges_total ? t->badges_total : pv_badges_count();
    int done = t->badges_unlocked;
    int pct = total ? (int)((long)done * 100 / total) : 0;

    pv_fill(th);
    pv_kicker(22, "ACHIEVEMENTS", th->accent);
    pv_underline(th, 43);

    snprintf(buf, sizeof(buf), "of %d badges earned", total);
    pv_text_centre(130, buf, th->accent);

    /* The wall in miniature: one pip per badge, lit or not. Twenty across is
     * what fits with room to read them. */
    {
        const int per_row = 20, pitch = 8, r = 2;
        int rows = (total + per_row - 1) / per_row;
        int gx0 = (PV_W - (per_row - 1) * pitch) / 2;
        int gy0 = 153;

        if (rows > 6)
            rows = 6;           /* a taller grid would not fit under the text */

        for (int i = 0; i < rows * per_row && i < total; i++)
        {
            int x = gx0 + (i % per_row) * pitch;
            int y = gy0 + (i / per_row) * pitch;

            if (i < done)
                pv_disc(th, x, y, r, th->accent, 256);
            else
                pv_disc(th, x, y, r, LCD_RGBPACK(255, 255, 255), 50);
        }
    }

    pv_text_centre(208, "SELECT", PV_TEXT_DIM);
    pv_page_dots(th, idx, PV_CARD_COUNT);
    pv_present(dir);

    return pv_animate_percent(th, 64, pct);
}
