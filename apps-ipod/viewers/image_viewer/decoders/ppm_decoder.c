/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/ppm/ppm_decoder.c
 * Copyright (C) 2008 Alexander Papst
 * GNU General Public License (version 2+)
 *
 * Netpbm parser: reads the ASCII or binary PPM/PGM header and pixel data.
 ****************************************************************************/

#include <limits.h>
#include "file.h"
#include "lcd.h"
#include "ppm_decoder.h"
#include "../image_viewer.h"

static int ppm_read_magic_number(int fd)
{
    unsigned char i1, i2;
    if(read(fd, &i1, 1) < 1 || read(fd, &i2, 1) < 1)
    {
        ppm_error( "Error reading magic number from ppm image stream. "\
                   "Most often, this means your input file is empty." );
        return PLUGIN_ERROR;
    }
    return i1 * 256 + i2;
}

static int ppm_getc(int fd)
{
    unsigned char ch;

    if (read(fd, &ch, 1) < 1) {
        ppm_error("EOF. Read error reading a byte");
        return PLUGIN_ERROR;
    }

    if (ch == '#') {
        do {
            if (read(fd, &ch, 1) < 1) {
                ppm_error("EOF. Read error reading a byte");
                return PLUGIN_ERROR;
            }
        } while (ch != '\n' && ch != '\r');
    }
    return (int)ch;
}

static int ppm_getuint(int fd)
{
    int ch;
    int i;
    int digitVal;

    do {
        ch = ppm_getc(fd);
    } while (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r');

    if (ch == PLUGIN_ERROR) return PLUGIN_ERROR;
    if (ch < '0' || ch > '9') {
        ppm_error("Junk (%c) in file where an integer should be.", ch);
        return PLUGIN_ERROR;
    }

    i = 0;

    do {
        digitVal = ch - '0';

        if (i > INT_MAX/10 - digitVal) {
            ppm_error("ASCII decimal integer in file is "\
                      "too large to be processed.");
            return PLUGIN_ERROR;
        }

        i = i * 10 + digitVal;
        ch = ppm_getc(fd);

    } while (ch >= '0' && ch <= '9');
    if (ch == PLUGIN_ERROR) return PLUGIN_ERROR;

    return i;
}

static int ppm_getrawbyte(int fd)
{
    unsigned char by;

    if (read(fd, &by, 1) < 1) {
        ppm_error("EOF. Read error while reading a one-byte sample.");
        return PLUGIN_ERROR;
    }

    return (int)by;
}

static int ppm_getrawsample(int fd, int const maxval)
{
    if (maxval < 256) {
        /* The sample is just one byte. Read it. */
        return(ppm_getrawbyte(fd));
    } else {
        /* The sample is two bytes. Read both. */
        unsigned char byte_pair[2];

        if (read(fd, byte_pair, 2) < 2) {
            ppm_error("EOF. Read error while reading a long sample.");
            return PLUGIN_ERROR;
        }
        return((byte_pair[0]<<8) | byte_pair[1]);
    }
}

/* Read from the file header dimensions as well as max
 * int value used
 */
static int read_ppm_init_rest(int fd, struct ppm_info *ppm)
{
    /* Read size. */
    ppm->x = ppm_getuint(fd);
    ppm->y = ppm_getuint(fd);

    /* Both are int, and ppm_getuint() lets either reach a fifth of
     * INT_MAX, so the product below is where a header decides how much
     * gets written -- and it is the only thing that decides. 65536 by
     * 65536 multiplies out to zero, clears the test that follows, and
     * then writes a row at a time until the file runs out. -1 is also
     * what ppm_getuint() returns for a header it could not read.
     *
     * Divide instead of multiplying: x*y*FB_DATA_SZ > buf_size is the
     * same question, and this form cannot leave the range it is testing.
     * y is at least 1 by the line above. */
    if (ppm->x < 1 || ppm->y < 1) {
        ppm_error("Image has no area.");
        return PLUGIN_ERROR;
    }

    if ((size_t)ppm->x > ppm->buf_size / FB_DATA_SZ / (size_t)ppm->y) {
        return PLUGIN_OUTOFMEM;
    }

    ppm->native_img_size = (unsigned int)ppm->x * ppm->y * FB_DATA_SZ;

    /* Read maxval. */
    ppm->maxval = ppm_getuint(fd);

    if (ppm->maxval > PPM_OVERALLMAXVAL) {
        ppm_error("maxval of input image (%u) is too large. "\
                  "The maximum allowed by the PPM is %u.",
                  ppm->maxval, PPM_OVERALLMAXVAL);
        return PLUGIN_ERROR;
    }
    if (ppm->maxval == 0) {
        ppm_error("maxval of input image is zero.");
        return PLUGIN_ERROR;
    }
    return PLUGIN_OK;
}

static int read_ppm_init(int fd, struct ppm_info *ppm)
{
    /* Check magic number. */
    ppm->format = ppm_read_magic_number( fd );

    if (ppm->format == PLUGIN_ERROR) return PLUGIN_ERROR;
    switch (ppm->format) {
        case PPM_FORMAT:
        case RPPM_FORMAT:
            return read_ppm_init_rest(fd, ppm);

        default:
            ppm_error( "Bad magic number - not a ppm or rppm file." );
            return PLUGIN_ERROR;
    }
    return PLUGIN_OK;
}

static int read_ppm_row(int fd, struct ppm_info *ppm, int row)
{
    int col;
    int r, g, b;
    fb_data *dst = (fb_data *) ppm->buf + ppm->x*row;
    const int stride = 1;
    switch (ppm->format) {
        case PPM_FORMAT:
            for (col = 0; col < ppm->x; ++col) {
                r = ppm_getuint(fd);
                g = ppm_getuint(fd);
                b = ppm_getuint(fd);

                if (r == PLUGIN_ERROR || g == PLUGIN_ERROR ||
                    b == PLUGIN_ERROR)
                {
                    return PLUGIN_ERROR;
                }
                *dst = FB_RGBPACK(
                    (255 * r)/ppm->maxval,
                    (255 * g)/ppm->maxval,
                    (255 * b)/ppm->maxval);
                dst += stride;
            }
            break;

        case RPPM_FORMAT:
            for (col = 0; col < ppm->x; ++col) {
                r = ppm_getrawsample(fd, ppm->maxval);
                g = ppm_getrawsample(fd, ppm->maxval);
                b = ppm_getrawsample(fd, ppm->maxval);

                if (r == PLUGIN_ERROR || g == PLUGIN_ERROR ||
                    b == PLUGIN_ERROR)
                {
                    return PLUGIN_ERROR;
                }
                *dst = FB_RGBPACK(
                    (255 * r)/ppm->maxval,
                    (255 * g)/ppm->maxval,
                    (255 * b)/ppm->maxval);
                dst += stride;
            }
            break;

        default:
            ppm_error("What?!");
            return PLUGIN_ERROR;
    }
    return PLUGIN_OK;
}

/* public */
int read_ppm(int fd, struct ppm_info *ppm)
{
    int row, ret;

    ret = read_ppm_init(fd, ppm);
    if(ret != PLUGIN_OK) {
        return ret;
    }

    for (row = 0; row < ppm->y; ++row) {
        ret = read_ppm_row(fd, ppm, row);
        if(ret != PLUGIN_OK) {
            return ret;
        }
        cb_progress(row, ppm->y);
    }
    return PLUGIN_OK;
}
