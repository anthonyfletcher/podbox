/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/gif/gif_decoder.h
 * Copyright (c) 2012 Marcin Bukat
 * GNU General Public License (version 2+)
 *
 * Interface to gif_decoder.c.
 ****************************************************************************/

struct gif_decoder {
    unsigned char *mem;
    size_t mem_size;
    int width;
    int height;
    int frames_count;
    int delay;
    size_t native_img_size;
    int error;
};

void gif_decoder_init(struct gif_decoder *decoder, void *mem, size_t size);
void gif_decoder_destroy_memory_pool(struct gif_decoder *d);
void gif_open(char *filename, struct gif_decoder *d);
void gif_decode(struct gif_decoder *d, void (*pf_progress)(int current, int total));

