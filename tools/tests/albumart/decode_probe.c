#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "draw/image_load.h"

void yield(void)
{
}
void debugf(const char *fmt, ...)
{
    (void)fmt;
}

void panicf(const char *fmt, ...)
{
    (void)fmt;
    abort();
}

static const char *test_path;
static bool nested;
static int callbacks;
static int plain_decode(void)
{
    int fd = open(test_path, O_RDONLY);
    unsigned char *buf = malloc(1024 * 1024);
    struct bitmap bm = {.width = 100, .height = 100, .data = buf};
    struct image_src src = {.fd = fd, .len = -1};
    int rc = image_load(&src, &bm, 1024 * 1024,
                        FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT,
                        NULL);
    free(buf);
    close(fd);
    return rc;
}
static bool cancel_row(int done, int rows)
{
    (void)done; (void)rows;
    callbacks++;
    if (!nested)
    {
        nested = true;
        assert(plain_decode() > 0); /* An unrelated job has no callback. */
    }
    return false;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 5)
        return 2;
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0)
        return 3;
    const int cap = argc > 3 ? atoi(argv[3]) : 1024 * 1024;
    unsigned char *buf = malloc(cap);
    struct bitmap b = {.width = 100, .height = 100, .data = buf};
    struct image_src src = {
        .fd = fd, .pos = argc > 4 ? atoi(argv[4]) : 0, .len = -1, .flags = 0};
    struct custom_format request = format_native;
    const struct custom_format *format = NULL;
    bool cancel = getenv("ART_TEST_CALLBACKS") != NULL;
    if (cancel)
    {
        test_path = argv[1];
#ifdef REQUEST_CALLBACKS
        request.progress = cancel_row;
        format = &request;
#else
        (void)request;
        image_row_hook = cancel_row;
#endif
    }
    int rc = image_load(&src, &b, cap,
                        FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT |
                            FORMAT_DITHER,
                        format);
    printf("%s: rc=%d dimensions=%dx%d\n", argv[1], rc, b.width, b.height);
    if (cancel)
    {
#ifndef REQUEST_CALLBACKS
        image_row_hook = NULL;
#endif
        assert(callbacks > 0 && rc <= 0);
        assert(plain_decode() > 0);
        close(fd);
        free(buf);
        return 0;
    }
    if (rc > 0)
    {
        FILE *out = fopen(argv[2], "wb");
        fprintf(out, "P6\n%d %d\n255\n", b.width, b.height);
        for (int i = 0; i < b.width * b.height; i++)
        {
            fb_data p = ((fb_data *)b.data)[i];
            unsigned char rgb[] = {RGB_UNPACK_RED(p), RGB_UNPACK_GREEN(p),
                                   RGB_UNPACK_BLUE(p)};
            fwrite(rgb, 1, 3, out);
        }
        fclose(out);
    }
    close(fd);
    free(buf);
    return rc > 0 ? 0 : 4;
}
