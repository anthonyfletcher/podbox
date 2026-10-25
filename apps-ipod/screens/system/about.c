/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The About page: the logo, a few lines on what this firmware is and its
 * version, and a QR code to the project page, all in the theme's colours.
 *
 * Parts: the QR code, the page, then the loop.
 ****************************************************************************/

#include <stdio.h>
#include "config.h"
#include "kernel.h"
#include "system.h"
#include "lang.h"
#include "lcd.h"
#include "version.h"
#include "input/action.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "draw/text_wrap.h"
#include "settings/settings.h"
#include "system/shutdown.h"
#include "speech/talk.h"
#include "skin/skin_albumart_color.h" /* dynamic_colors_resolve */
#include "about_art.h"
#include "about.h"

#define AB_PAD     16                    /* from the screen's edges         */
#define AB_GAP     12                    /* between the text and the code   */
#define AB_QR_PX   3                     /* pixels to a module              */
#define AB_QR_QUIET 4                    /* modules of light border         */
#define AB_QR_SIZE ((ABOUT_QR_N + 2 * AB_QR_QUIET) * AB_QR_PX)
#define AB_TEXT_W  (LCD_WIDTH - 2 * AB_PAD - AB_GAP - AB_QR_SIZE)

static const char ab_tagline[] = "Rockbox, reworked for the iPod.";
static const char ab_licence[] = "GPL v2, no warranty.";

static int ab_luma(unsigned c)
{
    return RGB_UNPACK_RED(c) * 77 + RGB_UNPACK_GREEN(c) * 150
           + RGB_UNPACK_BLUE(c) * 29;
}

/* Dark modules on a light square, whichever of the pair is which: a scanner
 * expects that polarity, and a light-on-dark theme would otherwise invert it. */
static void ab_draw_qr(struct screen *d, struct viewport *vp, int x, int y)
{
    unsigned fg = vp->fg_pattern, bg = vp->bg_pattern;
    unsigned light = ab_luma(fg) > ab_luma(bg) ? fg : bg;
    int stride = (ABOUT_QR_N + 7) / 8;

    vp->fg_pattern = light;
    d->fillrect(x, y, AB_QR_SIZE, AB_QR_SIZE);
    vp->fg_pattern = light == fg ? bg : fg;
    x += AB_QR_QUIET * AB_QR_PX;
    y += AB_QR_QUIET * AB_QR_PX;
    for (int r = 0; r < ABOUT_QR_N; r++)
        for (int c = 0; c < ABOUT_QR_N; c++)
            if (about_qr[r * stride + c / 8] & (0x80 >> (c % 8)))
                d->fillrect(x + c * AB_QR_PX, y + r * AB_QR_PX,
                            AB_QR_PX, AB_QR_PX);
    vp->fg_pattern = fg;
}

/* The logo at the top left, then the text beside the code, with the screen's
 * spare height split evenly above, between and below the two. */
static void ab_draw(struct screen *d, struct viewport *vp,
                    const char *const *text, int count, int line_h)
{
    int text_h = (count - 1) * line_h / 2;   /* half a line between items */
    int block, gap, top, y;

    for (int i = 0; i < count; i++)
        text_h += text_wrap_count(text[i], AB_TEXT_W, vp->font) * line_h;
    block = MAX(text_h, AB_QR_SIZE);
    gap = MAX((LCD_HEIGHT - ABOUT_LOGO_H - block) / 3, 0);
    top = 2 * gap + ABOUT_LOGO_H;

    d->clear_viewport();
    d->alpha_bitmap_part(about_logo, 0, 0, ABOUT_LOGO_W, AB_PAD, gap,
                         ABOUT_LOGO_W, ABOUT_LOGO_H);
    ab_draw_qr(d, vp, LCD_WIDTH - AB_PAD - AB_QR_SIZE,
               top + (block - AB_QR_SIZE) / 2);

    y = top + (block - text_h) / 2;
    for (int i = 0; i < count; i++)
        y += text_wrap_draw(d, text[i], AB_PAD, y, AB_TEXT_W, 'l') * line_h
             + line_h / 2;
    d->update_viewport();
}

int about_screen(void)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport vp, saved, *last;
    char version[64];
    const char *text[3];
    unsigned fg = 0, bg = 0;
    bool drawn = false;
    int line_h;
    int ret = 0;

    snprintf(version, sizeof(version), "%s %s", str(LANG_VERSION), rbversion);
    text[0] = ab_tagline;
    text[1] = version;
    text[2] = ab_licence;

    /* The whole screen, as the credits reel takes it: with the theme on, the
     * status bar would repaint over the page. viewport_set_defaults() also
     * sets buffer and flags, which viewport_set_fullscreen() leaves alone. */
    viewportmanager_theme_enable(SCREEN_MAIN, false, &saved);
    vp.buffer = NULL;
    viewport_set_defaults(&vp, SCREEN_MAIN);
    vp.x = 0;
    vp.y = 0;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT;
    vp.font = d->getuifont();
    vp.drawmode = DRMODE_FG;
    last = d->set_viewport(&vp);
    d->getstringsize((const unsigned char *)"A", NULL, &line_h);

    if (global_settings.talk_menu)
    {
        talk_force_shutup();
        talk_text_part(ab_tagline);
        talk_id(LANG_VERSION, true);
        talk_spell(rbversion, true);
        talk_text_part(ab_licence);
    }

    while (1)
    {
        /* Redrawn only when the colours move: they follow the album palette,
         * which fades between tracks. */
        vp.fg_pattern = dynamic_colors_resolve(global_settings.fg_color);
        vp.bg_pattern = dynamic_colors_resolve(global_settings.bg_color);
        if (!drawn || vp.fg_pattern != fg || vp.bg_pattern != bg)
        {
            drawn = true;
            fg = vp.fg_pattern;
            bg = vp.bg_pattern;
            ab_draw(d, &vp, text, 3, line_h);
        }

        int action = get_action(CONTEXT_STD, HZ / 10);
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU
            || action == ACTION_STD_OK)
            break;
        if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = SYS_USB_CONNECTED;
            break;
        }
    }

    talk_shutup();
    d->set_viewport(last);
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    return ret;
}
