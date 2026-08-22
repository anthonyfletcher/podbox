/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/skin_engine/skin_display.c
 * Copyright (C) 2002-2007 Björn Stenberg
 * Copyright (C) 2007-2008 Nicolas Pennequin
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * Drawing helpers the renderer calls: progress bars, the embedded playlist
 * viewer, album art placement and A-B markers.
 ****************************************************************************/
#include "config.h"
#include <stdio.h>
#include "string-extra.h"
#include "core_alloc.h"
#include "kernel.h"
#include "system/volume.h"
#include "font.h"
#include "system.h"
#include "rbunicode.h"
#include "sound.h"
#include "powermgmt.h"
#ifdef DEBUG
#include "debug.h"
#endif
#include "input/action.h"
#include "audio/ab_repeat.h"
#include "lang.h"
#include "speech/language.h"
#include "statusbar.h"
#include "settings/settings.h"
#include "draw/scrollbar.h"
#include "draw/screen_access.h"
#include "draw/line.h"
#include "draw/round_rect.h"
#include "playlist/playlist.h"
#include "audio.h"
#include "database/tagcache.h"
#include "widgets/list.h"
#include "widgets/option_select.h"
#include "audio/buffering.h"

#include "audio/peak_meter.h"
#include "audio/spectrum_meter.h"
/* Image stuff */
#include "draw/bmp.h"
#include "metadata/albumart.h"

#include "metadata/cuesheet.h"
#include "audio/playback.h"
#include "backdrop.h"
#include "draw/viewport.h"
#include "root_menu.h"

#include "screens/playback/wps.h"
#include "wps_internals.h"
#include "skin_engine.h"
#include "statusbar_skinned.h"
#include "skin_display.h"
#include "skin_albumart_color.h"

void skin_render(struct gui_wps *gwps, unsigned refresh_mode);

/* update a skinned screen, update_type is WPS_REFRESH_* values.
 * Usually it should only be WPS_REFRESH_NON_STATIC
 * A full update will be done if required (skin_do_full_update() == true)
 */
/* Set when a skin has been rendered into the framebuffer but not yet flushed.
 * Nothing in the skin engine pushes pixels to the LCD any more; whoever runs
 * the update (normally GUI_EVENT_ACTIONUPDATE) asks here whether one is owed.
 * Reading the flag clears it -- the caller is taking responsibility for it.
 *
 * `box` is the union of what actually changed, which is worth tracking because
 * the flush is the expensive half of a frame -- pushing the whole 320x240 to
 * the LCD costs several times the render that produced it, so a skin that
 * repaints one small viewport should not pay for the other 99% of the screen.
 * `whole` is the escape hatch for a caller that knows something changed but
 * not where; a full refresh takes it, since that repaints the ground between
 * viewports as well and there is nothing left to save. */
static struct
{
    bool whole;
    int  count;
    struct { int x1, y1, x2, y2; } r[SKIN_MAX_DIRTY_RECTS];  /* x2/y2 exclusive */
} pending[NB_SCREENS];

static bool flush_inhibited = false;

/* Hold the flush off across an action that owns the screen itself -- a
 * progress splash, say, which the status bar would otherwise be redrawn over
 * and flushed on top of. Dirty flags survive, so the next update still paints. */
void skin_inhibit_flush(bool inhibit)
{
    flush_inhibited = inhibit;
}

bool skin_flush_inhibited(void)
{
    return flush_inhibited;
}

static void pending_clear(enum screen_type screen)
{
    pending[screen].whole = false;
    pending[screen].count = 0;
}

bool skin_take_dirty(enum screen_type screen)
{
    bool ret = pending[screen].whole || pending[screen].count > 0;
    pending_clear(screen);
    return ret;
}

void skin_mark_dirty(enum screen_type screen)
{
    pending[screen].whole = true;
}

/* Area a box would grow by to swallow another -- the cost of merging them. */
static long merge_cost(int ax1, int ay1, int ax2, int ay2,
                       int bx1, int by1, int bx2, int by2)
{
    int ux1 = MIN(ax1, bx1), uy1 = MIN(ay1, by1);
    int ux2 = MAX(ax2, bx2), uy2 = MAX(ay2, by2);

    return (long)(ux2 - ux1) * (uy2 - uy1)
         - (long)(ax2 - ax1) * (ay2 - ay1);
}

void skin_mark_dirty_rect(enum screen_type screen, int x, int y, int w, int h)
{
    int x2 = x + w, y2 = y + h;
    int i, best = 0;
    long best_cost = 0;

    if (w <= 0 || h <= 0 || pending[screen].whole)
        return;

    /* Already covered by something on the list -- most repaints are the same
     * handful of viewports over and over, so this is the common case. */
    for (i = 0; i < pending[screen].count; i++)
    {
        if (x >= pending[screen].r[i].x1 && y >= pending[screen].r[i].y1 &&
            x2 <= pending[screen].r[i].x2 && y2 <= pending[screen].r[i].y2)
            return;
    }

    if (pending[screen].count < SKIN_MAX_DIRTY_RECTS)
    {
        i = pending[screen].count++;
        pending[screen].r[i].x1 = x;
        pending[screen].r[i].y1 = y;
        pending[screen].r[i].x2 = x2;
        pending[screen].r[i].y2 = y2;
        return;
    }

    /* Out of slots: fold it into whichever box grows least. Merging beats
     * giving up and flushing everything, and the alternative -- an unbounded
     * list -- costs more in per-rect flush overhead than it saves. */
    for (i = 0; i < pending[screen].count; i++)
    {
        long cost = merge_cost(pending[screen].r[i].x1, pending[screen].r[i].y1,
                               pending[screen].r[i].x2, pending[screen].r[i].y2,
                               x, y, x2, y2);
        if (i == 0 || cost < best_cost)
        {
            best = i;
            best_cost = cost;
        }
    }
    pending[screen].r[best].x1 = MIN(pending[screen].r[best].x1, x);
    pending[screen].r[best].y1 = MIN(pending[screen].r[best].y1, y);
    pending[screen].r[best].x2 = MAX(pending[screen].r[best].x2, x2);
    pending[screen].r[best].y2 = MAX(pending[screen].r[best].y2, y2);
}

int skin_take_dirty_rects(enum screen_type screen,
                          struct skin_dirty_rect *out)
{
    int n = 0;

    if (pending[screen].whole)
    {
        out[0].x = 0;
        out[0].y = 0;
        out[0].w = screens[screen].lcdwidth;
        out[0].h = screens[screen].lcdheight;
        pending_clear(screen);
        return 1;
    }

    for (int i = 0; i < pending[screen].count; i++)
    {
        int x1 = pending[screen].r[i].x1, y1 = pending[screen].r[i].y1;
        int x2 = pending[screen].r[i].x2, y2 = pending[screen].r[i].y2;

        /* lcd_update_rect() walks the framebuffer straight from FBADDR(x, y)
         * without clipping -- the 6G's does no bounds check at all -- so a box
         * reaching past an edge would read off the end of it. A viewport may
         * be declared larger than the display. */
        if (x1 < 0)
            x1 = 0;
        if (y1 < 0)
            y1 = 0;
        if (x2 > screens[screen].lcdwidth)
            x2 = screens[screen].lcdwidth;
        if (y2 > screens[screen].lcdheight)
            y2 = screens[screen].lcdheight;

        if (x2 <= x1 || y2 <= y1)
            continue;

        out[n].x = x1;
        out[n].y = y1;
        out[n].w = x2 - x1;
        out[n].h = y2 - y1;
        n++;
    }
    pending_clear(screen);
    return n;
}

/* Flush whatever a skin render left in the framebuffer. Only for code that
 * draws outside the action loop and needs the result on screen now -- a
 * progress indicator advancing while its caller blocks, say. Everything else
 * should let GUI_EVENT_ACTIONUPDATE do it. */
/* Cost of the UI redraw, accumulated since boot. A frame is a render -- walk
 * the skin or list and paint into the framebuffer -- followed by a flush, the
 * push of those pixels to the LCD. Splitting the two says whether a slow
 * screen is slow because of what it draws or because of what it sends, which
 * are fixed in completely different places. Never reset; the debug menu
 * differences two readings to get a rate. */
static unsigned int flush_count;
static unsigned int render_usec;
static unsigned int flush_usec;

unsigned int skin_flush_count(void) { return flush_count; }
unsigned int skin_render_usec(void) { return render_usec; }
unsigned int skin_flush_usec(void)  { return flush_usec; }

void skin_note_render(unsigned int usec)
{
    render_usec += usec;
}

void skin_note_flush(unsigned int usec)
{
    flush_usec += usec;
    flush_count++;
}

void skin_flush_dirty(void)
{
    FOR_NB_SCREENS(i)
    {
        struct skin_dirty_rect dr[SKIN_MAX_DIRTY_RECTS];
        int n = skin_take_dirty_rects(i, dr);

        if (n > 0)
        {
            unsigned int t0 = USEC_TIMER;
            for (int j = 0; j < n; j++)
                screens[i].update_rect(dr[j].x, dr[j].y, dr[j].w, dr[j].h);
            skin_note_flush(USEC_TIMER - t0);
        }
    }
}

void skin_update(enum skinnable_screens skin, enum screen_type screen,
                 unsigned int update_type)
{
    struct gui_wps *gwps = skin_get_gwps(skin, screen);
    /* This maybe shouldnt be here,
     * This is also safe for skined screen which dont use the id3 */
    struct mp3entry *id3 = get_wps_state()->id3;
    bool cuesheet_update = (id3 != NULL ? cuesheet_subtrack_changed(id3) : false);
    if (cuesheet_update)
        skin_request_full_update(skin);

    unsigned int t0 = USEC_TIMER;
    /* skin_render() marks what it drew as it goes, per viewport, so a render
     * that touched nothing owes no flush at all. */
    skin_render(gwps, skin_do_full_update(skin, screen) ?
                        SKIN_REFRESH_ALL : update_type);
    skin_note_render(USEC_TIMER - t0);
}


#define DIRECTION_RIGHT 1
#define DIRECTION_LEFT -1

static int ab_calc_mark_x_pos(int mark, int capacity,
        int offset, int size)
{
    return offset + ( (size * mark) / capacity );
}

static void ab_draw_vertical_line_mark(struct screen * screen,
                                              int x, int y, int h)
{
    screen->set_drawmode(DRMODE_COMPLEMENT);
    screen->vline(x, y, y+h-1);
}

static void ab_draw_arrow_mark(struct screen * screen,
                                      int x, int y, int h, int direction)
{
    /* draw lines in decreasing size until a height of zero is reached */
    screen->set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
    while( h > 0 )
    {
        screen->vline(x, y, y+h-1);
        h -= 2;
        y++;
        x += direction;
        screen->set_drawmode(DRMODE_COMPLEMENT);
    }
}

void ab_draw_markers(struct screen * screen, int capacity,
                     int x, int y, int w, int h)
{
    bool a_set, b_set;
    unsigned int a, b;
    int xa, xb;

    a_set = ab_get_A_marker(&a);
    b_set = ab_get_B_marker(&b);
    xa = ab_calc_mark_x_pos(a, capacity, x, w);
    xb = ab_calc_mark_x_pos(b, capacity, x, w);
    /* if both markers are set, determine if they're far enough apart
    to draw arrows */
    if ( a_set && b_set )
    {
        int arrow_width = (h+1) / 2;
        if ( (xb-xa) < (arrow_width*2) )
        {
            ab_draw_vertical_line_mark(screen, xa, y, h);
            ab_draw_vertical_line_mark(screen, xb, y, h);
            return;
        }
    }

    if (a_set)
        ab_draw_arrow_mark(screen, xa, y, h, DIRECTION_RIGHT);

    if (b_set)
        ab_draw_arrow_mark(screen, xb, y, h, DIRECTION_LEFT);
}


void draw_progressbar(struct gui_wps *gwps, struct skin_viewport* skin_viewport,
                      int line, struct progressbar *pb)
{
    struct screen *display = gwps->display;
    struct viewport *vp = &skin_viewport->vp;
    struct wps_state *state = get_wps_state();
    struct mp3entry *id3 = state->id3;
    int x = pb->x, y = pb->y, width = pb->width, height = pb->height;
    unsigned long length, end;
    int flags = HORIZONTAL;

    if (height < 0)
        height = font_get(vp->font)->height;

    if (y < 0)
    {
        int line_height = font_get(vp->font)->height;
        /* center the pb in the line, but only if the line is higher than the pb */
        int center = (line_height-height)/2;
        /* if Y was not set calculate by font height,Y is -line_number-1 */
        y = line*line_height + (0 > center ? 0 : center);
    }

    if (pb->type == SKIN_TOKEN_VOLUMEBAR)
    {
        int minvol = sound_min(SOUND_VOLUME);
        int maxvol = sound_max(SOUND_VOLUME);
        length = 1000;
        end = to_normalized_volume(global_status.volume, minvol, maxvol, length);
    }
    else if (pb->type == SKIN_TOKEN_BATTERY_PERCENTBAR)
    {
        length = 100;
        end = battery_level();
    }
    else if (pb->type == SKIN_TOKEN_PLAYLIST_PROGRESSBAR)
    {
        /* elapsed time across the whole playlist, not the current track.
         * Needs every track's length, which is only known once the scan
         * enabled by parsing %pX has run -- show an empty bar until then. */
        unsigned long pl_elapsed, pl_total;
        if (id3 && wps_get_playlist_percent(id3,
                       id3->elapsed + state->ff_rewind_count,
                       &pl_elapsed, &pl_total)
            && pl_total > 0)
        {
            length = pl_total;
            end = pl_elapsed;
        }
        else
        {
            length = 1;
            end = 0;
        }
    }
    else if (pb->type == SKIN_TOKEN_PEAKMETER_LEFTBAR ||
             pb->type == SKIN_TOKEN_PEAKMETER_RIGHTBAR)
    {
        int left, right, val;
        peak_meter_current_vals(&left, &right);
        val = pb->type == SKIN_TOKEN_PEAKMETER_LEFTBAR ? left : right;
        length = MAX_PEAK;
        end = peak_meter_scale_value(val, length);
    }
    else if (pb->type == SKIN_TOKEN_PLAYLIST_PERCENTBAR)
    {
        length = playlist_amount();
        end = playlist_get_display_index();
    }
    else if (pb->type == SKIN_TOKEN_LIST_SCROLLBAR)
    {
        int val, min, max;
        skinlist_get_scrollbar(&val, &min, &max);
        end = val - min;
        length = max - min;
    }
    else if (pb->type == SKIN_TOKEN_SETTINGBAR)
    {
        int val, count;
        get_setting_info_for_bar(pb->setting, pb->setting_offset, &count, &val);
        length = count - 1;
        end = val;
    }
    else if (id3 && id3->length)
    {
        length = id3->length;
        end = id3->elapsed + state->ff_rewind_count;
    }
    else
    {
        length = 1;
        end = 0;
    }

    /* A zero range divides by zero in the bar/scrollbar fill maths -- e.g. an
     * always-drawn list scrollbar on an empty or fully-visible list, where
     * max == min. Clamp to a full bar. */
    if (length == 0)
    {
        length = 1;
        end = 1;
    }

    if (!pb->horizontal)
    {
        /* we want to fill upwards which is technically inverted. */
        flags = INVERTFILL;
    }

    if (pb->invert_fill_direction)
    {
        flags ^= INVERTFILL;
    }

    if (pb->nofill)
    {
        flags |= INNER_NOFILL;
    }

    if (pb->noborder)
    {
        flags |= BORDER_NOFILL;
    }

    if (SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->slider))
    {
        struct gui_img *img = SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->slider);
        /* clear the slider */
        screen_clear_area(display, x, y, width, height);

        /* account for the sliders width in the progressbar */
        if (flags&HORIZONTAL)
        {
            width -= img->bm.width;
        }
        else
        {
            height -= img->bm.height;
        }
    }

    if (SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->backdrop))
    {
        struct gui_img *img = SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->backdrop);
        img->bm.data = core_get_data(img->buflib_handle);
        display->bmp_part(&img->bm, 0, 0, x, y, pb->width, height);
        flags |= DONT_CLEAR_EXCESS;
    }

    if (!pb->nobar)
    {
        struct gui_img *img = SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->image);
        if (img)
        {
            char *img_data = core_get_data(img->buflib_handle);
            img->bm.data = img_data;
            gui_bitmap_scrollbar_draw(display, &img->bm,
                                    x, y, width, height,
                                    length, 0, end, flags);
        }
        else
            gui_scrollbar_draw(display, x, y, width, height,
                               length, 0, end, flags);
    }

    if (SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->slider))
    {
        int xoff = 0, yoff = 0;
        int w = width, h = height;
        struct gui_img *img = SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->slider);
        img->bm.data = core_get_data(img->buflib_handle);

        if (flags&HORIZONTAL)
        {
            w = img->bm.width;
            xoff = width * end / length;
            if (flags&INVERTFILL)
                xoff = width - xoff;
        }
        else
        {
            h = img->bm.height;
            yoff = height * end / length;
            if (flags&INVERTFILL)
                yoff = height - yoff;
        }
        display->bmp_part(&img->bm, 0, 0, x + xoff, y + yoff, w, h);
    }

    if (pb->type == SKIN_TOKEN_PROGRESSBAR)
    {
        if (id3 && id3->length)
        {
            if (ab_repeat_mode_enabled())
                ab_draw_markers(display, id3->length, x, y, width, height);

            if (id3->cuesheet)
                cue_draw_markers(display, id3->cuesheet, id3->length,
                                 x, y+1, width, height-2);
        }
    }
}

/* clears the area where the image was shown */
void clear_image_pos(struct gui_wps *gwps, struct gui_img *img)
{
    if(!gwps)
        return;
    gwps->display->set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
    gwps->display->fillrect(img->x, img->y, img->bm.width, img->subimage_height);
    gwps->display->set_drawmode(DRMODE_SOLID);
}

void wps_draw_image(struct gui_wps *gwps, struct gui_img *img,
                    int subimage, struct viewport* vp)
{
    struct screen *display = gwps->display;
    img->bm.data = core_get_data(img->buflib_handle);
    display->set_drawmode(DRMODE_SOLID);

    if (img->is_9_segment)
        display->nine_segment_bmp(&img->bm, 0, 0, vp->width, vp->height);
    else
        display->bmp_part(&img->bm, 0, img->subimage_height * subimage,
                          img->x, img->y, img->bm.width, img->subimage_height);
}

void wps_display_images(struct gui_wps *gwps, struct viewport* vp)
{
    if(!gwps || !gwps->data || !gwps->display)
        return;
    (void)vp;
    struct wps_data *data = gwps->data;
    struct screen *display = gwps->display;

    /* Album art goes down first so that mask images can be drawn over it.
     * Every %Cd rendered in this viewport left its own art marked to draw. */
    char *buf = get_skin_buffer(data);
    struct skin_token_list *aalist = SKINOFFSETTOPTR(buf, data->albumart);
    while (aalist)
    {
        struct skin_albumart *aa = skin_albumart_of(buf, aalist);
        if (aa && aa->draw_handle >= 0)
        {
            draw_album_art(gwps, aa, aa->draw_handle, false);
            aa->draw_handle = -1;
        }
        aalist = SKINOFFSETTOPTR(buf, aalist->next);
    }

    struct skin_token_list *list = SKINOFFSETTOPTR(get_skin_buffer(data), data->images);
    while (list)
    {
        struct wps_token *token = SKINOFFSETTOPTR(get_skin_buffer(data), list->token);
        struct gui_img *img = NULL;
        if (token)
            img = (struct gui_img*)SKINOFFSETTOPTR(get_skin_buffer(data), token->value.data);
        if (img) {
            if (img->using_preloaded_icons && img->display >= 0)
            {
                screen_put_icon(display, img->x, img->y, img->display);
            }
            else if (img->loaded)
            {
                if (img->display >= 0)
                {
                    wps_draw_image(gwps, img, img->display, vp);
                }
            }
        }
        list = SKINOFFSETTOPTR(get_skin_buffer(data), list->next);
    }
    display->set_drawmode(DRMODE_SOLID);
}

/* Evaluate the conditional that is at *token_index and return whether a skip
   has ocurred. *token_index is updated with the new position.
*/
int evaluate_conditional(struct gui_wps *gwps, int offset,
                         struct conditional *conditional, int num_options)
{
    if (!gwps)
        return false;

    char result[128];
    const char *value;

    int intval = num_options < 2 ? 2 : num_options;
    /* get_token_value needs to know the number of options in the enum */
    value = get_token_value(gwps, SKINOFFSETTOPTR(get_skin_buffer(gwps->data), conditional->token),
                    offset, result, sizeof(result), &intval);

    /* intval is now the number of the enum option we want to read,
       starting from 1. If intval is -1, we check if value is empty. */
    if (intval == -1)
    {
        if (num_options == 1) /* so %?AA<true> */
            intval = (value && *value) ? 1 : 0; /* returned as 0 for true, -1 for false */
        else
            intval = (value && *value) ? 1 : num_options;
    }
    else if (intval > num_options || intval < 1)
        intval = num_options;

    return intval -1;
}


/* Display a line appropriately according to its alignment format.
   format_align contains the text, separated between left, center and right.
   line is the index of the line on the screen.
   scroll indicates whether the line is a scrolling one or not.
*/
void write_line(struct screen *display, struct align_pos *format_align,
                int line, bool scroll, struct line_desc *linedes)
{
    int left_width = 0;
    int center_width = 0, center_xpos;
    int right_width = 0,  right_xpos;
    int space_width;
    int string_height;
    int scroll_width;
    int viewport_width = display->getwidth();

    /* calculate different string sizes and positions */
    display->getstringsize((unsigned char *)" ", &space_width, &string_height);
    if (format_align->left != 0) {
        display->getstringsize((unsigned char *)format_align->left,
                                &left_width, &string_height);
    }

    if (format_align->right != 0) {
        display->getstringsize((unsigned char *)format_align->right,
                                &right_width, &string_height);
    }

    if (format_align->center != 0) {
        display->getstringsize((unsigned char *)format_align->center,
                                &center_width, &string_height);
    }

    right_xpos = (viewport_width - right_width);
    center_xpos = (viewport_width - center_width) / 2;

    scroll_width = viewport_width;

    /* Checks for overlapping strings.
        If needed the overlapping strings will be merged, separated by a
        space */

    /* CASE 1: left and centered string overlap */
    /* there is a left string, need to merge left and center */
    if (center_width != 0)
    {
        if (left_width != 0 && left_width + space_width > center_xpos) {
            /* replace the former separator '\0' of left and
                center string with a space */
            *(--format_align->center) = ' ';
            /* calculate the new width and position of the merged string */
            left_width = left_width + space_width + center_width;
            /* there is no centered string anymore */
            center_width = 0;
        }
        /* there is no left string, move center to left */
        else if (left_width == 0 && center_xpos < 0) {
            /* move the center string to the left string */
            format_align->left = format_align->center;
            /* calculate the new width and position of the string */
            left_width = center_width;
            /* there is no centered string anymore */
            center_width = 0;
        }
    } /*(center_width != 0)*/

    /* CASE 2: centered and right string overlap */
    /* there is a right string, need to merge center and right */
    if (center_width != 0)
    {
        int center_left_x = center_xpos + center_width;
        if (right_width != 0 && center_left_x + space_width > right_xpos) {
            /* replace the former separator '\0' of center and
                right string with a space */
            *(--format_align->right) = ' ';
            /* move the center string to the right after merge */
            format_align->right = format_align->center;
            /* calculate the new width and position of the merged string */
            right_width = center_width + space_width + right_width;
            right_xpos = (viewport_width - right_width);
            /* there is no centered string anymore */
            center_width = 0;
        }
        /* there is no right string, move center to right */
        else if (right_width == 0 && center_left_x > right_xpos) {
            /* move the center string to the right string */
            format_align->right = format_align->center;
            /* calculate the new width and position of the string */
            right_width = center_width;
            right_xpos = (viewport_width - right_width);
            /* there is no centered string anymore */
            center_width = 0;
        }
    } /*(center_width != 0)*/

    /* CASE 3: left and right overlap
        There is no center string anymore, either there never
        was one or it has been merged in case 1 or 2 */
    /* there is a left string, need to merge left and right */
    if (center_width == 0 && right_width != 0)
    {
        if (left_width != 0 && left_width + space_width > right_xpos) {
            /* replace the former separator '\0' of left and
                right string with a space */
            *(--format_align->right) = ' ';
            /* calculate the new width and position of the string */
            left_width = left_width + space_width + right_width;
            /* there is no right string anymore */
            right_width = 0;
        }
        /* there is no left string, move right to left */
        else if (left_width == 0 && right_xpos < 0) {
            /* move the right string to the left string */
            format_align->left = format_align->right;
            /* calculate the new width and position of the string */
            left_width = right_width;
            /* there is no right string anymore */
            right_width = 0;
        }
    } /* (center_width == 0 && right_width != 0)*/

    if (scroll && ((left_width > scroll_width) ||
                   (center_width > scroll_width) ||
                   (right_width > scroll_width)))
    {
        /* strings can be as large as MAX_LINE which exceeds put_lines()
         * limit for inline strings. Use $t to avoid truncation */
        linedes->scroll = true;
        display->put_line(0, line * string_height, linedes, "$t", format_align->left);
    }
    else
    {
        linedes->scroll = false;
        /* clear the line first */
        display->set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
        display->fillrect(0, line*string_height, viewport_width, string_height);
        display->set_drawmode(DRMODE_SOLID);

        /* Nasty hack: we output an empty scrolling string,
        which will reset the scroller for that line */
        display->puts_scroll(0, line, (unsigned char *)"");
        line *= string_height;
        center_xpos = (viewport_width-center_width)/2;
        right_xpos = viewport_width-right_width;
        /* print aligned strings. print whole line at once so that %Vs works
         * across the full viewport width */
        char *left   = format_align->left   ?: "";
        char *center = format_align->center ?: "";
        char *right  = format_align->right  ?: "";

        display->put_line(0, line, linedes, "$t$*s$t$*s$t", left_width == 0 ? "" : left ,
                center_xpos - left_width, center_width == 0 ? "" : center,
                right_xpos - center_xpos - center_width, right_width == 0 ? "" : right);
    }
}

void draw_peakmeters(struct gui_wps *gwps, int line_number,
                     struct viewport *viewport)
{
    struct wps_data *data = gwps->data;
    if (!data->peak_meter_enabled)
    {
        peak_meter_enable(false);
    }
    else
    {
        int h = font_get(viewport->font)->height;
        int peak_meter_y = line_number * h;

        /* The user might decide to have the peak meter in the last
            line so that it is only displayed if no status bar is
            visible. If so we neither want do draw nor enable the
            peak meter. */
        if (peak_meter_y + h <= viewport->y+viewport->height) {
            peak_meter_enable(true);
            peak_meter_screen(gwps->display, 0, peak_meter_y,
                              MIN(h, viewport->y+viewport->height - peak_meter_y));
        }
    }
}

/* Draw the album art bitmap from the given handle ID onto the given WPS.
   Call with clear = true to clear the bitmap instead of drawing it. */
void draw_album_art(struct gui_wps *gwps, struct skin_albumart *aa,
                    int handle_id, bool clear)
{
    if (!gwps || !gwps->data || !gwps->display || handle_id < 0 || !aa)
        return;

    struct wps_data *data = gwps->data;

    /* A blurring chain has already rendered this track's art into the skin's
     * own buffer, at its own size; everything else drew, or will draw, from
     * the buffered bitmap. Only the source differs -- the cropping and
     * alignment below are the same either way. */
    const bool blurred = aa->filter_handle > 0
                      && aa->filtered_art == handle_id
                      && aa->filtered_gen == skin_albumart_gen();
    const fb_data *pixels;
    struct bitmap *bmp;
    short src_w, src_h;

    if (blurred)
    {
        pixels = core_get_data(aa->filter_handle);
        src_w = aa->filtered_width;
        src_h = aa->filtered_height;
    }
    else
    {
        if (bufgetdata(handle_id, 0, (void *)&bmp) <= 0)
            return;
        pixels = (const fb_data *)bmp->data;
        src_w = bmp->width;
        src_h = bmp->height;
    }

    short x = aa->x;
    short y = aa->y;
    short width = src_w;
    short height = src_h;

    if (aa->width > 0)
    {
        /* Crop if the bitmap is too wide */
        width = MIN(src_w, aa->width);

        /* Align */
        if (aa->xalign & WPS_ALBUMART_ALIGN_RIGHT)
            x += aa->width - width;
        else if (aa->xalign & WPS_ALBUMART_ALIGN_CENTER)
            x += (aa->width - width) / 2;
    }

    if (aa->height > 0)
    {
        /* Crop if the bitmap is too high */
        height = MIN(src_h, aa->height);

        /* Align */
        if (aa->yalign & WPS_ALBUMART_ALIGN_BOTTOM)
            y += aa->height - height;
        else if (aa->yalign & WPS_ALBUMART_ALIGN_CENTER)
            y += (aa->height - height) / 2;
    }

    /* A %Cd may ask for a window: a rectangle of the viewport to reveal
     * instead of the whole art box. The art stays anchored where %Cl put it,
     * so moving the window shows a different part of the cover -- which is
     * what lets one buffered bitmap serve several cut-outs. Clipped to the
     * art on both axes; a window that misses it entirely draws nothing. */
    short sx = 0, sy = 0;
    struct skin_albumart_draw *win =
            SKINOFFSETTOPTR(get_skin_buffer(data), aa->draw_win);

    if (win && win->w > 0 && win->h > 0)
    {
        int dx = win->x, dy = win->y, dw = win->w, dh = win->h;

        sx = dx - x;
        sy = dy - y;
        if (sx < 0) { dw += sx; dx -= sx; sx = 0; }
        if (sy < 0) { dh += sy; dy -= sy; sy = 0; }
        if (dw > width  - sx) dw = width  - sx;
        if (dh > height - sy) dh = height - sy;
        if (dw <= 0 || dh <= 0)
            return;

        x = dx; y = dy; width = dw; height = dh;
    }

    if (!clear)
    {
        int stride = STRIDE(gwps->display->screen_type, src_w, src_h);

        if (aa->radius)
        {
            /* The corners blend with what is under them, so the art has to go
             * down after whatever it sits on -- and DRMODE_FG is what makes
             * the blend read the screen rather than the viewport's colours. */
            const unsigned char *mask =
                    SKINOFFSETTOPTR(get_skin_buffer(data), aa->mask);

            gwps->display->set_drawmode(DRMODE_FG);
            /* No source offset in this one, so walk the pointer instead. The
             * radius rounds whatever rectangle is drawn, so a window gets
             * rounded corners of its own. */
            bitmap_part_round(gwps->display,
                              pixels + (size_t)sy * stride + sx, stride,
                              x, y, width, height, aa->radius, mask);
            gwps->display->set_drawmode(DRMODE_SOLID);
        }
        else
        {
            /* Draw the bitmap */
            gwps->display->bitmap_part(pixels, sx, sy, stride,
                                       x, y, width, height);
        }
    }
    else
    {
        /* Clear the bitmap */
        gwps->display->set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
        gwps->display->fillrect(x, y, width, height);
        gwps->display->set_drawmode(DRMODE_SOLID);
    }
}

bool skin_has_sbs(struct gui_wps *gwps)
{
    struct wps_data *data = gwps->data;

    bool draw = false;
    if (data->wps_sb_tag)
        draw = data->show_sb_on_wps;
    else if (statusbar_position(gwps->display->screen_type) != STATUSBAR_OFF)
        draw = true;
    return draw;
}

/* do the button loop as often as required for the peak meters to update
 * with a good refresh rate.
 */
int skin_wait_for_action(enum skinnable_screens skin, int context, int timeout)
{
    int button = ACTION_NONE;
    /* when the peak meter is enabled we want to have a
        few extra updates to make it look smooth. On the
        other hand we don't want to waste energy if it
        isn't displayed */
    bool pm=false;
    bool sb=false;
    FOR_NB_SCREENS(i)
    {
       if(skin_get_gwps(skin, i)->data->peak_meter_enabled)
           pm = true;
       if(skin_get_gwps(skin, i)->data->spectrum_enabled)
           sb = true;
    }

    bool recolouring = dynamic_colors_needs_repaint();
    bool pending = dynamic_colors_pending();

    if (pm || sb || recolouring || pending) {
        long next_pm_refresh = current_tick;
        long next_sb_refresh = current_tick;
        long next_recolour_refresh = current_tick;
        long next_big_refresh = current_tick + timeout;
        button = BUTTON_NONE;
        while (TIME_BEFORE(current_tick, next_big_refresh)) {
            button = get_action(context,TIMEOUT_NOBLOCK);
            if (button != ACTION_NONE) {
                break;
            }
            if (pm)
                peak_meter_peek();
            if (sb)
                spectrum_meter_peek();
            sleep(0);   /* Sleep until end of current tick. */

            if (pm && TIME_AFTER(current_tick, next_pm_refresh)) {
                FOR_NB_SCREENS(i)
                {
                    if(skin_get_gwps(skin, i)->data->peak_meter_enabled)
                        skin_update(skin, i, SKIN_REFRESH_PEAK_METER);
                }
                next_pm_refresh += HZ / PEAK_METER_FPS;
            }
            if (sb && TIME_AFTER(current_tick, next_sb_refresh)) {
                FOR_NB_SCREENS(i)
                {
                    if(skin_get_gwps(skin, i)->data->spectrum_enabled)
                        skin_update(skin, i, SKIN_REFRESH_SPECTRUM);
                }
                next_sb_refresh += HZ / SPECTRUM_FPS;
            }
            if ((recolouring || pending) && TIME_AFTER(current_tick, next_recolour_refresh)) {
                unsigned int refresh = SKIN_REFRESH_ALL;
                FOR_NB_SCREENS(i)
                    skin_update(skin, i, refresh);
                next_recolour_refresh += HZ / 20;
                recolouring = dynamic_colors_needs_repaint();
                pending = dynamic_colors_pending();
            }
        }

        if (dynamic_colors_needs_full_update()) {
            FOR_NB_SCREENS(i)
                skin_update(skin, i, SKIN_REFRESH_ALL);
        }
    }

    /* No peak meter or recolouring
       -> no additional screen updates needed */
    else
    {
        button = get_action(context, timeout);
    }
    return button;
}
