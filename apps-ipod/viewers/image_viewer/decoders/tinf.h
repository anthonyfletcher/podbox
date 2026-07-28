/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/png/tinf.h
 * Original source:
 * Copyright (c) 2003 by Joergen Ibsen / Jibz
 *
 * Rockbox adaptation:
 * Copyright (c) 2010 by Marcin Bukat
 * GNU General Public License (version 2+)
 *
 * Interface to the tinf decompressor.
 ****************************************************************************/

/*
 * tinf  -  tiny inflate library (inflate, gzip, zlib)
 *
 * version 1.00
 *
 * Copyright (c) 2003 by Joergen Ibsen / Jibz
 * All Rights Reserved
 *
 * http://www.ibsensoftware.com/
 */

/* removed from original file:
 * tinf_gzip_uncompress() prototype
 * tinf_init() prototype
 */

#ifndef TINF_H_INCLUDED
#define TINF_H_INCLUDED

/* calling convention */
#ifndef TINFCC
 #ifdef __WATCOMC__
  #define TINFCC __cdecl
 #else
  #define TINFCC
 #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TINF_OK             0
#define TINF_DATA_ERROR    (-3)

/* function prototypes */

int TINFCC tinf_uncompress(void *dest, unsigned int *destLen,
                           const void *source, unsigned int sourceLen);

int TINFCC tinf_zlib_uncompress(void *dest, unsigned int *destLen,
                                const void *source, unsigned int sourceLen);

unsigned int TINFCC tinf_adler32(const void *data, unsigned int length);

unsigned int TINFCC tinf_crc32(const void *data, unsigned int length);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TINF_H_INCLUDED */
