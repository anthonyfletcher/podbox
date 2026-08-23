/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/imageviewer.c
 * Core image viewer -- scene / event loop.
 * Ported from the imageviewer plugin.
 * GNU General Public License (version 2+)
 *
 * The image viewer UI: loads a picture through the decoder registry, then
 * handles zoom, pan, slideshow and next/previous within a directory.
 *
 * How it is built:
 *   - A plain core app, entered as image_viewer(file) and returning a GO_TO_*
 *     code, dispatched from the file browser like the text viewer.
 *   - A picture opens on the fit rung, DS_FIT, scaled so all of it is on
 *     screen. Zoom and pan live behind the Zoom / Pan setting, which reads the
 *     one key table (image_viewer_button.h) two ways: off, Left and Right page
 *     between files and a tap of Menu leaves, as in the text viewer; on, the
 *     same four buttons pan and the way out is Quit on the menu.
 *   - Colour 320x240 only; there are no greylib or mono paths.
 *   - Decoders are linked in and chosen from a static table (image_decoder.h),
 *     not loaded as overlays.
 *   - It owns the whole screen with the theme disabled, shows the file's name
 *     while the first image decodes, and draws decode progress as a dialog over
 *     the retained image rather than blanking the screen.
 *
 * Memory: one core_alloc_maximum() block holds both the file-name list and the
 * decoded image, carved up by get_pic_list() advancing `buf` past the list. It
 * is pinned for the whole session, so a USB connect would deadlock on it --
 * iv_usb_inserted() drops it from the USB thread before that can happen.
 *
 * Parts, in order:
 *   - session state and the shared iv_settings
 *   - the directory file list, and moving between files
 *   - the decode progress callback
 *   - pan, zoom and the redraw path
 *   - the settings menu
 *   - screen ownership (theme off, name splash) and the image_viewer() entry point
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "string-extra.h"   /* strlcpy */
#include <stdlib.h>
#include "config.h"
#include "system.h"          /* MIN, MAX */
#include "dir.h"             /* the folder read when the browser cache is not it */
#include "lcd.h"
#include "file.h"            /* MAX_PATH */
#include "fs_attr.h"         /* ATTR_DIRECTORY */
#include "kernel.h"          /* HZ, current_tick, TIME_AFTER, sleep, yield */
#include "logf.h"
#include "button.h"
#include "lang.h"            /* str(), LANG_* */
#include "settings/settings.h"        /* global_settings, set_option/int/bool, ID2P */
#include "speech/talk.h"            /* STR() */
#include "widgets/splash.h"
#include "draw/viewport.h"        /* viewportmanager_theme_* */
#include "draw/screen_access.h"   /* screens[] */
#include "widgets/menu.h"            /* do_menu, MENUITEM_STRINGLIST */
#include "system/activity.h"
#include "system/shutdown.h"
#include "root_menu.h"       /* GO_TO_*, MENU_ATTACHED_USB */
#include "screens/browse/browser.h"            /* browser_get_context/entries */
#include "core_alloc.h"
#include "events.h"          /* add_event/remove_event, SYS_EVENT_USB_INSERTED */
#include "audio.h"           /* audio_current_track, audio_status, audio_hard_stop */
#include "metadata/albumart.h"        /* search_albumart_files */
#include "metadata.h"        /* AA_TYPE_*, AA_CLEAR_FLAGS_MASK */
#include "image_viewer.h"
#include "image_viewer_pub.h"
#include "image_decoder.h"
#include "image_viewer_button.h"

/* Headings */
#define DIR_PREV  1
#define DIR_NEXT -1
#define DIR_NONE  0

/** Globals **/

/* Viewer iv_settings + slideshow state, shared with the decoders (declared extern
 * in image_viewer.h). Session-scoped; not persisted across launches. */
struct imgview_settings iv_settings =
{
    COLOURMODE_COLOUR,
    DITHER_NONE,
    SS_DEFAULT_TIMEOUT
};
bool iv_slideshow_enabled = false;
bool iv_running_slideshow = false;

static fb_data rgb_linebuf[LCD_WIDTH];  /* Line buffer for scrolling when
                                           DITHER_DIFFUSION is set        */

/* the buffer for loaded+resized images and the file list */
static int buf_handle = 0;
static unsigned char* buf;
static size_t buf_size;

/* True only while the main thread sits in button_get() with no decode in
 * flight -- the one window where another thread may free the buffer, because
 * nothing is holding a pointer into it. */
static volatile bool iv_buf_idle = false;

/* Set by the USB hook once it has freed the buffer; the main thread must then
 * leave without touching buf or image_info again. */
static volatile bool iv_usb_dropped = false;

static int ds, ds_min, ds_max; /* downscaling and limits */
static struct image_info image_info;

/* Size of the DS_FIT rendering; 0 when this picture has no fit rung. */
int iv_fit_width = 0, iv_fit_height = 0;

/* Zoom / Pan mode: false is the navigation key map, where Left and Right move
 * between pictures and a tap of Menu leaves. Session state, kept across a move
 * to the next picture; only the settings menu changes it.
 *
 * The mode spends Menu on panning up, so it has no tap to leave on. Quit on
 * the menu is the way out of it -- see show_menu(). */
static bool iv_zoom_mode = false;

/* The button the last pass of scroll_bmp() saw, so a release can be told from
 * a tap: the release that ends a Menu hold arrives after the menu has closed
 * and must not read as "leave". Reset on entry rather than merely static --
 * a session left mid-hold would otherwise swallow the next session's first
 * tap of Menu. */
static int iv_lastbutton = BUTTON_NONE;

/* the current full file name */
static char np_file[MAX_PATH];
static int curfile = -1, direction = DIR_NEXT, entries = 0;

/* list of the supported image files */
static char **file_pt;

/* progress update tick */
static long next_progress_tick;

static const struct image_decoder *imgdec = NULL;
static enum image_type image_type = IMAGE_UNKNOWN;

/* our full-screen viewport, saved by viewportmanager_theme_enable() */
static struct viewport iv_vp;

/* forward declarations */
static int show_menu(void); /* MENU_ATTACHED_USB, IV_MENU_QUIT, or 0 */

/** Implementation **/

/* Release the image buffer, once. */
static void iv_release_buffer(void)
{
    if (buf_handle > 0)
    {
        core_unpin(buf_handle);
        core_free(buf_handle);
        buf_handle = 0;
    }
}

/* SYS_EVENT_USB_INSERTED handler.
 *
 * Runs on the USB thread, and -- crucially -- before usb_set_host_present()
 * asks for exclusive storage (firmware/usb.c). By the time the main thread is
 * handed SYS_USB_CONNECTED via button_get() the USB machinery has already run,
 * which is far too late: connecting USB froze the device (a deadlock, no panic)
 * even when the viewer ignored the event entirely. Holding the whole heap
 * pinned is what blocks buflib, so the buffer has to go here.
 *
 * Only safe while the main thread is idle in button_get(); mid-decode the
 * decoder holds pointers into the buffer. Rockbox threading is cooperative, so
 * this runs to completion without the main thread resuming underneath it. */
static void iv_usb_inserted(unsigned short id, void *event_data)
{
    (void)id; (void)event_data;

    if (!iv_buf_idle || buf_handle <= 0)
        return;

    iv_release_buffer();
    iv_usb_dropped = true;
}

/* Read 'dir' for the files to page through, when the browser's cache is not
 * about this directory (see get_pic_list()).
 *
 * The names are copied into the working buffer. The cache path can point
 * straight at the browser's own strings because the browser owns them for as
 * long as the viewer is up; nothing owns a readdir() name past the next call,
 * so these have to be kept. The pointer table grows from the front of the
 * buffer and the names from the back, and they stop when they would meet --
 * the buffer is also what the images decode into, so this takes what it needs
 * and no more. */
static void get_pic_list_from_dir(const char *dir, const char *pname)
{
    char *names = (char *)buf + buf_size;   /* names fill downwards */
    DIR *d = opendir(dir);
    struct dirent *entry;

    if (!d)
        return;

    while ((entry = readdir(d)))
    {
        struct dirinfo info = dir_get_info(d, entry);
        size_t len = strlen((char *)entry->d_name) + 1;

        if (info.attribute & ATTR_DIRECTORY)
            continue;

        /* One more pointer at the front and one more name at the back. */
        if (buf_size < sizeof(char **) + len)
            break;

        names -= len;
        strcpy(names, (char *)entry->d_name);
        buf_size -= len;

        file_pt[entries] = names;
        if (!strcmp(names, pname))
            curfile = entries;
        entries++;

        buf = (char *)buf + sizeof(char **);
        buf_size -= sizeof(char **);
    }

    closedir(d);
}

static void get_pic_list(bool single_file)
{
    file_pt = (char **) buf;

    if (single_file)
    {
        file_pt[0] = np_file;
        buf += sizeof(char**);
        buf_size -= sizeof(char**);
        entries = 1;
        curfile = 0;
        return;
    }

    struct browser_context *tree = browser_get_context();
    struct entry *dircache = browser_get_entries(tree);
    int i;
    char *pname;
    char dir[MAX_PATH], cwd[MAX_PATH];

    /* Remove path and leave only the name.*/
    pname = strrchr(np_file,'/');
    pname++;

    /* The neighbours to page through come from the file browser's cached
     * directory, which describes this file only when the browser opened it --
     * the flat Images list (screens/browse/browser_flat.c) opens files from
     * anywhere without moving the browser, and the cache is shared with the
     * database browser, so it may hold something else entirely.
     *
     * Trap: comparing the paths is not enough. getcwd() returns the browser's
     * last directory whether or not it ever loaded it, so for a file at the
     * volume root the paths match, the empty cache is trusted, and the viewer
     * reports "No supported files". The cache must also contain the file being
     * opened. Preferred where it does apply, since it is ordered the way the
     * browser displays it and paging should follow that order. */
    /* np_file's directory, without the trailing slash -- which getcwd() does
     * not return either, except at the root where the slash is the whole
     * name. */
    size_t dlen = (size_t)(pname - np_file);
    if (dlen > 1)
        dlen--;
    if (dlen >= sizeof(dir))
        dlen = sizeof(dir) - 1;
    memcpy(dir, np_file, dlen);
    dir[dlen] = '\0';

    cwd[0] = '\0';
    getcwd(cwd, sizeof(cwd));

    bool cache_has_file = false;
    if (cwd[0] && !strcmp(cwd, dir))
    {
        for (i = 0; i < tree->filesindir; i++)
        {
            if (!(dircache[i].attr & ATTR_DIRECTORY)
                && !strcmp(dircache[i].name, pname))
            {
                cache_has_file = true;
                break;
            }
        }
    }

    if (cache_has_file)
    {
        for (i = 0; i < tree->filesindir && buf_size > sizeof(char**); i++)
        {
            /* Add all files. Non-image files will be filtered out while loading. */
            if (!(dircache[i].attr & ATTR_DIRECTORY))
            {
                file_pt[entries] = dircache[i].name;
                /* Set Selected File. */
                if (!strcmp(file_pt[entries], pname))
                    curfile = entries;
                entries++;

                buf += (sizeof(char**));
                buf_size -= (sizeof(char**));
            }
        }
        return;
    }

    get_pic_list_from_dir(dir, pname);
}

static int change_filename(int direct)
{
    bool file_erased;

    /* get_pic_list() leaves this at -1 when the file being shown was not in
     * the listing it built -- deleted between the browser reading the
     * directory and the viewer opening it, or a directory too large for the
     * buffer. Start at the top rather than indexing before the array. */
    if (curfile < 0)
        curfile = 0;

    file_erased = (file_pt[curfile] == NULL);
    direction = direct;

    curfile += (direct == DIR_PREV? entries - 1: 1);
    if (curfile >= entries)
        curfile -= entries;

    if (file_erased)
    {
        /* remove 'erased' file names from list. */
        int count, i;
        for (count = i = 0; i < entries; i++)
        {
            if (curfile == i)
                curfile = count;
            if (file_pt[i] != NULL)
                file_pt[count++] = file_pt[i];
        }
        entries = count;
    }

    if (entries == 0)
    {
        splash(HZ * 2, "No supported files");
        return PLUGIN_ERROR;
    }

    size_t np_file_length = strlen(np_file);
    size_t np_file_name_length = strlen(strrchr(np_file, '/')+1);
    size_t avail_length = sizeof(np_file) - (np_file_length - np_file_name_length);

    snprintf(strrchr(np_file, '/')+1, avail_length, "%s", file_pt[curfile]);

    return PLUGIN_OTHER;
}

/* The current file's name, without its directory. */
static const char *iv_filename(void)
{
    const char *name = strrchr(np_file, '/');
    return name ? name + 1 : np_file;
}

/* callback updating a progress meter while image decoding.
 *
 * Draws the shared splash dialog with a progress bar over the current screen
 * (the previous image or the name splash), rather than blanking to a black
 * screen with resolution/progress text. */
void cb_progress(int current, int total)
{
    long now = current_tick;

    /* do not yield or update the progress bar if we did so too recently */
    if(!TIME_AFTER(now, next_progress_tick))
        return;
    /* 8fps is the redraw rate, not a poll rate: splash_progress() re-wraps the
     * message and re-frames the dialog every call, which is what costs. */
    next_progress_tick = now + HZ/8;

    /* Every foreground decode reports: the first one, a zoom, and moving to
     * the next picture, which is the slowest of the three on a large image.
     * Slideshows keep the screen clear. Each decode arms a delay first, so a
     * picture that arrives quickly never flashes a dialog up. */
    if (!iv_running_slideshow)
        splash_progress(current, total, "%s\n%s", str(LANG_WAIT), iv_filename());

    yield(); /* be nice to the other threads */
}

#define VSCROLL (LCD_HEIGHT/8)
#define HSCROLL (LCD_WIDTH/10)

/* Pan the viewing window right - move image to the left and fill in
   the right-hand side */
static void pan_view_right(struct image_info *info)
{
    int move;

    move = MIN(HSCROLL, info->width - info->x - LCD_WIDTH);
    if (move > 0)
    {
        xlcd_scroll_left(move); /* scroll left */
        info->x += move;
        imgdec->draw_image_rect(info, LCD_WIDTH - move, 0,
                                move, info->height-info->y);
        lcd_update();
    }
}

/* Pan the viewing window left - move image to the right and fill in
   the left-hand side */
static void pan_view_left(struct image_info *info)
{
    int move;

    move = MIN(HSCROLL, info->x);
    if (move > 0)
    {
        xlcd_scroll_right(move); /* scroll right */
        info->x -= move;
        imgdec->draw_image_rect(info, 0, 0, move, info->height-info->y);
        lcd_update();
    }
}

/* Pan the viewing window up - move image down and fill in
   the top */
static void pan_view_up(struct image_info *info)
{
    int move;

    move = MIN(VSCROLL, info->y);
    if (move > 0)
    {
        xlcd_scroll_down(move); /* scroll down */
        info->y -= move;
        if (image_type == IMAGE_JPEG
         && iv_settings.jpeg_dither_mode == DITHER_DIFFUSION)
        {
            /* Draw over the band at the top of the last update
               caused by lack of error history on line zero. */
            move = MIN(move + 1, info->y + info->height);
        }
        imgdec->draw_image_rect(info, 0, 0, info->width-info->x, move);
        lcd_update();
    }
}

/* Pan the viewing window down - move image up and fill in
   the bottom */
static void pan_view_down(struct image_info *info)
{
    fb_data *lcd_fb = get_framebuffer(NULL, NULL);
    int move;

    move = MIN(VSCROLL, info->height - info->y - LCD_HEIGHT);
    if (move > 0)
    {
        xlcd_scroll_up(move); /* scroll up */
        info->y += move;
        if (image_type == IMAGE_JPEG
         && iv_settings.jpeg_dither_mode == DITHER_DIFFUSION)
        {
            /* Save the line that was on the last line of the display
               and draw one extra line above then recover the line with
               image data that had an error history when it was drawn.
             */
            move++, info->y--;
            memcpy(rgb_linebuf,
                    lcd_fb + (LCD_HEIGHT - move)*LCD_WIDTH,
                    LCD_WIDTH*sizeof (fb_data));
        }

        imgdec->draw_image_rect(info, 0, LCD_HEIGHT - move,
                                info->width-info->x, move);

        if (image_type == IMAGE_JPEG
         && iv_settings.jpeg_dither_mode == DITHER_DIFFUSION)
        {
            /* Cover the first row drawn with previous image data. */
            memcpy(lcd_fb + (LCD_HEIGHT - move)*LCD_WIDTH,
                        rgb_linebuf, LCD_WIDTH*sizeof (fb_data));
            info->y++;
        }
        lcd_update();
    }
}

/* interactively scroll around the image */
static int scroll_bmp(struct image_info *info)
{
    static long ss_timeout = 0;

    int button;

    if (!ss_timeout && iv_slideshow_enabled)
        ss_timeout = current_tick + iv_settings.ss_timeout * HZ;

    while (true)
    {
        /* Nothing holds a pointer into the buffer while we wait here, so the
         * USB hook is allowed to free it. */
        iv_buf_idle = true;

        if (iv_slideshow_enabled)
        {
            if (info->frames_count > 1 && info->delay &&
                iv_settings.ss_timeout * HZ > info->delay)
            {
                /* animated content and delay between subsequent frames
                 * is shorter then slideshow delay
                 */
                button = button_get_w_tmo(info->delay);
            }
            else
                button = button_get_w_tmo(iv_settings.ss_timeout * HZ);
        }
        else
        {
            if (info->frames_count > 1 && info->delay)
                button = button_get_w_tmo(info->delay);
            else
                button = button_get(true);
        }

        iv_buf_idle = false;

        /* USB took the buffer while we waited: `info` and everything else
         * pointing into it is dangling now, so leave without drawing. */
        if (iv_usb_dropped)
            return PLUGIN_USB_CONNECTED;

        iv_running_slideshow = false;

        switch(button)
        {
        case IMGVIEW_LEFT:
            if (iv_zoom_mode)
            {
                pan_view_left(info);
                break;
            }
            if (entries > 1)
            {
                int result = change_filename(DIR_PREV);
                if (entries > 1)
                    return result;
            }
            break;

        /* Panning repeats; paging does not. A held Left would otherwise queue
         * one decode per repeat, none of them interruptible. */
        case IMGVIEW_LEFT | BUTTON_REPEAT:
            if (iv_zoom_mode)
                pan_view_left(info);
            break;

        case IMGVIEW_RIGHT:
            if (iv_zoom_mode)
            {
                pan_view_right(info);
                break;
            }
            if (entries > 1)
            {
                int result = change_filename(DIR_NEXT);
                if (entries > 1)
                    return result;
            }
            break;

        case IMGVIEW_RIGHT | BUTTON_REPEAT:
            if (iv_zoom_mode)
                pan_view_right(info);
            break;

        case IMGVIEW_UP:
            /* no BUTTON_REPEAT variant: Menu+repeat is the settings gesture */
            if (iv_zoom_mode)
                pan_view_up(info);
            break;

        case IMGVIEW_EXIT:
            /* The release that ends a hold is not a tap: the menu has just
             * closed on it. */
            if (!iv_zoom_mode && iv_lastbutton != IMGVIEW_MENU)
                return PLUGIN_OK;
            break;

        case IMGVIEW_DOWN:
        case IMGVIEW_DOWN | BUTTON_REPEAT:
            if (iv_zoom_mode)
                pan_view_down(info);
            break;

        case BUTTON_NONE:
            if (iv_slideshow_enabled && entries > 1)
            {
                if (info->frames_count > 1)
                {
                    /* animations */
                    if (TIME_AFTER(current_tick, ss_timeout))
                    {
                        iv_running_slideshow = true;
                        ss_timeout = 0;
                        return change_filename(DIR_NEXT);
                    }
                    else
                        return NEXT_FRAME;
                }
                else
                {
                    /* still picture */
                    iv_running_slideshow = true;
                    return change_filename(DIR_NEXT);
                }
            }
            else if (info->frames_count > 1)
                return NEXT_FRAME;

            /* Nothing happened and a still picture has no next frame, so keep
             * waiting. Answering NEXT_FRAME here divides by frames_count,
             * which every decoder but the GIF one leaves at zero. */
            break;

        case IMGVIEW_ZOOM_IN:
            if (iv_zoom_mode)
                return ZOOM_IN;
            break;

        case IMGVIEW_ZOOM_OUT:
            if (iv_zoom_mode)
                return ZOOM_OUT;
            break;

        case IMGVIEW_MENU:
        {
            bool was_zoom_mode = iv_zoom_mode;
            int menu = show_menu();

            if (menu == MENU_ATTACHED_USB)
                return PLUGIN_USB_CONNECTED;
            if (menu == IV_MENU_QUIT)
                return PLUGIN_OK;

            /* Leaving the mode has to put the picture back to fitting the
             * screen: the navigation map has no pan in it, so a view left
             * zoomed could not be moved off the corner it was showing. */
            if (was_zoom_mode && !iv_zoom_mode)
                return ZOOM_FIT;

            /* the menu ran with the theme on; repaint our image */
            lcd_clear_display();
            imgdec->draw_image_rect(info, 0, 0,
                            info->width-info->x, info->height-info->y);
            lcd_update();
            break;
        }

        default:
            /* Reached only if the hook did not already drop the buffer (USB
             * arrived mid-decode, outside the idle window). Free it before the
             * USB screen runs so at least the reload has memory. */
            if (button == SYS_USB_CONNECTED)
                iv_release_buffer();
            if (default_event_handler(button) == SYS_USB_CONNECTED)
                return PLUGIN_USB_CONNECTED;
            break;

        } /* switch */

        if (button != BUTTON_NONE)
            iv_lastbutton = button;
    } /* while (true) */
}

/** Main function **/

/* how far can we zoom in without running out of memory */
static int min_downscale(int bufsize)
{
    int downscale = 8;

    if (imgdec->img_mem(8) > bufsize)
        return 0; /* error, too large, even 1:8 doesn't fit */

    while (downscale > 1 && imgdec->img_mem(downscale/2) <= bufsize)
        downscale /= 2;

    return downscale;
}

/* how far can we zoom out, to fit image into the LCD */
static int max_downscale(struct image_info *info)
{
    int downscale = 1;

    while (downscale < 8 && (info->x_size/downscale > LCD_WIDTH
                          || info->y_size/downscale > LCD_HEIGHT))
    {
        downscale *= 2;
    }

    return downscale;
}

/* Work out the DS_FIT rendering: the largest one that has all of the picture
 * on screen, one side landing exactly on a screen edge.
 *
 * The integer ladder cannot express it. It stops at 1:8 -- a hard limit, since
 * the decoders index their caches by the downscale -- so a 4032x3024 photo
 * gets no smaller than 504x378 and cannot fit at all; and even where it does
 * fit, 1000x750 lands on 1:4 and shows at 250x187 because there is no rung at
 * 1:3.125.
 *
 * Leaves iv_fit_width/height at 0 when there is nothing to add: a picture
 * already smaller than the screen (fitting never enlarges), or one where an
 * integer rung is the same size, which would put a duplicate on the ladder. */
static void calc_fit_size(struct image_info *info)
{
    int w, h;

    iv_fit_width = iv_fit_height = 0;

    if (info->x_size <= LCD_WIDTH && info->y_size <= LCD_HEIGHT)
        return;

    /* Match the width, and take the height instead if that overflows. */
    w = LCD_WIDTH;
    h = info->y_size * LCD_WIDTH / info->x_size;
    if (h > LCD_HEIGHT)
    {
        h = LCD_HEIGHT;
        w = info->x_size * LCD_HEIGHT / info->y_size;
    }

    if (w == info->x_size/ds_max && h == info->y_size/ds_max)
        return;

    iv_fit_width = MAX(1, w);
    iv_fit_height = MAX(1, h);
}

/* The rung above DS_FIT: the largest integer downscale that still renders
 * wider or taller than the screen, and so is the first one worth panning.
 * Stepping to ds_max instead would be a no-op or a shrink -- 1000x750 fits at
 * 1:4, which is smaller than its fit rendering. */
static int ds_above_fit(struct image_info *info)
{
    int downscale = 8;

    while (downscale > 1 && info->x_size/downscale <= LCD_WIDTH
                         && info->y_size/downscale <= LCD_HEIGHT)
    {
        downscale /= 2;
    }

    return downscale;
}

/* The integer rung a decoder that cannot scale freely should render before
 * resampling down to the fit size: the smallest rendering that is still at
 * least as big as the fit one, so the resample only ever shrinks. Clamped to
 * what memory allows, which can make it smaller -- a slightly soft picture
 * beats refusing to show one. */
int iv_fit_source_ds(int x_size, int y_size)
{
    int downscale = 8;

    while (downscale > 1 && (x_size/downscale < iv_fit_width
                          || y_size/downscale < iv_fit_height))
    {
        downscale /= 2;
    }

    return MAX(downscale, ds_min);
}

/* Displayed size at a given downscale, DS_FIT included. */
static void scaled_size(struct image_info *info, int downscale,
                        int *p_w, int *p_h)
{
    if (downscale == DS_FIT)
    {
        *p_w = iv_fit_width;
        *p_h = iv_fit_height;
    }
    else
    {
        *p_w = info->x_size/downscale;
        *p_h = info->y_size/downscale;
    }
}

/* set the view to the given center point, limit if necessary */
static void set_view(struct image_info *info, int cx, int cy)
{
    int x, y;

    /* plain center to available width/height */
    x = cx - MIN(LCD_WIDTH, info->width) / 2;
    y = cy - MIN(LCD_HEIGHT, info->height) / 2;

    /* limit against upper image size */
    x = MIN(info->width - LCD_WIDTH, x);
    y = MIN(info->height - LCD_HEIGHT, y);

    /* limit against negative side */
    x = MAX(0, x);
    y = MAX(0, y);

    info->x = x; /* set the values */
    info->y = y;
}

/* calculate the view center based on the bitmap position */
static void get_view(struct image_info *info, int *p_cx, int *p_cy)
{
    *p_cx = info->x + MIN(LCD_WIDTH, info->width) / 2;
    *p_cy = info->y + MIN(LCD_HEIGHT, info->height) / 2;
}

/* load, decode, display the image */
static int load_and_show(char *filename, struct image_info *info,
                         int offset, int filesize, int status)
{
    int cx, cy;
    ssize_t remaining;

    if (status == IMAGE_UNKNOWN) {
        /* file isn't supported image file, skip this. */
        file_pt[curfile] = NULL;
        return change_filename(direction);
    }

reload_decoder:
    /* Note: the screen is deliberately NOT cleared here -- the previous image
     * stays up while the next one decodes, with the progress dialog over it. */

    if (image_type != status) /* type of image is changed, load decoder. */
    {
        image_type = status;
        imgdec = get_image_decoder(image_type);
        if (imgdec == NULL)
        {
            logf("image viewer: no decoder for type %d", image_type);
            splash(HZ * 2, "Unsupported file");
            return PLUGIN_ERROR;
        }
    }
    memset(info, 0, sizeof(*info));
    remaining = buf_size;

    /* Reading and parsing the file reports progress too -- on a large picture
     * over a spinning disk it is the part that is worth watching. */
    splash_progress_set_delay(HZ/4);

    if (button_get(false) == IMGVIEW_MENU)
        status = PLUGIN_ABORT;
    else
        status = imgdec->load_image(filename, info, buf, &remaining, offset, filesize);

    if (status == PLUGIN_JPEG_PROGRESSIVE)
    {
        status = IMAGE_JPEG_PROGRESSIVE;
        goto reload_decoder;
    }

    if (status == PLUGIN_OUTOFMEM)
    {
        splash(HZ * 2, "Image too large");
        file_pt[curfile] = NULL;
        return change_filename(direction);
    }
    else if (status == PLUGIN_ERROR)
    {
        file_pt[curfile] = NULL;
        return change_filename(direction);
    }
    else if (status == PLUGIN_ABORT) {
        splash(HZ, "Cancelled");
        return PLUGIN_OK;
    }

    ds_max = max_downscale(info);       /* check display constraint */
    ds_min = min_downscale(remaining);  /* check memory constraint */
    if (ds_min == 0)
    {
        if (imgdec->unscaled_avail)
        {
            /* Can not resize the image but original one is available, so use it. */
            ds_min = ds_max = 1;
        }
        else
        {
            splash(HZ * 2, "Image too large");
            file_pt[curfile] = NULL;
            return change_filename(direction);
        }
    }
    else if (ds_max < ds_min && !(ds_max == 1 && imgdec->unscaled_avail))
        ds_max = ds_min;

    calc_fit_size(info);

    /* The fit rendering needs a buffer of its own, and there is not always one
     * to be had: a picture that only opened at all because its decoder can
     * show the unscaled original (ds_min == 0 above) has no room to resize
     * into. Drop the rung rather than decode past the end of the buffer. */
    if (iv_fit_width && imgdec->img_mem(DS_FIT) > remaining)
        iv_fit_width = iv_fit_height = 0;

    /* Open on the fit rung where there is one, so the whole picture is on
     * screen; navigation mode cannot pan, and would leave anything larger
     * stuck showing its middle. */
    ds = iv_fit_width ? DS_FIT : ds_max;
    scaled_size(info, ds, &cx, &cy); /* center the view */
    cx /= 2;
    cy /= 2;

    /* used to loop through subimages in animated gifs */
    int frame = 0;
    do  /* loop the image prepare and decoding when zoomed */
    {
        /* Hold the progress dialog back for 250ms, so an image that decodes
         * quickly never flashes one up. */
        splash_progress_set_delay(HZ/4);
        status = imgdec->get_image(info, frame, ds); /* decode or fetch from cache */
        if (status == PLUGIN_ERROR)
        {
            file_pt[curfile] = NULL;
            return change_filename(direction);
        }

        set_view(info, cx, cy);

        /* Clear then draw in the same framebuffer pass so the transition from
         * the old image to the new one is a single update -- no visible black
         * flash. The clear also paints the black border for smaller images. */
        if (frame == 0)
            lcd_clear_display();
        imgdec->draw_image_rect(info, 0, 0,
                        info->width-info->x, info->height-info->y);
        lcd_update();

        /* drawing is now finished, play around with scrolling
         * until you press OFF or connect USB
         */
        while (1)
        {
            status = scroll_bmp(info);

            if (status == ZOOM_IN)
            {
                if (ds == DS_FIT)
                {
                    int step = ds_above_fit(info);
                    if (step < ds_min)
                        continue; /* memory allows nothing larger */
                    ds = step;
                    /* the fit view showed all of it, so zoom to the middle */
                    cx = info->x_size/ds/2;
                    cy = info->y_size/ds/2;
                }
                else if (ds > ds_min || (imgdec->unscaled_avail && ds > 1))
                {
                    /* if 1/1 is always available, jump ds from ds_min to 1. */
                    int zoom = (ds == ds_min)? ds_min: 2;
                    ds /= zoom; /* reduce downscaling to zoom in */
                    get_view(info, &cx, &cy);
                    cx *= zoom; /* prepare the position in the new image */
                    cy *= zoom;
                }
                else
                    continue;
            }

            if (status == ZOOM_OUT)
            {
                if (ds == DS_FIT)
                    continue; /* the whole picture is already on screen */

                if (ds < ds_max)
                {
                    /* if ds is 1 and ds_min is > 1, jump ds to ds_min. */
                    int zoom = (ds < ds_min)? ds_min: 2;
                    ds *= zoom; /* increase downscaling to zoom out */
                    get_view(info, &cx, &cy);
                    cx /= zoom; /* prepare the position in the new image */
                    cy /= zoom;
                }
                else if (iv_fit_width)
                {
                    ds = DS_FIT; /* the rung below the integer ladder */
                    cx = iv_fit_width/2;
                    cy = iv_fit_height/2;
                }
                else
                    continue;
            }

            /* Zoom / Pan turned off: back to the whole picture. Always falls
             * through to the redraw below, even when the view was already
             * there -- the menu drew over the picture on its way out. */
            if (status == ZOOM_FIT)
            {
                ds = iv_fit_width ? DS_FIT : ds_max;
                scaled_size(info, ds, &cx, &cy);
                cx /= 2;
                cy /= 2;
                /* back to the first frame, which is also what makes the
                 * redraw below clear the screen the menu drew on */
                frame = 0;
            }

            /* Next frame in animated content. The frames_count test is not
             * redundant with the one guarding NEXT_FRAME in scroll_bmp():
             * every decoder but the GIF one leaves the count at zero, so
             * anything that reaches here another way divides by it. */
            if (status == NEXT_FRAME && info->frames_count > 1)
                frame = (frame + 1)%info->frames_count;

            break;
        }
    }
    while (status > PLUGIN_OTHER);
    return status;
}

static bool find_album_art(int *offset, int *filesize, int *status)
{
    struct mp3entry *current_track = audio_current_track();

    if (current_track == NULL)
    {
        return false;
    }

    switch (current_track->albumart.type & AA_CLEAR_FLAGS_MASK)
    {
        case AA_TYPE_BMP:
            (*status) = IMAGE_BMP;
            break;
        case AA_TYPE_PNG:
            (*status) = IMAGE_PNG;
            break;
        case AA_TYPE_JPG:
            (*status) = IMAGE_JPEG;
            break;
        default:
            (*status) = IMAGE_UNKNOWN;
    }

    if (IMAGE_UNKNOWN == *status
        || AA_PREFER_IMAGE_FILE == global_settings.album_art)
    {
        if (search_albumart_files(current_track, "", np_file, MAX_PATH))
        {
            (*status) = get_image_type(np_file, false);
            return true;
        }

        if (*status == IMAGE_UNKNOWN)
            return false;
    }
    strcpy(np_file, current_track->path);
    (*offset) = current_track->albumart.pos;
    (*filesize) = current_track->albumart.size;
    return true;
}

/** Iv_settings menu **/

/* Returns MENU_ATTACHED_USB if the viewer must leave, otherwise 0. */
static int show_menu(void)
{
    int result;

    enum menu_id
    {
        MIID_ZOOM_PAN = 0,
        MIID_TOGGLE_SS_MODE,
        MIID_CHANGE_SS_MODE,
        MIID_DITHERING,
        MIID_QUIT,
    };

    /* Two spellings of one menu, differing only in the Quit row.
     *
     * Quit earns a row in Zoom / Pan mode alone: that map spends Menu on
     * panning up, so the tap of Menu that leaves the viewer is not available
     * and the menu is the only way out. The navigation map already leaves on
     * a tap, where a Quit row is one more thing to read past.
     *
     * The rows above it are identical in both, so one set of ids serves both
     * and MIID_QUIT can only ever come back from the second. */
    MENUITEM_STRINGLIST(menu, "Image Viewer", NULL,
                        ID2P(LANG_IV_ZOOM_PAN),
                        ID2P(LANG_SLIDESHOW_MODE),
                        ID2P(LANG_SLIDESHOW_TIME),
                        ID2P(LANG_DITHERING));
    MENUITEM_STRINGLIST(menu_zoom, "Image Viewer", NULL,
                        ID2P(LANG_IV_ZOOM_PAN),
                        ID2P(LANG_SLIDESHOW_MODE),
                        ID2P(LANG_SLIDESHOW_TIME),
                        ID2P(LANG_DITHERING),
                        ID2P(LANG_MENU_QUIT));

    static const struct opt_items slideshow[2] = {
        { STR(LANG_OFF) },
        { STR(LANG_ON) },
    };
    static const struct opt_items dithering[DITHER_NUM_MODES] = {
        [DITHER_NONE]      = { STR(LANG_OFF) },
        [DITHER_ORDERED]   = { STR(LANG_ORDERED) },
        [DITHER_DIFFUSION] = { STR(LANG_DIFFUSION) },
    };

    /* re-enable the theme for the menu chrome, then hand the screen back to
     * our own full-screen drawing when we return. */
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    push_current_activity(ACTIVITY_CONTEXTMENU);

    result = do_menu(iv_zoom_mode ? &menu_zoom : &menu, NULL, NULL, false);

    switch (result)
    {
        case MIID_ZOOM_PAN:
            set_option(str(LANG_IV_ZOOM_PAN), &iv_zoom_mode, RB_BOOL,
                       slideshow, 2, NULL);
            break;
        case MIID_TOGGLE_SS_MODE:
            set_option(str(LANG_SLIDESHOW_MODE), &iv_slideshow_enabled, RB_BOOL,
                       slideshow, 2, NULL);
            break;
        case MIID_CHANGE_SS_MODE:
            set_int(str(LANG_SLIDESHOW_TIME), "s", UNIT_SEC,
                    &iv_settings.ss_timeout, NULL, 1,
                    SS_MIN_TIMEOUT, SS_MAX_TIMEOUT, NULL);
            break;
        case MIID_DITHERING:
            set_option(str(LANG_DITHERING), &iv_settings.jpeg_dither_mode, RB_INT,
                       dithering, DITHER_NUM_MODES, NULL);
            break;
    }

    pop_current_activity();
    viewportmanager_theme_undo(SCREEN_MAIN, false);

    /* re-establish our own drawing environment */
    lcd_set_backdrop(NULL);
    lcd_set_foreground(LCD_WHITE);
    lcd_set_background(LCD_BLACK);

    /* USB arriving while the menu was up has already freed the buffer, so the
     * caller must leave rather than redraw from it. Quit is the other answer
     * the caller has to act on; everything else is a setting it redraws for. */
    if (result == MENU_ATTACHED_USB)
        return MENU_ATTACHED_USB;
    return (result == MIID_QUIT) ? IV_MENU_QUIT : 0;
}

/** Screen ownership **/

/* Names the file while the first image decodes. */
static void iv_splash(void)
{
    /* splashf, not splash: splash()'s string is a format string, and a '%' in a
     * filename would be read as a conversion. 0 ticks = until overdrawn. */
    splashf(0, "%s\n%s", str(LANG_WAIT), iv_filename());
}

static void iv_setup_screen(void)
{
    /* This screen owns the whole display. Disabling the theme drops the
     * status-bar skin and its backdrop and hands back a full-screen viewport,
     * matching the text viewer. */
    viewportmanager_theme_enable(SCREEN_MAIN, false, &iv_vp);
    lcd_set_backdrop(NULL);
    lcd_set_foreground(LCD_WHITE);
    lcd_set_background(LCD_BLACK);
    lcd_clear_display();
    lcd_update();
}

/** Core entry point **/

/* file == NULL requests the current track's album art. */
int image_viewer(const char *file)
{
    int condition;
    int offset = 0, filesize = 0, status;
    bool is_album_art = false;

    iv_lastbutton = BUTTON_NONE;

    if (!file)
    {
        if (!find_album_art(&offset, &filesize, &status))
        {
            splash(HZ * 2, "Could not open the file");
            return GO_TO_PREVIOUS;
        }
        is_album_art = true;
    }
    else
    {
        strlcpy(np_file, file, sizeof(np_file));
        if ((status = get_image_type(np_file, false)) == IMAGE_UNKNOWN)
        {
            splash(HZ * 2, "Unsupported file");
            return GO_TO_PREVIOUS;
        }
    }

    /* Grab the largest free buffer for the file list + decoded images, so large
     * images decode at full quality. This costs: core_alloc_maximum() runs
     * buflib_compact_and_shrink() first, invoking playback.c's shrink callback
     * on the audio buffer -- so PLAYBACK STOPS as the viewer opens. Held
     * pinned, it also blocks buflib compaction, which deadlocked the USB
     * connect path; iv_usb_inserted() below drops it before that can happen. */
    buf_handle = core_alloc_maximum(&buf_size, NULL);
    if (buf_handle <= 0)
    {
        splash(HZ * 2, "Out of memory");
        return GO_TO_PREVIOUS;
    }

    /* PIN IT. Passing NULL ops does not make an allocation immovable -- it
     * makes it freely movable, because buflib's move_block() treats a missing
     * move_callback as "no one needs telling" and relocates the block. We hold
     * `buf` and every pointer derived from it for the whole session, and the
     * session yields constantly, so a compaction would leave them dangling. */
    core_pin(buf_handle);
    buf = core_get_data(buf_handle);

    /* Fires on the USB thread before the exclusive-storage request, so the
     * pinned buffer is gone before anything can deadlock on it. */
    iv_usb_dropped = false;
    add_event(SYS_EVENT_USB_INSERTED, iv_usb_inserted);

    curfile = -1;
    direction = DIR_NEXT;
    entries = 0;
    get_pic_list(is_album_art);

    if (entries == 0)
    {
        iv_release_buffer();
        remove_event(SYS_EVENT_USB_INSERTED, iv_usb_inserted);
        splash(HZ * 2, "No supported files");
        return GO_TO_PREVIOUS;
    }

    image_type = IMAGE_UNKNOWN;
    imgdec = NULL;
    iv_running_slideshow = false;
    iv_zoom_mode = false;

    push_current_activity(ACTIVITY_IMAGEVIEWER);
    iv_setup_screen();
    iv_splash();

    do
    {
        condition = load_and_show(np_file, &image_info, offset, filesize, status);
        if (condition >= PLUGIN_OTHER)
        {
            if(!is_album_art)
            {
                /* suppress warning while running slideshow */
                status = get_image_type(np_file, iv_running_slideshow);
            }
            continue;
        }
        break;
    } while (true);

    remove_event(SYS_EVENT_USB_INSERTED, iv_usb_inserted);
    iv_release_buffer();

    viewportmanager_theme_undo(SCREEN_MAIN, false);
    pop_current_activity();

    return (condition == PLUGIN_USB_CONNECTED) ? GO_TO_ROOT : GO_TO_PREVIOUS;
}
