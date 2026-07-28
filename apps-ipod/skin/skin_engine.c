/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/skin_engine/skin_engine.c
 * Copyright (C) 2010 Jonathan Gordon
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * Owns the loaded skins: which skin file is active per screen, loading and
 * freeing them, and the update entry point the rest of the UI calls.
 ****************************************************************************/
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <limits.h>
#include "inttypes.h"
#include "config.h"
#include "input/action.h"
#include "crc32.h"
#include "settings/settings.h"
#include "rbpaths.h"
#include "screens/playback/wps.h"
#include "file.h"
#include "system/activity.h"
#include "widgets/list.h"
#include "skin_engine.h"
#include "skin_buffer.h"
#include "statusbar_skinned.h"
#include "wps_internals.h"
#include "skin_albumart_color.h"

#define FAILSAFENAME "rockbox_failsafe"

void skin_data_free_buflib_allocs(struct wps_data *wps_data);
void playback_release_aa_slot(int slot);
char* wps_default_skin(enum screen_type screen);
char* default_radio_skin(enum screen_type screen);
static bool skins_initialised = false;

struct gui_skin_helper {
    void (*process)(enum screen_type screen, struct wps_data *data, bool preprocess);
    char* (*default_skin)(enum screen_type screen);
    bool load_on_boot;
};

void dummy_process(enum screen_type screen, struct wps_data *data, bool preprocess)
{ (void)screen, (void)data, (void)preprocess; } /* dummy replaces conditionals */

static const struct gui_skin_helper empty_skin_helper = {&dummy_process,NULL,false};
static const struct gui_skin_helper * const skin_helpers[SKINNABLE_SCREENS_COUNT] =
{
#define SKH(proc, def, lob) &((struct gui_skin_helper){proc, def, lob})
    &empty_skin_helper,
    [CUSTOM_STATUSBAR] = SKH(sb_process, sb_create_from_settings, true),
    [WPS] =  SKH(dummy_process, wps_default_skin, true),
};

static struct gui_skin {
    struct gui_wps      gui_wps;
    struct wps_data     data;
    struct skin_stats   stats;
    bool                failsafe_loaded;
    bool                needs_full_update;
} skins[SKINNABLE_SCREENS_COUNT][NB_SCREENS];

int skin_get_num_skins(void)
{
    return SKINNABLE_SCREENS_COUNT;
}

struct skin_stats *skin_get_stats(int number, int screen)
{
    return &skins[number][screen].stats;
}

static void gui_skin_reset(struct gui_skin *skin)
{
    struct wps_data *data;
    skin->failsafe_loaded = false;
    skin->needs_full_update = true;
    skin->gui_wps.data = data = &skin->data;
    struct skin_albumart *aa_save;
    unsigned char *buffer = get_skin_buffer(data);
    /* copy to temp var to protect against memset */
    if (buffer && (aa_save = SKINOFFSETTOPTR(buffer, data->albumart)))
    {
        short old_width, old_height;
        old_width = aa_save->width;
        old_height = aa_save->height;
        memset(data, 0, sizeof(struct wps_data));
        data->last_albumart_width = old_width;
        data->last_albumart_height = old_height;
    }
    else
        memset(data, 0, sizeof(struct wps_data));
    skin->data.wps_loaded = false;
    skin->data.buflib_handle = -1;
    skin->data.tree = -1;
    skin->data.font_ids = -1;
    skin->data.images = -1;
    skin->data.albumart = -1;
    skin->data.playback_aa_slot = -1;
    skin->gui_wps.data->backdrop_id = -1;
}

void gui_sync_skin_init(void)
{
    int j;
    for(j=0; j<SKINNABLE_SCREENS_COUNT; j++)
    {
        FOR_NB_SCREENS(i)
        {
            skin_data_free_buflib_allocs(&skins[j][i].data);
            if (skins[j][i].data.backdrop_id != -1)
                skin_backdrop_unload(skins[j][i].data.backdrop_id);
            gui_skin_reset(&skins[j][i]);
            skins[j][i].gui_wps.display = &screens[i];
        }
    }
    dynamic_colors_init();
}

static void skin_reset_buffers(int item, int screen)
{
    skin_data_free_buflib_allocs(&skins[item][screen].data);
    if (skins[item][screen].data.playback_aa_slot >= 0)
        playback_release_aa_slot(skins[item][screen].data.playback_aa_slot);
    if (skins[item][screen].data.backdrop_id >= 0)
        skin_backdrop_unload(skins[item][screen].data.backdrop_id);
}

void settings_apply_skins(void)
{
    int i;

    /* Save theme colors before re-parsing skins */
    dynamic_colors_save_theme();

    if (audio_status() & AUDIO_STATUS_PLAY)
        audio_stop();

    bool first_run = skin_backdrop_init();
    
    if (!first_run)
    {
        /* Make sure all skins unloaded */
        for (i=0; i<SKINNABLE_SCREENS_COUNT; i++)
        {
            FOR_NB_SCREENS(j)
                skin_reset_buffers(i, j);
        }
    }
    skins_initialised = true;

    /* Make sure each skin is loaded */
    for (i=0; i<SKINNABLE_SCREENS_COUNT; i++)
    {
        FOR_NB_SCREENS(j)
        {
            gui_skin_reset(&skins[i][j]);
            skins[i][j].gui_wps.display = &screens[j];
            if (skin_helpers[i]->load_on_boot)
                skin_get_gwps(i, j);
        }
    }

    /* any backdrop that was loaded with "-" has to be reloaded because
     * the setting may have changed */
    skin_backdrop_load_setting();
    viewportmanager_theme_changed(THEME_STATUSBAR);

    FOR_NB_SCREENS(i)
        skin_backdrop_show(sb_get_backdrop(i));
}

void skin_load(enum skinnable_screens skin, enum screen_type screen,
               const char *buf, bool isfile)
{
    bool loaded = false;

    skin_helpers[skin]->process(screen, &skins[skin][screen].data, true);

    if (buf && *buf)
        loaded = skin_data_load(screen, &skins[skin][screen].data, buf, isfile,
                                &skins[skin][screen].stats);

    if (!loaded && skin_helpers[skin]->default_skin)
    {
        loaded = skin_data_load(screen, &skins[skin][screen].data,
                                skin_helpers[skin]->default_skin(screen), false,
                                &skins[skin][screen].stats);
        skins[skin][screen].failsafe_loaded = loaded;
    }

    skins[skin][screen].needs_full_update = true;
    skin_helpers[skin]->process(screen, &skins[skin][screen].data, false);
    if (loaded)
        skin_backdrops_preload();
}

static char* get_skin_filename(char *buf, size_t buf_size,
                                enum skinnable_screens skin, enum screen_type screen)
{
    (void)screen;
    char *setting = NULL, *ext = NULL;
    switch (skin)
    {
        case CUSTOM_STATUSBAR:
            {
                setting = global_settings.sbs_file;
                ext = "sbs";
            }
            break;
        case WPS:
            {
                setting = global_settings.wps_file;
                ext = "wps";
            }
            break;
        default:
            return NULL;
    }

    buf[0] = '\0'; /* force it to reload the default */
    if (strcmp(setting, FAILSAFENAME) && strcmp(setting, "-"))
    {
        snprintf(buf, buf_size, WPS_DIR "/%s.%s", setting, ext);
    }
    return buf;
}

struct gui_wps *skin_get_gwps(enum skinnable_screens skin, enum screen_type screen)
{
    if (skin == CUSTOM_STATUSBAR && !skins_initialised)
        return &skins[skin][screen].gui_wps;

    if (skins[skin][screen].data.wps_loaded == false)
    {
        char filename[MAX_PATH];
        char *buf = get_skin_filename(filename, MAX_PATH, skin, screen);
        cpu_boost(true);
        skin_load(skin, screen, buf, true);
        cpu_boost(false);
    }
    return &skins[skin][screen].gui_wps;
}

/* This is called to find out if we the screen needs a full update.
 * if true you MUST do a full update as the next call will return false */
bool skin_do_full_update(enum skinnable_screens skin,
                            enum screen_type screen)
{
    struct viewport *vp = *(screens[screen].current_viewport);

    bool vp_is_dirty = ((vp->flags & VP_FLAG_VP_SET_CLEAN) == VP_FLAG_VP_DIRTY) &&
                       get_current_activity() == ACTIVITY_WPS;

    bool ret = (skins[skin][screen].needs_full_update || vp_is_dirty);
    skins[skin][screen].needs_full_update = false;
    return ret;
}

/* tell a skin to do a full update next time */
void skin_request_full_update(enum skinnable_screens skin)
{
    FOR_NB_SCREENS(i)
        skins[skin][i].needs_full_update = true;
}

/* the hold switch moved; redraw the statusbar so it shows the new state.
 * Called from firmware/backlight.c. */
void skin_request_update_locked(bool locked)
{
    if (get_current_activity() == ACTIVITY_WPS)
        return;

    sb_skin_force_next_update();

    /* a theme may draw a lock screen over the UI viewport. Unlocking has to
     * repaint the whole statusbar skin or the viewport stays hidden until
     * the next button press. */
    if (!locked)
        skin_request_full_update(CUSTOM_STATUSBAR);
#ifdef HAS_BUTTON_HOLD
    button_queue_post(BUTTON_NONE, 0);
#endif
}

bool dbg_skin_engine(void)
{
    struct simplelist_info info;
    int i, total = 0;
    int ref_count;
    char *path;
    size_t bytes;
    int path_prefix_len = strlen(ROCKBOX_DIR "/wps/");
    simplelist_info_init(&info, "Skin engine usage", 0, NULL);
    simplelist_reset_lines();
    FOR_NB_SCREENS(j) {
        for (i = 0; i < skin_get_num_skins(); i++) {
            struct skin_stats *stats = skin_get_stats(i, j);
            if (stats->buflib_handles)
            {
                simplelist_addline("Skin ID: %d, %zd allocations",
                        i, stats->buflib_handles);
                simplelist_addline("\t%s: %zd bytes",
                        "Skin", stats->browser_size);
                simplelist_addline("\t%s: %zd bytes",
                        "Images", stats->images_size);
                simplelist_addline("\t%s: %zd bytes",
                        "Total", stats->browser_size + stats->images_size);
                total += stats->browser_size + stats->images_size;
            }
        }
    }
    simplelist_addline("%s usage: %d bytes", "Skin total", total);
    simplelist_setline("Backdrop Images:");
    i = 0;
    while (skin_backdrop_get_debug(i++, &path, &ref_count, &bytes)) {
        if (ref_count > 0) {

            if (!strncasecmp(path, ROCKBOX_DIR "/wps/", path_prefix_len))
                path += path_prefix_len;
            simplelist_addline("%s", path);
            simplelist_addline("\tref_count: %d", ref_count);
            simplelist_addline("\tsize: %zd", bytes);
            total += bytes;
        }
    }
    simplelist_addline("%s usage: %d bytes", "Total", total);
    return simplelist_show_list(&info);
}
