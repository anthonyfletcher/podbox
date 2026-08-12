/***************************************************************************
 * Original code from RockBox
 * was: apps/screen_access.c
 * Copyright (C) 2005 by Kevin Ferrare
 * GNU General Public License (version 2+)
 *
 * Builds the screens[] array: one struct screen per physical display,
 * wiring each to its LCD driver's drawing functions. The abstraction
 * everything else draws through.
 ****************************************************************************/

#include <stdio.h>
#include "config.h"
#include <lcd.h>
#include "scroll_engine.h"
#include <font.h>
#include <button.h>
#include "settings/settings.h"
#include <kernel.h>
#include "icon_bitmaps.h"

#include "backlight.h"
#include "screen_access.h"
#include "skin/backdrop.h"
#include "viewport.h"

/* some helper functions to calculate metrics on the fly */
static int screen_helper_getcharwidth(void)
{
    return font_get(lcd_getfont())->maxwidth;
}

static int screen_helper_getcharheight(void)
{
    return font_get(lcd_getfont())->height;
}

static int screen_helper_getnblines(void)
{
    int height=screens[0].lcdheight;
    if(global_settings.statusbar != STATUSBAR_OFF)
        height -= STATUSBAR_HEIGHT;
    return height / screens[0].getcharheight();
}

void screen_helper_setfont(int font)
{
    if (font == FONT_UI)
    {
        font = global_status.font_id[SCREEN_MAIN];
        set_ui_font(font);
    }
    lcd_setfont(font);
}

static int screen_helper_getuifont(void)
{
    return global_status.font_id[SCREEN_MAIN];
}

static void screen_helper_setuifont(int font)
{
    global_status.font_id[SCREEN_MAIN] = font;
    set_ui_font(font);
}

static void screen_helper_set_drawmode(int mode)
{
    lcd_set_drawmode(mode);
}

static void screen_helper_put_line(int x, int y, struct line_desc *line,
                                   const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vput_line(&screens[SCREEN_MAIN], x, y, line, fmt, ap);
    va_end(ap);
}

void screen_helper_lcd_viewport_set_buffer(struct viewport *vp, struct frame_buffer_t *buffer)
{
    viewport_set_buffer(vp, buffer, SCREEN_MAIN);
}


struct screen screens[NB_SCREENS] =
{
    {
        .screen_type=SCREEN_MAIN,
        .lcdwidth=LCD_WIDTH,
        .lcdheight=LCD_HEIGHT,
        .depth=LCD_DEPTH,
        .getnblines=&screen_helper_getnblines,
        .is_color=true,
        .pixel_format=LCD_PIXELFORMAT,
        .getcharwidth=screen_helper_getcharwidth,
        .getcharheight=screen_helper_getcharheight,
        .has_disk_led=false,
        .set_drawmode=&screen_helper_set_drawmode,
        .init_viewport=&lcd_init_viewport,
        .set_viewport=&lcd_set_viewport,
        .set_viewport_ex=&lcd_set_viewport_ex,
        .viewport_set_buffer = &screen_helper_lcd_viewport_set_buffer,
        .current_viewport = &lcd_current_viewport,
        .getwidth=&lcd_getwidth,
        .getheight=&lcd_getheight,
        .getstringsize=&lcd_getstringsize,
        .setfont=screen_helper_setfont,
        .getuifont=screen_helper_getuifont,
        .setuifont=screen_helper_setuifont,
        .mono_bitmap=&lcd_mono_bitmap,
        .mono_bitmap_part=&lcd_mono_bitmap_part,
        .bitmap=(screen_bitmap_func*)&lcd_bitmap,
        .bitmap_part=(screen_bitmap_part_func*)&lcd_bitmap_part,
        .transparent_bitmap=(screen_bitmap_func*)&lcd_bitmap_transparent,
        .transparent_bitmap_part=(screen_bitmap_part_func*)&lcd_bitmap_transparent_part,
        .bmp = &lcd_bmp,
        .bmp_part = &lcd_bmp_part,
        .nine_segment_bmp = &lcd_nine_segment_bmp,
        .get_background=&lcd_get_background,
        .get_foreground=&lcd_get_foreground,
        .set_background=&lcd_set_background,
        .set_foreground=&lcd_set_foreground,
        .set_drawinfo = &lcd_set_drawinfo,
        .update_rect=&lcd_update_rect,
        .update_viewport_rect=&lcd_update_viewport_rect,
        .fillrect=&lcd_fillrect,
        .drawrect=&lcd_drawrect,
        .draw_border_viewport=&lcd_draw_border_viewport,
        .fill_viewport=&lcd_fill_viewport,
        .drawpixel=&lcd_drawpixel,
        .drawline=&lcd_drawline,
        .vline=&lcd_vline,
        .hline=&lcd_hline,
        .scroll_step=&lcd_scroll_step,

        .putsxy=&lcd_putsxy,
        .puts=&lcd_puts,
        .putsf=&lcd_putsf,
        .putsxyf=&lcd_putsxyf,
        .puts_scroll=&lcd_puts_scroll,
        .putsxy_scroll_func=&lcd_putsxy_scroll_func,
        .scroll_speed=&lcd_scroll_speed,
        .scroll_delay=&lcd_scroll_delay,
        .clear_display=&lcd_clear_display,
        .clear_viewport=&lcd_clear_viewport,
        .scroll_stop_viewport_rect=&lcd_scroll_stop_viewport_rect,
        .scroll_stop=&lcd_scroll_stop,
        .scroll_stop_viewport=&lcd_scroll_stop_viewport,
        .update=&lcd_update,
        .update_viewport=&lcd_update_viewport,
        .backlight_on=&backlight_on,
        .backlight_off=&backlight_off,
        .is_backlight_on=&is_backlight_on,
        .backlight_set_timeout=&backlight_set_timeout,
        .backdrop_load=&backdrop_load,
        .backdrop_show=&backdrop_show,
        .blendrect = lcd_blendrect,
        .gradient_fillrect = lcd_gradient_fillrect,
        .gradient_fillrect_part = lcd_gradient_fillrect_part,
        .put_line = screen_helper_put_line,
    },
};

void screen_clear_area(struct screen * display, int xstart, int ystart,
                       int width, int height)
{
    display->set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
    display->fillrect(xstart, ystart, width, height);
    display->set_drawmode(DRMODE_SOLID);
}
