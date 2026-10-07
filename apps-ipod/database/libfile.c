/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The library's binary container; see libfile.h. The checksum is crc_32()
 * from 0xffffffff with no final inversion, over every byte after the header,
 * which is what lets an append extend it from the header's value.
 *
 * Parts, in order:
 *   - reading: libfile_open(), libfile_peek()
 *   - writing a whole file
 *   - changing a file in place: append, marks
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "file.h"
#include "crc32.h"
#include "database/libfile.h"

_Static_assert(sizeof(struct libfile_header) == 28, "libfile header layout");

#define CRC_START 0xffffffffu

void libfile_no_marks(struct libfile_marks *m)
{
    m->entries = -1;
    m->commitid = -1;
    m->deleted = -1;
}

/* ------------------------------------------------------------------ *
 * reading                                                            *
 * ------------------------------------------------------------------ */

static bool read_header(int fd, uint32_t magic, uint16_t version,
                        struct libfile_header *hdr)
{
    return read(fd, hdr, sizeof(*hdr)) == (ssize_t)sizeof(*hdr)
           && hdr->magic == magic && hdr->version == version;
}

/* CRC of everything from the current position to the end */
static bool crc_rest(int fd, uint32_t *crc)
{
    unsigned char buf[256];
    ssize_t n;

    *crc = CRC_START;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        *crc = crc_32(buf, n, *crc);
    return n == 0;
}

int libfile_open(const char *path, uint32_t magic, uint16_t version,
                 uint16_t record_size, struct libfile_header *hdr,
                 uint32_t *tail)
{
    uint32_t crc;
    off_t body;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return -1;
    body = ffilesize(fd) - (off_t)sizeof(*hdr);
    if (!read_header(fd, magic, version, hdr)
        || hdr->record_size != record_size
        || body < (off_t)hdr->count * record_size
        || !crc_rest(fd, &crc) || crc != hdr->checksum
        || lseek(fd, sizeof(*hdr), SEEK_SET) != (off_t)sizeof(*hdr))
    {
        close(fd);
        return -1;
    }
    if (tail)
        *tail = body - (off_t)hdr->count * record_size;
    return fd;
}

bool libfile_peek(const char *path, uint32_t magic, uint16_t version,
                  struct libfile_header *hdr)
{
    int fd = open(path, O_RDONLY);
    bool ok;

    if (fd < 0)
        return false;
    ok = read_header(fd, magic, version, hdr);
    close(fd);
    return ok;
}

/* ------------------------------------------------------------------ *
 * writing a whole file                                               *
 * ------------------------------------------------------------------ */

bool libfile_begin(struct libfile_writer *w, const char *path,
                   uint32_t magic, uint16_t version, uint16_t record_size,
                   const struct libfile_marks *marks)
{
    memset(&w->hdr, 0, sizeof(w->hdr));
    w->hdr.magic = magic;
    w->hdr.version = version;
    w->hdr.record_size = record_size;
    w->hdr.checksum = CRC_START;
    if (marks)
        w->hdr.marks = *marks;
    else
        libfile_no_marks(&w->hdr.marks);

    snprintf(w->path, sizeof(w->path), "%s.new", path);
    w->fd = open(w->path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (w->fd < 0)
        return false;
    /* A placeholder until the count and checksum are known */
    if (write(w->fd, &w->hdr, sizeof(w->hdr)) != (ssize_t)sizeof(w->hdr))
    {
        libfile_finish(w, false);
        return false;
    }
    return true;
}

bool libfile_write(struct libfile_writer *w, const void *data, size_t size,
                   uint32_t records)
{
    if (w->fd < 0 || write(w->fd, data, size) != (ssize_t)size)
        return false;
    w->hdr.checksum = crc_32(data, size, w->hdr.checksum);
    w->hdr.count += records;
    return true;
}

bool libfile_finish(struct libfile_writer *w, bool ok)
{
    if (w->fd < 0)
        return false;
    if (ok)
        ok = lseek(w->fd, 0, SEEK_SET) == 0
             && write(w->fd, &w->hdr, sizeof(w->hdr))
                == (ssize_t)sizeof(w->hdr);
    close(w->fd);
    w->fd = -1;

    if (ok)
    {
        char path[MAX_PATH];
        size_t len = strlen(w->path) - 4;   /* less ".new" */

        memcpy(path, w->path, len);
        path[len] = '\0';
        ok = rename(w->path, path) == 0;
    }
    if (!ok)
        remove(w->path);
    return ok;
}

/* ------------------------------------------------------------------ *
 * changing a file in place                                           *
 * ------------------------------------------------------------------ */

bool libfile_append(const char *path, uint32_t magic, uint16_t version,
                    uint16_t record_size, const void *records, uint32_t n)
{
    struct libfile_header hdr;
    size_t size = (size_t)n * record_size;
    off_t end;
    bool ok;
    int fd = open(path, O_RDWR);

    if (fd < 0)
    {
        struct libfile_writer w;

        return libfile_begin(&w, path, magic, version, record_size, NULL)
               && libfile_finish(&w, libfile_write(&w, records, size, n));
    }

    end = ffilesize(fd);
    ok = read_header(fd, magic, version, &hdr)
         && hdr.record_size == record_size
         && end == (off_t)(sizeof(hdr) + hdr.count * record_size)
         && lseek(fd, 0, SEEK_END) == end;
    if (ok && write(fd, records, size) != (ssize_t)size)
    {
        ftruncate(fd, end);
        ok = false;
    }
    if (ok)
    {
        hdr.count += n;
        hdr.checksum = crc_32(records, size, hdr.checksum);
        ok = lseek(fd, 0, SEEK_SET) == 0
             && write(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr);
    }
    close(fd);
    return ok;
}

bool libfile_set_marks(const char *path, uint32_t magic, uint16_t version,
                       const struct libfile_marks *marks)
{
    struct libfile_header hdr;
    bool ok;
    int fd = open(path, O_RDWR);

    if (fd < 0)
        return false;
    ok = read_header(fd, magic, version, &hdr);
    if (ok)
    {
        hdr.marks = *marks;
        ok = lseek(fd, 0, SEEK_SET) == 0
             && write(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr);
    }
    close(fd);
    return ok;
}
