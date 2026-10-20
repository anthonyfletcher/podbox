/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The text reel: lines crawling up the whole screen, centred, over a solid
 * fill. Backs the credits list and the About page.
 *
 * Parts: backlight and colours, then the scroll loop. The word wrap is
 * draw/text_wrap.c's.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "input/action.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "settings/settings.h"
#include "backlight.h"
#include "powermgmt.h"
#include "system/shutdown.h"
#include "skin/skin_albumart_color.h" /* dynamic_colors_resolve */
#include "draw/text_wrap.h"
#include "text_reel.h"

/* Text centred across the screen, in the theme's foreground and background
 * colours -- or, while dynamic colours are running, the pair the album art is
 * currently lending them. The column is inset from the side edges so a full
 * line of prose does not run into them; the fill still covers the screen. */
#define TR_PAD 10                          /* inset from the side edges      */
#define TR_W   (LCD_WIDTH - 2 * TR_PAD)    /* text column, lines centred     */

/* Pixels the reel climbs per frame while auto-scrolling, and the frame period.
 * One pixel every ~20ms is a smooth ~50 px/s crawl. */
#define TR_AUTO_STEP   1
#define TR_FRAME_TICK  (HZ / 50 > 0 ? HZ / 50 : 1)

/* Idle time after the last wheel movement before auto-scroll resumes. */
#define TR_RESUME_DELAY  HZ

static void tr_backlight_ignore_timeout(void)
{
    if (global_settings.backlight_timeout > 0)
        backlight_set_timeout(0);
    if (global_settings.backlight_timeout_plugged > 0)
        backlight_set_timeout_plugged(0);
}

static void tr_backlight_use_settings(void)
{
    backlight_set_timeout(global_settings.backlight_timeout);
    backlight_set_timeout_plugged(global_settings.backlight_timeout_plugged);
}

/* Point the reel's colours at the theme's, resolved through the album palette.
 * Called every frame: the palette fades between tracks, so a pair taken once
 * at open would freeze part-way through. */
static void tr_apply_colours(struct viewport *vp)
{
    vp->fg_pattern = dynamic_colors_resolve(global_settings.fg_color);
    vp->bg_pattern = dynamic_colors_resolve(global_settings.bg_color);
}

int text_reel_run(const char *const *lines, unsigned char *heights, int count)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport region, saved, *last;
    int line_h, pitch, total_lines, end_scroll;
    int scroll = 0;
    int i;
    bool manual = false;
    long resume_at = 0;
    int ret = 0;

    tr_backlight_ignore_timeout();

    /* Take the whole screen: theme off so the status bar / SBS backdrop can't
     * repaint over the reel between our own draws. */
    viewportmanager_theme_enable(SCREEN_MAIN, false, &saved);

    /* The crawling text, over the whole screen. Transparent glyphs
     * (DRMODE_FG) over the fill, cleared and redrawn each frame.
     *
     * viewport_set_defaults() rather than viewport_set_fullscreen(): with the
     * theme off the two give the same geometry, but only the former also sets
     * buffer and flags. A stray VP_FLAG_OWNER_UPDATE left in an uninitialised
     * flags field makes every update_viewport() silently transfer nothing --
     * the reel would run with the screen frozen. */
    region.buffer = NULL;
    viewport_set_defaults(&region, SCREEN_MAIN);
    region.x = 0;
    region.y = 0;
    region.width = LCD_WIDTH;
    region.height = LCD_HEIGHT;
    region.font = d->getuifont();
    region.drawmode = DRMODE_FG;
    tr_apply_colours(&region);
    last = d->set_viewport(&region);

    /* Clear once up front. The loop below waits on get_action() before its
     * first draw, and without this whatever was on screen shows through until
     * that returns. */
    d->clear_viewport();
    d->update_viewport();

    d->getstringsize((const unsigned char *)"A", NULL, &line_h);
    pitch = line_h;

    /* Measure how tall each line is once, up front. */
    total_lines = 0;
    for (i = 0; i < count; i++)
    {
        /* an empty line still takes one: it is the gap between sections */
        heights[i] = MAX(text_wrap_count(lines[i], TR_W, region.font), 1);
        total_lines += heights[i];
    }

    /* Line 0 enters from the bottom (top == LCD_HEIGHT at scroll 0); at
     * end_scroll the last line has cleared the top and the screen exits. */
    end_scroll = LCD_HEIGHT + total_lines * pitch;

    while (1)
    {
        int action = get_action(CONTEXT_LIST, TR_FRAME_TICK);
        int cum;

        switch (action)
        {
            case ACTION_STD_CANCEL:
            case ACTION_STD_MENU:
                goto done;

            case ACTION_STD_NEXT:        /* wheel forward: nudge the reel up   */
            case ACTION_STD_NEXTREPEAT:
                manual = true;
                resume_at = current_tick + TR_RESUME_DELAY;
                scroll += pitch;
                break;

            case ACTION_STD_PREV:        /* wheel back: nudge the reel down    */
            case ACTION_STD_PREVREPEAT:
                manual = true;
                resume_at = current_tick + TR_RESUME_DELAY;
                scroll -= pitch;
                break;

            case ACTION_NONE:            /* frame tick: advance the auto-scroll */
                if (manual && TIME_AFTER(current_tick, resume_at))
                    manual = false;
                if (!manual)
                    scroll += TR_AUTO_STEP;
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

        tr_apply_colours(&region);
        d->clear_viewport();
        cum = 0;
        for (i = 0; i < count; i++)
        {
            int top = LCD_HEIGHT + cum * pitch - scroll;

            if (top >= LCD_HEIGHT)       /* below the screen; so are the rest  */
                break;
            if (top + heights[i] * pitch > 0)     /* any part still on screen  */
                text_wrap_draw(d, lines[i], TR_PAD, top, TR_W, 'c');
            cum += heights[i];
        }
        d->update_viewport();
        reset_poweroff_timer();
    }

  done:
    d->set_viewport(last);
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    tr_backlight_use_settings();
    return ret;
}
