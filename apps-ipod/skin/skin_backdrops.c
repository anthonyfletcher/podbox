/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/skin_engine/skin_backdrops.c
 * Copyright (C) 2010 Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * Manages backdrop bitmaps for skins: loading, reference counting and
 * sharing them between screens.
 ****************************************************************************/

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include "core_alloc.h"
#include "string-extra.h"
#include "settings/settings.h"
#include "wps_internals.h"
#include "skin_engine.h"


#define NB_BDROPS SKINNABLE_SCREENS_COUNT*NB_SCREENS
static struct skin_backdrop {
    char name[MAX_PATH];
    char *buffer;
    int buflib_handle;
    int ref_count;
    bool initialized;
    bool loaded;
    bool UNUSED_;
    uint8_t screen;
} backdrops[NB_BDROPS];

static int current_lcd_backdrop[NB_SCREENS];

bool skin_backdrop_get_debug(int index, char **path, int *ref_count, size_t *size)
{

    if (index + 1 >= NB_BDROPS)
        return false;

    *path = backdrops[index].name;
    *ref_count = backdrops[index].ref_count;

    *size = LCD_BACKDROP_BYTES;
    return true;
}

static int buflib_move_callback(int handle, void* current, void* new)
{
    (void)handle;

    for (int i=0; i<NB_BDROPS; i++)
    {
        if (backdrops[i].buffer == current)
        {
            backdrops[i].buffer = new;
            break;
        }
    }
    FOR_NB_SCREENS(n)
        skin_backdrop_show(current_lcd_backdrop[n]);
    return BUFLIB_CB_OK;
}
static struct buflib_callbacks buflib_ops = {buflib_move_callback, NULL, NULL};

bool skin_backdrop_init(void)
{
    bool go_status = !backdrops[0].initialized;
    if (go_status)
    {
        for (int i=0; i<NB_BDROPS; i++)
        {
            backdrops[i].buflib_handle = -1;
            backdrops[i].name[0] = '\0';
            backdrops[i].buffer = NULL;
            backdrops[i].initialized = true;
            backdrops[i].loaded = false;
            backdrops[i].ref_count = 0;
        }
        FOR_NB_SCREENS(i)
            current_lcd_backdrop[i] = -1;
    }
    return go_status;
}

int skin_backdrop_assign(char* backdrop, char *bmpdir,
                         enum screen_type screen)
{
    char filename[MAX_PATH];
    int i, free = -1;
    if (!backdrop)
        return -1;
    if (backdrop[0] == '-')
    {
        filename[0] = '-';
        filename[1] = '\0';
        filename[2] = '\0'; /* we check this later to see if we actually have an
                               image to load. != '\0' means display the image */
    }
    else if (!strcmp(backdrop, BACKDROP_BUFFERNAME))
    {
        strcpy(filename, backdrop);
    }
    else
    {
        get_image_filename(backdrop, bmpdir, filename, sizeof(filename));
    }
    for (i=0; i<NB_BDROPS; i++)
    {
        if (!backdrops[i].name[0] && free < 0)
        {
            free = i;
            break;
        }
        else if (!strcmp(backdrops[i].name, filename))
        {
            backdrops[i].ref_count++;
            break;
        }
    }
    if (free >= 0)
    {
        strmemccpy(backdrops[free].name, filename, MAX_PATH);
        backdrops[free].buffer = NULL;
        (void) screen;
        backdrops[free].ref_count = 1;
        return free;
    }
    else if (i < NB_BDROPS)
        return i;
    return -1;
}

bool skin_backdrops_preload(void)
{
    bool retval = true;
    int i;
    char *filename;
    for (i=0; i<NB_BDROPS; i++)
    {
        if (backdrops[i].name[0] && !backdrops[i].buffer)
        {
            size_t buf_size;

            enum screen_type screen = SCREEN_MAIN;
            buf_size = LCD_BACKDROP_BYTES;

            filename = backdrops[i].name;
            if (screen == SCREEN_MAIN && global_settings.backdrop_file[0] &&
                global_settings.backdrop_file[0] != '-' && filename[0] == '-')
            {
                filename = global_settings.backdrop_file;
            }
            if (*filename && *filename != '-')
            {
                backdrops[i].buflib_handle = core_alloc_ex(buf_size, &buflib_ops);
                if (backdrops[i].buflib_handle > 0)
                {
                    backdrops[i].buffer = core_get_data(backdrops[i].buflib_handle);
                    if (strcmp(filename, BACKDROP_BUFFERNAME))
                    {
                        core_pin(backdrops[i].buflib_handle);
                        backdrops[i].loaded =
                                screens[screen].backdrop_load(filename, backdrops[i].buffer);
                        core_unpin(backdrops[i].buflib_handle);
                        if (!backdrops[i].loaded)
                        {
                            core_free(backdrops[i].buflib_handle);
                            backdrops[i].buflib_handle = -1;
                            retval = false;
                        }
                    }
                    else
                    {
                        backdrops[i].loaded = true;
                        /* Bugfix themes which don't use %VB properly */
                        extern struct frame_buffer_t lcd_framebuffer_default;
                        {
                            memcpy(backdrops[i].buffer,
                                    lcd_framebuffer_default.data, LCD_BACKDROP_BYTES);
                        }
                    }
                }
                else
                    retval = false;
            }
            if (backdrops[i].name[0] == '-' && backdrops[i].loaded)
                backdrops[i].name[2] = '.';
        }
    }
    return retval;
}

void skin_backdrop_set_buffer(int backdrop_id, struct skin_viewport *svp)
{
    if (UNLIKELY(!svp))
        return;
    else if (backdrop_id < 0)
    {
#if 1
        /* ensure the current vp has been removed so it has to be reselected */
        screens[SCREEN_MAIN].set_viewport_ex(NULL, 0);
#endif
        /* WARNING: vp-> buffer is invaid till viewport is set to a screen */
        svp->vp.buffer = NULL; /*Default*/
        return;
    }

    enum screen_type screen = SCREEN_MAIN;
    {
        svp->framebuf.elems = LCD_BACKDROP_BYTES / sizeof(fb_data);
    }
    svp->framebuf.ch_ptr = backdrops[backdrop_id].buffer;
    svp->framebuf.stride = 0; /* default stride */
    svp->framebuf.get_address_fn = NULL; /*Default iterator*/
    screens[screen].viewport_set_buffer(&svp->vp, &svp->framebuf);
}

void skin_backdrop_show(int backdrop_id)
{
    if (backdrop_id < 0)
    {
        screens[0].backdrop_show(NULL);
        current_lcd_backdrop[0] = -1;
        return;
    }
    enum screen_type screen = SCREEN_MAIN;

    if ((backdrops[backdrop_id].loaded == false) ||
        (backdrops[backdrop_id].name[0] == '-' &&
        backdrops[backdrop_id].name[2] == '\0'))
    {
        screens[screen].backdrop_show(NULL);
        current_lcd_backdrop[screen] = -1;
    }
    else if (backdrops[backdrop_id].buffer)
    {
        screens[screen].backdrop_show(backdrops[backdrop_id].buffer);
        current_lcd_backdrop[screen] = backdrop_id;
    }
}

void skin_backdrop_unload(int backdrop_id)
{
    backdrops[backdrop_id].ref_count--;
    if (backdrops[backdrop_id].ref_count <= 0)
    {
        core_free(backdrops[backdrop_id].buflib_handle);
        backdrops[backdrop_id].buffer = NULL;
        backdrops[backdrop_id].buflib_handle = -1;
        backdrops[backdrop_id].loaded = false;
        backdrops[backdrop_id].name[0] = '\0';
        backdrops[backdrop_id].ref_count = 0;
    }
}

void skin_backdrop_load_setting(void)
{
    int i;
    for(i=0;i<SKINNABLE_SCREENS_COUNT*NB_SCREENS;i++)
    {
        if (backdrops[i].name[0] == '-')
        {
            if (global_settings.backdrop_file[0] &&
                global_settings.backdrop_file[0] != '-')
            {
                if (backdrops[i].buflib_handle <= 0)
                {
                    backdrops[i].buflib_handle =
                            core_alloc_ex(LCD_BACKDROP_BYTES, &buflib_ops);
                    if (backdrops[i].buflib_handle <= 0)
                        return;
                }
                bool loaded;
                core_pin(backdrops[i].buflib_handle);
                backdrops[i].buffer = core_get_data(backdrops[i].buflib_handle);
                loaded = screens[SCREEN_MAIN].backdrop_load(
                                                   global_settings.backdrop_file,
                                                   backdrops[i].buffer);
                core_unpin(backdrops[i].buflib_handle);
                backdrops[i].name[2] = loaded ? '.' : '\0';
                backdrops[i].loaded = loaded;
                return;
            }
            else
                backdrops[i].name[2] = '\0';
        }
    }
}
