/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * One member of a book, its cover or a picture, unpacked into a file of its
 * own: a zip's deflated member, or an FB2's base64 one. An image is decoded
 * from a whole file or a byte range of one, and a progressive JPEG seeks
 * back, which a stream still being unpacked cannot: so such a picture is
 * unpacked to a temporary file first.
 ****************************************************************************/

#include <string.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "files/byte_stream.h"
#include "zip_member.h"

#define ZIP_CHUNK 512           /* bytes unpacked per write */

static long fd_read(void *ctx, void *buf, size_t n)
{
    return read(*(int *)ctx, buf, n);
}

static bs_off_t fd_seek(void *ctx, bs_off_t off, int whence)
{
    return lseek(*(int *)ctx, off, whence);
}

static bs_off_t fd_size(void *ctx)
{
    return ffilesize(*(int *)ctx);
}

bool zip_member_to_file(const char *book, long at, uint32_t csize,
                        uint32_t usize, int method, const char *out,
                        void *work, size_t work_size)
{
    int in = open(book, O_RDONLY), fd;
    bs_io io = { fd_read, fd_seek, fd_size, NULL, &in };
    bs_arena a = { work, work_size, 0 };
    bs_stream *s;
    bool to_end = method == MEMBER_BASE64;
    uint32_t left = to_end ? UINT32_MAX : usize, wrote = 0;
    bool ok = false;
    /* From `work`, not static: the reader and the art cache's thread both
     * extract */
    uint8_t *chunk = bs_alloc(&a, ZIP_CHUNK);

    if (in < 0)
        return false;
    s = chunk ? bs_file_stream(&a, &io, at, csize) : NULL;
    if (s)
        s = to_end ? bs_base64_stream(&a, s)
                   : bs_inflate_stream(&a, s, BS_INFLATE_RAW);
    fd = s ? open(out, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    if (fd >= 0)
    {
        while (left > 0)
        {
            size_t got;

            if (bs_pull(s, chunk, MIN(ZIP_CHUNK, left), &got) != BS_OK
                || write(fd, chunk, got) != (ssize_t)got)
                break;
            if (got == 0)
            {
                ok = to_end && wrote > 0;
                break;
            }
            left -= got;
            wrote += got;
        }
        close(fd);
        ok = ok || left == 0;
        if (!ok)
            remove(out);
    }
    close(in);
    return ok;
}
