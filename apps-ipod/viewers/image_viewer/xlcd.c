/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/lib/xlcd_core.c
 * Framebuffer access and hardware scrolling used to pan a zoomed image.
 * Ported from apps/plugins/lib/xlcd_core.c and xlcd_scroll.c; the plugin API
 * indirection (rb->) is gone and only the >= 8bpp code paths are kept, since
 * the only targets are the 16bpp iPods.
 *
 * Copyright (C) 2005 Jens Arnold
 * GNU General Public License (version 2+)
 *
 * Hardware-assisted LCD block scrolling, used to pan a large image without
 * redrawing it.
 ****************************************************************************/

#include <string.h>
#include "lcd.h"
#include "draw/screen_access.h"
#include "image_viewer.h"

fb_data* get_framebuffer(struct viewport *vp, size_t *stride)
{
    struct viewport *vp_main = *(screens[SCREEN_MAIN].current_viewport);
    if (vp)
        *vp = *vp_main;
    if (stride)
        *stride = vp_main->buffer->stride;
    return vp_main->buffer->fb_ptr;
}


void xlcd_scroll_left(int count)
{
    fb_data *lcd_fb = get_framebuffer(NULL, NULL);
    fb_data *data, *data_end;
    int length, oldmode;

    if ((unsigned)count >= LCD_WIDTH)
    {
        lcd_clear_display();
        return;
    }

    data = lcd_fb;
    data_end = data + LCD_WIDTH*LCD_FBHEIGHT;
    length = LCD_WIDTH - count;

    do
    {
        memmove(data, data + count, length * sizeof(fb_data));
        data += LCD_WIDTH;
    }
    while (data < data_end);

    oldmode = lcd_get_drawmode();
    lcd_set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
    lcd_fillrect(length, 0, count, LCD_HEIGHT);
    lcd_set_drawmode(oldmode);
}

void xlcd_scroll_right(int count)
{
    fb_data *lcd_fb = get_framebuffer(NULL, NULL);
    fb_data *data, *data_end;
    int length, oldmode;

    if ((unsigned)count >= LCD_WIDTH)
    {
        lcd_clear_display();
        return;
    }

    data = lcd_fb;
    data_end = data + LCD_WIDTH*LCD_FBHEIGHT;
    length = LCD_WIDTH - count;

    do
    {
        memmove(data + count, data, length * sizeof(fb_data));
        data += LCD_WIDTH;
    }
    while (data < data_end);

    oldmode = lcd_get_drawmode();
    lcd_set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
    lcd_fillrect(0, 0, count, LCD_HEIGHT);
    lcd_set_drawmode(oldmode);
}

void xlcd_scroll_up(int count)
{
    fb_data *lcd_fb = get_framebuffer(NULL, NULL);
    int length, oldmode;

    if ((unsigned)count >= LCD_HEIGHT)
    {
        lcd_clear_display();
        return;
    }

    length = LCD_HEIGHT - count;

    memmove(lcd_fb,
            lcd_fb + count * LCD_FBWIDTH,
            length * LCD_FBWIDTH * sizeof(fb_data));

    oldmode = lcd_get_drawmode();
    lcd_set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
    lcd_fillrect(0, length, LCD_WIDTH, count);
    lcd_set_drawmode(oldmode);
}

void xlcd_scroll_down(int count)
{
    fb_data *lcd_fb = get_framebuffer(NULL, NULL);
    int length, oldmode;

    if ((unsigned)count >= LCD_HEIGHT)
    {
        lcd_clear_display();
        return;
    }

    length = LCD_HEIGHT - count;

    memmove(lcd_fb + count * LCD_FBWIDTH,
            lcd_fb,
            length * LCD_FBWIDTH * sizeof(fb_data));

    oldmode = lcd_get_drawmode();
    lcd_set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
    lcd_fillrect(0, 0, LCD_WIDTH, count);
    lcd_set_drawmode(oldmode);
}

