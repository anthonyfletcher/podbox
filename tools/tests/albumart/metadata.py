#!/usr/bin/env python3
"""Generated ID3, FLAC, Vorbis and MP4 artwork regressions; no player needed."""
import base64
import struct
from host import ROOT, OUT, run, compile_sources, link
FIX = OUT / "fixtures"
FIX.mkdir(exist_ok=True)
red, blue = b"RED-PICTURE", b"BLUE-PICTURE"
def apic(pic, kind, encoding=0):
    description = b"\xff\xfeA\0\0\0" if encoding == 1 else b"A\0"
    payload = bytes([encoding]) + b"image/png\0" + bytes([kind]) + description + pic
    return b"APIC" + struct.pack(">I", len(payload)) + b"\0\0" + payload
def id3(frames):
    n = len(frames)
    return b"ID3\x03\0\0" + bytes([(n >> shift) & 127 for shift in (21, 14, 7, 0)]) + frames
front = id3(apic(red, 4) + apic(blue, 3, 1))
utf16 = id3(apic(blue, 3, 1))
# A broken second front cover must not erase the valid fallback.
broken = id3(apic(red, 4) + apic(b"", 3, 1))
for name, data in [("front.mp3", front), ("utf16.mp3", utf16), ("broken.mp3", broken)]:
    (FIX / name).write_bytes(data)
def picture(pic, kind):
    return struct.pack(">II", kind, 9) + b"image/png" + struct.pack(">I", 0) + struct.pack(">IIIII", 80, 48, 24, 0, len(pic)) + pic
stream = bytearray(34)
stream[10:18] = ((44100 << 44) | 44100).to_bytes(8, "big")
blocks = [bytes(stream), picture(red, 4), picture(blue, 3)]
flac = b"fLaC"
for i, block in enumerate(blocks):
    flac += bytes([(0 if i == 0 else 6) | (128 if i == 2 else 0)]) + len(block).to_bytes(3, "big") + block
(FIX / "pictures.flac").write_bytes(flac)
comments = [b"METADATA_BLOCK_PICTURE=" + base64.b64encode(picture(pic, kind)) for pic, kind in [(red, 4), (blue, 3)]]
vorbis = struct.pack("<II", 0, 2) + b"".join(struct.pack("<I", len(c)) + c for c in comments)
(FIX / "pictures.vorbis").write_bytes(vorbis)
jpeg = b"\xff\xd8\xff\xe1" + b"Exif\0\0"
data = struct.pack(">I4sII", 16 + len(jpeg), b"data", 13, 0) + jpeg
(FIX / "covr.bin").write_bytes(struct.pack(">I4s", 8 + len(data), b"covr") + data)
source = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "metadata.h"
#include "lib/rbcodec/metadata/mp4.c"
void setid3v2title(int, struct mp3entry *);
bool get_flac_metadata(int, struct mp3entry *);
void yield(void) {}
void debugf(const char *fmt, ...) { (void)fmt; }
void panicf(const char *fmt, ...) { (void)fmt; abort(); }
int main(int argc, char **argv)
{
    assert(argc == 4);
    struct mp3entry id3 = {0};
    int fd = open(argv[2], O_RDONLY);
    assert(fd >= 0);
    int expected = atoi(argv[3]);
    if (!strcmp(argv[1], "id3"))
    {
        id3.id3v2len = ffilesize(fd);
        setid3v2title(fd, &id3);
        assert(id3.has_embedded_albumart);
        assert(id3.albumart.pos == expected);
        assert(id3.albumart.size == (expected == 0 ? 0 : 12) || id3.albumart.size == 11);
    }
    else if (!strcmp(argv[1], "mp4"))
    {
        assert(read_mp4_tags(fd, &id3, ffilesize(fd)));
        assert(id3.has_embedded_albumart && id3.albumart.type == AA_TYPE_JPG);
    }
    else if (!strcmp(argv[1], "flac"))
    {
        assert(get_flac_metadata(fd, &id3));
        assert(id3.has_embedded_albumart && id3.albumart.pos == expected);
    }
    else if (!strcmp(argv[1], "vorbis"))
    {
        id3.codectype = AFMT_FLAC;
        assert(read_vorbis_tags(fd, &id3, ffilesize(fd)) > 0);
        assert(id3.has_embedded_albumart && id3.albumart.pos > expected);
    }
    else if (!strcmp(argv[1], "base64"))
    {
        unsigned char out[80];
        const char *input = "////////////////////////////////////////";
        assert(base64_decode(input, strlen(input), out) == 30);
    }
    else
    {
        enum mp3_aa_type type;
        int pos;
        unsigned char bad[8] = {0, 0, 0, 3, 0xff, 0xff, 0xff, 0xff};
        assert(!parse_flac_album_art(bad, sizeof(bad), &type, &pos));
        unsigned char picture[41] = {0};
        picture[3] = 3;
        picture[7] = 9;
        memcpy(picture + 8, "image/png", 9);
        for (int n = 0; n < (int)sizeof(picture); n++)
        {
            unsigned char *prefix = malloc(n ? n : 1);
            assert(prefix);
            memcpy(prefix, picture, n);
            assert(!parse_flac_album_art(prefix, n, &type, &pos));
            free(prefix);
        }
        memset(picture + 17, 0xff, 4);
        assert(!parse_flac_album_art(picture, sizeof(picture), &type, &pos));
    }
    close(fd);
    return 0;
}
'''
files = ["lib/rbcodec/metadata/" + s + ".c" for s in ["id3tags", "flac", "vorbis", "ogg", "metadata_common", "replaygain", "mp3data"]]
files += ["apps-ipod/system/strutil.c", "firmware/common/unicode.c", "firmware/common/strcasestr.c", "firmware/common/pathfuncs.c", "lib/fixedpoint/fixedpoint.c", "tools/tests/albumart/host_io.c"]
probe = link("metadata", source, compile_sources(files))
tests = [("front", "id3", "front.mp3", front.index(blue)), ("utf16", "id3", "utf16.mp3", utf16.index(blue)), ("fallback", "id3", "broken.mp3", broken.index(red)), ("mp4", "mp4", "covr.bin", 0), ("flac", "flac", "pictures.flac", flac.index(blue)), ("vorbis", "vorbis", "pictures.vorbis", 12 + len(comments[0])), ("base64", "base64", "covr.bin", 0), ("bounds", "bounds", "covr.bin", 0)]
failed = []
for name, mode, file, expected in tests:
    result = run([probe, mode, FIX / file, expected], name, check=False)
    print(name, "PASS" if result.returncode == 0 else "FAIL", flush=True)
    if result.returncode:
        failed.append(name)
assert not failed, failed
