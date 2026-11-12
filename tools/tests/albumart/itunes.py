#!/usr/bin/env python3
"""Generated, indexed iTunesDB/ArtworkDB/ithmb regressions under ASan/UBSan."""
import struct
from host import ROOT, OUT, run, CC, FLAGS, compile_sources, link, function
FIX = OUT / 'itunes-fixtures'
CTRL = FIX / 'iPod_Control'
(CTRL / 'iTunes').mkdir(parents=True, exist_ok=True)
(CTRL / 'Artwork').mkdir(exist_ok=True)
def record(kind, header, body=b'', fields=()):
    data = bytearray(header)
    struct.pack_into('<4sII', data, 0, kind, header, header + len(body))
    for offset, fmt, value in fields:
        struct.pack_into(fmt, data, offset, value)
    return bytes(data) + body
N = 2000
tracks = []
for i in range(N):
    path = (':iPod_Control:Music:F00:track%d.mp3' % i).encode('utf-16le')
    text = record(b'mhod', 24, struct.pack('<IIII', 1, len(path), 0, 0) + path, [(12, '<I', 2)])
    tracks.append(record(b'mhit', 0x164, text, [(0x70, '<Q', i + 100), (0x160, '<I', i + 1)]))
db = record(b'mhbd', 12, record(b'mhsd', 16, record(b'mhlt', 12, b''.join(tracks), [(8, '<I', N)]), [(12, '<I', 1)]))
(CTRL / 'iTunes/iTunesDB').write_bytes(db)
def thumb(name, dim, fmt):
    name = name.encode('utf-16le')
    leaf = record(b'mhod', 36, name, [(12, '<I', 3), (24, '<I', len(name)), (28, '<B', 2)])
    info = record(b'mhni', 40, leaf, [(16, '<I', fmt), (20, '<I', 0), (24, '<I', dim * dim * 2), (32, '<H', dim), (34, '<H', dim)])
    return record(b'mhod', 24, info, [(12, '<I', 2)])
small = thumb(':small.ithmb', 56, 1061)
large = thumb(':missing.ithmb', 320, 1060)
short = thumb(':short.ithmb', 128, 1055)
traversal = thumb(':..:escape.ithmb', 320, 1060)
images = [record(b'mhii', 40, small + large + short + traversal, [(16, '<I', i + 1), (20, '<Q', i + 100)]) for i in range(N)]
art = record(b'mhfd', 12, record(b'mhsd', 16, record(b'mhli', 12, b''.join(images), [(8, '<I', N)]), [(12, '<I', 1)]))
(CTRL / 'Artwork/ArtworkDB').write_bytes(art)
(CTRL / 'Artwork/small.ithmb').write_bytes(b'\0\xf8' * 56 * 56)
(CTRL / 'Artwork/short.ithmb').write_bytes(b'\0' * 20)
(CTRL / 'Artwork/low-surrogate.bin').write_bytes(b'\0\xdc')
source = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "file.h"
#define __CORE_ALLOC_H__
#include "draw/bmp.h"
#include "metadata/itunes_art.h"
static void *handles[4];
static int allocations;
static bool refuse;
int core_alloc(size_t size)
{
    if (refuse) return -1;
    for (int i = 1; i < 4; i++)
        if (!handles[i]) { handles[i] = malloc(size); assert(handles[i]); allocations++; return i; }
    abort();
}
int core_free(int handle) { free(handles[handle]); handles[handle] = NULL; allocations--; return 0; }
void core_pin(int handle) { (void)handle; }
void core_unpin(int handle) { (void)handle; }
void *core_get_data_pinned(int handle) { return handles[handle]; }
size_t core_allocatable(void) { return refuse ? 0 : 16 * 1024 * 1024; }
void yield(void) {}
void debugf(const char *fmt, ...) { (void)fmt; }
void panicf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); abort(); }
static unsigned reads;
static ssize_t counted_read(int fd, void *buf, size_t size)
{ reads++; return read(fd, buf, size); }
#undef read
#define read counted_read
static unsigned track_opens;
static int counted_open(const char *path, int flags)
{
    if (strstr(path, "/iTunesDB")) track_opens++;
    return open(path, flags);
}
#undef open
#define open counted_open
#include "apps-ipod/metadata/itunes_art.c"
static bool deny(int done, int total) { (void)done; (void)total; return false; }
static bool stop_scan(void) { return true; }
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "empty-art"))
    {
        assert(itunes_art_open(NULL));
        assert(!track_opens && !allocations);
        return 0;
    }
    if (!strcmp(argv[1], "short"))
    {
        assert(!itunes_art_open(NULL));
        assert(!allocations);
        return 0;
    }
    assert(!itunes_art_open(stop_scan));
    assert(!allocations);
    assert(itunes_art_open(NULL));
    assert(track_count == 2000 && image_count == 4000);
    assert(reads < 8 * 2000 + 100);
    reads = 0;
    struct itunes_art art = {0};
    for (int i = 0; i < 10000; i++)
    {
        char path[100];
        snprintf(path, sizeof(path), "/<HDD0>/IPOD_CONTROL/Music/F00/track%d.mp3", i % 2000);
        assert(itunes_art_find(path, &art));
        assert(strstr(art.path, "/small.ithmb") && art.width == 56 && art.height == 56 && art.stride == 56);
    }
    assert(reads < 16 * 10000);
    assert(!itunes_art_find("/iPod_Control/Music/F00/missing.mp3", &art));
    unsigned char pixels[56 * 56 * FB_DATA_SZ];
    struct bitmap bm = {.width = 40, .height = 40, .data = pixels};
    assert(itunes_art_decode(&art, &bm, sizeof(pixels), FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT, NULL) > 0);
    assert(RGB_UNPACK_RED(((fb_data *)pixels)[0]) > 240);
    assert(itunes_art_decode(&art, &bm, 1, FORMAT_NATIVE | FORMAT_RESIZE, NULL) < 0);
    struct custom_format request = {.progress = deny};
    request.progress = deny;
    assert(itunes_art_decode(&art, &bm, sizeof(pixels), FORMAT_NATIVE | FORMAT_RESIZE, &request) < 0);
    /* Exact path verification rejects an index-hash collision. */
    for (unsigned i = 0; i < track_count; i++)
        if (tracks[i].hash == path_key("/iPod_Control/Music/F00/track0.mp3"))
            tracks[i].text++;
    assert(!itunes_art_find("/iPod_Control/Music/F00/track0.mp3", &art));
    int fd = open(ITUNES_CONTROL "/Artwork/low-surrogate.bin", O_RDONLY);
    char text[20];
    assert(fd >= 0 && !string_at(fd, 0, 2, true, text, sizeof(text)));
    close(fd);
    itunes_art_close();
    assert(!allocations);
    refuse = true;
    assert(!itunes_art_open(NULL));
    assert(!allocations);
    puts("one-pass indexes, 10000 lookups, valid-small fallback, pixels, cancellation, collision and allocation cleanup PASS");
    return 0;
}
'''
objects = compile_sources(['apps-ipod/database/path_key.c', 'firmware/common/strlcpy.c', 'firmware/common/strmemccpy.c', 'tools/tests/albumart/host_io.c'])
dimension = OUT / 'dimension.c'
dimension.write_text('#include "config.h"\n#include "lcd.h"\n#include "draw/bmp.h"\n#include "draw/resize.h"\n' + function('apps-ipod/draw/resize.c', 'recalc_dimension'))
run([CC, *FLAGS, '-c', dimension, '-o', OUT / 'dimension.o'], 'compile-dimension')
objects.append(OUT / 'dimension.o')
path = OUT / 'itunes.c'
path.write_text(source)
run([CC, *FLAGS, '-DITUNES_CONTROL="' + str(CTRL) + '"', path, *objects, '-Wl,--gc-sections', '-o', OUT / 'itunes'], 'link-itunes')
print(run([OUT / 'itunes', 'normal'], 'itunes').stdout)
# Too-short section headers and isolated low surrogates fail deterministically.
(CTRL / 'iTunes/iTunesDB').write_bytes(record(b'mhbd', 12, record(b'mhsd', 12)))
print(run([OUT / 'itunes', 'short'], 'short-header').stdout)

# An empty ArtworkDB must be rejected before opening/scanning the track DB.
(CTRL / 'iTunes/iTunesDB').write_bytes(db)
(CTRL / 'Artwork/ArtworkDB').write_bytes(record(b'mhfd', 12, record(b'mhsd', 16, record(b'mhli', 12, fields=[(8, '<I', 0)]), [(12, '<I', 1)])))
print(run([OUT / 'itunes', 'empty-art'], 'empty-art').stdout)
