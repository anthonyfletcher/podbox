/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The one container for the library's binary files: a header, then count
 * fixed-size records, then optionally a tail of whatever the file needs
 * beside them. The header names the file (magic), the layout of its records
 * (version, record_size), how many there are, the database marks they were
 * made for, and a CRC-32 of everything after the header.
 *
 * A file is written whole to name.new and renamed over name, which replaces
 * it in one step: until the rename the old file is intact. One record can be
 * appended in place, and the marks rewritten in place, for files that grow a
 * record at a time.
 ****************************************************************************/

#ifndef _LIBFILE_H
#define _LIBFILE_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "file.h"          /* MAX_PATH */

/* What the database looked like when a file was made; all -1 for none */
struct libfile_marks {
    int32_t entries;
    int32_t commitid;
    int32_t deleted;
};

struct libfile_header {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint32_t count;
    struct libfile_marks marks;
    uint32_t checksum;
};

/* Marks meaning "made for no database in particular" */
void libfile_no_marks(struct libfile_marks *m);

/* What libfile_open() returns in place of a descriptor */
#define LIBFILE_UNREAD (-1) /* absent, or open() or a read failed */
#define LIBFILE_BAD    (-2) /* read, and not a good file of this version */
#define LIBFILE_NEWER  (-3) /* this kind of file, from a later version */

/* Opens path for reading, checking its magic, version, record size and
 * checksum. The descriptor is left at the first record; one of the codes
 * above if not. Only LIBFILE_BAD says the file itself is damaged. tail, if
 * given, gets the bytes after the records. */
int libfile_open(const char *path, uint32_t magic, uint16_t version,
                 uint16_t record_size, struct libfile_header *hdr,
                 uint32_t *tail);

/* The header alone, unchecked beyond magic and version: cheap enough for a
 * question asked often, such as whether the marks still hold. */
bool libfile_peek(const char *path, uint32_t magic, uint16_t version,
                  struct libfile_header *hdr);

struct libfile_writer {
    int fd;
    struct libfile_header hdr;
    char path[MAX_PATH];
};

/* Starts path.new. Records and then the tail are written with
 * libfile_write(); libfile_finish() renames it over path when ok, and
 * removes it otherwise. */
bool libfile_begin(struct libfile_writer *w, const char *path,
                   uint32_t magic, uint16_t version, uint16_t record_size,
                   const struct libfile_marks *marks);
bool libfile_write(struct libfile_writer *w, const void *data, size_t size,
                   uint32_t records);
bool libfile_finish(struct libfile_writer *w, bool ok);

/* Appends n records, creating the file if there is none. A file with a
 * tail cannot be appended to; one cut short is put back as it was. */
bool libfile_append(const char *path, uint32_t magic, uint16_t version,
                    uint16_t record_size, const void *records, uint32_t n);

/* Rewrites the marks of an existing file in place */
bool libfile_set_marks(const char *path, uint32_t magic, uint16_t version,
                       const struct libfile_marks *marks);

#endif /* _LIBFILE_H */
