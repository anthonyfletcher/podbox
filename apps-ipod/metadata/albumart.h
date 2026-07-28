/***************************************************************************
 * Original code from RockBox
 * was: apps/recorder/albumart.h
 * Copyright (C) 2007 Nicolas Pennequin
 * GNU General Public License (version 2+)
 *
 * Interface to albumart.c.
 ****************************************************************************/

#ifndef _ALBUMART_H_
#define _ALBUMART_H_


#include <stdbool.h>
#include "metadata.h"
#include "skin/skin_engine.h"

/* Look for albumart bitmap in the same dir as the track and in its parent dir.
 * Calls size_func to get the dimensions to look for
 * Stores the found filename in the buf parameter.
 * Returns true if a bitmap was found, false otherwise */
bool find_albumart(const struct mp3entry *id3, char *buf, int buflen,
                    const struct dim *dim);

bool search_albumart_files(const struct mp3entry *id3, const char *size_string,
                           char *buf, int buflen);

void get_albumart_size(struct bitmap *bmp);


#endif /* _ALBUMART_H_ */
