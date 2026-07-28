/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * A command-line tool to export a single glyph from a Rockbox .fnt font
 * to an 8-bit greyscale .bmp, and to import an edited .bmp back into the
 * font.  Intended for editing icon fonts, where the glyphs are artwork
 * rather than text.
 *
 * The .fnt layout is the one written by convttf:
 *
 *   0   char    header[4]     "RB12"
 *   4   uint16  maxwidth      widest glyph in pixels
 *   6   uint16  height        all glyphs are this tall
 *   8   uint16  ascent        baseline position from the top
 *   10  uint16  depth         0 = 1 bit/pixel, 1 = 4 bits/pixel
 *   12  uint32  firstchar     first codepoint in the font
 *   16  uint32  defaultchar   codepoint drawn for anything missing
 *   20  uint32  size          number of codepoints (a contiguous range)
 *   24  uint32  nbits         bytes of glyph bitmap data
 *   28  uint32  noffset       entries in the offset table
 *   32  uint32  nwidth        entries in the width table
 *   36          bits[nbits]   packed glyph bitmaps
 *       (pad to a 4 byte multiple for long offsets, 2 for short)
 *               offset[]      uint32 if nbits >= 0xFFDB, else uint16
 *               width[]       one byte per codepoint
 *
 * At depth 1 each pixel is a nibble, two per byte, low nibble first,
 * packed row by row.  The values are inverted with respect to ink: 15 is
 * background and 0 is a fully inked pixel.  The .bmp mirrors what you see
 * on screen, so white is empty and black is full ink.
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define FNT_HDR_SIZE    36
#define LONG_OFFSET_MIN 0xFFDB
#define NO_MAPPING      0xFFFFFFFFu

struct fnt
{
    uint16_t maxwidth;
    uint16_t height;
    uint16_t ascent;
    uint16_t depth;
    uint32_t firstchar;
    uint32_t defaultchar;
    uint32_t size;
    uint32_t nbits;
    uint32_t noffset;
    uint32_t nwidth;

    uint8_t *bits;
    uint32_t *offset;   /* widened to 32 bits whatever the file used */
    uint8_t *width;
};

static void panic(const char *message)
{
    fprintf(stderr, "%s\n", message);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        panic("Out of memory");
    return p;
}

/* the font data is little endian regardless of the host */
static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint8_t *read_file(const char *path, long *len)
{
    FILE *fp;
    uint8_t *buf;
    long n;

    fp = fopen(path, "rb");
    if (!fp)
    {
        fprintf(stderr, "Could not open %s\n", path);
        exit(1);
    }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 0)
        panic("Could not size input file");

    buf = xmalloc((size_t)n);
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n)
        panic("Short read on input file");
    fclose(fp);

    *len = n;
    return buf;
}

/* bytes of packed bitmap for one glyph, matching how convttf lays them out */
static uint32_t glyph_bytes(uint32_t w, uint32_t h)
{
    return (w * h + 1) / 2;
}

static uint32_t offset_table_start(uint32_t nbits, int long_offset)
{
    if (long_offset)
        return FNT_HDR_SIZE + ((nbits + 3) & ~3u);
    return FNT_HDR_SIZE + ((nbits + 1) & ~1u);
}

static void load_fnt(const char *path, struct fnt *f)
{
    uint8_t *raw;
    long len;
    uint32_t i, off_start, width_start, need;
    int long_offset;

    raw = read_file(path, &len);
    if (len < FNT_HDR_SIZE || memcmp(raw, "RB12", 4) != 0)
        panic("Not a Rockbox RB12 font file");

    f->maxwidth    = get16(raw + 4);
    f->height      = get16(raw + 6);
    f->ascent      = get16(raw + 8);
    f->depth       = get16(raw + 10);
    f->firstchar   = get32(raw + 12);
    f->defaultchar = get32(raw + 16);
    f->size        = get32(raw + 20);
    f->nbits       = get32(raw + 24);
    f->noffset     = get32(raw + 28);
    f->nwidth      = get32(raw + 32);

    if (f->depth != 1)
        panic("Only 4 bit/pixel fonts (depth 1) are supported.\n"
              "1 bit fonts are built from the .bdf sources in fonts/ "
              "with convbdf.");
    if (f->size == 0 || f->height == 0)
        panic("Font has no glyphs");
    if (f->noffset != f->size || f->nwidth != f->size)
        panic("Offset/width tables do not match the character count");

    long_offset = (f->nbits >= LONG_OFFSET_MIN);
    off_start   = offset_table_start(f->nbits, long_offset);
    width_start = off_start + f->noffset * (long_offset ? 4 : 2);
    need        = width_start + f->nwidth;

    if ((uint32_t)len < need)
        panic("Font file is truncated");

    f->bits   = xmalloc(f->nbits);
    memcpy(f->bits, raw + FNT_HDR_SIZE, f->nbits);

    f->offset = xmalloc(f->size * sizeof(uint32_t));
    f->width  = xmalloc(f->size);

    for (i = 0; i < f->size; i++)
    {
        if (long_offset)
            f->offset[i] = get32(raw + off_start + i * 4);
        else
            f->offset[i] = get16(raw + off_start + i * 2);
        f->width[i] = raw[width_start + i];
    }

    for (i = 0; i < f->size; i++)
    {
        if (f->offset[i] + glyph_bytes(f->width[i], f->height) > f->nbits)
            panic("Glyph bitmap runs past the end of the bitmap data");
    }

    free(raw);
}

static void write_fnt(const char *path, const struct fnt *f)
{
    FILE *fp;
    uint8_t hdr[FNT_HDR_SIZE];
    uint8_t pad[4] = { 0, 0, 0, 0 };
    uint32_t i, skip;
    int long_offset = (f->nbits >= LONG_OFFSET_MIN);

    memcpy(hdr, "RB12", 4);
    put16(hdr + 4,  f->maxwidth);
    put16(hdr + 6,  f->height);
    put16(hdr + 8,  f->ascent);
    put16(hdr + 10, f->depth);
    put32(hdr + 12, f->firstchar);
    put32(hdr + 16, f->defaultchar);
    put32(hdr + 20, f->size);
    put32(hdr + 24, f->nbits);
    put32(hdr + 28, f->noffset);
    put32(hdr + 32, f->nwidth);

    fp = fopen(path, "wb");
    if (!fp)
    {
        fprintf(stderr, "Could not write %s\n", path);
        exit(1);
    }

    fwrite(hdr, 1, FNT_HDR_SIZE, fp);
    fwrite(f->bits, 1, f->nbits, fp);

    skip = offset_table_start(f->nbits, long_offset) - FNT_HDR_SIZE - f->nbits;
    fwrite(pad, 1, skip, fp);

    for (i = 0; i < f->size; i++)
    {
        uint8_t buf[4];
        if (long_offset)
        {
            put32(buf, f->offset[i]);
            fwrite(buf, 1, 4, fp);
        }
        else
        {
            put16(buf, (uint16_t)f->offset[i]);
            fwrite(buf, 1, 2, fp);
        }
    }

    for (i = 0; i < f->size; i++)
        fputc(f->width[i], fp);

    if (ferror(fp))
        panic("Write error");
    fclose(fp);
}

/* unpack one glyph into 8 bit greyscale, white background, black ink */
static void glyph_to_grey(const struct fnt *f, uint32_t idx, uint8_t *grey)
{
    const uint8_t *src = f->bits + f->offset[idx];
    uint32_t n = (uint32_t)f->width[idx] * f->height;
    uint32_t p;

    for (p = 0; p < n; p++)
    {
        uint8_t byte = src[p / 2];
        uint8_t nibble = (p & 1) ? (byte >> 4) : (byte & 0x0f);
        grey[p] = (uint8_t)(nibble * 17);
    }
}

static void grey_to_glyph(const uint8_t *grey, uint32_t w, uint32_t h,
                          uint8_t *dst)
{
    uint32_t n = w * h;
    uint32_t p;

    memset(dst, 0, glyph_bytes(w, h));
    for (p = 0; p < n; p++)
    {
        uint32_t nibble = (grey[p] + 8) / 17;
        if (nibble > 15)
            nibble = 15;
        if (p & 1)
            dst[p / 2] |= (uint8_t)(nibble << 4);
        else
            dst[p / 2] |= (uint8_t)nibble;
    }
}

/* ---- .bmp, 8 bit greyscale, uncompressed ---- */

static void write_bmp(const char *path, const uint8_t *grey,
                      uint32_t w, uint32_t h)
{
    FILE *fp;
    uint8_t hdr[54];
    uint8_t zero[4] = { 0, 0, 0, 0 };
    uint32_t stride = (w + 3) & ~3u;
    uint32_t datasize = stride * h;
    uint32_t i;
    int32_t row;

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    put32(hdr + 2,  54 + 1024 + datasize);
    put32(hdr + 10, 54 + 1024);
    put32(hdr + 14, 40);
    put32(hdr + 18, w);
    put32(hdr + 22, h);
    put16(hdr + 26, 1);
    put16(hdr + 28, 8);
    put32(hdr + 34, datasize);
    put32(hdr + 38, 2835);
    put32(hdr + 42, 2835);
    put32(hdr + 46, 256);
    put32(hdr + 50, 256);

    fp = fopen(path, "wb");
    if (!fp)
    {
        fprintf(stderr, "Could not write %s\n", path);
        exit(1);
    }
    fwrite(hdr, 1, sizeof(hdr), fp);

    /* greyscale palette */
    for (i = 0; i < 256; i++)
    {
        fputc((int)i, fp);
        fputc((int)i, fp);
        fputc((int)i, fp);
        fputc(0, fp);
    }

    /* .bmp rows run bottom to top */
    for (row = (int32_t)h - 1; row >= 0; row--)
    {
        fwrite(grey + (uint32_t)row * w, 1, w, fp);
        fwrite(zero, 1, stride - w, fp);
    }

    if (ferror(fp))
        panic("Write error");
    fclose(fp);
}

static uint8_t luma(uint8_t r, uint8_t g, uint8_t b)
{
    /* weights sum to 256 so a neutral grey survives unchanged */
    return (uint8_t)((r * 77 + g * 150 + b * 29 + 128) >> 8);
}

static uint8_t *read_bmp(const char *path, uint32_t *wp, uint32_t *hp)
{
    uint8_t *raw, *grey;
    long len;
    uint32_t dataoff, hdrsize, compression, palette_off, stride;
    int32_t w, h;
    uint16_t bpp;
    int top_down;
    int32_t y;

    raw = read_file(path, &len);
    if (len < 54 || raw[0] != 'B' || raw[1] != 'M')
        panic("Not a .bmp file");

    dataoff     = get32(raw + 10);
    hdrsize     = get32(raw + 14);
    if (hdrsize < 40)
        panic("Unsupported .bmp header (needs BITMAPINFOHEADER or later)");

    w           = (int32_t)get32(raw + 18);
    h           = (int32_t)get32(raw + 22);
    bpp         = get16(raw + 28);
    compression = get32(raw + 30);

    top_down = (h < 0);
    if (top_down)
        h = -h;
    if (w <= 0 || h <= 0)
        panic("Bad .bmp dimensions");

    /* BI_RGB, or BI_BITFIELDS which for our purposes is still raw BGRA */
    if (compression != 0 && !(compression == 3 && bpp == 32))
        panic("Compressed .bmp files are not supported. "
              "Re-save as an uncompressed .bmp.");
    if (bpp != 8 && bpp != 24 && bpp != 32)
        panic("Only 8, 24 and 32 bit .bmp files are supported. "
              "Re-save as 8 bit greyscale.");

    stride = ((uint32_t)w * (bpp / 8) + 3) & ~3u;
    if (dataoff + stride * (uint32_t)h > (uint32_t)len)
        panic(".bmp file is truncated");

    palette_off = 14 + hdrsize;
    if (bpp == 8 && palette_off + 1024 > dataoff + 0u)
    {
        /* palette must sit between the header and the pixel data */
        if (palette_off + 4 > (uint32_t)len)
            panic(".bmp palette is missing");
    }

    grey = xmalloc((size_t)w * (size_t)h);

    for (y = 0; y < h; y++)
    {
        /* rows run bottom to top unless the height was negative */
        const uint8_t *src = raw + dataoff +
                             (uint32_t)(top_down ? y : h - 1 - y) * stride;
        uint8_t *dst = grey + (uint32_t)y * (uint32_t)w;
        int32_t x;

        for (x = 0; x < w; x++)
        {
            if (bpp == 8)
            {
                const uint8_t *pal = raw + palette_off + src[x] * 4;
                dst[x] = luma(pal[2], pal[1], pal[0]);
            }
            else
            {
                const uint8_t *px = src + (uint32_t)x * (bpp / 8);
                dst[x] = luma(px[2], px[1], px[0]);
            }
        }
    }

    free(raw);
    *wp = (uint32_t)w;
    *hp = (uint32_t)h;
    return grey;
}

/* ---- export ---- */

/* convttf points every codepoint with no glyph of its own at slot 0 */
static int is_alias_of_first(const struct fnt *f, uint32_t idx)
{
    return idx != 0 && f->offset[idx] == f->offset[0] &&
           f->width[idx] == f->width[0];
}

static void export_glyph(const char *fontpath, uint32_t code,
                         const char *outpath)
{
    struct fnt f;
    char namebuf[1024];
    uint32_t idx, w;
    uint8_t *grey;

    load_fnt(fontpath, &f);

    if (code < f.firstchar || code >= f.firstchar + f.size)
    {
        fprintf(stderr, "U+%04X is outside the font range U+%04X..U+%04X.\n"
                        "Export any codepoint in range, edit it, then import "
                        "to U+%04X to extend the font.\n",
                code, f.firstchar, f.firstchar + f.size - 1, code);
        exit(1);
    }
    idx = code - f.firstchar;

    if (!outpath)
    {
        const char *base = fontpath;
        const char *p;
        size_t n;

        for (p = fontpath; *p; p++)
            if (*p == '/' || *p == '\\')
                base = p + 1;
        n = strlen(base);
        if (n > 4 && strcmp(base + n - 4, ".fnt") == 0)
            n -= 4;
        snprintf(namebuf, sizeof(namebuf), "%.*s_%04X.bmp", (int)n, base, code);
        outpath = namebuf;
    }

    if (f.width[idx] == 0 || is_alias_of_first(&f, idx))
    {
        w = f.maxwidth;
        grey = xmalloc((size_t)w * f.height);
        memset(grey, 0xff, (size_t)w * f.height);
        printf("U+%04X has no glyph of its own; "
               "writing a blank %ux%u canvas.\n", code, w, f.height);
        printf("Crop it to the width you want before importing.\n");
    }
    else
    {
        w = f.width[idx];
        grey = xmalloc((size_t)w * f.height);
        glyph_to_grey(&f, idx, grey);
    }

    write_bmp(outpath, grey, w, f.height);
    printf("Wrote %s (%ux%u, ascent %u)\n", outpath, w, f.height, f.ascent);

    free(grey);
    free(f.bits);
    free(f.offset);
    free(f.width);
}

/* ---- import ---- */

static void import_glyph(const char *fontpath, uint32_t code,
                         const char *bmppath, const char *outpath)
{
    struct fnt f, n;
    uint8_t *grey;
    uint32_t w, h, i, newbits, cap;
    uint32_t *map;
    uint8_t *map_width;
    uint32_t first, last;
    int extended;

    load_fnt(fontpath, &f);
    grey = read_bmp(bmppath, &w, &h);

    if (h != f.height)
    {
        fprintf(stderr, "%s is %u pixels tall but the font is %u.\n"
                        "Every glyph in a .fnt must be the full font "
                        "height.\n", bmppath, h, f.height);
        exit(1);
    }
    if (w == 0 || w > 255)
        panic("Glyph width must be between 1 and 255 pixels");

    first = f.firstchar < code ? f.firstchar : code;
    last  = f.firstchar + f.size - 1;
    if (code > last)
        last = code;
    extended = (first != f.firstchar || last != f.firstchar + f.size - 1);

    n = f;
    n.firstchar = first;
    n.size      = last - first + 1;
    n.noffset   = n.size;
    n.nwidth    = n.size;
    n.offset    = xmalloc(n.size * sizeof(uint32_t));
    n.width     = xmalloc(n.size);

    /* worst case every old glyph is copied, plus the replacement */
    cap = f.nbits + glyph_bytes(w, h) + 4;
    n.bits = xmalloc(cap);
    newbits = 0;

    /* old bitmap offset -> new bitmap offset, so shared glyphs stay shared.
       map_width records the width the bitmap was emitted at, since two
       codepoints sharing an offset must also share a width to share data. */
    map = xmalloc((f.nbits + 1) * sizeof(uint32_t));
    map_width = xmalloc(f.nbits + 1);
    for (i = 0; i <= f.nbits; i++)
        map[i] = NO_MAPPING;

    for (i = 0; i < n.size; i++)
    {
        uint32_t code_i = first + i;
        uint32_t oldi;

        if (code_i == code)
        {
            /* the edited glyph always gets its own bitmap, even if it
               used to share one with another codepoint */
            n.offset[i] = newbits;
            n.width[i]  = (uint8_t)w;
            grey_to_glyph(grey, w, h, n.bits + newbits);
            newbits += glyph_bytes(w, h);

            /* Codepoints with no glyph of their own share this bitmap to
               render the default character.  As long as the width has not
               changed they keep following it, so repainting the default
               repaints every one of them.  If the width did change they
               fall through below and keep a copy of the old bitmap
               instead, rather than silently changing width. */
            if (code >= f.firstchar && code < f.firstchar + f.size)
            {
                uint32_t oldt = code - f.firstchar;
                if (f.width[oldt] == (uint8_t)w)
                {
                    map[f.offset[oldt]] = n.offset[i];
                    map_width[f.offset[oldt]] = (uint8_t)w;
                }
            }
            continue;
        }

        if (code_i < f.firstchar || code_i > f.firstchar + f.size - 1)
        {
            n.width[i] = 0;     /* filled in below */
            continue;
        }

        oldi = code_i - f.firstchar;
        if (map[f.offset[oldi]] != NO_MAPPING &&
            map_width[f.offset[oldi]] == f.width[oldi])
        {
            n.offset[i] = map[f.offset[oldi]];
        }
        else
        {
            uint32_t len = glyph_bytes(f.width[oldi], f.height);
            n.offset[i] = newbits;
            memcpy(n.bits + newbits, f.bits + f.offset[oldi], len);
            map[f.offset[oldi]] = newbits;
            map_width[f.offset[oldi]] = f.width[oldi];
            newbits += len;
        }
        n.width[i] = f.width[oldi];
    }

    /* codepoints added by extending the range alias the old first glyph,
       which is what convttf does for anything it could not render */
    if (extended)
    {
        uint32_t anchor = f.firstchar - first;
        for (i = 0; i < n.size; i++)
        {
            uint32_t code_i = first + i;
            if (code_i == code)
                continue;
            if (code_i < f.firstchar || code_i > f.firstchar + f.size - 1)
            {
                n.offset[i] = n.offset[anchor];
                n.width[i]  = n.width[anchor];
            }
        }
    }

    n.nbits = newbits;

    n.maxwidth = 1;
    for (i = 0; i < n.size; i++)
        if (n.width[i] > n.maxwidth)
            n.maxwidth = n.width[i];

    if (!outpath)
        outpath = fontpath;
    write_fnt(outpath, &n);

    printf("Imported %s into U+%04X (%ux%u)\n", bmppath, code, w, h);
    if (extended)
        printf("Range extended to U+%04X..U+%04X (%u codepoints)\n",
               first, last, n.size);
    printf("Wrote %s (%u glyph bytes, maxwidth %u)\n",
           outpath, n.nbits, n.maxwidth);

    free(map);
    free(map_width);
    free(grey);
    free(f.bits);
    free(f.offset);
    free(f.width);
    free(n.bits);
    free(n.offset);
    free(n.width);
}

static void usage(void)
{
    printf(
        "Usage: convfnt -x <font.fnt> <codepoint> [-o <out.bmp>]\n"
        "       convfnt -i <font.fnt> <codepoint> <in.bmp> [-o <out.fnt>]\n"
        "\n"
        "  -x   export one glyph to an 8 bit greyscale .bmp\n"
        "       (white is empty, black is full ink)\n"
        "  -i   import an edited .bmp back into the font\n"
        "       (rewrites the font in place unless -o is given)\n"
        "\n"
        "The codepoint accepts 0x hex, 0 octal or plain decimal.\n"
        "The .bmp must be exactly as tall as the font; its width becomes\n"
        "the glyph's advance width.\n"
        "\n"
        "Example:\n"
        "  convfnt -x Player_Icons.fnt 0xE010\n"
        "  convfnt -i Player_Icons.fnt 0xE010 Player_Icons_E010.bmp\n");
    exit(1);
}

int main(int argc, char *argv[])
{
    const char *outpath = NULL;
    int i, mode = 0;
    const char *fontpath = NULL;
    const char *bmppath = NULL;
    uint32_t code = 0;
    int have_code = 0;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-x") == 0)
            mode = 'x';
        else if (strcmp(argv[i], "-i") == 0)
            mode = 'i';
        else if (strcmp(argv[i], "-o") == 0)
        {
            if (++i >= argc)
                usage();
            outpath = argv[i];
        }
        else if (!fontpath)
            fontpath = argv[i];
        else if (!have_code)
        {
            /* A codepoint is either a number (0x53, 83, 0123) or, for
               convenience with icon fonts referred to by glyph, a single
               character whose own code is used (S -> 0x53). Anything else is
               rejected rather than silently parsed as 0, which used to send
               every "convfnt -i font S ..." to U+0000. */
            char *end;
            unsigned long v = strtoul(argv[i], &end, 0);
            if (end != argv[i] && *end == '\0')
                code = (uint32_t)v;
            else if (argv[i][0] != '\0' && argv[i][1] == '\0')
                code = (unsigned char)argv[i][0];
            else
            {
                fprintf(stderr, "Invalid codepoint '%s' -- use 0x53, 83, "
                                "or a single character like S.\n", argv[i]);
                exit(1);
            }
            have_code = 1;
        }
        else if (!bmppath)
            bmppath = argv[i];
        else
            usage();
    }

    if (!mode || !fontpath || !have_code)
        usage();

    if (mode == 'x')
        export_glyph(fontpath, code, outpath);
    else
    {
        if (!bmppath)
            usage();
        import_glyph(fontpath, code, bmppath, outpath);
    }

    return 0;
}
