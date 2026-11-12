#!/usr/bin/env python3
"""Actual atomic writer, with real files and injected open/write/close/rename errors."""
from host import OUT, run, CC, function
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
typedef uint16_t fb_data;
#define FB_DATA_SZ 2
#define MAX_PATH 260
#define ART_CACHE_MAGIC 0x5441
#define ART_CACHE_FORMAT_VERSION 8
enum art_layout { AA_ROWS, AA_COLUMNS };
struct bitmap { int width, height; unsigned char *data; };
struct art_cache_header { uint16_t magic, version, width, height, layout, pad; };
static struct { int layout; } art_sizes[1] = {{AA_ROWS}};
static char aa_tmp_path[MAX_PATH];
static unsigned aa_generation;
static bool aa_io_failed;
static const char *mode;
#define strlcpy test_strlcpy
static size_t strlcpy(char *out, const char *in, size_t size)
{ size_t n = strlen(in); if (size) { size_t take = n < size - 1 ? n : size - 1; memcpy(out, in, take); out[take] = 0; } return n; }
static int failing_open(const char *p, int flags, int perms)
{ if (!strcmp(mode, "open")) { errno = ENOSPC; return -1; } return open(p, flags, perms); }
static ssize_t failing_write(int fd, const void *p, size_t n)
{ if (!strcmp(mode, "write")) { errno = ENOSPC; return -1; } return write(fd, p, n); }
static int failing_close(int fd)
{ int rc = close(fd); return !strcmp(mode, "close") ? -1 : rc; }
static int failing_rename(const char *a, const char *b)
{ return !strcmp(mode, "rename") ? -1 : rename(a, b); }
#define open failing_open
#define write failing_write
#define close failing_close
#define rename failing_rename
'''
code += function('apps-ipod/metadata/art_cache.c', 'aa_write_aat')
code += r'''
#undef open
#undef write
#undef close
#undef rename
int main(int argc, char **argv)
{
    assert(argc == 3);
    mode = argv[1];
    int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, "OLD", 3) == 3);
    close(fd);
    uint16_t pixels[4] = {1, 2, 3, 4};
    struct bitmap bm = {2, 2, (unsigned char *)pixels};
    bool ok = aa_write_aat(argv[2], &bm, 0);
    bool success = !strcmp(mode, "success");
    assert(ok == success);
    assert(aa_io_failed == !success);
    assert(aa_generation == (unsigned)success);
    fd = open(argv[2], O_RDONLY);
    assert(fd >= 0);
    if (!success)
    {
        char b[3];
        assert(read(fd, b, 3) == 3 && !memcmp(b, "OLD", 3));
        assert(access(aa_tmp_path, F_OK) != 0);
    }
    close(fd);
    return 0;
}
'''
source = OUT / 'publication.c'
source.write_text(code)
run([CC, '-std=gnu11', '-g', '-O1', '-fno-pie', '-no-pie', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', source, '-o', OUT / 'publication'], 'compile-publication')
failed = []
for mode in ['success', 'open', 'write', 'close', 'rename']:
    result = run([OUT / 'publication', mode, OUT / (mode + '.aat')], mode, check=False)
    print(mode, 'PASS' if result.returncode == 0 else 'FAIL')
    if result.returncode:
        failed.append(mode)
assert not failed, failed
