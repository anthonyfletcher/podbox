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
 *   - Colour 320x240 only; there are no greylib or mono paths.
 *   - Decoders are linked in and chosen from a static table (image_decoder.h),
 *     not loaded as overlays.
 *   - It owns the whole screen with the theme disabled, shows the file's name
 *     while the first image decodes, and draws decode/zoom progress as a dialog
 *     over the retained image rather than blanking the screen.
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
#include "lcd.h"
#include "file.h"            /* MAX_PATH */
#include "fs_attr.h"         /* ATTR_DIRECTORY */
#include "kernel.h"          /* HZ, current_tick, TIME_AFTER, sleep, yield */
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

/* True only while a zoom is being re-decoded. */
static bool iv_zooming = false;

/* True until the first image has been decoded, so that decode reports progress
 * under the name splash. */
static bool iv_first_load = true;

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
static int show_menu(void); /* returns 1 to quit the viewer */

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

/* Read directory contents for scrolling. */
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

    /* Remove path and leave only the name.*/
    pname = strrchr(np_file,'/');
    pname++;

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
}

static int change_filename(int direct)
{
    bool file_erased = (file_pt[curfile] == NULL);
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
        splash(HZ, "No supported files");
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

    /* Shown for the first decode and while zooming; on file navigation the
     * previous image stays up, and slideshows keep the screen clear. */
    if ((iv_first_load || iv_zooming) && !iv_running_slideshow)
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
            if (entries > 1 && info->width <= LCD_WIDTH
                            && info->height <= LCD_HEIGHT)
            {
                int result = change_filename(DIR_PREV);
                if (entries > 1)
                    return result;
            }
            /* fallthrough */
        case IMGVIEW_LEFT | BUTTON_REPEAT:
            pan_view_left(info);
            break;

        case IMGVIEW_RIGHT:
            if (entries > 1 && info->width <= LCD_WIDTH
                            && info->height <= LCD_HEIGHT)
            {
                int result = change_filename(DIR_NEXT);
                if (entries > 1)
                    return result;
            }
            /* fallthrough */
        case IMGVIEW_RIGHT | BUTTON_REPEAT:
            pan_view_right(info);
            break;

        case IMGVIEW_UP:
            /* no BUTTON_REPEAT variant: Menu+repeat is the settings gesture */
            pan_view_up(info);
            break;

        case IMGVIEW_DOWN:
        case IMGVIEW_DOWN | BUTTON_REPEAT:
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
            else
                return NEXT_FRAME;

            break;

        case IMGVIEW_ZOOM_IN:
            return ZOOM_IN;
            break;

        case IMGVIEW_ZOOM_OUT:
            return ZOOM_OUT;
            break;

        case IMGVIEW_MENU:
            if (show_menu() == 1)
                return PLUGIN_OK;

            /* the menu ran with the theme on; repaint our image */
            lcd_clear_display();
            imgdec->draw_image_rect(info, 0, 0,
                            info->width-info->x, info->height-info->y);
            lcd_update();
            break;

#ifdef IMGVIEW_QUIT
        case IMGVIEW_QUIT:
            return PLUGIN_OK;
            break;
#endif

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

    /* Load-time decoding shows no progress dialog (the name splash stays up);
     * only a later zoom re-decode does. */
    iv_zooming = false;

reload_decoder:
    /* Note: the screen is deliberately NOT cleared here -- the previous image
     * stays up while the next one decodes, with the progress dialog over it. */

    if (image_type != status) /* type of image is changed, load decoder. */
    {
        image_type = status;
        imgdec = get_image_decoder(image_type);
        if (imgdec == NULL)
        {
            splashf(2*HZ, "Unknown type: %d", image_type);
            return PLUGIN_ERROR;
        }
    }
    memset(info, 0, sizeof(*info));
    remaining = buf_size;

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
        splash(HZ, "Too large");
        file_pt[curfile] = NULL;
        return change_filename(direction);
    }
    else if (status == PLUGIN_ERROR)
    {
        file_pt[curfile] = NULL;
        return change_filename(direction);
    }
    else if (status == PLUGIN_ABORT) {
        splash(HZ, "Aborted");
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
            splash(HZ, "Too large");
            file_pt[curfile] = NULL;
            return change_filename(direction);
        }
    }
    else if (ds_max < ds_min && !(ds_max == 1 && imgdec->unscaled_avail))
        ds_max = ds_min;

    ds = ds_max; /* initialize setting */
    cx = info->x_size/ds/2; /* center the view */
    cy = info->y_size/ds/2;

    /* used to loop through subimages in animated gifs */
    int frame = 0;
    do  /* loop the image prepare and decoding when zoomed */
    {
        /* a re-entry here after ZOOM_IN/OUT is a zoom re-decode -> show progress */
        iv_zooming = (status == ZOOM_IN || status == ZOOM_OUT);
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
                if (ds > ds_min || (imgdec->unscaled_avail && ds > 1))
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
                if (ds < ds_max)
                {
                    /* if ds is 1 and ds_min is > 1, jump ds to ds_min. */
                    int zoom = (ds < ds_min)? ds_min: 2;
                    ds *= zoom; /* increase downscaling to zoom out */
                    get_view(info, &cx, &cy);
                    cx /= zoom; /* prepare the position in the new image */
                    cy /= zoom;
                }
                else
                    continue;
            }

            /* next frame in animated content */
            if (status == NEXT_FRAME)
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

/* return 1 to quit */
static int show_menu(void)
{
    int result;

    enum menu_id
    {
        MIID_TOGGLE_SS_MODE = 0,
        MIID_CHANGE_SS_MODE,
        MIID_DITHERING,
        MIID_QUIT,
    };

    MENUITEM_STRINGLIST(menu, "Image Viewer", NULL,
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

    result = do_menu(&menu, NULL, NULL, false);

    switch (result)
    {
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
        case MIID_QUIT:
            result = 1;
            break;
    }

    pop_current_activity();
    viewportmanager_theme_undo(SCREEN_MAIN, false);

    /* re-establish our own drawing environment */
    lcd_set_backdrop(NULL);
    lcd_set_foreground(LCD_WHITE);
    lcd_set_background(LCD_BLACK);

    return (result == 1) ? 1 : 0;
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

    if (!file)
    {
        if (!find_album_art(&offset, &filesize, &status))
        {
            splash(HZ * 2, "No file");
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
        splash(HZ * 2, "Out of Memory");
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
        splash(HZ, "No supported files");
        return GO_TO_PREVIOUS;
    }

    image_type = IMAGE_UNKNOWN;
    imgdec = NULL;
    iv_running_slideshow = false;
    iv_first_load = true;

    push_current_activity(ACTIVITY_IMAGEVIEWER);
    iv_setup_screen();
    iv_splash();

    do
    {
        condition = load_and_show(np_file, &image_info, offset, filesize, status);
        iv_first_load = false;
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
