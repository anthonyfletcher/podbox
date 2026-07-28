/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The credits screen: scrolls the contributor list generated into
 * credits.raw from docs/CREDITS at build time.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "kernel.h"
#include <string.h>
#include "lcd.h"
#include "input/action.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "settings/settings.h"
#include "backlight.h"
#include "powermgmt.h"
#include "system/shutdown.h"
#include "credits.h"

/* Artwork panel down the left edge, names scrolling in the column to its
 * right over a solid fill. The panel is full screen height, so the column is
 * simply what the artwork leaves behind. */
#include "bitmaps/native/podbox_colors.h"
#include "bitmaps/podboxcredits.h"

#define CR_ART_W BMPWIDTH_podboxcredits    /* artwork panel, at screen left  */
#define CR_X   CR_ART_W                     /* names column: right of the art */
#define CR_W   (LCD_WIDTH - CR_ART_W)       /*   width, text centred          */
#define CR_FG  PODBOX_COLOR_FG             /* scrolling name text            */
#define CR_BG  PODBOX_COLOR_BG             /* solid screen fill              */

static const char* const credits[] = {
#include "credits.raw" /* generated list of names from docs/CREDITS */
};

#define CR_NUMNAMES  ((int)(sizeof(credits) / sizeof(credits[0])))

/* Display lines each name wraps to, measured once per open so scroll positions
 * stay stable. Names are short, so a byte each is plenty. */
static unsigned char cr_lines[sizeof(credits) / sizeof(credits[0])];

/* Pixels the reel climbs per frame while auto-scrolling, and the frame period.
 * One pixel every ~20ms is a smooth ~50 px/s crawl. */
#define CR_AUTO_STEP   1
#define CR_FRAME_TICK  (HZ / 50 > 0 ? HZ / 50 : 1)

/* Idle time after the last wheel movement before auto-scroll resumes. */
#define CR_RESUME_DELAY  HZ

static void cr_backlight_ignore_timeout(void)
{
    if (global_settings.backlight_timeout > 0)
        backlight_set_timeout(0);
    if (global_settings.backlight_timeout_plugged > 0)
        backlight_set_timeout_plugged(0);
}

static void cr_backlight_use_settings(void)
{
    backlight_set_timeout(global_settings.backlight_timeout);
    backlight_set_timeout_plugged(global_settings.backlight_timeout_plugged);
}

/* Greedily word-wraps one name to the column width, returning the number of
 * lines it occupies. When draw is set, each wrapped line is drawn centred at
 * top + line*pitch (lines outside the viewport are skipped). A single word too
 * wide to fit is kept on its own line and left to clip -- there is nowhere else
 * for it to go. */
static int cr_wrap_name(struct screen *d, const char *s, bool draw,
                        int top, int pitch)
{
    char buf[128];
    const char *p = s;
    int lines = 0;

    while (*p)
    {
        const char *line_start;
        const char *fit_end = NULL;   /* end of the last word that fit        */
        const char *q;
        int len, tw;

        while (*p == ' ')
            p++;
        if (!*p)
            break;

        line_start = p;
        q = p;
        while (*q)
        {
            const char *word_end = q;
            while (*word_end && *word_end != ' ')
                word_end++;

            len = word_end - line_start;
            if (len >= (int)sizeof(buf))
                len = sizeof(buf) - 1;
            memcpy(buf, line_start, len);
            buf[len] = '\0';
            d->getstringsize((const unsigned char *)buf, &tw, NULL);

            if (tw <= CR_W || fit_end == NULL)
            {
                fit_end = word_end;      /* this word fits (or must be taken)  */
                q = word_end;
                while (*q == ' ')
                    q++;
            }
            else
                break;                   /* adding this word overflows         */
        }

        if (draw)
        {
            int y = top + lines * pitch;
            if (y > -pitch && y < LCD_HEIGHT)
            {
                len = fit_end - line_start;
                if (len >= (int)sizeof(buf))
                    len = sizeof(buf) - 1;
                memcpy(buf, line_start, len);
                buf[len] = '\0';
                d->getstringsize((const unsigned char *)buf, &tw, NULL);
                d->putsxy((CR_W - tw) / 2, y, (const unsigned char *)buf);
            }
        }

        lines++;
        p = fit_end;
    }

    return lines > 0 ? lines : 1;
}

/* Paints the static backdrop once: the solid fill over the whole screen, then
 * the artwork panel over the left of it. The scrolling region repaints the
 * names each frame over the fill, and is narrow enough that it never clears
 * any part of the panel. */
static void cr_draw_backdrop(struct screen *d)
{
    struct viewport vp, *last;

    vp.buffer = NULL;
    viewport_set_fullscreen(&vp, SCREEN_MAIN);
    vp.bg_pattern = CR_BG;
    last = d->set_viewport(&vp);
    d->clear_viewport();
    d->bmp(&bm_podboxcredits, 0, 0);
    d->update_viewport();
    d->set_viewport(last);
}

int credits_screen(void)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport region, saved, *last;
    int line_h, pitch, total_lines, end_scroll;
    int scroll = 0;
    int i;
    bool manual = false;
    long resume_at = 0;
    int ret = 0;

    cr_backlight_ignore_timeout();

    /* Take the whole screen: theme off so the status bar / SBS backdrop can't
     * repaint over the reel between our own draws. */
    viewportmanager_theme_enable(SCREEN_MAIN, false, &saved);

    cr_draw_backdrop(d);

    /* The scrolling names column. Transparent glyphs (DRMODE_FG) over the fill,
     * cleared and redrawn each frame. */
    region.buffer = NULL;
    viewport_set_defaults(&region, SCREEN_MAIN);
    region.x = CR_X;
    region.y = 0;
    region.width = CR_W;
    region.height = LCD_HEIGHT;
    region.font = d->getuifont();
    region.drawmode = DRMODE_FG;
    region.fg_pattern = CR_FG;
    region.bg_pattern = CR_BG;
    last = d->set_viewport(&region);

    d->getstringsize((const unsigned char *)"A", NULL, &line_h);
    pitch = line_h;

    /* Measure how tall each name is once, up front. */
    total_lines = 0;
    for (i = 0; i < CR_NUMNAMES; i++)
    {
        cr_lines[i] = cr_wrap_name(d, credits[i], false, 0, pitch);
        total_lines += cr_lines[i];
    }

    /* Line 0 enters from the bottom (top == LCD_HEIGHT at scroll 0); at
     * end_scroll the last line has cleared the top and the screen exits. */
    end_scroll = LCD_HEIGHT + total_lines * pitch;

    while (1)
    {
        int action = get_action(CONTEXT_LIST, CR_FRAME_TICK);
        int cum;

        switch (action)
        {
            case ACTION_STD_CANCEL:
            case ACTION_STD_MENU:
                goto done;

            case ACTION_STD_NEXT:        /* wheel forward: nudge the reel up   */
            case ACTION_STD_NEXTREPEAT:
                manual = true;
                resume_at = current_tick + CR_RESUME_DELAY;
                scroll += pitch;
                break;

            case ACTION_STD_PREV:        /* wheel back: nudge the reel down    */
            case ACTION_STD_PREVREPEAT:
                manual = true;
                resume_at = current_tick + CR_RESUME_DELAY;
                scroll -= pitch;
                break;

            case ACTION_NONE:            /* frame tick: advance the auto-scroll */
                if (manual && TIME_AFTER(current_tick, resume_at))
                    manual = false;
                if (!manual)
                    scroll += CR_AUTO_STEP;
                break;

            default:
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                {
                    ret = SYS_USB_CONNECTED;
                    goto done;
                }
                break;
        }

        if (scroll < 0)
            scroll = 0;
        if (scroll > end_scroll)
            scroll = end_scroll;

        /* Auto-exit once the reel has run off the top; manual scrolling to the
         * very end just parks there until auto-scroll resumes and exits. */
        if (!manual && scroll >= end_scroll)
            goto done;

        d->clear_viewport();
        cum = 0;
        for (i = 0; i < CR_NUMNAMES; i++)
        {
            int top = LCD_HEIGHT + cum * pitch - scroll;

            if (top >= LCD_HEIGHT)       /* below the screen; so are the rest  */
                break;
            if (top + cr_lines[i] * pitch > 0)   /* any part still on screen   */
                cr_wrap_name(d, credits[i], true, top, pitch);
            cum += cr_lines[i];
        }
        d->update_viewport();
        reset_poweroff_timer();
    }

  done:
    d->set_viewport(last);
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    cr_backlight_use_settings();
    return ret;
}
