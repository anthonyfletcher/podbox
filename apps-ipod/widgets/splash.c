/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/splash.c
 * Copyright (C) Daniel Stenberg (2002)
 * GNU General Public License (version 2+)
 *
 * Transient centred message overlay -- splash(), splashf() and the
 * progress variant. Saves and restores whatever it covers.
 ****************************************************************************/
#include "stdarg.h"
#include "string.h"
#include "rbunicode.h"
#include "stdio.h"
#include "kernel.h"
#include "draw/screen_access.h"
#include "lang.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "splash.h"
#include "draw/viewport.h"
#include "draw/text_wrap.h"
#include "draw/progress_bar.h"
#include "font.h"
#include "system/activity.h"
#include "dialog.h"

static long progress_next_tick, talked_tick, progress_last_call;

#define MAXLINES  (LCD_HEIGHT/6)
#define MAXBUFFER 512
#define RECT_SPACING 10     /* inner padding between the box border and content */
#define PROGRESS_SPACING 4  /* extra gap between the text and the progress bar */
#define SPLASH_MEMORY_INTERVAL (HZ)

static bool splash_internal(struct screen * screen, const char *fmt, va_list ap,
                            struct viewport *vp, int addl_lines)
{
    static int max_width[NB_SCREENS] = {2*RECT_SPACING};
    static enum current_activity last_act = ACTIVITY_UNKNOWN;
    enum current_activity act = get_current_activity();

    if (last_act != act) /* changed activities reset max_width */
    {
        FOR_NB_SCREENS(i)
            max_width[i] = 2*RECT_SPACING;
        last_act = act;
    }
    /* prevent screen artifacts by keeping the max width seen */
    int min_width = max_width[screen->screen_type];
    char splash_buf[MAXBUFFER];
    struct splash_lines {
        const char *str;
        size_t len;
    } lines[MAXLINES];
    const char *next;
    int line = 0;
    int y, i;
    int space_w, chr_h;
    int width, height;
    int maxw = min_width - 2*RECT_SPACING;
    int fontnum = vp->font;
    struct font *pf = font_get(fontnum);

    /* the popup is framed with the default dialog style, but keeps its own
     * padding */
    struct dialog_style style = *dialog_get_default_style();
    style.box_margin = RECT_SPACING;

    font_getstringsize(" ", &space_w, &chr_h, fontnum);
    /* Reserve a line for each additional element (e.g. the progress bar),
     * plus an explicit gap so the bar isn't drawn hard against the text. */
    y = chr_h + (addl_lines * chr_h) + (addl_lines ? PROGRESS_SPACING : 0);

    int res = vsnprintf(splash_buf, sizeof(splash_buf), fmt, ap);
    va_end(ap);

    /* A viewport too narrow for a single space, or too short for one line of
     * text, gives the word wrap below nothing to make progress on and it spins
     * forever. Nothing useful could be drawn at that size anyway. */
    if (res <= 0 || vp->width - RECT_SPACING*2 < space_w || vp->height < chr_h)
        return false;

    /* break splash string into display lines, doing proper word wrap */
    next = splash_buf;
    while (*next == ' ' || *next == '\r' || *next == '\n')
        next++;
    if (!*next)
        return false; /* nothing to display */

    font_lock(fontnum, true);
    while (*next)
    {
        const char *end;
        int w;

        if (line > 0)
        {
            if ((y + chr_h * 2 > vp->height) || (line >= MAXLINES))
                break;  /* screen full or out of lines */
            y += chr_h;
        }
        lines[line].str = next;
        next = text_wrap_next(next, vp->width - RECT_SPACING*2, pf, &end);
        lines[line].len = end - lines[line].str;
        w = font_getstringnsize((const unsigned char *)lines[line].str,
                                lines[line].len, NULL, NULL, fontnum);
        if (w > maxw)
            maxw = w;
        line++;
    }
    font_lock(fontnum, false);

    /* prepare viewport
     * First boundaries, then the background filling, then the border and finally
     * the text*/

    screen->scroll_stop();

    width = maxw + 2*RECT_SPACING;
    height = y + 2*RECT_SPACING;

    if (width > vp->width)
        width = vp->width;
    if (height > screen->lcdheight)
        height = screen->lcdheight;

    /* Centre on the physical display, not within vp: a themed SBS can inset the
     * UI viewport (e.g. %Vi(-,3,28,302,-,-)), which would leave the box visibly
     * off-centre on screen. */
    vp->x = (screen->lcdwidth - width) / 2;
    vp->y = (screen->lcdheight - height) / 2;
    vp->width = width;
    vp->height = height;

    /* prevent artifacts by locking to max width observed on repeated calls */
    max_width[screen->screen_type] = width;

    vp->flags |=  VP_FLAG_ALIGN_CENTER;

    /* opaque box + border via the shared dialog frame, which substitutes a
     * legible pair of its own when the theme's is unusable */
    struct viewport content;
    dialog_frame_box(screen, vp, &style, &content);

    /* print the message into the content column, then restore the box
     * viewport: splash_progress draws its progress bar in box
     * coordinates and the caller flushes the box. */
    screen->set_viewport(&content);
    /* DRMODE_FG so glyphs draw over our solid fill without the theme backdrop
     * bleeding into each character's background */
    screen->set_drawmode(DRMODE_FG);
    for(i = 0, y = 0; i < line; i++, y+= chr_h)
    {
        screen->putsxyf(0, y, "%.*s", lines[i].len, lines[i].str);
    }
    screen->set_drawmode(DRMODE_SOLID);
    screen->set_viewport(vp);

    return true; /* needs update */
}

void splashf(int ticks, const char *fmt, ...)
{
    va_list ap;

    /* fmt may be a so called virtual pointer. See settings.h. */
    long id;
    if((id = P2ID((const unsigned char*)fmt)) >= 0)
        /* If fmt specifies a voicefont ID, and voice menus are
           enabled, then speak it. */
        cond_talk_ids_fq(id);

    /* If fmt is a lang ID then get the corresponding string (which
       still might contain % place holders). */
    fmt = P2STR((unsigned char *)fmt);

    /* Text that is not a lang ID only the synthesiser can say. A splash
     * shown for no time is a status redrawn in a loop, so is left alone, as
     * is one whose caller has just said it and forced it to be heard out. */
    if (id < 0 && ticks > 0 && global_settings.talk_menu
        && talk_on_ui_thread() && !talk_force_enqueue_pending())
    {
        char text[128];
        va_start(ap, fmt);
        vsnprintf(text, sizeof(text), fmt, ap);
        va_end(ap);
        talk_text(text, false);
        talk_force_enqueue_next();
    }

    FOR_NB_SCREENS(i)
    {
        struct screen * screen = &(screens[i]);
        struct viewport vp;
        viewport_set_defaults(&vp, screen->screen_type);
        struct viewport *last_vp = screen->set_viewport(&vp);

        va_start(ap, fmt);
        if (splash_internal(screen, fmt, ap, &vp, 0))
            screen->update_viewport();
        va_end(ap);

        screen->set_viewport(last_vp);
    }
    if (ticks)
        sleep(ticks);
}

/* set delay before progress meter is shown */
void splash_progress_set_delay(long delay_ticks)
{
    progress_next_tick = current_tick + delay_ticks;
    talked_tick = 0;
}

/* Hold the meter's own "loading, n per cent" for a caller that announces the
 * same work itself and says more about it. Sticky, like the delay above: it
 * belongs to the run of calls rather than to one of them, and the caller
 * clears it when the run ends. */
static bool progress_silent = false;

void splash_progress_set_silent(bool silent)
{
    progress_silent = silent;
}

/* splash a progress meter */
void splash_progress(int current, int total, const char *fmt, ...)
{
    va_list ap;
    int vp_flag = VP_FLAG_VP_DIRTY;
    /* progress update tick */
    long now = current_tick;
    /* a run of calls with no gap of two seconds is one meter */
    bool new_run = talked_tick == 0
                   || TIME_AFTER(now, progress_last_call + HZ*2);
    progress_last_call = now;

    if (current < total)
    {
        if(TIME_BEFORE(now, progress_next_tick))
            return;
        /* limit to 20fps */
        progress_next_tick = now + HZ/20;
        vp_flag = 0; /* don't mark vp dirty to prevent flashing */
    }

    /* The meter's message, once: a percentage read out every few seconds
     * talks over everything else and says nothing the bar does not. */
    if (global_settings.talk_menu && !progress_silent && new_run)
    {
        long id = P2ID((const unsigned char *)fmt);
        talked_tick = now;
        if (id >= 0)
            talk_id(id, false);
        else
        {
            char text[128];
            va_start(ap, fmt);
            vsnprintf(text, sizeof(text), fmt, ap);
            va_end(ap);
            talk_text(text, false);
        }
    }

    /* If fmt is a lang ID then get the corresponding string (which
       still might contain % place holders). */
    fmt = P2STR((unsigned char *)fmt);
    FOR_NB_SCREENS(i)
    {
        struct screen * screen = &(screens[i]);
        struct viewport vp;
        viewport_set_defaults(&vp, screen->screen_type);
        struct viewport *last_vp = screen->set_viewport_ex(&vp, vp_flag);

        va_start(ap, fmt);
        if (splash_internal(screen, fmt, ap, &vp, 1))
        {
            int size = screen->getcharheight();
            int x = RECT_SPACING;
            int y = vp.height - size - RECT_SPACING;
            int w = vp.width - RECT_SPACING * 2;
            int h = size;
            progress_bar_draw(screen, x, y, w, h, current, total,
                              global_settings.progress_bar_radius);

            screen->update_viewport();
        }
        va_end(ap);

        screen->set_viewport(last_vp);
    }
}
