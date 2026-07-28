/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.c (string, path and line parsing)
 * Copyright (C) 2002 by Daniel Stenberg
 * GNU General Public License (version 2+)
 *
 * String, path and line-parsing helpers: reading lines from a file,
 * splitting settings lines, path fixups, and the UTF-8 aware open helpers.
 ****************************************************************************/

#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"
#include "file.h"
#include "dir.h"
#include "pathfuncs.h"
#include "rbunicode.h"
#include "strutil.h"

/* Performance optimized version of the read_line() (see below) function. */
int fast_readline(int fd, char *buf, int buf_size, void *parameters,
                  int (*callback)(int n, char *buf, void *parameters))
{
    char *p, *next;
    int rc, pos = 0;
    int count = 0;

    while ( 1 )
    {
        next = NULL;

        rc = read(fd, &buf[pos], buf_size - pos - 1);
        if (rc >= 0)
            buf[pos+rc] = '\0';

        if ( (p = strchr(buf, '\n')) != NULL)
        {
            *p = '\0';
            next = ++p;
        }

        if ( (p = strchr(buf, '\r')) != NULL)
        {
            *p = '\0';
            if (!next)
                next = ++p;
        }

        rc = callback(count, buf, parameters);
        if (rc < 0)
            return rc;

        count++;
        if (next)
        {
            pos = buf_size - ((intptr_t)next - (intptr_t)buf) - 1;
            memmove(buf, next, pos);
        }
        else
            break ;
    }

    return 0;
}

/* parse a line from a configuration file. the line format is:

   name: value

   Any whitespace before setting name or value (after ':') is ignored.
   A # as first non-whitespace character discards the whole line.
   Function sets pointers to null-terminated setting name and value.
   Returns false if no valid config entry was found.
*/

bool settings_parseline(char* line, char** name, char** value)
{
    char* ptr;

    line = skip_whitespace(line);

    if ( *line == '#' )
        return false;

    ptr = strchr(line, ':');
    if ( !ptr )
        return false;

    *name = line;
    *ptr = '\0'; /* terminate previous */
    ptr++;
    ptr = skip_whitespace(ptr);
    *value = ptr;

    /* strip whitespace from the right side of value */
    ptr += strlen(ptr);
    ptr--;
    while ((ptr > (*value) - 1) && isspace(*ptr))
    {
        *ptr = '\0';
        ptr--;
    }

    return true;
}

char* strrsplt(char* str, int c)
{
    char* s = strrchr(str, c);

    if (s != NULL)
    {
        *s++ = '\0';
    }
    else
    {
        s = str;
    }

    return s;
}

/*
 * removes the extension of filename (if it doesn't start with a .)
 * puts the result in buffer
 */
char *strip_extension(char* buffer, int buffer_size, const char *filename)
{
    if (!buffer || !filename || buffer_size <= 0)
    {
        return NULL;
    }

    off_t dotpos = (strrchr(filename, '.') - filename) + 1;

    /* no match on filename beginning with '.' or beyond buffer_size */
    if(dotpos > 1 && dotpos < buffer_size)
        buffer_size = dotpos;
    strmemccpy(buffer, filename, buffer_size);

    return buffer;
}

/* Read (up to) a line of text from fd into buffer and return number of bytes
 * read (which may be larger than the number of bytes stored in buffer). If
 * an error occurs, -1 is returned (and buffer contains whatever could be
 * read). A line is terminated by a LF char. Neither LF nor CR chars are
 * stored in buffer.
 */
int read_line(int fd, char* buffer, int buffer_size)
{
    if (!buffer || buffer_size-- <= 0)
    {
        errno = EINVAL;
        return -1;
    }

    unsigned char rdbuf[32];
    off_t rdbufend = 0;
    int rdbufidx = 0;
    int count = 0;
    int num_read = 0;

    while (count < buffer_size)
    {
        if (rdbufidx >= rdbufend)
        {
            rdbufidx = 0;
            rdbufend = read(fd, rdbuf, sizeof (rdbuf));

            if (rdbufend <= 0)
                break;

            num_read += rdbufend;
        }

        int c = rdbuf[rdbufidx++];

        if (c == '\n')
            break;

        if (c == '\r')
            continue;

        buffer[count++] = c;
    }

    rdbufidx -= rdbufend;

    if (rdbufidx < 0)
    {
        /* "put back" what wasn't read from the buffer */
        num_read += rdbufidx;
        rdbufend = lseek(fd, rdbufidx, SEEK_CUR);
    }

    buffer[count] = '\0';

    return rdbufend >= 0 ? num_read : -1;
}

char* skip_whitespace(char* const str)
{
    char *s = str;

    while (isspace(*s))
        s++;

    return s;
}

/**
 * Splits str at each occurence of split_char and puts the substrings into vector,
 * but at most vector_lenght items. Empty substrings are ignored.
 *
 * Modifies str by replacing each split_char following a substring with nul
 *
 * Returns the number of substrings found, i.e. the number of valid strings
 * in vector
 */
int split_string(char *str, const char split_char, char *vector[], const int vector_length)
{
    int i;
    char sep[2] = {split_char, '\0'};
    char *e, *p = strtok_r(str, sep, &e);

    /* strtok takes care of leading & trailing splitters */
    for(i = 0; i < vector_length; i++)
    {
        vector[i] = p;
        if (!p)
            break;
        p = strtok_r(NULL, sep, &e);
    }

    return i;
}

/* returns match index from option list
 * returns -1 if option was not found
 * option list is array of char pointers with the final item set to null
 * ex - const char * const option[] = { "op_a", "op_b", "op_c", NULL}
 */
int string_option(const char *option, const char *const oplist[], bool ignore_case)
{
    const char *op;
    int (*cmp_fn)(const char*, const char*) = &strcasecmp;
    if (!ignore_case)
        cmp_fn = strcmp;
    for (int i=0; (op=oplist[i]) != NULL; i++)
    {
        if (cmp_fn(op, option) == 0)
            return i;
    }
    return -1;
}

/* Make sure part of path only contain chars valid for a FAT32 long name.
 * Double quotes are replaced with single quotes, other unsupported chars
 * are replaced with an underscore.
 *
 * path   - path to modify.
 * offset - where in path to start checking.
 * count  - number of chars to check.
 */
void fix_path_part(char* path, int offset, int count)
{
    static const char invalid_chars[] = "*/:<>?\\|";
    int i;

    path += offset;

    for (i = 0; i <= count; i++, path++)
    {
        if (*path == 0)
            return;
        if (*path == '"')
            *path = '\'';
        else if (strchr(invalid_chars, *path))
            *path = '_';
    }
}

/* open but with a builtin printf for assembling the path */
int open_pathfmt(char *buf, size_t size, int oflag, const char *pathfmt, ...)
{
    va_list ap;
    va_start(ap, pathfmt);
    vsnprintf(buf, size, pathfmt, ap);
    va_end(ap);
    if ((oflag & O_PATH) == O_PATH)
        return -1;
    return open(buf, oflag, 0666);
}

/** Open a UTF-8 file and set file descriptor to first byte after BOM.
 *  If no BOM is present this behaves like open().
 *  If the file is opened for writing and O_TRUNC is set, write a BOM to
 *  the opened file and leave the file pointer set after the BOM.
 */

int open_utf8(const char* pathname, int flags)
{
    ssize_t ret;
    int fd;
    unsigned char bom[BOM_UTF_8_SIZE];

    fd = open(pathname, flags, 0666);
    if(fd < 0)
        return fd;

    if(flags & (O_TRUNC | O_WRONLY))
    {
        ret = write(fd, BOM_UTF_8, BOM_UTF_8_SIZE);
    }
    else
    {
        ret = read(fd, bom, BOM_UTF_8_SIZE);
        /* check for BOM */
        if (ret == BOM_UTF_8_SIZE)
        {
            if(memcmp(bom, BOM_UTF_8, BOM_UTF_8_SIZE))
                lseek(fd, 0, SEEK_SET);
        }
    }
    /* read or write failure, do not continue */
    if (ret < 0)
        close(fd);

    return ret >= 0 ? fd : -1;
}

