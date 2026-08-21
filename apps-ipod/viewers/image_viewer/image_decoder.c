/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/image_decoder.c
 * Image type detection and decoder selection (core build).
 * GNU General Public License (version 2+)
 *
 * The decoder registry: maps a file's type to the right decoder and
 * exposes a uniform load interface to image_viewer.c.
 ****************************************************************************/

#include <string.h>
#include "file.h"
#include "kernel.h"
#include "system.h"      /* ARRAYLEN */
#include "widgets/splash.h"
#include "image_viewer.h"
#include "image_decoder.h"

/* Static decoder table, indexed by enum image_type. IMAGE_JPEG_PROGRESSIVE is
 * reached from the baseline decoder when it detects a progressive stream. */
static const struct image_decoder * const decoders[MAX_IMAGE_TYPES] = {
    [IMAGE_BMP]              = &bmp_decoder,
    [IMAGE_JPEG]             = &jpeg_decoder,
    [IMAGE_PNG]              = &png_decoder,
    [IMAGE_PPM]             = &ppm_decoder,
    [IMAGE_GIF]              = &gif_decoder,
    [IMAGE_JPEG_PROGRESSIVE] = &jpegp_decoder,
};

const struct image_decoder *get_image_decoder(enum image_type type)
{
    if (type < 0 || type >= MAX_IMAGE_TYPES)
        return NULL;
    return decoders[type];
}

/* Check file type by magic number or file extension
 *
 * If the file contains magic number, use it to determine image type.
 * Otherwise use file extension to determine image type.
 * If the file contains magic number and file extension is not correct,
 * informs user that something is wrong.
 */
enum image_type get_image_type(const char *name, bool quiet)
{
    static const struct {
        char *ext;
        enum image_type type;
    } ext_list[] = {
        { ".bmp",   IMAGE_BMP  },
        { ".jpg",   IMAGE_JPEG },
        { ".jpe",   IMAGE_JPEG },
        { ".jpeg",  IMAGE_JPEG },
        { ".png",   IMAGE_PNG  },
        { ".ppm",   IMAGE_PPM  },
        { ".gif",   IMAGE_GIF  },
    };
    static const struct {
        char *magic;    /* magic number */
        int length;     /* length of the magic number */
        enum image_type type;
    } magic_list[] = {
        { "BM", 2, IMAGE_BMP },
        { "\xff\xd8\xff\xe0", 4, IMAGE_JPEG },
        { "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a", 8, IMAGE_PNG },
        { "P3", 2, IMAGE_PPM },
        { "P6", 2, IMAGE_PPM },
        { "GIF87a", 6, IMAGE_GIF },
        { "GIF89a", 6, IMAGE_GIF },
    };

    enum image_type type = IMAGE_UNKNOWN;
    const char *ext = strrchr(name, '.');
    int i, fd;
    char buf[12];

    /* check file extention */
    if (ext)
    {
        for (i = 0; i < (int)ARRAYLEN(ext_list); i++)
        {
            if (!strcasecmp(ext, ext_list[i].ext))
            {
                type = ext_list[i].type;
                break;
            }
        }
    }

    /* check magic value in the file */
    fd = open(name, O_RDONLY);
    if (fd >= 0)
    {
        memset(buf, 0, sizeof buf);
        read(fd, buf, sizeof buf);
        close(fd);
        for (i = 0; i < (int)ARRAYLEN(magic_list); i++)
        {
            if (!memcmp(buf, magic_list[i].magic, magic_list[i].length))
            {
                if (!quiet && type != magic_list[i].type)
                {
                    /* file extension is wrong. */
                    splash(HZ * 2, "Wrong file extension");
                }
                type = magic_list[i].type;
                break;
            }
        }
    }
    return type;
}
