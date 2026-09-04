/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/lib/simple_viewer.c
 * Copyright (C) 2010 Teruaki Kawashima
 *
 * Core (non-plugin) full-screen scrollable text viewer, ported from the
 * simple_viewer plugin lib (apps/plugins/lib/simple_viewer.c). Used by the
 * Track Info screen (browse_id3) to display a single tag value full-screen.
 * GNU General Public License (version 2+)
 *
 * Full-screen scrollable display for a string already in memory: word
 * wraps, paginates and scrolls. Distinct from viewers/text_viewer, which
 * streams documents from a file.
 ****************************************************************************/

#include <ctype.h>
#include <string.h>
#include "config.h"
#include "system.h"          /* MIN */
#include "lcd.h"
#include "font.h"
#include "kernel.h"          /* yield, TIMEOUT_BLOCK */
#include "input/action.h"
#include "settings/settings.h"        /* global_settings */
#include "draw/screen_access.h"   /* screens[] */
#include "draw/viewport.h"
#include "rbunicode.h"       /* utf8decode, utf8seek */
#include "diacritic.h"       /* is_diacritic */
#include "draw/scrollbar.h"
#include "skin/skin_albumart_color.h"  /* dynamic_colors_resolve */
#include "system/shutdown.h"
#include "screens/playback/track_info.h"
#include "text_box.h"         /* view_text prototype */

struct view_info {
    struct font* pf;
    struct viewport scrollbar_vp; /* viewport for scrollbar */
    struct viewport vp;
    const char *title;
    const char *text;   /* displayed text */
    int display_lines;  /* number of lines can be displayed */
    int line_count;     /* number of lines */
    int line;           /* current first line */
    int start;          /* possition of first line in text  */
};

static bool isbrchr(const unsigned char *str, int len)
{
    const unsigned char *p = (const unsigned char *)
                             "!,-.:;?　、。！，．：；？―";
    if (isspace(*str))
        return true;

    while(*p)
    {
        int n = utf8seek(p, 1);
        if (len == n && !strncmp((const char *)p, (const char *)str, len))
            return true;
        p += n;
    }
    return false;
}

static const char* get_next_line(const char *text, struct view_info *info)
{
    const char *ptr = text;
    const char *space = NULL;
    int total, n, w;
    total = 0;
    while(*ptr)
    {
        ucschar_t ch;
        n = ((intptr_t)utf8decode(ptr, &ch) - (intptr_t)ptr);
        if (is_diacritic(ch, NULL))
            w = 0;
        else
            w = font_get_width(info->pf, ch);
        if (isbrchr((const unsigned char *)ptr, n))
            space = ptr+(isspace(*ptr) || total + w <= info->vp.width? n: 0);
        if (*ptr == '\n')
        {
            ptr += n;
            break;
        }
        if (total + w > info->vp.width)
            break;
        ptr += n;
        total += w;
    }
    return *ptr && space? space: ptr;
}

static void calc_line_count(struct view_info *info)
{
    const char *ptr = info->text;
    int i = 0;
    bool scrollbar = false;

    while (*ptr)
    {
        ptr = get_next_line(ptr, info);
        i++;
        if (!scrollbar && i > info->display_lines)
        {
            ptr = info->text;
            i = 0;
            info->scrollbar_vp = info->vp;
            info->scrollbar_vp.width = global_settings.scrollbar_width;
            info->vp.width -= info->scrollbar_vp.width;
            if (global_settings.scrollbar != SCROLLBAR_RIGHT)
                info->vp.x = info->scrollbar_vp.width;
            else
                info->scrollbar_vp.x = info->vp.width;
            scrollbar = true;
        }
    }
    info->line_count = i;
}

static void calc_first_line(struct view_info *info, int line)
{
    const char *ptr = info->text;
    int i = 0;

    if (line > info->line_count - info->display_lines)
        line = info->line_count - info->display_lines;
    if (line < 0)
        line = 0;

    if (info->line <= line)
    {
        ptr += info->start;
        i = info->line;
    }
    while (*ptr && i < line)
    {
        ptr = get_next_line(ptr, info);
        i++;
    }
    info->start = ptr - info->text;
    info->line = i;
}

static int init_view(struct view_info *info,
                     const char *title, const char *text)
{
    viewport_set_defaults(&info->vp, SCREEN_MAIN);
    info->pf = font_get(screens[SCREEN_MAIN].getuifont());
    info->display_lines = info->vp.height / info->pf->height;

    info->title = title;
    info->text = text;
    info->line_count = 0;
    info->line = 0;
    info->start = 0;

    /* no title for small screens. */
    if (info->display_lines < 4)
    {
        info->title = NULL;
    }
    else
    {
        info->display_lines--;
        info->vp.y += info->pf->height;
        info->vp.height -= info->pf->height;
    }

    calc_line_count(info);
    return 0;
}

static void draw_text(struct view_info *info)
{
#define OUTPUT_SIZE LCD_WIDTH+1
    static char output[OUTPUT_SIZE];
    const char *text, *ptr;
    int max_show, line;
    struct screen* display = &screens[SCREEN_MAIN];

    /* The colours this page is painted in, taken fresh every draw.
     *
     * Trap: the settings hold the theme's own pair, and what is on screen is
     * whatever the album art has made of them -- viewport_set_fullscreen()
     * copies the settings straight through, so a view built from it paints
     * itself in the static theme while the list it was opened from is
     * dynamic. Resolving is a no-op when the feature is off or no palette has
     * been extracted, so it is unconditional.
     *
     * Every draw rather than once in init_view(), because the album the
     * palette comes from can change while the page is still up. */
    unsigned fg = dynamic_colors_resolve(global_settings.fg_color);
    unsigned bg = dynamic_colors_resolve(global_settings.bg_color);

    /* The ground and the title, through a viewport of this screen's own.
     * clear_display() and the NULL viewport are the *firmware's* default
     * viewport, which carries neither pair and would leave the page a
     * different colour from the text on it. */
    struct viewport all;

    /* buffer and flags before the call, not after: viewport_set_fullscreen()
     * hands straight to lcd_init_viewport(), which reads vp->buffer->elems --
     * on an uninitialised stack viewport that is a garbage pointer and a data
     * abort. Same order as viewport_set_defaults(). */
    all.buffer = NULL;                  /* the default framebuffer */
    all.flags  = VP_DEFAULT_FLAGS;
    viewport_set_fullscreen(&all, SCREEN_MAIN);
    all.fg_pattern = fg;
    all.bg_pattern = bg;
    display->set_viewport(&all);
    display->clear_viewport();

    if(info->title)
        display->puts(0, 0, info->title);

    info->vp.fg_pattern = fg;
    info->vp.bg_pattern = bg;
    info->scrollbar_vp.fg_pattern = fg;
    info->scrollbar_vp.bg_pattern = bg;

    max_show = MIN(info->line_count - info->line, info->display_lines);
    text = info->text + info->start;

    display->set_viewport(&info->vp);
    for (line = 0; line < max_show; line++)
    {
        int len;
        ptr = get_next_line(text, info);
        len = ptr-text;
        while(len > 0 && isspace(text[len-1]))
            len--;
        memcpy(output, text, len);
        output[len] = 0;
        display->puts(0, line, output);
        text = ptr;
    }
    if (info->line_count > info->display_lines)
    {
        display->set_viewport(&info->scrollbar_vp);
        gui_scrollbar_draw(display, (info->scrollbar_vp.width? 0: 1), 0,
                info->scrollbar_vp.width - 1, info->scrollbar_vp.height,
                info->line_count, info->line, info->line + max_show,
                VERTICAL);
    }

    display->set_viewport(NULL);
    display->update();
}

static void scroll_up(struct view_info *info, int n)
{
    if (info->line <= 0)
        return;

    calc_first_line(info, info->line-n);
    draw_text(info);
    yield();
}

static void scroll_down(struct view_info *info, int n)
{
    if (info->line + info->display_lines >= info->line_count)
        return;

    calc_first_line(info, info->line+n);
    draw_text(info);
    yield();
}

static void scroll_to_top(struct view_info *info)
{
    if (info->line <= 0)
        return;

    calc_first_line(info, 0);
    draw_text(info);
}

static void scroll_to_bottom(struct view_info *info)
{
    if (info->line + info->display_lines >= info->line_count)
        return;

    calc_first_line(info, info->line_count - info->display_lines);
    draw_text(info);
}

int view_text(const char *title, const char *text)
{
    struct view_info info;
    int button;

    init_view(&info, title, text);
    draw_text(&info);

    /* wait for keypress */
    while(1)
    {
        button = get_action(CONTEXT_LIST, TIMEOUT_BLOCK);
        switch (button)
        {
        case ACTION_STD_PREV:
        case ACTION_STD_PREVREPEAT:
            scroll_up(&info, 1);
            break;
        case ACTION_STD_NEXT:
        case ACTION_STD_NEXTREPEAT:
            scroll_down(&info, 1);
            break;
        case ACTION_LISTTREE_PGUP:
            scroll_up(&info, info.display_lines);
            break;
        case ACTION_LISTTREE_PGDOWN:
            scroll_down(&info, info.display_lines);
            break;
        /* The two jumps are the two holds: MENU held for the top, SELECT held
         * for the bottom. A MENU tap is left to mean what it means everywhere
         * else. */
        case ACTION_STD_QUICKSCREEN:
            scroll_to_top(&info);
            break;
        case ACTION_STD_CONTEXT:
            scroll_to_bottom(&info);
            break;
        case ACTION_STD_MENU:
            return 1;
        case ACTION_STD_OK:
        case ACTION_STD_CANCEL:
            return 0;
        default:
            if (default_event_handler(button) == SYS_USB_CONNECTED)
                return 1;
            break;
        }
   }

    return 0;
}
