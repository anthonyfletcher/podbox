/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/jpeg/yuv2rgb.h
 * JPEG image viewer
 * (This is a real mess if it has to be coded in one single C file)
 *
 * File scrolling addition (C) 2005 Alexander Spyridakis
 * Copyright (C) 2004 Jörg Hohensohn aka [IDC]Dragon
 * Heavily borrowed from the IJG implementation (C) Thomas G. Lane
 * Small & fast downscaling IDCT (C) 2002 by Guido Vollbeding  JPEGclub.org
 * GNU General Public License (version 2+)
 *
 * Interface to yuv2rgb.c.
 ****************************************************************************/

#ifndef _JPEG_YUV2RGB_H
#define _JPEG_YUV2RGB_H

/* enum color_modes / dither_modes now live in image_viewer.h */
#include "../image_viewer.h"

void yuv_bitmap_part(unsigned char *src[3], int csub_x, int csub_y,
                     int src_x, int src_y, int stride,
                     int x, int y, int width, int height,
                     int colour_mode, int dither_mode);

#endif
