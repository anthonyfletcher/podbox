/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Screen Transitions: the next screen slides in over the one it replaces,
 * whatever theme drew them.
 ****************************************************************************/

/* It works on finished pixels, so no theme has to know about it. Before a
 * screen changes, transition_begin() copies the framebuffer and holds the
 * panel (lcd-slide.h). The next screen draws as it always does, into the
 * framebuffer, and nothing reaches the panel. When that screen first waits
 * for a button, get_action() calls transition_run(), and the slide is sent
 * frame by frame, each row from the copy and from the framebuffer, so the
 * new screen is never disturbed. A screen that takes longer than the hold
 * to draw ends it and simply appears.
 *
 * What slides is the theme's list area, the UI viewport its %Vi gives, or
 * the bounding box of the old and new ones when they differ. Everything
 * outside it, the status bar and any panel, changes at once as the slide
 * starts. A change to or from a screen with no list, such as Now Playing,
 * slides the whole screen. Columns are kept even, as the 5G's BCM takes
 * pixels in pairs.
 *
 * The copy is static: taken from the heap when the setting is switched on,
 * it would come out of the audio buffer and stop the music.
 *
 * Parts: the list area, the slide's frames, then the three calls. */

#include "config.h"
#include <string.h>
#include "lcd.h"
#include "lcd-slide.h"
#include "kernel.h"
#include "system.h"
#include "backlight.h"
#include "settings/settings.h"
#include "speech/language.h"
#include "draw/viewport.h"
#include "draw/transition.h"

#define SLIDE_TICKS (HZ / 5)
/* How long the next screen may take to draw before it shows unslid */
#define HOLD_TICKS  (HZ / 2)

struct area
{
    int x, y, w, h;
};

static fb_data old_screen[LCD_WIDTH * LCD_HEIGHT];
static bool armed;
static bool whole;
static enum transition_dir armed_dir;
static struct area old_area;

/* ---- the list area ------------------------------------------------------ */

static void list_area(struct area *a)
{
    struct viewport vp;
    viewport_set_defaults(&vp, SCREEN_MAIN);

    int x0 = MAX(vp.x, 0) & ~1;
    int x1 = MIN((MIN(vp.x + vp.width, LCD_WIDTH) + 1) & ~1, LCD_WIDTH);
    int y0 = MAX(vp.y, 0);
    int y1 = MIN(vp.y + vp.height, LCD_HEIGHT);

    a->x = x0;
    a->y = y0;
    a->w = MAX(x1 - x0, 0);
    a->h = MAX(y1 - y0, 0);
}

static bool area_empty(const struct area *a)
{
    return a->w <= 0 || a->h <= 0;
}

static void area_union(struct area *a, const struct area *b)
{
    if (area_empty(b))
        return;
    if (area_empty(a))
    {
        *a = *b;
        return;
    }
    int x0 = MIN(a->x, b->x), y0 = MIN(a->y, b->y);
    int x1 = MAX(a->x + a->w, b->x + b->w);
    int y1 = MAX(a->y + a->h, b->y + b->h);
    a->x = x0;
    a->y = y0;
    a->w = x1 - x0;
    a->h = y1 - y0;
}

/* ---- the slide's frames ------------------------------------------------- */

/* The new screen's pixels around the area, sent once as the slide starts.
 * False when the panel cannot show a slide. */
static bool send_outside(const struct area *a, const fb_data *screen)
{
    int right = a->x + a->w, below = a->y + a->h;

    return lcd_slide_rect(0, 0, LCD_WIDTH, a->y, 0, screen, 0, screen, 0)
        && lcd_slide_rect(0, below, LCD_WIDTH, LCD_HEIGHT - below, 0,
                          screen, 0, screen, 0)
        && lcd_slide_rect(0, a->y, a->x, a->h, 0, screen, 0, screen, 0)
        && lcd_slide_rect(right, a->y, LCD_WIDTH - right, a->h, 0,
                          screen, 0, screen, right);
}

/* The new screen `off` columns in. Forward, the old one leaves to the left
 * and the new one follows it in from the right; back, the reverse. */
static void send_frame(const struct area *a, int off, bool forward,
                       const fb_data *screen)
{
    if (forward)
        lcd_slide_rect(a->x, a->y, a->w, a->h, a->w - off,
                       old_screen, a->x + off, screen, a->x);
    else
        lcd_slide_rect(a->x, a->y, a->w, a->h, off,
                       screen, a->x + a->w - off, old_screen, a->x);
}

/* How far in at tick t: an ease out of the third power, most of the way at
 * once and settling at the end */
static int offset_at(int width, int t)
{
    int left = SLIDE_TICKS - t;
    return (width - width * left * left * left
                    / (SLIDE_TICKS * SLIDE_TICKS * SLIDE_TICKS)) & ~1;
}

/* ---- the three calls ---------------------------------------------------- */

void transition_begin(enum transition_dir dir)
{
    /* The panel still shows the first picture taken; later ones may be of a
     * screen half torn down. The latest direction is the way the screen
     * goes. */
    if (armed && lcd_updates_held())
    {
        armed_dir = dir;
        return;
    }
    transition_cancel();
    if (!global_settings.screen_transitions || !is_backlight_on(true))
        return;
#ifdef HAVE_LCD_SLEEP
    if (!lcd_active())
        return;
#endif

    struct viewport *vp = lcd_set_viewport(NULL);
    memcpy(old_screen, FBADDR(0, 0), sizeof(old_screen));
    lcd_set_viewport(vp);
    list_area(&old_area);

    armed_dir = dir;
    armed = true;
    whole = false;
    lcd_hold_updates(HOLD_TICKS);
}

void transition_whole_screen(void)
{
    whole = true;
}

void transition_cancel(void)
{
    if (!armed)
        return;
    armed = false;
    struct viewport *vp = lcd_set_viewport(NULL);
    lcd_release_updates();
    lcd_set_viewport(vp);
}

void transition_run(void)
{
    if (!armed)
        return;
    armed = false;

    struct viewport *vp = lcd_set_viewport(NULL);
    struct area a = { 0, 0, LCD_WIDTH, LCD_HEIGHT };
    if (!whole)
    {
        list_area(&a);
        area_union(&a, &old_area);
    }
    if (!lcd_updates_held() || area_empty(&a))
    {
        lcd_release_updates();
        lcd_set_viewport(vp);
        return;
    }

    const fb_data *screen = FBADDR(0, 0);
    bool forward = (armed_dir == TRANSITION_FORWARD) == !lang_is_rtl();

    /* The slide sends the whole screen. Other threads' updates wait for it,
     * and show as it ends. */
    lcd_hold_updates(SLIDE_TICKS + HOLD_TICKS);
    if (!send_outside(&a, screen))
    {
        lcd_release_updates();
        lcd_update();
        lcd_set_viewport(vp);
        return;
    }

    cpu_boost(true);
    long start = current_tick, now = start;
    int t;
    do
    {
        /* A 5G frame can take longer than a tick */
        do
            yield();
        while (current_tick == now);
        now = current_tick;
        t = MIN(now - start, SLIDE_TICKS);
        send_frame(&a, offset_at(a.w, t), forward, screen);
    }
    while (t < SLIDE_TICKS);
    cpu_boost(false);

    lcd_release_updates();
    lcd_set_viewport(vp);
}
