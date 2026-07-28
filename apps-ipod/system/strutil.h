/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.h (string, path and line parsing)
 * Copyright (C) 2002 by Daniel Stenberg
 * GNU General Public License (version 2+)
 *
 * Interface to strutil.c, plus the byte-order-mark constants.
 ****************************************************************************/

#ifndef _STRUTIL_H_
#define _STRUTIL_H_

#include <stdbool.h>
#include <stddef.h>

/* Read (up to) a line of text from fd into buffer and return number of bytes
 * read (which may be larger than the number of bytes stored in buffer). If
 * an error occurs, -1 is returned (and buffer contains whatever could be
 * read). A line is terminated by a LF char. Neither LF nor CR chars are
 * stored in buffer.
 */
int read_line(int fd, char* buffer, int buffer_size);
int fast_readline(int fd, char *buf, int buf_size, void *parameters,
                  int (*callback)(int n, char *buf, void *parameters));

bool settings_parseline(char* line, char** name, char** value);

/* Unicode byte order mark sequences and lengths */
#define BOM_UTF_8 "\xef\xbb\xbf"
#define BOM_UTF_8_SIZE 3
#define BOM_UTF_16_LE "\xff\xfe"
#define BOM_UTF_16_BE "\xfe\xff"
#define BOM_UTF_16_SIZE 2

int split_string(char *str, const char needle, char *vector[], int vector_length);
#ifndef O_PATH
#define O_PATH 0x2000
#endif

void fix_path_part(char* path, int offset, int count);
int open_pathfmt(char *buf, size_t size, int oflag, const char *pathfmt, ...);
int open_utf8(const char* pathname, int flags);
int string_option(const char *option, const char *const oplist[], bool ignore_case);

char* strrsplt(char* str, int c);
char* skip_whitespace(char* const str);

/*
 * removes the extension of filename (if it doesn't start with a .)
 * puts the result in buffer
 */
char *strip_extension(char* buffer, int buffer_size, const char *filename);


#endif /* _STRUTIL_H_ */
