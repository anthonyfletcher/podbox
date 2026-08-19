/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/skin_engine/skin_render.c
 * $Id: skin_parser.c 26752 2010-06-10 21:22:16Z bieber $
 *
 * Copyright (C) 2010 Jonathan Gordon
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * Walks the parsed element tree and draws it: viewports, conditionals,
 * alternators and progress bars, resolving each tag as it goes.
 *
 * The counterpart to skin_parser.c: that file builds the tree, this one
 * executes it, once per repaint. It recurses through the tree structure, so a
 * conditional's branches and a viewport's contents are walked by the same
 * code as the top level.
 *
 * Rendering is two passes over each line. The first evaluates tags into a
 * text buffer and notes what is dynamic; the second draws. That split is what
 * lets a line containing a scrolling tag be handed to the scroller as a whole
 * rather than redrawn piecemeal.
 *
 * Same offset rule as the parser: the tree is in a movable buflib block, so
 * everything is reached through SKINOFFSETTOPTR rather than by pointer.
 *
 * Parts, in order:
 *   - the render state carried down the walk
 *   - evaluating conditionals and alternators to pick a branch
 *   - building a line's text from its tags
 *   - drawing a line, including the scrolling case
 *   - viewport handling and the top-level skin_render() entry points
 ****************************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include "strlcat.h"

#include "config.h"
#include "core_alloc.h"
#include "kernel.h"
#include "system/appevents.h"
#include "metadata/albumart.h"
#include "settings/settings.h"
#include "skin_display.h"
#include "skin_engine.h"
#include "skin_parser.h"
#include "skin_buffer.h"
#include "tag_table.h"
#include "skin_scan.h"
#include "draw/viewport.h"
#include "draw/round_rect.h"
#include "metadata/cuesheet.h"
#include "speech/language.h"
#include "audio/playback.h"
#include "audio/spectrum_meter.h"
#include "playlist/playlist.h"
#include "root_menu.h"
#include "system/activity.h"
#include "widgets/list.h"
#include "screens/playback/wps.h"
#include "strmemccpy.h"
#include "skin_albumart_color.h"
#include "font.h"
#include "rbunicode.h"
#include "diacritic.h"
#include "custom_tokens.h"

#define MAX_LINE 1024

/* Tinted %dr rectangles held back per viewport; see pending_blend below. Beyond
 * this many in one viewport the extras are dropped rather than mis-ordered. */
#define SKIN_MAX_PENDING_BLENDS 8

struct skin_draw_info {
    struct gui_wps *gwps;
    struct skin_viewport *skin_vp;
    int line_number;
    unsigned long refresh_type;
    struct line_desc line_desc;

    char* cur_align_start;
    struct align_pos align;
    bool no_line_break;
    bool line_scrolls;
    bool force_redraw;

    char *buf;
    size_t buf_size;

    int offset; /* used by the playlist viewer */

    /* List album art (%La) is blitted like text tokens are, i.e. before the
     * line's write_line() runs -- but write_line() clears the line background
     * first, which would wipe the top of the cover. So stash it here and blit
     * it AFTER write_line(), the same way real images are drawn after the line
     * loop. NULL when the current line has no album art. */
    const struct bitmap *pending_aa;
    const struct listitem *pending_aa_item;   /* the %La that asked for it */
    /* Anything in this viewport actually reached the framebuffer, so the
     * viewport's rectangle has to be flushed. See do_non_text_tags(). */
    bool drew;

    /* Same problem one step further out. A tinted %dr composites with whatever
     * is under it, and the album art and images are drawn once for the whole
     * viewport by wps_display_images() -- after every line. Drawn where the tag
     * sits, a tint would land under the art and vanish. Queued here instead and
     * flushed after wps_display_images().
     *
     * So a tint only ever covers content from its OWN viewport. The viewport's
     * clear_viewport() fills with bg_pattern when drawing into the backdrop
     * buffer (lcd-16bit-common.c), which destroys whatever an earlier viewport
     * put there -- so a tint in a viewport of its own has nothing to blend
     * with. It belongs in the same viewport as the art it darkens. */
    const struct draw_rectangle *pending_blend[SKIN_MAX_PENDING_BLENDS];
    int pending_blend_count;
};

extern void sb_set_info_vp(enum screen_type screen, OFFSETTYPE(char*) label);

typedef bool (*skin_render_func)(struct skin_element* alternator, struct skin_draw_info *info);
bool skin_render_alternator(struct skin_element* alternator, struct skin_draw_info *info);

static void skin_render_playlistviewer(struct playlistviewer* viewer,
                                       struct gui_wps *gwps,
                                       struct skin_viewport* skin_viewport,
                                       unsigned long refresh_type);

static char* skin_buffer;

static inline struct skin_element*
get_child(OFFSETTYPE(struct skin_element**) children, int child)
{
    OFFSETTYPE(struct skin_element*) *kids = SKINOFFSETTOPTR(skin_buffer, children);
    return SKINOFFSETTOPTR(skin_buffer, kids[child]);
}

/* The deferred list-row cover, rounded if its %La asked for that. By this
 * point write_line() has drawn the row's background, so a rounded corner
 * blends with the row rather than with whatever was there before it. Only a
 * plain native bitmap can take the rounded path -- a mono or already-alpha'd
 * one needs bmp_part()'s own dispatch. */
static void draw_pending_aa(struct screen *display,
                            const struct viewport *vp,
                            const struct skin_draw_info *info)
{
    const struct bitmap *bmp = info->pending_aa;
    const struct listitem *li = info->pending_aa_item;
    int aw = MIN(bmp->width, vp->width);
    int ah = MIN(bmp->height, vp->height);

    if (li && li->radius && bmp->format == FORMAT_NATIVE && !bmp->alpha_offset)
    {
        display->set_drawmode(DRMODE_FG);
        bitmap_part_round(display, (const fb_data *)bmp->data,
                          LCD_FBSTRIDE(bmp->width, bmp->height),
                          0, 0, aw, ah, li->radius,
                          SKINOFFSETTOPTR(skin_buffer, li->mask));
    }
    else
    {
        display->set_drawmode(DRMODE_SOLID);
        display->bmp_part(bmp, 0, 0, 0, 0, aw, ah);
    }
    display->set_drawmode(DRMODE_SOLID);
}

/* A %dr given a corner radius, drawn through the coverage mask cut for it when
 * the skin was parsed. The corners blend with what is under them, which is
 * what DRMODE_FG means here; the caller has already set the foreground. */
static void draw_rounded_rect(struct screen *display,
                              const struct draw_rectangle *rect)
{
    const unsigned char *mask = SKINOFFSETTOPTR(skin_buffer, rect->mask);

    display->set_drawmode(DRMODE_FG);
    fill_round_rect_aa(display, rect->x, rect->y, rect->width, rect->height,
                       rect->radius, mask, rect->opacity);
    display->set_drawmode(DRMODE_SOLID);
}


#define WT_MAX_LINES 8
#define WT_BUF 512

/* Truncate line so line + "..." fits maxwidth, then append the "...". */
static void wt_ellipsize(char *line, int size, struct font *pf, int maxwidth)
{
    int ellw = 3 * font_get_width(pf, '.');
    int width = 0;
    unsigned char *p = (unsigned char *)line;
    unsigned char *cut = p;
    ucschar_t ch;

    while (*p)
    {
        unsigned char *prev = p;
        p = (unsigned char *)utf8decode(p, &ch);
        if (IS_DIACRITIC(ch))
            continue;
        if (width + font_get_width(pf, ch) + ellw > maxwidth)
        {
            p = prev;
            break;
        }
        width += font_get_width(pf, ch);
        cut = p;
    }
    if ((cut - (unsigned char *)line) + 4 <= size)
        strcpy((char *)cut, "...");
    else if (size >= 4)
        strcpy(line + size - 4, "...");
}

/* %wt: word-wrap text to the current viewport, align the block vertically
 * (valign t/c/b) and each line horizontally (halign l/c/r), ellipsise if it
 * overflows the height, and draw it. Wrapping is one forward pass (as %wr):
 * accumulate pixel width, remember the last space, break there on overflow.
 *
 * noinline and with its own buffers so the ~1KB of scratch lives here, on the
 * stack only while a %wt actually renders -- NOT baked into do_non_text_tags's
 * frame, which runs at every level of the (deeply nested) skin render and would
 * otherwise overflow the tight skin stack even on screens with no %wt. */
static void __attribute__((noinline))
draw_textbox(struct gui_wps *gwps, struct skin_draw_info *info,
             struct skin_textbox *tb)
{
    struct screen *display = gwps->display;
    struct viewport *vp = &info->skin_vp->vp;
    struct wps_token *ttok = SKINOFFSETTOPTR(skin_buffer, tb->token);
    struct font *pf = font_get(vp->font);
    int fh = pf->height;
    int maxlines, nlines = 0, yoff = 0, i;
    const char *lstart[WT_MAX_LINES];
    int llen[WT_MAX_LINES];
    char src[WT_BUF], line[WT_BUF];
    const char *text;
    const unsigned char *p;
    bool overflow;

    display->set_viewport(vp);
    display->clear_viewport();          /* box may have held a longer title */
    if (fh <= 0)
        return;
    text = get_token_value(gwps, ttok, info->offset, src, sizeof(src), NULL);
    if ((!text || !*text) && tb->fallback != INVALID_OFFSET)
    {
        struct wps_token *ftok = SKINOFFSETTOPTR(skin_buffer, tb->fallback);
        if (ftok)
            text = get_token_value(gwps, ftok, info->offset, src,
                                   sizeof(src), NULL);
    }
    if (!text || !*text)
        return;
    if (text != src)
        strmemccpy(src, text, sizeof(src));

    maxlines = vp->height / fh;
    if (maxlines < 1) maxlines = 1;
    if (maxlines > WT_MAX_LINES) maxlines = WT_MAX_LINES;

    font_lock(vp->font, true);
    p = (const unsigned char *)src;
    while (*p && nlines < maxlines)
    {
        const unsigned char *cursor = p, *last_space = NULL, *lend, *next;
        int width = 0;
        ucschar_t ch;
        for (;;)
        {
            const unsigned char *prev = cursor;
            cursor = utf8decode(cursor, &ch);
            if (ch == 0) { lend = next = prev; break; }
            if (IS_DIACRITIC(ch)) continue;
            if (ch == ' ') last_space = prev;
            width += font_get_width(pf, ch);
            if (width > vp->width)
            {
                if (last_space && last_space > p) { lend = last_space; next = last_space + 1; }
                else if (prev > p)                { lend = next = prev; }
                else                              { lend = next = cursor; }
                break;
            }
        }
        lstart[nlines] = (const char *)p;
        llen[nlines] = lend - p;
        nlines++;
        p = next;
    }
    overflow = (*p != 0);

    if (tb->valign == 'b')      yoff = vp->height - nlines * fh;
    else if (tb->valign == 'c') yoff = (vp->height - nlines * fh) / 2;
    if (yoff < 0) yoff = 0;

    display->set_drawmode(DRMODE_SOLID);
    for (i = 0; i < nlines; i++)
    {
        int w, h, x = 0;
        int len = llen[i] < WT_BUF ? llen[i] : WT_BUF - 1;
        memcpy(line, lstart[i], len);
        line[len] = '\0';
        if (overflow && i == nlines - 1)
            wt_ellipsize(line, WT_BUF, pf, vp->width);
        display->getstringsize((const unsigned char *)line, &w, &h);
        if (tb->halign == 'r')      x = vp->width - w;
        else if (tb->halign == 'c') x = (vp->width - w) / 2;
        if (x < 0) x = 0;
        display->putsxy(x, yoff + i * fh, (const unsigned char *)line);
    }
    font_lock(vp->font, false);
}

static bool do_non_text_tags(struct gui_wps *gwps, struct skin_draw_info *info,
                             struct skin_element *element)
{
    struct wps_token *token = (struct wps_token *)SKINOFFSETTOPTR(skin_buffer, element->data);
    if (!token) return false;
    struct skin_viewport *skin_vp = info->skin_vp;
    struct wps_data *data = gwps->data;
    bool do_refresh = (element->tag->flags & info->refresh_type) > 0;

    /* Most of the cases below put pixels on the screen themselves rather than
     * contributing text to the line, and they all gate on do_refresh -- so
     * this is the one place that catches every direct draw. It is deliberately
     * before the switch: a tag that refreshes without drawing costs a flush of
     * a region that did not change, where a draw that went unreported would
     * leave a stale one on the LCD. Note that `needs_update` cannot stand in
     * for this, as it only tracks tags that produce text. */
    if (do_refresh)
        info->drew = true;

    switch (token->type)
    {
        case SKIN_TOKEN_VIEWPORT_BGCOLOUR:
        case SKIN_TOKEN_VIEWPORT_FGCOLOUR:
        {
            struct viewport_colour *col = SKINOFFSETTOPTR(skin_buffer, token->value.data);
            if (!col) return false;
            unsigned colour = dynamic_colors_resolve(col->colour);
            if (token->type == SKIN_TOKEN_VIEWPORT_FGCOLOUR)
                skin_vp->vp.fg_pattern = colour;
            else
                skin_vp->vp.bg_pattern = colour;
            skin_vp->fgbg_changed = true;
        }
        break;
        case SKIN_TOKEN_VIEWPORT_TEXTSTYLE:
        {
            struct line_desc *data = SKINOFFSETTOPTR(skin_buffer, token->value.data);
            struct line_desc *linedes = &info->line_desc;
            if (!data || !linedes) return false;
            /* gradient colors are handled with a separate tag
             * (SKIN_TOKEN_VIEWPORT_GRADIENT_SETUP, see below). since it may
             * come before the text style tag color fields need to be preserved */
            if (data->style & STYLE_GRADIENT)
            {
                unsigned tc  = linedes->text_color,
                         lc  = linedes->line_color,
                         lec = linedes->line_end_color;
                *linedes = *data;
                linedes->text_color     = tc;
                linedes->line_color     = lc;
                linedes->line_end_color = lec;
            }
            else
            {
                *linedes = *data;
                /* Resolved from the parsed original every frame, like the
                 * viewport colours are, so it follows the album. Only under
                 * STYLE_COLORED, because that is the only style the field
                 * means anything in -- resolving it unconditionally would put
                 * an unused zero through the mapping, and a theme whose
                 * foreground happens to be black would see it come back as
                 * something else. */
                if (data->style & STYLE_COLORED)
                    linedes->text_color =
                        dynamic_colors_resolve(data->text_color);
            }
        }
        break;
        case SKIN_TOKEN_VIEWPORT_GRADIENT_SETUP:
        {
            struct gradient_config *cfg = SKINOFFSETTOPTR(skin_buffer, token->value.data);
            struct line_desc *linedes = &info->line_desc;
            if (!cfg || !linedes) return false;
            linedes->text_color     = dynamic_colors_resolve(cfg->text);
            linedes->line_color     = dynamic_colors_resolve(cfg->start);
            linedes->line_end_color = dynamic_colors_resolve(cfg->end);
        }
        break;
        case SKIN_TOKEN_VIEWPORT_ENABLE:
        {
            char *label = SKINOFFSETTOPTR(skin_buffer, token->value.data);
            char temp = VP_DRAW_HIDEABLE;
            struct skin_element *viewport = SKINOFFSETTOPTR(skin_buffer, gwps->data->tree);
            while (viewport)
            {
                struct skin_viewport *skinvp = SKINOFFSETTOPTR(skin_buffer, viewport->data);

                if (skinvp) {
                    char *vplabel = SKINOFFSETTOPTR(skin_buffer, skinvp->label);
                    if (skinvp->label == VP_DEFAULT_LABEL)
                        vplabel = VP_DEFAULT_LABEL_STRING;
                    if (vplabel && !skinvp->is_infovp &&
                        !strcmp(vplabel, label))
                    {
                        if (skinvp->hidden_flags&VP_DRAW_HIDDEN)
                        {
                            temp |= VP_DRAW_WASHIDDEN;
                        }
                        skinvp->hidden_flags = temp;
                    }
                }
                viewport = SKINOFFSETTOPTR(skin_buffer, viewport->next);
            }
        }
        break;
        case SKIN_TOKEN_LIST_ITEM_CFG:
            skinlist_set_cfg(gwps->display->screen_type,
                                SKINOFFSETTOPTR(skin_buffer, token->value.data));
            break;
        case SKIN_TOKEN_UIVIEWPORT_ENABLE:
            sb_set_info_vp(gwps->display->screen_type, token->value.data);
            break;
        case SKIN_TOKEN_VAR_SET:
            {
                struct skin_var_changer *data = SKINOFFSETTOPTR(skin_buffer, token->value.data);
                struct skin_var *var = SKINOFFSETTOPTR(skin_buffer, data->var);
                if (data->direct)
                    var->value = data->newval;
                else
                {
                    var->value += data->newval;
                    if (data->max)
                    {
                        if (var->value > data->max)
                            var->value = 1;
                        else if (var->value < 1)
                            var->value = data->max;
                    }
                }
                if (var->value < 1)
                    var->value = 1;
                var->last_changed = current_tick;
            }
            break;
        case SKIN_TOKEN_PEAKMETER:
            data->peak_meter_enabled = true;
            if (do_refresh)
                draw_peakmeters(gwps, info->line_number, &skin_vp->vp);
            break;
        case SKIN_TOKEN_DRAWRECTANGLE:
            if (do_refresh)
            {
                struct draw_rectangle *rect =
                        SKINOFFSETTOPTR(skin_buffer, token->value.data);
                if (!rect) break;
                if (rect->opacity < LCD_BLEND_OPAQUE)
                {
                    /* Held back until after the art -- see pending_blend. */
                    if (info->pending_blend_count < SKIN_MAX_PENDING_BLENDS)
                        info->pending_blend[info->pending_blend_count++] = rect;
                    break;
                }
                unsigned dr_start = dynamic_colors_resolve(rect->start_colour);
                unsigned dr_end = dynamic_colors_resolve(rect->end_colour);
                /* Rounding wins over a gradient, the way an opacity already
                 * does: the corners blend one colour with what is under
                 * them, and a per-row one would have to be interpolated
                 * twice over and agree with the driver both times. */
                if (dr_start != dr_end && !rect->radius &&
                    gwps->display->screen_type == SCREEN_MAIN)
                {
                    gwps->display->gradient_fillrect(rect->x, rect->y, rect->width,
                        rect->height, dr_start, dr_end);
                }
                else
                {
                    unsigned backup = skin_vp->vp.fg_pattern;
                    skin_vp->vp.fg_pattern = dr_start;
                    if (rect->radius)
                        draw_rounded_rect(gwps->display, rect);
                    else
                        gwps->display->fillrect(rect->x, rect->y, rect->width,
                            rect->height);
                    skin_vp->vp.fg_pattern = backup;
                }
            }
            break;
        case SKIN_TOKEN_PEAKMETER_LEFTBAR:
        case SKIN_TOKEN_PEAKMETER_RIGHTBAR:
            data->peak_meter_enabled = true;
            /* fall through to the progressbar code */
        case SKIN_TOKEN_VOLUMEBAR:
        case SKIN_TOKEN_BATTERY_PERCENTBAR:
        case SKIN_TOKEN_PLAYLIST_PERCENTBAR:
        case SKIN_TOKEN_PLAYLIST_PROGRESSBAR:
        case SKIN_TOKEN_SETTINGBAR:
        case SKIN_TOKEN_PROGRESSBAR:
        case SKIN_TOKEN_TUNER_RSSI_BAR:
        case SKIN_TOKEN_LIST_SCROLLBAR:
        {
            struct progressbar *bar = (struct progressbar*)SKINOFFSETTOPTR(skin_buffer, token->value.data);
            if (do_refresh)
                draw_progressbar(gwps, info->skin_vp, info->line_number, bar);
        }
        break;
        case SKIN_TOKEN_IMAGE_DISPLAY:
        {
            struct gui_img *img = SKINOFFSETTOPTR(skin_buffer, token->value.data);
            if (img && img->loaded && do_refresh)
                img->display = 0;
        }
        break;
        case SKIN_TOKEN_IMAGE_DISPLAY_LISTICON:
        case SKIN_TOKEN_IMAGE_PRELOAD_DISPLAY:
        case SKIN_TOKEN_IMAGE_DISPLAY_9SEGMENT:
        {
            struct image_display *id = SKINOFFSETTOPTR(skin_buffer, token->value.data);
            if (!id) break;
            const char* label = SKINOFFSETTOPTR(skin_buffer, id->label);
            struct gui_img *img = skin_find_item(label,SKIN_FIND_IMAGE, data);
            if (img && img->loaded)
            {
                if (SKINOFFSETTOPTR(skin_buffer, id->token) == NULL)
                {
                    img->display = id->subimage;
                }
                else
                {
                    char buf[16];
                    const char *out;
                    int a = img->num_subimages;
                    out = get_token_value(gwps, SKINOFFSETTOPTR(skin_buffer, id->token),
                            info->offset, buf, sizeof(buf), &a);

                    /* NOTE: get_token_value() returns values starting at 1! */
                    if (a == -1)
                        a = (out && *out) ? 1 : 2;
                    if (token->type == SKIN_TOKEN_IMAGE_DISPLAY_LISTICON)
                        a -= 2; /* 2 is added in statusbar-skinned.c! */
                    else
                        a--;
                    a += id->offset;

                    /* Clear the image, as in conditionals */
                    clear_image_pos(gwps, img);

                    /* If the token returned a value which is higher than
                     * the amount of subimages, don't draw it. */
                    if (a >= 0 && a < img->num_subimages)
                    {
                        img->display = a;
                    }
                }
            }
            break;
        }
        case SKIN_TOKEN_ALBUMART_DISPLAY:
        {
            /* now draw the AA */
            if (do_refresh)
            {
                struct skin_albumart_draw *ad =
                        SKINOFFSETTOPTR(skin_buffer, token->value.data);
                struct skin_albumart *aa =
                        ad ? SKINOFFSETTOPTR(skin_buffer, ad->art) : NULL;
                /* NULL only for a %Cd written above every %Cl, which the
                 * parser could not bind. Take the skin's first. */
                if (!aa)
                    aa = skin_albumart_first(skin_buffer, data->albumart);
                if (aa)
                {
                    aa->draw_handle = playback_current_aa_hid(aa->slot);
                    aa->draw_win = PTRTOSKINOFFSET(skin_buffer, ad);
                }
            }
            break;
        }
        case SKIN_TOKEN_LIST_ITEM_ALBUMART:
        {
            if (do_refresh)
            {
                struct listitem *li = (struct listitem *)
                        SKINOFFSETTOPTR(skin_buffer, token->value.data);
                if (!li) break;
                struct dim size = { skin_vp->vp.width, skin_vp->vp.height };
                const struct bitmap *bmp =
                        skinlist_get_item_albumart(li->offset, li->wrap, &size,
                            SKINOFFSETTOPTR(skin_buffer, li->filter),
                            li->filter_hash);
                /* Defer the blit to after write_line() (see pending_aa) so the
                 * line-background clear can't wipe the cover's top. */
                if (bmp)
                {
                    info->pending_aa = bmp;
                    info->pending_aa_item = li;
                }
            }
            break;
        }
        case SKIN_TOKEN_SPECTRUM_BARS:
        {
            data->spectrum_enabled = true;
            if (do_refresh)
            {
                struct spectrum_bars *sb = (struct spectrum_bars *)
                        SKINOFFSETTOPTR(skin_buffer, token->value.data);
                if (!sb) break;

                int vp_w = skin_vp->vp.width;
                int vp_h = skin_vp->vp.height;
                int gap = sb->gap;
                int bar_w = (vp_w - gap * (sb->bars - 1)) / sb->bars;
                int i;

                if (bar_w < 1)
                    bar_w = 1;

                /* This is a partial (~20fps) refresh, not a full redraw, so
                 * nothing else clears stale pixels between frames -- without
                 * this, a shrinking bar leaves the previous, taller fill in
                 * place next to the new, shorter one. Buffer-only op (no
                 * lcd_update() here), so it doesn't cause a visible blank
                 * frame -- the actual screen push happens once, after this
                 * whole token pass completes. */
                gwps->display->clear_viewport();

                gwps->display->set_drawmode(DRMODE_SOLID);
                for (i = 0; i < sb->bars; i++)
                {
                    int level = spectrum_meter_get_bar(i, sb->bars);
                    int bar_h = (level * vp_h) / 100;
                    int x = i * (bar_w + gap);
                    int y;

                    /* Always show at least a sliver -- a fully-empty gap
                     * reads as "the meter stopped", not "it's quiet". */
                    if (bar_h < 1)
                        bar_h = 1;

                    if (sb->center_aligned)
                        y = (vp_h - bar_h) / 2;
                    else
                        y = vp_h - bar_h;

                    /* Both ends round. A quiet (one-pixel) bar squares off
                     * on its own -- fill_round_rect() fits the radius to
                     * the bar and falls back to a plain fillrect. */
                    fill_round_rect(gwps->display, x, y, bar_w, bar_h,
                                    sb->radius);
                }
            }
            break;
        }
        case SKIN_TOKEN_TEXT_BOX:
            if (do_refresh)
            {
                struct skin_textbox *tb = (struct skin_textbox *)
                        SKINOFFSETTOPTR(skin_buffer, token->value.data);
                if (tb)
                    draw_textbox(gwps, info, tb);
            }
            break;
        case SKIN_TOKEN_DRAW_INBUILTBAR:
            gui_statusbar_draw(&(statusbars.statusbars[gwps->display->screen_type]),
                               info->refresh_type == SKIN_REFRESH_ALL,
                               SKINOFFSETTOPTR(skin_buffer, token->value.data));
            break;
        case SKIN_TOKEN_VIEWPORT_CUSTOMLIST:
            if (do_refresh)
                skin_render_playlistviewer(SKINOFFSETTOPTR(skin_buffer, token->value.data), gwps,
                                           info->skin_vp, info->refresh_type);
            break;
        default:
            return false;
    }
    return true;
}

/* Erasing is drawing. Three of the cases below put pixels down -- an image
 * cleared, a hideable viewport cleared, album art cleared -- and none of them
 * reaches do_non_text_tags(), which is where every other direct draw is caught
 * for the flush. So each marks for itself.
 *
 * Two of them land in the current viewport, and `info->drew` covers those. The
 * viewport clear does not: it paints a *different* viewport, one the render
 * loop then skips outright (VP_DRAW_HIDDEN), so its rectangle would never be
 * marked by anyone and the clear would sit in the framebuffer unflushed. That
 * one asks for its own region. */
static void do_tags_in_hidden_conditional(struct skin_element* branch,
                                          struct skin_draw_info *info)
{
    struct gui_wps *gwps = info->gwps;
    struct wps_data *data = gwps->data;

    /* Tags here are ones which need to be "turned off" or cleared
     * if they are in a conditional branch which isnt being used */
    if (branch->type == LINE_ALTERNATOR)
    {
        int i;
        for (i=0; i<branch->children_count; i++)
        {
            do_tags_in_hidden_conditional(get_child(branch->children, i), info);
        }
    }
    else if (branch->type == LINE && branch->children_count)
    {
        struct skin_element *child = get_child(branch->children, 0);
        struct wps_token *token;
        while (child)
        {
            if (child->type == CONDITIONAL)
            {
                int i;
                for (i=0; i<child->children_count; i++)
                {
                    do_tags_in_hidden_conditional(get_child(child->children, i), info);
                }
                goto skip;
            }
            else if (child->type != TAG || !SKINOFFSETTOPTR(skin_buffer, child->data))
            {
                goto skip;
            }

            token = (struct wps_token *)SKINOFFSETTOPTR(skin_buffer, child->data);

            /* clear all pictures in the conditional and nested ones */
            if (token->type == SKIN_TOKEN_IMAGE_PRELOAD_DISPLAY)
            {
                struct image_display *id = SKINOFFSETTOPTR(skin_buffer, token->value.data);
                if (!id) goto skip;

                struct gui_img *img = skin_find_item(SKINOFFSETTOPTR(skin_buffer, id->label),
                                                     SKIN_FIND_IMAGE, data);
                clear_image_pos(gwps, img);
                info->drew = true;
            }
            else if (token->type == SKIN_TOKEN_PEAKMETER)
            {
                data->peak_meter_enabled = false;
            }
            else if (token->type == SKIN_TOKEN_UIVIEWPORT_ENABLE)
            {
                sb_set_info_vp(gwps->display->screen_type, VP_DEFAULT_LABEL);
            }
            else if (token->type == SKIN_TOKEN_VIEWPORT_ENABLE)
            {
                char *label = SKINOFFSETTOPTR(skin_buffer, token->value.data);
                struct skin_element *viewport;
                for (viewport = SKINOFFSETTOPTR(skin_buffer, data->tree);
                     viewport;
                     viewport = SKINOFFSETTOPTR(skin_buffer, viewport->next))
                {
                    struct skin_viewport *skin_viewport = SKINOFFSETTOPTR(skin_buffer, viewport->data);
                    if (!skin_viewport) continue;
                    char *vplabel = SKINOFFSETTOPTR(skin_buffer, skin_viewport->label);
                    if (skin_viewport->label == VP_DEFAULT_LABEL)
                        vplabel = VP_DEFAULT_LABEL_STRING;
                    if (vplabel && strcmp(vplabel, label))
                        continue;
                    if (skin_viewport->hidden_flags&VP_NEVER_VISIBLE)
                    {
                        continue;
                    }
                    if (skin_viewport->hidden_flags&VP_DRAW_HIDEABLE)
                    {
                        if (skin_viewport->hidden_flags&VP_DRAW_HIDDEN)
                            skin_viewport->hidden_flags |= VP_DRAW_WASHIDDEN;
                        else
                        {
                            if (skin_viewport->output_to_backdrop_buffer)
                            {
                                skin_backdrop_set_buffer(data->backdrop_id, skin_viewport);
                                skin_backdrop_show(-1);
                            }

                            gwps->display->set_viewport_ex(&skin_viewport->vp, VP_FLAG_VP_SET_CLEAN);
                            skin_viewport->vp.bg_pattern =
                                dynamic_colors_resolve(skin_viewport->dc_orig_bg);
                            gwps->display->clear_viewport();
                            /* Nobody else will: the render loop skips this
                             * viewport from here on. */
                            skin_mark_dirty_rect(gwps->display->screen_type,
                                                 skin_viewport->vp.x,
                                                 skin_viewport->vp.y,
                                                 skin_viewport->vp.width,
                                                 skin_viewport->vp.height);
                            gwps->display->set_viewport_ex(&info->skin_vp->vp, VP_FLAG_VP_SET_CLEAN);

                            if (skin_viewport->output_to_backdrop_buffer)
                            {
                                skin_backdrop_set_buffer(-1, skin_viewport);
                                skin_backdrop_show(data->backdrop_id);
                            }
                            skin_viewport->hidden_flags |= VP_DRAW_HIDDEN;
                        }
                    }
                }
            }
            else if (token->type == SKIN_TOKEN_ALBUMART_DISPLAY)
            {
                struct skin_albumart_draw *ad =
                        SKINOFFSETTOPTR(skin_buffer, token->value.data);
                struct skin_albumart *aa =
                        ad ? SKINOFFSETTOPTR(skin_buffer, ad->art) : NULL;
                if (!aa)
                    aa = skin_albumart_first(skin_buffer, data->albumart);
                if (aa)
                {
                    /* Clear the same rectangle this %Cd would have drawn */
                    aa->draw_win = PTRTOSKINOFFSET(skin_buffer, ad);
                    draw_album_art(gwps, aa,
                                   playback_current_aa_hid(aa->slot), true);
                    info->drew = true;
                }
            }
        skip:
            child = SKINOFFSETTOPTR(skin_buffer, child->next);
        }
    }
}

static void fix_line_alignment(struct skin_draw_info *info, struct skin_element *element)
{
    struct align_pos *align = &info->align;
    char *cur_pos = info->cur_align_start + strlen(info->cur_align_start);
    char *next_pos = cur_pos + 1;
    switch (element->tag->type)
    {
        case SKIN_TOKEN_ALIGN_LEFT:
            align->left = next_pos;
            break;
        case SKIN_TOKEN_ALIGN_LEFT_RTL:
            if (UNLIKELY(lang_is_rtl()))
                align->right = next_pos;
            else
                align->left = next_pos;
            break;
        case SKIN_TOKEN_ALIGN_CENTER:
            align->center = next_pos;
            break;
        case SKIN_TOKEN_ALIGN_RIGHT:
            align->right = next_pos;
            break;
        case SKIN_TOKEN_ALIGN_RIGHT_RTL:
            if (UNLIKELY(lang_is_rtl()))
                align->left = next_pos;
            else
                align->right = next_pos;
            break;
        default:
            return;
    }
    info->cur_align_start = next_pos;
    *cur_pos = '\0';
    *next_pos = '\0';
}

/* Draw a LINE element onto the display */
static bool skin_render_line(struct skin_element* line, struct skin_draw_info *info)
{
    bool needs_update = false;

    int last_value, value;

    if (line->children_count == 0)
        return false; /* empty line, do nothing */

    struct skin_element *child = get_child(line->children, 0);
    struct conditional *conditional;
    skin_render_func func = skin_render_line;
    int old_refresh_mode = info->refresh_type;
    while (child)
    {
        switch (child->type)
        {
            case CONDITIONAL:
                conditional = SKINOFFSETTOPTR(skin_buffer, child->data);
                if (!conditional) break;
                last_value = conditional->last_value;
                value = evaluate_conditional(info->gwps, info->offset,
                                             conditional, child->children_count);
                conditional->last_value = value;
                if (child->children_count == 1)
                {
                    /* special handling so
                     * %?aa<true> and %?<true|false> need special handlng here */

                    if (value == -1) /* tag is false */
                    {
                        /* we are in a false branch of a %?aa<true> conditional */
                        if (last_value == 0)
                            do_tags_in_hidden_conditional(get_child(child->children, 0), info);
                        break;
                    }
                }
                else
                {
                    if (last_value >= 0 && value != last_value && last_value < child->children_count)
                        do_tags_in_hidden_conditional(get_child(child->children, last_value), info);
                }

                struct skin_element* se_child =  get_child(child->children, value);
                if (se_child->type == LINE_ALTERNATOR)
                {
                    func = skin_render_alternator;
                }
                else if (se_child->type == LINE)
                    func = skin_render_line;

                if (value != last_value)
                {
                    info->refresh_type = SKIN_REFRESH_ALL;
                    info->force_redraw = true;
                }

                if (func(se_child, info))
                    needs_update = true;
                else
                    needs_update = needs_update || (last_value != value);

                info->refresh_type = old_refresh_mode;
                break;
            case TAG:
                if (child->tag->flags & NOBREAK)
                    info->no_line_break = true;
                if (child->tag->type == SKIN_TOKEN_SUBLINE_SCROLL)
                    info->line_scrolls = true;

                fix_line_alignment(info, child);

                if (!SKINOFFSETTOPTR(skin_buffer, child->data))
                {
                    break;
                }

                if (!do_non_text_tags(info->gwps, info, child))
                {
                    size_t used = strlen(info->cur_align_start);
                    char *bufstart = info->cur_align_start + used;
                    size_t bufsz = info->buf_size - used;

                    const char *valuestr = get_token_value(info->gwps,
                                               SKINOFFSETTOPTR(skin_buffer, child->data),
                                               info->offset, bufstart, bufsz, NULL);
                    if (valuestr)
                    {
                        if (child->tag->flags&SKIN_RTC_REFRESH)
                            needs_update = needs_update || info->refresh_type&SKIN_REFRESH_DYNAMIC;
                        needs_update = needs_update ||
                                ((child->tag->flags&info->refresh_type)!=0);
                        if (valuestr != bufstart)
                        {
                            strmemccpy(bufstart, valuestr, bufsz);
                        }
                    }
                    else
                        bufstart[0] = '\0';
                }
                break;
            case TEXT:
                strlcat(info->cur_align_start, SKINOFFSETTOPTR(skin_buffer, child->data),
                        info->buf_size - (info->cur_align_start-info->buf));
                needs_update = needs_update ||
                                (info->refresh_type&SKIN_REFRESH_STATIC) != 0;
                break;
            case COMMENT:
            default:
                break;
        }

        child = SKINOFFSETTOPTR(skin_buffer, child->next);
    }
    return needs_update;
}

static int get_subline_timeout(struct gui_wps *gwps, struct skin_element* line)
{
    struct skin_element *element=line;
    struct wps_token *token = NULL;
    int retval = DEFAULT_SUBLINE_TIME_MULTIPLIER*TIMEOUT_UNIT;
    if (element->type == LINE)
    {
        if (element->children_count == 0)
            return retval; /* empty line, so force redraw */
        element = get_child(element->children, 0);
    }
    while (element)
    {
        int type = element->type;
        if (type == TAG && element->tag->type == SKIN_TOKEN_SUBLINE_TIMEOUT)
        {
            token = SKINOFFSETTOPTR(skin_buffer, element->data);
            if (token)
            {
                if (token->type == SKIN_TOKEN_SUBLINE_TIMEOUT_HIDE)
                {
                    struct wps_subline_timeout *st;
                    st = SKINOFFSETTOPTR(skin_buffer, token->value.data);
                    retval = st->show * TIMEOUT_UNIT;
                    if (st->hide != 0)
                    {
                        if(TIME_BEFORE(current_tick, st->next_tick))
                            retval = 0; /* don't display yet.. */
                        else
                            st->next_tick = current_tick + st->hide * TIMEOUT_UNIT;
                    }
                }
                else
                    retval = token->value.i;
            }
        }
        else if (type == CONDITIONAL)
        {
            struct conditional *conditional = SKINOFFSETTOPTR(skin_buffer, element->data);
            int val = evaluate_conditional(gwps, 0, conditional, element->children_count);

            if (val >= 0 || (token && token->type == SKIN_TOKEN_SUBLINE_TIMEOUT_HIDE))
            {/* only need tmoval in false case if SKIN_TOKEN_SUBLINE_TIMEOUT_HIDE */
                int tmoval = get_subline_timeout(gwps, get_child(element->children, val));
                if (tmoval >= 0)
                    return MAX(retval, tmoval); /* Bugfix %t()%?CONDITIONAL tmo ignored */
            }
        }
        else if (type == COMMENT)
        {
            retval = 0; /* don't display this item */
        }
        element = SKINOFFSETTOPTR(skin_buffer, element->next);
    }
    return retval;
}

bool skin_render_alternator(struct skin_element* element, struct skin_draw_info *info)
{
    bool changed_lines = false;
    struct line_alternator *alternator = SKINOFFSETTOPTR(skin_buffer, element->data);
    unsigned old_refresh = info->refresh_type;

    if (info->refresh_type == SKIN_REFRESH_ALL)
    {
        alternator->current_line = element->children_count-1;
        changed_lines = true;
    }
    else if (TIME_AFTER(current_tick, alternator->next_change_tick))
    {
        changed_lines = true;
    }

    if (changed_lines)
    {
        struct skin_element *current_line;
        int start = alternator->current_line;
        int try_line = start;
        bool suitable = false;
        int rettimeout = DEFAULT_SUBLINE_TIME_MULTIPLIER*TIMEOUT_UNIT;

        /* find a subline which has at least one token in it,
         * and that line doesnt have a timeout set to 0 through conditionals */
        do {
            try_line++;
            if (try_line >= element->children_count)
                try_line = 0;

            struct skin_element* child =  get_child(element->children, try_line);
            if (child->children_count != 0)
            {
                current_line = child;
                rettimeout = get_subline_timeout(info->gwps,
                                    get_child(current_line->children, 0));
                if (rettimeout > 0)
                {
                    suitable = true;
                }
            }
        }
        while (try_line != start && !suitable);

        if (info->refresh_type == SKIN_REFRESH_ALL
            || try_line != alternator->current_line)
        {
            info->force_redraw = true;
        }
        info->refresh_type = SKIN_REFRESH_ALL;

        if (suitable)
        {
            alternator->current_line = try_line;
            alternator->next_change_tick = current_tick + rettimeout;
        }
    }
    bool ret = skin_render_line(get_child(element->children, alternator->current_line), info);
    info->refresh_type = old_refresh;
    return changed_lines || ret;
}

/* Returns whether anything was drawn, so the caller knows if this viewport's
 * rectangle is owed a flush. */
bool skin_render_viewport(struct skin_element* viewport, struct gui_wps *gwps,
                        struct skin_viewport* skin_viewport, unsigned long refresh_type)
{
    struct screen *display = gwps->display;
    char linebuf[MAX_LINE];
    skin_render_func func = skin_render_line;
    struct skin_element* line = viewport;
    struct skin_draw_info info = {
        .gwps = gwps,
        .buf = linebuf,
        .buf_size = sizeof(linebuf),
        .line_number = 0,
        .no_line_break = false,
        .line_scrolls = false,
        .refresh_type = refresh_type,
        .skin_vp = skin_viewport,
        .offset = 0,
        .line_desc = LINE_DESC_DEFINIT,
    };

    struct align_pos * align = &info.align;
    bool needs_update, update_all = false;
    skin_buffer = get_skin_buffer(gwps->data);
    if (!skin_buffer)       /* unloaded skin -- see skin_render() */
        return false;
    /* Set images to not to be displayed */
    struct skin_token_list *imglist = SKINOFFSETTOPTR(skin_buffer, gwps->data->images);
    while (imglist)
    {
        struct wps_token *token = SKINOFFSETTOPTR(skin_buffer, imglist->token);
        if (token) {
            struct gui_img *img = (struct gui_img *)SKINOFFSETTOPTR(skin_buffer, token->value.data);
        if (img)
                img->display = -1;
        }
        imglist = SKINOFFSETTOPTR(skin_buffer, imglist->next);
    }

    /* fix font ID's */
    if (skin_viewport->parsed_fontid == 1)
        skin_viewport->vp.font = display->getuifont();

    while (line)
    {
        linebuf[0] = '\0';
        info.no_line_break = false;
        info.line_scrolls = false;
        info.force_redraw = false;
        info.pending_aa = NULL;
        info.pending_aa_item = NULL;
        skin_viewport->fgbg_changed = false;
        if (info.line_desc.style&STYLE_GRADIENT)
        {
            if (++info.line_desc.line > info.line_desc.nlines)
                info.line_desc.style = STYLE_DEFAULT;
        }
        info.cur_align_start = info.buf;
        align->left = info.buf;
        align->center = NULL;
        align->right = NULL;

        if (line->type == LINE_ALTERNATOR)
            func = skin_render_alternator;
        else if (line->type == LINE)
            func = skin_render_line;

        needs_update = func(line, &info);
        if (skin_viewport->fgbg_changed)
        {
            /* if fg/bg changed due to a conditional tag the colors
             * need to be set (2bit displays requires set_{fore,back}ground
             * for this. the rest of the viewport needs to be redrawn
             * to get the new colors */
            display->set_foreground(skin_viewport->vp.fg_pattern);
            display->set_background(skin_viewport->vp.bg_pattern);
            if (needs_update)
                update_all = true;
        }
        /* only update if the line needs to be, and there is something to write */
        if (refresh_type && (needs_update || update_all))
        {
            if (info.force_redraw)
            {
                int h = display->getcharheight();
                display->scroll_stop_viewport_rect(&skin_viewport->vp,
                    0, info.line_number*h, skin_viewport->vp.width, h);
            }
            write_line(display, align, info.line_number,
                    info.line_scrolls, &info.line_desc);
            info.drew = true;
        }
        /* Blit deferred list album art now, after write_line()'s background
         * clear, so the cover's top row isn't wiped (see pending_aa). */
        if (info.pending_aa)
            draw_pending_aa(display, &skin_viewport->vp, &info);
        if (!info.no_line_break)
            info.line_number++;
        line = SKINOFFSETTOPTR(skin_buffer, line->next);
    }
    wps_display_images(gwps, &skin_viewport->vp);

    /* Tints go last: they blend with what is beneath them, which includes the
     * album art and images wps_display_images() has just drawn. */
    for (int i = 0; i < info.pending_blend_count; i++)
    {
        const struct draw_rectangle *rect = info.pending_blend[i];
        unsigned backup = skin_viewport->vp.fg_pattern;
        /* Start colour only. A tinted gradient would need a per-row blend and
         * nothing wants one yet. */
        skin_viewport->vp.fg_pattern = dynamic_colors_resolve(rect->start_colour);
        if (rect->radius)
            draw_rounded_rect(display, rect);
        else
            display->blendrect(rect->x, rect->y, rect->width, rect->height,
                               rect->opacity);
        skin_viewport->vp.fg_pattern = backup;
        info.drew = true;
    }
    return info.drew;
}

void skin_render(struct gui_wps *gwps, unsigned refresh_mode)
{
    const int vp_is_appearing = (VP_DRAW_WASHIDDEN|VP_DRAW_HIDEABLE);
    struct wps_data *data = gwps->data;
    struct screen *display = gwps->display;

    struct skin_element* viewport;
    struct skin_viewport* skin_viewport;
    char *label;

    int old_refresh_mode = refresh_mode;
    skin_buffer = get_skin_buffer(gwps->data);
    /* No buffer means the skin is unloaded -- settings_apply_skins() frees
     * every skin before reloading, and `tree` still holds its offset until
     * gui_skin_reset() runs in the next loop. Without this the offsets below
     * resolve against a NULL base, which SKINOFFSETTOPTR turns into the raw
     * offset rather than NULL, so the `if (!viewport)` guards pass and the
     * dereference lands near address zero. That traps on the simulator and,
     * on the player, quietly reads mapped memory instead. */
    if (!skin_buffer)
        return;

    /* Framebuffer is likely dirty */
    if ((refresh_mode&SKIN_REFRESH_ALL) == SKIN_REFRESH_ALL)
    {
        /* A full refresh repaints the ground between the viewports as well as
         * the viewports themselves, so there is nothing a bounding box could
         * save here. Partial refreshes are where it pays, and they take the
         * per-viewport path at the bottom of the loop. */
        skin_mark_dirty(display->screen_type);
        /* should already be the default buffer */
        struct viewport * first_vp = display->set_viewport_ex(NULL, 0);
        if (get_current_activity() == ACTIVITY_WPS)
        {
            bool dirty = (first_vp->flags & VP_FLAG_VP_SET_CLEAN)
                          == VP_FLAG_VP_DIRTY;
            unsigned resolved_bg =
                dynamic_colors_resolve(first_vp->bg_pattern);
            if (dirty || resolved_bg != first_vp->bg_pattern
                || dynamic_colors_screen_clear_needed())
            {
                unsigned saved_bg = first_vp->bg_pattern;
                first_vp->bg_pattern = resolved_bg;
                display->clear_viewport();
                first_vp->bg_pattern = saved_bg;
            }
        }
    }

    viewport = SKINOFFSETTOPTR(skin_buffer, data->tree);
    if (!viewport) return;
    skin_viewport = SKINOFFSETTOPTR(skin_buffer, viewport->data);
    if (!skin_viewport) return;
    label = SKINOFFSETTOPTR(skin_buffer, skin_viewport->label);
    if (skin_viewport->label == VP_DEFAULT_LABEL)
        label = VP_DEFAULT_LABEL_STRING;
    if (label && SKINOFFSETTOPTR(skin_buffer, viewport->next) &&
        !strcmp(label,VP_DEFAULT_LABEL_STRING))
        refresh_mode = 0;

    bool art_checks_done = false;

    for (viewport = SKINOFFSETTOPTR(skin_buffer, data->tree);
         viewport;
         viewport = SKINOFFSETTOPTR(skin_buffer, viewport->next))
    {

        /* SETUP */
        skin_viewport = SKINOFFSETTOPTR(skin_buffer, viewport->data);
        if (!skin_viewport) continue;

        /* The two once-per-art-change jobs, run once per render pass. The
         * filter goes second and must stay there: it rewrites the art in
         * place, and the palette has to come from the unfiltered image.
         *
         * Every %Cl is filtered -- each has its own chain -- but only the
         * first sources the palette. There is one palette for the whole UI,
         * so a later %Cl asking for a second one could only fight the first. */
        if (!art_checks_done)
        {
            struct skin_token_list *node =
                    SKINOFFSETTOPTR(skin_buffer, data->albumart);
            struct skin_albumart *first = skin_albumart_of(skin_buffer, node);

            dynamic_colors_check_extraction(first ? first->slot : -1);

            while (node)
            {
                struct skin_albumart *aa = skin_albumart_of(skin_buffer, node);
                if (aa)
                    skin_albumart_filter(aa->slot, aa);
                node = SKINOFFSETTOPTR(skin_buffer, node->next);
            }

            art_checks_done = true;
        }

        unsigned vp_refresh_mode = refresh_mode;
        if (skin_viewport->output_to_backdrop_buffer)
        {
            skin_backdrop_set_buffer(data->backdrop_id, skin_viewport);
            skin_backdrop_show(-1);
        }
        else
        {
            skin_backdrop_set_buffer(-1, skin_viewport);
            skin_backdrop_show(data->backdrop_id);
        }

        /* dont redraw the viewport if its disabled */
        if (skin_viewport->hidden_flags&VP_NEVER_VISIBLE)
        {   /* don't draw anything into this one */
            vp_refresh_mode = 0;
        }
        else if ((skin_viewport->hidden_flags&VP_DRAW_HIDDEN))
        {
            skin_viewport->hidden_flags |= VP_DRAW_WASHIDDEN;
            continue;
        }
        else if ((skin_viewport->hidden_flags & vp_is_appearing) == vp_is_appearing)
        {
            vp_refresh_mode = SKIN_REFRESH_ALL;
            skin_viewport->hidden_flags = VP_DRAW_HIDEABLE;
        }

        display->set_viewport_ex(&skin_viewport->vp, VP_FLAG_VP_SET_CLEAN);

        /* Dynamic colors: resolve from stored originals (not current vp values).
         * Colors stay in the viewport permanently so the scroll engine
         * reads the correct resolved colors (no save/restore). */
        skin_viewport->vp.fg_pattern =
            dynamic_colors_resolve(skin_viewport->dc_orig_fg);
        skin_viewport->vp.bg_pattern =
            dynamic_colors_resolve(skin_viewport->dc_orig_bg);
        display->set_foreground(skin_viewport->vp.fg_pattern);
        display->set_background(skin_viewport->vp.bg_pattern);

        bool drew = false;
        if ((vp_refresh_mode&SKIN_REFRESH_ALL) == SKIN_REFRESH_ALL)
        {
            display->clear_viewport();
            drew = true;
        }
        /* render */
        if (viewport->children_count)
            drew |= skin_render_viewport(get_child(viewport->children, 0), gwps,
                                         skin_viewport, vp_refresh_mode);

        /* Backdrop viewports are marked too, though what they draw lands in
         * the backdrop buffer rather than on screen: the region is the same
         * one their foreground twin occupies, so the flush is owed anyway,
         * and guessing otherwise would risk holding back a real change. */
        if (drew)
            skin_mark_dirty_rect(display->screen_type,
                                 skin_viewport->vp.x, skin_viewport->vp.y,
                                 skin_viewport->vp.width,
                                 skin_viewport->vp.height);

        refresh_mode = old_refresh_mode;
    }
    skin_backdrop_set_buffer(-1, skin_viewport);
    skin_backdrop_show(data->backdrop_id);

    if (((refresh_mode&SKIN_REFRESH_ALL) == SKIN_REFRESH_ALL))
    {
        /* If this is the UI viewport then let the UI know
         * to redraw itself */
        send_event(GUI_EVENT_NEED_UI_UPDATE, NULL);
    }
    /* Restore the default viewport. The screen is deliberately NOT updated
     * here -- skin_update() marks it dirty and the flush happens once, at the
     * end of the action, rather than mid-frame for every skin that renders. */
    display->set_viewport_ex(NULL, VP_FLAG_VP_SET_CLEAN);
}

static __attribute__((noinline))
void skin_render_playlistviewer(struct playlistviewer* viewer,
                                struct gui_wps *gwps,
                                struct skin_viewport* skin_viewport,
                                unsigned long refresh_type)
{
    struct screen *display = gwps->display;
    char linebuf[MAX_LINE];
    skin_render_func func = skin_render_line;
    struct skin_element* line;
    struct skin_draw_info info = {
        .gwps = gwps,
        .buf = linebuf,
        .buf_size = sizeof(linebuf),
        .line_number = 0,
        .no_line_break = false,
        .line_scrolls = false,
        .refresh_type = refresh_type,
        .skin_vp = skin_viewport,
        .offset = viewer->start_offset,
        .line_desc = LINE_DESC_DEFINIT,
    };

    struct align_pos * align = &info.align;
    bool needs_update;
    int cur_pos, start_item, max;
    int nb_lines = viewport_get_nb_lines(&skin_viewport->vp);
    {
        struct wps_state *state = get_wps_state();
        struct cuesheet *cue = state->id3 ? state->id3->cuesheet : NULL;
        cur_pos = playlist_get_display_index();
        max = playlist_amount()+1;
        if (cue)
            max += cue->track_count;
        start_item = MAX(0, cur_pos + viewer->start_offset);
    }
    if (max-start_item > nb_lines)
        max = start_item + nb_lines;

    line = SKINOFFSETTOPTR(skin_buffer, viewer->line);
    while (start_item < max)
    {
        linebuf[0] = '\0';
        info.no_line_break = false;
        info.line_scrolls = false;
        info.force_redraw = false;
        info.pending_aa = NULL;
        info.pending_aa_item = NULL;

        info.cur_align_start = info.buf;
        align->left = info.buf;
        align->center = NULL;
        align->right = NULL;


        if (line->type == LINE_ALTERNATOR)
            func = skin_render_alternator;
        else if (line->type == LINE)
            func = skin_render_line;

        needs_update = func(line, &info);

        /* only update if the line needs to be, and there is something to write */
        if (refresh_type && needs_update)
        {
            struct viewport *vp = &skin_viewport->vp;
            if (!info.force_redraw)
            {
                int h = display->getcharheight();
                display->scroll_stop_viewport_rect(vp,
                    0, info.line_number*h, vp->width, h);
            }
            write_line(display, align, info.line_number,
                    info.line_scrolls, &info.line_desc);
        }
        if (info.pending_aa)
            draw_pending_aa(display, &skin_viewport->vp, &info);
        info.line_number++;
        info.offset++;
        start_item++;
    }
}
