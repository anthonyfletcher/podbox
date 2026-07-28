/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/image_decoder.h
 * Image decoder selection (core build).
 *
 * The plugin loaded each format's decoder as a separate .ovl overlay; the core
 * build links them all in and picks one from a static table by type.
 * GNU General Public License (version 2+)
 *
 * The image_decoder vtable each format back end implements.
 ****************************************************************************/

#ifndef _IMAGE_DECODER_H
#define _IMAGE_DECODER_H

#include "image_viewer.h"

enum image_type {
    IMAGE_UNKNOWN = -1,
    IMAGE_BMP = 0,
    IMAGE_JPEG,
    IMAGE_PNG,
    IMAGE_PPM,
    IMAGE_GIF,
    IMAGE_JPEG_PROGRESSIVE,

    MAX_IMAGE_TYPES
};

/* Check file type by magic number or file extension */
enum image_type get_image_type(const char *name, bool quiet);
/* Get the decoder for a given image type (static table lookup). */
const struct image_decoder *get_image_decoder(enum image_type type);

/* Each decoder exports its own struct image_decoder. */
extern const struct image_decoder bmp_decoder;
extern const struct image_decoder jpeg_decoder;
extern const struct image_decoder jpegp_decoder;
extern const struct image_decoder png_decoder;
extern const struct image_decoder ppm_decoder;
extern const struct image_decoder gif_decoder;

#endif /* _IMAGE_DECODER_H */
