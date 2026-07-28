/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/list.h
 * Copyright (C) 2005 by Kevin Ferrare
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * Interface to the list widget -- gui_synclist and the callback types
 * every list screen implements.
 ****************************************************************************/

#ifndef _GUI_LIST_H_
#define _GUI_LIST_H_

#include "config.h"
#include "input/action.h"
#include "draw/icon.h"
#include "draw/screen_access.h"
#include "skin/skin_engine.h"
#include "draw/line.h"

#define SCROLLBAR_WIDTH global_settings.scrollbar_width

enum synclist_cursor
{
    SYNCLIST_CURSOR_NOSTYLE = 0,
    SYNCLIST_CURSOR_INVERT,
    SYNCLIST_CURSOR_COLOR,
    SYNCLIST_CURSOR_GRADIENT,
};

/*
 * The gui_list is based on callback functions, if you want the list
 * to display something you have to provide it a function that
 * tells it what to display.
 * There are three callback function :
 * one to get the text, one to get the icon and one to get the color
 */

/*
 * Icon callback
 *  - selected_item : an integer that tells the number of the item to display
 *  - data : a void pointer to the data you gave to the list when you
 *           initialized it
 *  Returns a pointer to the icon, the value inside it is used to display the
 *          icon after the function returns.
 * Note : we use the ICON type because the real type depends of the plateform
 */
typedef enum themable_icons list_get_icon(int selected_item, void * data);
/*
 * Album art callback
 *  - selected_item : an integer that tells the number of the item to display
 *  - data : a void pointer to the data you gave to the list when you
 *           initialized it
 *  - size : the requested thumbnail dimensions to decode/scale to
 *  Returns a pointer to a ready-to-draw bitmap, or NULL if this item has
 *          no album art (e.g. it isn't an album row, or none was found).
 *  Note: the returned pointer is only valid until the next call into the
 *        callback's backing cache, so it must be drawn immediately.
 */
typedef const struct bitmap * list_get_albumart(int selected_item, void * data,
                                                struct dim *size);
/*
 * Text callback
 *  - selected_item : an integer that tells the number of the item to display
 *  - data : a void pointer to the data you gave to the list when you
 *           initialized it
 *  - buffer : a buffer to put the resulting text on it
 *             (The content of the buffer may not be used by the list, we use
 *             the return value of the function in all cases to avoid filling
 *             a buffer when it's not necessary)
 *  - buffer_len : length of the buffer
 * Returns a pointer to a string that contains the text to display
 */
typedef const char * list_get_name(int selected_item, void * data,
                                   char * buffer, size_t buffer_len);
/*
 * Draw callback
 *  - display : functions supplied depends on the screen call originated from (typ: MAIN)
 *  - list_info : a pointer to an internal struct containing item display information
 */
/* owner drawn lists need to know this info */
struct list_putlineinfo_t {
    int x;
    int y;
    int item_indent;
    int item_offset;
    int line;

    int icon;
    int icon_width;

    struct screen *display;
    struct viewport *vp;
    struct line_desc *linedes;
    struct gui_synclist * list;
    const char *dsp_text;

    bool is_selected;
    bool is_title;
    bool show_cursor;
    bool have_icons;
};

typedef void list_draw_item(struct list_putlineinfo_t *list_info);
/*
 * Voice callback
 *  - selected_item : an integer that tells the number of the item to speak
 *  - data : a void pointer to the data you gave to the list when you
 *           initialized it
 * Returns an integer, 0 means success, ignored really...
 */
typedef int list_speak_item(int selected_item, void * data);
/*
 * Color callback
 *  - selected_item : an integer that tells the number of the item to display
 *  - data : a void pointer to the data you gave to the list when you
 *           initialized it
 *  Returns an int with the lower 16 bits representing the color to display the
 *          selected item, negative value for default coloring.
 */
typedef int list_get_color(int selected_item, void * data);

struct list_selection_color
{
    /* text color, in native lcd format
     * (convert with LCD_RGBPACK() if necessary) */
    unsigned text_color;
    /* only STYLE_GRADIENT supported set line_color & line_end_color the same
     * for solid color, in native
     * lcd format (convert with LCD_RGBPACK() if necessary) */
    unsigned line_color;
    unsigned line_end_color;
    /* viewport foreground and background, in native
     * lcd format (convert with LCD_RGBPACK() if necessary) */
    unsigned fg_color;
    unsigned bg_color;
    /* To enable:
     * call gui_synclist_set_sel_color(gui_synclist*, list_selection_color*)
     * If using the default viewport you should call
     * gui_synclist_set_sel_color(gui_synclist*, NULL) when finished */
};

struct gui_synclist
{
    /*flags to hold settings show: icons, scrollbar etc..*/
    int scrollbar;
    int cursor_style;
    bool show_icons;
    bool keyclick;
    bool talk_menu;
    bool wraparound;
    bool scroll_paginated;
    /* whether the text of the whole items of the list have to be
     * scrolled or only for the selected item */
    bool scroll_all;
    int nb_items;
    int selected_item;

    int start_item[NB_SCREENS]; /* the item that is displayed at the top of the screen */
    /* the number of lines that are selected at the same time */
    int selected_size;
    /* the number of pixels each line occupies (including optional padding on touchscreen */
    int line_height[NB_SCREENS];
    int offset_position[NB_SCREENS]; /* the list's screen scroll placement in pixels */
    long scheduled_talk_tick, last_talked_tick, dirty_tick;

    list_get_icon *callback_get_item_icon;
    list_get_albumart *callback_get_item_albumart;
    /* Uniform row height override in pixels; 0 == use the skin/font height.
     * Lets a list (e.g. the database album list) ask for taller rows without a
     * per-item callback -- every row is this height. */
    int line_height_override;
    list_get_name *callback_get_item_name;
    list_speak_item *callback_speak_item;
    list_draw_item *callback_draw_item;

    /* The data that will be passed to the callback function YOU implement */
    void * data;
    /* The optional title, set to NULL for none */
    const char * title;
    /* Optional title icon */
    enum themable_icons title_icon;

    int title_color;
    list_get_color *callback_get_item_color;
    struct list_selection_color *selection_color;
    struct viewport *parent[NB_SCREENS];

};


extern void list_init(void);

extern void gui_synclist_init(
    struct gui_synclist * lists,
    list_get_name callback_get_item_name,
    void * data,
    bool scroll_all,
    int selected_size,
    struct viewport parent[NB_SCREENS] /* NOTE: new screens should NOT set this to NULL */
    );
extern void gui_synclist_set_nb_items(struct gui_synclist * lists, int nb_items);
extern void gui_synclist_set_icon_callback(struct gui_synclist * lists, list_get_icon icon_callback);
extern void gui_synclist_set_albumart_callback(struct gui_synclist * lists, list_get_albumart albumart_callback);
/* Uniform row height in pixels; 0 restores the default skin/font height. */
extern void gui_synclist_set_row_height(struct gui_synclist * lists, int height);
/* The list's uniform row height (the override if set, else the skin/font height). */
int list_item_height(struct gui_synclist *list, enum screen_type screen);
extern void gui_synclist_set_voice_callback(struct gui_synclist * lists, list_speak_item voice_callback);
extern void gui_synclist_set_viewport_defaults(struct viewport *vp, enum screen_type screen);
extern void gui_synclist_set_color_callback(struct gui_synclist * lists, list_get_color color_callback);
extern void gui_synclist_set_sel_color(struct gui_synclist * lists, struct list_selection_color *list_sel_color);
extern void gui_synclist_speak_item(struct gui_synclist * lists);
extern int gui_synclist_get_nb_items(struct gui_synclist * lists);

extern int  gui_synclist_get_sel_pos(struct gui_synclist * lists);

extern void gui_synclist_draw(struct gui_synclist * lists);
/* Suppress the LCD flush of the next list draw(s) -- render only, no update. */
void gui_synclist_inhibit_flush(bool inhibit);
bool gui_synclist_flush_inhibited(void);
extern bool list_need_full_update(void);
extern void gui_synclist_scroll_stop(struct gui_synclist *lists);
extern void gui_synclist_select_item(struct gui_synclist * lists,
                                     int item_number);
/* Force the row shown at the top of the screen, overriding the scroll margin
 * gui_synclist_select_item() applies. Call it after selecting; it holds only
 * until the selection next moves. */
extern void gui_synclist_set_top_item(struct gui_synclist * lists,
                                      int item_number);
extern void gui_synclist_add_item(struct gui_synclist * lists);
extern void gui_synclist_del_item(struct gui_synclist * lists);
extern void gui_synclist_set_title(struct gui_synclist * lists, const char * title,
                                   enum themable_icons icon);

extern bool gui_synclist_keyclick_callback(int action, void* data);
/*
 * Do the action implied by the given button,
 * returns true if the action was handled.
 * NOTE: *action may be changed regardless of return value
 */
extern bool gui_synclist_do_button(struct gui_synclist * lists, int *action);
struct listitem_viewport_cfg;

bool skinlist_get_item(struct screen *display, struct gui_synclist *list, int x, int y, int *item);
bool skinlist_draw(struct screen *display, struct gui_synclist *list);
bool skinlist_is_selected_item(void);
void skinlist_set_cfg(enum screen_type screen,
                      struct listitem_viewport_cfg *cfg);
const char* skinlist_get_item_text(int offset, bool wrap, char* buf, size_t buf_size);
int skinlist_get_item_number(void);
int skinlist_get_item_row(void);
int skinlist_get_item_column(void);
enum themable_icons skinlist_get_item_icon(int offset, bool wrap);
const struct bitmap* skinlist_get_item_albumart(int offset, bool wrap, struct dim *size);
/* True when the current skinned row is one drawn taller than the list config's
 * pitch (an album-art row). Backs the %?La conditional. */
bool skinlist_item_is_art_row(enum screen_type screen, int offset, bool wrap);
bool skinlist_needs_scrollbar(enum screen_type screen);
void skinlist_get_scrollbar(int* nb_item, int* first_shown, int* last_shown);
int skinlist_get_line_count(enum screen_type screen, struct gui_synclist *list);
/* The skin's own row pitch (%Lb) when a non-tiled skinned list is drawing
 * `list`; -1 otherwise, so the caller falls back to the font height. */
int skinlist_row_height(enum screen_type screen, struct gui_synclist *list);


/* If the list has a pending postponed scheduled announcement, that
   may become due before the next get_action tmieout. This function
   adjusts the get_action timeout appropriately. */
extern int list_do_action_timeout(struct gui_synclist *lists, int timeout);
/* This one combines a get_action call (with timeout overridden by
   list_do_action_timeout) with the gui_synclist_do_button call, for
   convenience. */
extern bool list_do_action(int context, int timeout,
                           struct gui_synclist *lists, int *action);


/** Simplelist implementation.
    USe this if you dont need to reimplement the list code,
    and just need to show a list
 **/

struct simplelist_info {
    const char *title; /* title to show on the list */
    int  count; /* number of items in the list, each item is selection_size high */
    int selection_size; /* list selection size, usually 1 */
    bool scroll_all;
    bool hide_theme;
    bool speak_onshow; /* list speaks first item or 'empty list' */
    int  timeout;
    int  selection; /* the item to select when the list is first displayed */
                    /* when the list is exited, this will be set to the
                       index of the last item selected, or -1 if the list
                       was exited with ACTION_STD_CANCEL                  */
    int (*action_callback)(int action, struct gui_synclist *lists); /* can be NULL */
        /* action_callback notes:
            action == the action pressed by the user
                _after_ gui_synclist_do_button returns.
            lists == the lists struct so the callback can get selection and count etc. */
    enum themable_icons title_icon;
    list_get_icon *get_icon; /* can be NULL */
    list_get_name *get_name; /* NULL if you're using simplelist_addline() */
    list_speak_item *get_talk; /* can be NULL to not speak */
    list_get_color *get_color;
    struct list_selection_color *selection_color;
    void *callback_data; /* data for callbacks */
};

#define SIMPLELIST_MAX_LINES 32
#define SIMPLELIST_MAX_LINELENGTH 48

/** The next three functions are used if the text is mostly static.
    These should be called in the action callback for the list.
 **/
/* reset the amount of lines shown in the list to 0
   Only needed if simplelist_info.get_name == NULL */
void simplelist_reset_lines(void);
/* get the current amount of lines shown */
int simplelist_get_line_count(void);
/* add a line in the list. */
void simplelist_addline(const char *fmt, ...) ATTRIBUTE_PRINTF(1,2);
void simplelist_setline(const char *text);
/* setup the info struct. members not setup in this function need to be assigned manually
   members set in this function:
    info.selection_size = 1;
    info.scroll_all = false;
    info.hide_theme = false;
    info.speak_onshow = true;
    info.action_callback = NULL;
    info.title_icon = Icon_NOICON;
    info.get_icon = NULL;
    info.get_name = NULL;
    info.get_voice = NULL;
    info.get_color = NULL;
    info.list_selection_color = NULL;
    info.timeout = HZ/10;
    info.selection = 0;
*/
void simplelist_info_init(struct simplelist_info *info, char* title,
                          int count, void* data);

/* show a list.
   if list->action_callback != NULL it is called with the action ACTION_REDRAW
    before the list is dislplayed for the first time */
bool simplelist_show_list(struct simplelist_info *info);

#endif /* _GUI_LIST_H_ */
