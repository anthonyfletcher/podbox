/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/bitmap/list-skinned.c
 * Copyright (C) 2011 by Jonathan Gordon
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * Paints a list using a skin-defined %Vl viewport instead of the core
 * renderer, including album-art rows. Configured per screen by the skin
 * engine.
 ****************************************************************************/

#include "config.h"
#include "system.h"
#include "lcd.h"
#include "font.h"
#include "button.h"
#include "string.h"
#include "settings/settings.h"
#include "kernel.h"
#include "file.h"

#include "input/action.h"
#include "draw/screen_access.h"
#include "list.h"
#include "draw/scrollbar.h"
#include "lang.h"
#include "sound.h"
#include "draw/viewport.h"
#include "skin/statusbar_skinned.h"
#include "skin/skin_engine.h"
#include "skin/skin_display.h"
#include "skin/skin_albumart_color.h"
#include "system/appevents.h"

static struct listitem_viewport_cfg *listcfg[NB_SCREENS] = {NULL};
static struct gui_synclist *current_list;

/* Working copies of the selected row's viewports, one per %Vl in the layout.
 *
 * The selected row cannot draw straight from the skin buffer the way the other
 * rows do: its scrolling lines outlive the draw, and the scroll engine keeps a
 * *pointer* to the viewport rather than a snapshot of it. So each viewport that
 * can hold a live line needs its own address -- sharing one made a later
 * viewport's geometry capture an earlier viewport's scrolling line, which then
 * animated at the wrong y.
 *
 * Static rather than per-cfg because only one list draws at a time per screen,
 * and skinlist_set_cfg() retires the lines when that changes. A layout with
 * more viewports than slots reuses the last one, which is the old behaviour for
 * the overflow only. */
#define SKINLIST_SEL_VPS 12
static struct skin_viewport selected_vps[NB_SCREENS][SKINLIST_SEL_VPS];

static void skinlist_stop_selected_scroll(enum screen_type screen)
{
    for (int i = 0; i < SKINLIST_SEL_VPS; i++)
        screens[screen].scroll_stop_viewport(&selected_vps[screen][i].vp);
}

static int current_row;
static int current_column;
static bool needs_scrollbar[NB_SCREENS];

void skinlist_set_cfg(enum screen_type screen,
                      struct listitem_viewport_cfg *cfg)
{
    if (listcfg[screen] != cfg)
    {
        if (listcfg[screen])
            skinlist_stop_selected_scroll(screen);
        listcfg[screen] = cfg;
        /* the old list's scrollbar must go with it */
        needs_scrollbar[screen] = false;
        current_list = NULL;
        current_column = -1;
        current_row = -1;
    }
}

static bool skinlist_is_configured(enum screen_type screen,
                                    struct gui_synclist *list)
{
    return (listcfg[screen] != NULL) &&
            (!list || (list && list->selected_size == 1));
}
static int current_drawing_line;
static int offset_to_item(int offset, bool wrap)
{
    int item = current_drawing_line + offset;
    if (!current_list || current_list->nb_items == 0)
        return -1;
    if (item < 0)
    {
        if (!wrap)
            return -1;
        else
            item = (item + current_list->nb_items) % current_list->nb_items;
    }
    else if (item >= current_list->nb_items && !wrap)
        return -1;
    else
        item = item % current_list->nb_items;
    return item;
}

int skinlist_get_item_number()
{
    return current_drawing_line;
}

int skinlist_get_item_row()
{
    return current_row;
}

int skinlist_get_item_column()
{
    return current_column;
}


const char* skinlist_get_item_text(int offset, bool wrap, char* buf, size_t buf_size)
{
    int item = offset_to_item(offset, wrap);
    if (item < 0 || !current_list)
        return NULL;
    const char* ret = current_list->callback_get_item_name(
                    item, current_list->data, buf, buf_size);
    return P2STR((unsigned char*)ret);
}

enum themable_icons skinlist_get_item_icon(int offset, bool wrap)
{
    int item = offset_to_item(offset, wrap);
    if (item < 0 || !current_list || current_list->callback_get_item_icon == NULL)
        return Icon_NOICON;
    return current_list->callback_get_item_icon(item, current_list->data);
}

const struct bitmap* skinlist_get_item_albumart(int offset, bool wrap, struct dim *size)
{
    int item = offset_to_item(offset, wrap);
    if (item < 0 || !current_list || current_list->callback_get_item_albumart == NULL)
        return NULL;
    return current_list->callback_get_item_albumart(item, current_list->data, size);
}

/* Whether the current list is an album-art list -- one drawing rows taller than
 * the skin's own pitch, with an album-art callback to fill them. This is what
 * %?La tests, so a theme can lay art rows out differently (art + inset text)
 * from ordinary rows sharing the same list config. It's a list-level property
 * (every row is the same height), so the item offset is irrelevant. */
bool skinlist_item_is_art_row(enum screen_type screen, int offset, bool wrap)
{
    (void)offset; (void)wrap;
    if (!current_list ||
        current_list->callback_get_item_albumart == NULL ||
        listcfg[screen] == NULL || listcfg[screen]->tile)
    {
        return false;
    }
    return list_item_height(current_list, screen) > listcfg[screen]->height;
}

static bool is_selected = false;
bool skinlist_is_selected_item(void)
{
    return is_selected;
}

/* The skin's own row pitch (%Lb height) when a non-tiled skinned list is drawing
 * `list`; -1 otherwise, so list_item_height() falls back to the font height. */
int skinlist_row_height(enum screen_type screen, struct gui_synclist *list)
{
    if (!skinlist_is_configured(screen, list) || listcfg[screen]->tile)
        return -1;
    return listcfg[screen]->height;
}

int skinlist_get_line_count(enum screen_type screen, struct gui_synclist *list)
{
    struct viewport *parent = (list->parent[screen]);
    if (!skinlist_is_configured(screen, list))
        return -1;
    /* Zero is a real answer, and must stay one: a %Vi UI viewport smaller than a
     * single %Lb row fits no rows, and skinlist_draw() hands the list back to
     * the core renderer rather than drawing an empty box (see there). Do not
     * floor this at one to "always draw something" -- that makes the skinned
     * renderer lay out a row against a config that has no room for it, and it
     * faults inside skin_render_viewport(). */
    if (listcfg[screen]->tile == true)
    {
        int rows = (parent->height / listcfg[screen]->height);
        int cols = (parent->width / listcfg[screen]->width);
        return rows*cols;
    }
    /* uniform rows: how many of the list's row height fit in the viewport */
    return parent->height / list_item_height(list, screen);
}

static int current_item;
static int current_nbitems;
bool skinlist_needs_scrollbar(enum screen_type screen)
{
    return needs_scrollbar[screen];
}

void skinlist_get_scrollbar(int* nb_item, int* first_shown, int* last_shown)
{
    if (!skinlist_is_configured(0, NULL))
    {
        *nb_item = 0;
        *first_shown = 0;
        *last_shown = 0;
    }
    else
    {
        *nb_item = current_item;
        *first_shown = 0;
        *last_shown = current_nbitems - 1; /* an index, not a count */
    }
}

bool skinlist_get_item(struct screen *display, struct gui_synclist *list, int x, int y, int *item)
{
    const int screen = display->screen_type;
    if (!skinlist_is_configured(screen, list))
        return false;

    int rh = listcfg[screen]->tile ? listcfg[screen]->height
                                   : list_item_height(list, screen);
    int row = y / rh;
    int column = x / listcfg[screen]->width;
    struct viewport *parent = (list->parent[screen]);
    int cols = (parent->width / listcfg[screen]->width);
    *item = row * cols + column;
    return true;
}

bool skinlist_draw(struct screen *display, struct gui_synclist *list)
{
    int cur_line, display_lines;
    const int screen = display->screen_type;
    struct viewport *parent = (list->parent[screen]);
    char* label = NULL;
    const int list_start_item = list->start_item[screen];
    struct gui_wps wps;
    if (!skinlist_is_configured(screen, list))
        return false;

    /* Re-assert the title, as the unskinned renderer does in list_render.c.
     * toggle_theme() clears it whenever the theme is switched off, which is
     * what any full-screen takeover does, and nothing restores it on the way
     * back -- so without this the list returns with no title and the default
     * icon until something calls gui_synclist_set_title() again. */
    if (list->title)
        sb_set_title_text(list->title, list->title_icon, screen);

    current_list = list;
    dynamic_colors_check_extraction(-1);
    wps.display = display;
    wps.data = listcfg[screen]->data;
    display_lines = skinlist_get_line_count(screen, list);
    /* No room for even one row -- a %Vi shorter than the skin's %Lb pitch. Give
     * the list back to the core renderer (gui_synclist_draw falls through to
     * list_draw when this returns false), which clips a row instead. Drawing an
     * empty box was the old outcome and is no better than the fault that comes
     * of forcing a row through a config with no room for it. */
    if (display_lines < 1)
        return false;
    label = (char *)SKINOFFSETTOPTR(get_skin_buffer(wps.data), listcfg[screen]->label);
    if (!label)
        return false;

    display->set_viewport(parent);
    unsigned int dc_saved_fg = parent->fg_pattern;
    unsigned int dc_saved_bg = parent->bg_pattern;

    /* The parent's colours are already resolved: it is a copy of the skin's UI
     * viewport, which skin_render() resolves in place from its own stored
     * originals. Resolving them again here maps an album-derived colour as
     * though it were a colour the theme had named -- the background comes back
     * turned a second time, and the area no row covers ends up a different
     * colour from the rows themselves.
     *
     * Only where there is no theme is there anything to resolve, because the
     * viewport was then filled from the global settings and nothing else has
     * touched it. Same rule as list_render.c. */
    if (!viewportmanager_theme_enabled(screen))
    {
        parent->fg_pattern = dynamic_colors_resolve(dc_saved_fg);
        parent->bg_pattern = dynamic_colors_resolve(dc_saved_bg);
    }
    display->set_foreground(parent->fg_pattern);
    display->set_background(parent->bg_pattern);
    display->clear_viewport();
    current_item = list->selected_item;
    current_nbitems = list->nb_items;
    needs_scrollbar[screen] = list->nb_items > display_lines;

    const int row_height = list_item_height(list, screen);  /* uniform */
    int row_top = 0;    /* top of the current row, relative to parent */

    /* Tall (album) rows rarely divide the viewport evenly, leaving dead space at
     * the bottom. Centre them in that leftover so the list isn't top-heavy --
     * only for lists taller than the skin's own pitch; ordinary menus stay
     * top-aligned. The offset uses the (uniform) height, so it stays put while
     * scrolling rather than jumping near the list's end. */
    if (row_height > listcfg[screen]->height && list->nb_items > 0)
        row_top = (parent->height % row_height) / 2;

    for (cur_line = 0; cur_line < display_lines; cur_line++)
    {
        struct skin_element* viewport;
        struct skin_viewport* skin_viewport = NULL;
        if (list_start_item+cur_line+1 > list->nb_items)
            break;
        current_drawing_line = list_start_item+cur_line;
        is_selected = list_start_item+cur_line == list->selected_item;
        /* Nth matching %Vl takes the Nth slot, so a viewport keeps the same
         * address from one draw to the next and its scrolling line with it. */
        int sel_slot = 0;

        for (viewport = SKINOFFSETTOPTR(get_skin_buffer(wps.data), listcfg[screen]->data->tree);
             viewport;
             viewport = SKINOFFSETTOPTR(get_skin_buffer(wps.data), viewport->next))
        {
            int original_x, original_y;
            skin_viewport = SKINOFFSETTOPTR(get_skin_buffer(wps.data), viewport->data);
            char *viewport_label = NULL;
            if (skin_viewport)
                viewport_label = SKINOFFSETTOPTR(get_skin_buffer(wps.data), skin_viewport->label);
            if (viewport->children == 0 || !viewport_label ||
                (skin_viewport->label && strcmp(label, viewport_label))
                )
                continue;
            if (is_selected)
            {
                struct skin_viewport *sel = &selected_vps[screen][sel_slot];
                if (sel_slot < SKINLIST_SEL_VPS - 1)
                    sel_slot++;
                /* Retire last draw's line before reusing this slot, so it
                 * cannot animate against the geometry we are about to write. */
                display->scroll_stop_viewport(&sel->vp);
                memcpy(sel, skin_viewport, sizeof(struct skin_viewport));
                skin_viewport = sel;
            }
            original_x = skin_viewport->vp.x;
            original_y = skin_viewport->vp.y;
            if (listcfg[screen]->tile)
            {
                int cols = (parent->width / listcfg[screen]->width);
                current_column = (cur_line)%cols;
                current_row = (cur_line)/cols;

                skin_viewport->vp.x = parent->x + listcfg[screen]->width*current_column + original_x;
                skin_viewport->vp.y = parent->y + listcfg[screen]->height*current_row + original_y;
            }
            else
            {
                current_column = 1;
                current_row = cur_line;
                skin_viewport->vp.x = parent->x + original_x;
                skin_viewport->vp.y = parent->y + original_y + row_top;
                /* row_top is where this row starts; each %Vl keeps its declared
                 * height and positions its elements within the row (e.g. a cover
                 * on the left and the name bar centred beside it), rather than
                 * stretching to fill a taller row. */
            }
            display->set_viewport(&skin_viewport->vp);
            /* Dynamic colors: resolve from stored originals */
            skin_viewport->vp.fg_pattern =
                dynamic_colors_resolve(skin_viewport->dc_orig_fg);
            skin_viewport->vp.bg_pattern =
                dynamic_colors_resolve(skin_viewport->dc_orig_bg);
            display->set_foreground(skin_viewport->vp.fg_pattern);
            display->set_background(skin_viewport->vp.bg_pattern);
            /* Set images to not to be displayed */
            struct skin_token_list *imglist = SKINOFFSETTOPTR(get_skin_buffer(wps.data), wps.data->images);
            while (imglist)
            {
                struct wps_token *token = SKINOFFSETTOPTR(get_skin_buffer(wps.data), imglist->token);
                struct gui_img *img = NULL;
                if (token)
                    img = SKINOFFSETTOPTR(get_skin_buffer(wps.data), token->value.data);
                if (img)
                    img->display = -1;
                imglist = SKINOFFSETTOPTR(get_skin_buffer(wps.data), imglist->next);
            }
            /* The child list holds skinoffset_t, not pointers. Reading it as
             * struct skin_element** takes a pointer's worth of bytes per entry,
             * which on a 64-bit host is twice the stride: the offset comes back
             * with the next entry's bytes stuck on its top half, and rendering
             * it lands far outside the buffer. Same declaration get_child()
             * uses in skin_render.c. On the player the two widths coincide, so
             * the fault only ever showed up in the simulator. */
            OFFSETTYPE(struct skin_element*) *children =
                SKINOFFSETTOPTR(get_skin_buffer(wps.data), viewport->children);
            struct skin_element *first = children ?
                SKINOFFSETTOPTR(get_skin_buffer(wps.data), children[0]) : NULL;
            if (first)
                skin_render_viewport(first, &wps, skin_viewport,
                                     SKIN_REFRESH_ALL);
            wps_display_images(&wps, &skin_viewport->vp);
            /* force disableing scroll because it breaks later */
            if (!is_selected)
            {
                /* skin_viewport points into the skin buffer here (the selected
                 * row works on a copy), so every field we moved must go back */
                display->scroll_stop_viewport(&skin_viewport->vp);
                skin_viewport->vp.x = original_x;
                skin_viewport->vp.y = original_y;
            }
        }
        row_top += row_height;
    }
    current_column = -1;
    current_row = -1;
    parent->fg_pattern = dc_saved_fg;
    parent->bg_pattern = dc_saved_bg;
    display->set_viewport(parent);
    if (!gui_synclist_flush_inhibited())
    {
        if (list_need_full_update() | skin_is_dirty(display->screen_type))
        {
            display->set_viewport(NULL);
            display->update();
            sb_skin_force_next_update();
        }
        else
            display->update_viewport();
    }
    /* A custom scrollbar is drawn by the skin, and nothing else tells the skin
     * that the list moved -- so without this the bar lags behind the selection
     * by however long the status bar's rate limiter holds off the next update.
     * This only lifts the rate limit; it does not force a full refresh. */
    sb_skin_force_next_update();
    current_drawing_line = list->selected_item;
    return true;
}
