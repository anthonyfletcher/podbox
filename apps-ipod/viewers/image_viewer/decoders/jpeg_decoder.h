/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/jpeg/jpeg_decoder.h
 * JPEG image viewer
 * (This is a real mess if it has to be coded in one single C file)
 *
 * File scrolling addition (C) 2005 Alexander Spyridakis
 * Copyright (C) 2004 Jörg Hohensohn aka [IDC]Dragon
 * Heavily borrowed from the IJG implementation (C) Thomas G. Lane
 * Small & fast downscaling IDCT (C) 2002 by Guido Vollbeding  JPEGclub.org
 * GNU General Public License (version 2+)
 *
 * Interface to jpeg_decoder.c.
 ****************************************************************************/

#ifndef _JPEG_JPEG_DECODER_H
#define _JPEG_JPEG_DECODER_H
#include "draw/jpeg_common.h"

struct jpeg
{
    int x_size, y_size; /* size of image (can be less than block boundary) */
    int x_phys, y_phys; /* physical size, block aligned */
    int x_mbl; /* x dimension of MBL */
    int y_mbl; /* y dimension of MBL */
    int blocks; /* blocks per MB */
    int restart_interval; /* number of MCUs between RSTm markers */
    int store_pos[4]; /* for Y block ordering */

    unsigned char* p_entropy_data;
    unsigned char* p_entropy_end;

    int quanttable[4][QUANT_TABLE_LENGTH]; /* raw quantization tables 0-3 */
    int qt_idct[2][QUANT_TABLE_LENGTH]; /* quantization tables for IDCT */

    struct huffman_table hufftable[2]; /* Huffman tables  */
    struct derived_tbl dc_derived_tbls[2]; /* Huffman-LUTs */
    struct derived_tbl ac_derived_tbls[2];

    struct frame_component frameheader[3]; /* Component descriptor */
    struct scan_component scanheader[3]; /* currently not used */

    int mcu_membership[6]; /* info per block */
    int tab_membership[6];
    int subsample_x[3]; /* info per component */
    int subsample_y[3];
};

/* various helper functions */
void default_huff_tbl(struct jpeg* p_jpeg);
void build_lut(struct jpeg* p_jpeg);
int process_markers(unsigned char* p_src, long size, struct jpeg* p_jpeg);

/* the main decode function */
int jpeg_decode(struct jpeg* p_jpeg, unsigned char* p_pixel[3],
                int downscale, void (*pf_progress)(int current, int total));


#endif /* _JPEG_JPEG_DECODER_H */
