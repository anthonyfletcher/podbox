/***************************************************************************
 * Original code from RockBox
 * was: apps/filetypes.h
 * Copyright (C) 2002 Henrik Backe
 * GNU General Public License (version 2+)
 *
 * Interface to filetypes.c and the FILE_ATTR_* attribute constants.
 ****************************************************************************/
#ifndef _FILETYPES_H_
#define _FILETYPES_H_

#include <stdbool.h>
#include "config.h"
#include "screens/browse/browser.h"

/* using attribute bits not used by FAT (FAT uses lower 7) */
#define FILE_ATTR_THUMBNAIL 0x0080 /* corresponding .talk file exists */
/* (this also reflects the sort order if by type) */
#define FILE_ATTR_BMARK 0x0100 /* book mark file */
#define FILE_ATTR_M3U   0x0200 /* playlist */
#define FILE_ATTR_AUDIO 0x0300 /* audio file */
#define FILE_ATTR_CFG   0x0400 /* config file */
#define FILE_ATTR_WPS   0x0500 /* wps config file */
#define FILE_ATTR_FONT  0x0600 /* font file */
#define FILE_ATTR_LNG   0x0700 /* binary lang file */
#define FILE_ATTR_ROCK  0x0800 /* binary rockbox plugin */
#define FILE_ATTR_MOD   0x0900 /* firmware file */
#define FILE_ATTR_RWPS  0x0A00 /* remote-wps config file */
#define FILE_ATTR_BMP   0x0B00 /* backdrop bmp file */
#define FILE_ATTR_KBD   0x0C00 /* keyboard file */
#define FILE_ATTR_FMR   0x0D00 /* preset file */
#define FILE_ATTR_CUE   0x0E00 /* cuesheet file */
#define FILE_ATTR_SBS   0x0F00 /* statusbar file */
#define FILE_ATTR_RSBS  0x1000 /* remote statusbar file */
#define FILE_ATTR_LUA   0x1100 /* Lua rockbox plugin */
/* 0x1200 and 0x1300 are deliberately unused. These constants are explicit
 * rather than sequential, and the gap keeps every value below stable; do not
 * fill it in or renumber to close it. */
#define FILE_ATTR_OPX   0x1400 /* open plugins shortcut */
#define FILE_ATTR_LOG   0x1500 /* log file */
#define FILE_ATTR_TXT   0x1600 /* document handled by the core text viewer */
#define FILE_ATTR_IMG   0x1700 /* image handled by the core image viewer */
#define FILE_ATTR_MASK  0xFF00 /* which bits tree.c uses for file types */

long filetype_get_voiceclip(int attr);

/* init the filetypes structs.
   uses audio buffer for storage, so call early in init... */
void filetype_init(void) INIT_ATTR;

void read_viewer_theme_file(void);
void read_color_theme_file(void);

/* Return the attribute (FILE_ATTR_*) of the file */
int filetype_get_attr(const char* file);
int filetype_get_color(const char* name, int attr);
int filetype_get_icon(int attr);

/* returns true if the attr is supported */
bool  filetype_supported(int attr);

/* If `attr` is a type owned by a core-linked viewer (text/image), runs that
 * viewer on `file`, stores its GO_TO_* code in *rc and returns true. Returns
 * false (leaving *rc alone) when there is no core viewer for the type. */
bool  filetype_open_core_viewer(int attr, const char *file, int *rc);


#endif
