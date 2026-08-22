/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/imageviewer.h
 * Core image viewer -- shared definitions.
 *
 * Ported from the imageviewer plugin (apps/plugins/imageviewer) into a plain
 * core app. Targets are the colour 320x240 iPods only, so the greylib
 * (USEGSLIB) and non-colour code paths of the original are gone.
 * GNU General Public License (version 2+)
 *
 * Shared interface between the image viewer and its decoders: the decoder
 * vtable, the shared buffer, and the progress callback.
 ****************************************************************************/

#ifndef _IMAGE_VIEWER_H
#define _IMAGE_VIEWER_H

#include <sys/types.h>  /* ssize_t */
#include "config.h"
#include "lcd.h"
#include "kernel.h"     /* HZ, current_tick (decoders use HZ in splashf) */
#include "widgets/splash.h"     /* splashf (decoders report errors) */
#include "draw/bmp.h"        /* struct bitmap */

/* JPEG colour/dither iv_settings (were in jpeg/yuv2rgb.h in the plugin). */
enum color_modes
{
    COLOURMODE_COLOUR = 0,
    COLOURMODE_GRAY,
    COLOUR_NUM_MODES
};

enum dither_modes
{
    DITHER_NONE = 0,    /* No dithering */
    DITHER_ORDERED,     /* Bayer ordered */
    DITHER_DIFFUSION,   /* Floyd/Steinberg error diffusion */
    DITHER_NUM_MODES
};

/* Slideshow times */
#define SS_MIN_TIMEOUT      1
#define SS_MAX_TIMEOUT      20
#define SS_DEFAULT_TIMEOUT  5

/* Internal status / control codes shared between the viewer loop and the
 * decoders. OK/ERROR/USB mirror the old plugin_status protocol the decoders
 * were written against; the 0x200+ values are viewer-internal control codes.
 * The core entry point translates these into GO_TO_* codes for the browser. */
enum {
    PLUGIN_OK = 0,
    PLUGIN_ERROR = -1,
    PLUGIN_USB_CONNECTED = 1,

    PLUGIN_OTHER = 0x200,
    PLUGIN_ABORT,
    PLUGIN_OUTOFMEM,
    PLUGIN_JPEG_PROGRESSIVE,

    ZOOM_IN,
    ZOOM_OUT,
    ZOOM_FIT,
    NEXT_FRAME,

    /* show_menu()'s "the user asked to leave", which is not a button and so
     * cannot come back as one. */
    IV_MENU_QUIT,
};

/* The downscale value naming the fit-to-screen rendering, as opposed to the
 * integer downscales 1..8 the decoders were written around. It is 0 because
 * that is the one index the decoders' disp[9] caches never used. */
#define DS_FIT 0

/* Settings. jpeg needs these */
struct imgview_settings
{
    int jpeg_colour_mode;
    int jpeg_dither_mode;
    int ss_timeout;
};

/* structure passed to image decoder. */
struct image_info {
    int x_size, y_size; /* set size of loaded image in load_image(). */
    int width, height;  /* set size of resized image in get_image(). */
    int x, y;           /* display position */
    int frames_count;   /* number of subframes */
    int delay;          /* delay expressed in ticks between frames */
    void *data;         /* use freely in decoder. not touched in ui. */
};

/* functions need to be implemented in each image decoder. */
struct image_decoder {
    /* set true if unscaled image can be always displayed even when there isn't
     * enough memory for resized image. e.g. when using native format to store
     * image. */
    const bool unscaled_avail;

    /* return needed size of buffer to store downscaled image by ds.
     * this is used to calculate min downscale. */
    int (*img_mem)(int ds);

    /* load image from filename. use the passed buffer to store loaded, decoded
     * or resized image later, so save it to local variables if needed.
     * set width and height of info properly. also, set buf_size to remaining
     * size of buf after load image. it is used to calculate min downscale.
     * return PLUGIN_ERROR for error. ui will skip to next image. */
    int (*load_image)(char *filename, struct image_info *info,
                      unsigned char *buf, ssize_t *buf_size, int offset, int filesize);
    /* downscale loaded image by ds. use the buffer passed to load_image to
     * reszie image and/or store resized image.
     * return PLUGIN_ERROR for error. ui will skip to next image. */
    int (*get_image)(struct image_info *info, int frame, int ds);

    /* draw part of image */
    void (*draw_image_rect)(struct image_info *info,
                            int x, int y, int width, int height);
};

/* ---- shared viewer state exposed to decoders ------------------------------
 * The plugin passed these through a struct imgdec_api (iv->...). In the core
 * build the decoders are ordinary TUs linked with the viewer, so they read the
 * state directly. */
extern struct imgview_settings iv_settings;
extern bool iv_running_slideshow;   /* loading image because of slideshow */
extern bool iv_slideshow_enabled;   /* run slideshow */

/* Size of the DS_FIT rendering, set by the viewer once the image's own size is
 * known and read by the decoders. Zero when the picture has no fit rung --
 * either it already fits at 1:1, or an integer downscale lands on the same
 * size, and DS_FIT is then never asked for. */
extern int iv_fit_width, iv_fit_height;

/* For decoders that cannot scale to an arbitrary size in one pass: the integer
 * downscale to render before resampling down to the fit size. */
int iv_fit_source_ds(int x_size, int y_size);

/* callback updating a progress meter while image decoding (image_viewer.c) */
void cb_progress(int current, int total);

/* ---- support helpers ported alongside the viewer -------------------------- */

/* Advanced (bilinear) image scale from src to dst; dimensions come from the
 * struct bitmap. (was plugins/lib/bmp_smooth_scale.c) */
void smooth_resize_bitmap(struct bitmap *src, struct bitmap *dst);

/* framebuffer access + hardware scroll for panning (was plugins/lib/xlcd_*) */
fb_data *get_framebuffer(struct viewport *vp, size_t *stride);
void xlcd_scroll_left(int count);
void xlcd_scroll_right(int count);
void xlcd_scroll_up(int count);
void xlcd_scroll_down(int count);

#endif /* _IMAGE_VIEWER_H */
