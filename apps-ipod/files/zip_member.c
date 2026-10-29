/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * One member of a zip, a book's cover or picture, unpacked into a file of
 * its own. An image is decoded from a whole file or a byte range of one, and
 * a progressive JPEG seeks back, which a stream still being inflated cannot:
 * so a deflated picture is inflated to a temporary file first.
 ****************************************************************************/

#include <string.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "files/byte_stream.h"
#include "zip_member.h"

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

bool zip_member_to_file(const char *zip, long at, uint32_t csize,
                        uint32_t usize, const char *out, void *work,
                        size_t work_size)
{
    static uint8_t chunk[512];
    int in = open(zip, O_RDONLY), fd;
    bs_io io = { fd_read, fd_seek, fd_size, NULL, &in };
    bs_arena a = { work, work_size, 0 };
    bs_stream *s;
    uint32_t left = usize;

    if (in < 0)
        return false;
    s = bs_file_stream(&a, &io, at, csize);
    if (s)
        s = bs_inflate_stream(&a, s, BS_INFLATE_RAW);
    fd = s ? open(out, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    if (fd >= 0)
    {
        while (left > 0)
        {
            size_t got;

            if (bs_pull(s, chunk, MIN(sizeof chunk, left), &got) != BS_OK
                || got == 0 || write(fd, chunk, got) != (ssize_t)got)
                break;
            left -= got;
        }
        close(fd);
        if (left > 0)
            remove(out);
    }
    close(in);
    return fd >= 0 && left == 0;
}
