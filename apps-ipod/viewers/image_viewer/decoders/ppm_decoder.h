/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/ppm/ppm_decoder.h
 * Copyright (C) 2010 Marcin Bukat
 * GNU General Public License (version 2+)
 *
 * Interface to ppm_decoder.c.
 ****************************************************************************/

#ifndef _PPM_DECODER_H
#define _PPM_DECODER_H

#include "kernel.h"
#include "widgets/splash.h"

/* Magic constants. */
#define PPM_MAGIC1 'P'
#define PPM_MAGIC2 '3'
#define RPPM_MAGIC2 '6'
#define PPM_FORMAT (PPM_MAGIC1 * 256 + PPM_MAGIC2)
#define RPPM_FORMAT (PPM_MAGIC1 * 256 + RPPM_MAGIC2)

#define PPM_OVERALLMAXVAL 65535

#define ppm_error(...) splashf(HZ*2, __VA_ARGS__ )

struct ppm_info {
    int x;
    int y;
    int maxval;
    int format;
    unsigned char *buf;
    size_t buf_size;
    unsigned int native_img_size;
};

/* public prototype */
int read_ppm(int fd, struct ppm_info *ppm);

#endif /* _PPM_DECODER_H */
