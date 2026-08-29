/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The glyph table and the block renderer. See spike_text.h.
 ****************************************************************************/

#include "config.h"
#include "lcd.h"
#include "games/spike/spike_text.h"

/* Seven rows of five bits, most significant bit leftmost. Written as binary
 * so the table is the picture. */
#define R(a,b,c,d,e) ((a)<<4 | (b)<<3 | (c)<<2 | (d)<<1 | (e))

/* Seven wide, so it needs a row macro of its own. */
#define C(a,b,c,d,e,f,g) \
    ((a)<<6 | (b)<<5 | (c)<<4 | (d)<<3 | (e)<<2 | (f)<<1 | (g))

/* The band is a row clear of the body rather than joined to it: at these
 * sizes a solid block reads as a chess piece, and the gap is what makes it
 * a crown. */
static const unsigned char crown[SPK_GLYPH_H] =
{
    C(0,0,0,1,0,0,0),
    C(1,0,0,1,0,0,1),
    C(1,0,1,1,1,0,1),
    C(1,1,1,1,1,1,1),
    C(1,1,1,1,1,1,1),
    C(0,0,0,0,0,0,0),
    C(1,1,1,1,1,1,1)
};

static const unsigned char glyphs[37][SPK_GLYPH_H] =
{
    { R(0,1,1,1,0), R(1,0,0,0,1), R(1,0,0,1,1), R(1,0,1,0,1),
      R(1,1,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0) },                 /* 0 */
    { R(0,0,1,0,0), R(0,1,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0),
      R(0,0,1,0,0), R(0,0,1,0,0), R(0,1,1,1,0) },                 /* 1 */
    { R(0,1,1,1,0), R(1,0,0,0,1), R(0,0,0,0,1), R(0,0,0,1,0),
      R(0,0,1,0,0), R(0,1,0,0,0), R(1,1,1,1,1) },                 /* 2 */
    { R(1,1,1,1,1), R(0,0,0,1,0), R(0,0,1,0,0), R(0,0,0,1,0),
      R(0,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0) },                 /* 3 */
    { R(0,0,0,1,0), R(0,0,1,1,0), R(0,1,0,1,0), R(1,0,0,1,0),
      R(1,1,1,1,1), R(0,0,0,1,0), R(0,0,0,1,0) },                 /* 4 */
    { R(1,1,1,1,1), R(1,0,0,0,0), R(1,1,1,1,0), R(0,0,0,0,1),
      R(0,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0) },                 /* 5 */
    { R(0,0,1,1,0), R(0,1,0,0,0), R(1,0,0,0,0), R(1,1,1,1,0),
      R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0) },                 /* 6 */
    { R(1,1,1,1,1), R(0,0,0,0,1), R(0,0,0,1,0), R(0,0,1,0,0),
      R(0,1,0,0,0), R(0,1,0,0,0), R(0,1,0,0,0) },                 /* 7 */
    { R(0,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0),
      R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0) },                 /* 8 */
    { R(0,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,1),
      R(0,0,0,0,1), R(0,0,0,1,0), R(0,1,1,0,0) },                 /* 9 */

    { R(0,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,1,1,1,1),
      R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1) },                 /* A */
    { R(1,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,1,1,1,0),
      R(1,0,0,0,1), R(1,0,0,0,1), R(1,1,1,1,0) },                 /* B */
    { R(0,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,0), R(1,0,0,0,0),
      R(1,0,0,0,0), R(1,0,0,0,1), R(0,1,1,1,0) },                 /* C */
    { R(1,1,1,0,0), R(1,0,0,1,0), R(1,0,0,0,1), R(1,0,0,0,1),
      R(1,0,0,0,1), R(1,0,0,1,0), R(1,1,1,0,0) },                 /* D */
    { R(1,1,1,1,1), R(1,0,0,0,0), R(1,0,0,0,0), R(1,1,1,1,0),
      R(1,0,0,0,0), R(1,0,0,0,0), R(1,1,1,1,1) },                 /* E */
    { R(1,1,1,1,1), R(1,0,0,0,0), R(1,0,0,0,0), R(1,1,1,1,0),
      R(1,0,0,0,0), R(1,0,0,0,0), R(1,0,0,0,0) },                 /* F */
    { R(0,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,0), R(1,0,1,1,1),
      R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,1) },                 /* G */
    { R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,1,1,1,1),
      R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1) },                 /* H */
    { R(0,1,1,1,0), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0),
      R(0,0,1,0,0), R(0,0,1,0,0), R(0,1,1,1,0) },                 /* I */
    { R(0,0,1,1,1), R(0,0,0,1,0), R(0,0,0,1,0), R(0,0,0,1,0),
      R(0,0,0,1,0), R(1,0,0,1,0), R(0,1,1,0,0) },                 /* J */
    { R(1,0,0,0,1), R(1,0,0,1,0), R(1,0,1,0,0), R(1,1,0,0,0),
      R(1,0,1,0,0), R(1,0,0,1,0), R(1,0,0,0,1) },                 /* K */
    { R(1,0,0,0,0), R(1,0,0,0,0), R(1,0,0,0,0), R(1,0,0,0,0),
      R(1,0,0,0,0), R(1,0,0,0,0), R(1,1,1,1,1) },                 /* L */
    { R(1,0,0,0,1), R(1,1,0,1,1), R(1,0,1,0,1), R(1,0,1,0,1),
      R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1) },                 /* M */
    { R(1,0,0,0,1), R(1,0,0,0,1), R(1,1,0,0,1), R(1,0,1,0,1),
      R(1,0,0,1,1), R(1,0,0,0,1), R(1,0,0,0,1) },                 /* N */
    { R(0,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1),
      R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0) },                 /* O */
    { R(1,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,1,1,1,0),
      R(1,0,0,0,0), R(1,0,0,0,0), R(1,0,0,0,0) },                 /* P */
    { R(0,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1),
      R(1,0,1,0,1), R(1,0,0,1,0), R(0,1,1,0,1) },                 /* Q */
    { R(1,1,1,1,0), R(1,0,0,0,1), R(1,0,0,0,1), R(1,1,1,1,0),
      R(1,0,1,0,0), R(1,0,0,1,0), R(1,0,0,0,1) },                 /* R */
    { R(0,1,1,1,1), R(1,0,0,0,0), R(1,0,0,0,0), R(0,1,1,1,0),
      R(0,0,0,0,1), R(0,0,0,0,1), R(1,1,1,1,0) },                 /* S */
    { R(1,1,1,1,1), R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0),
      R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0) },                 /* T */
    { R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1),
      R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,1,1,0) },                 /* U */
    { R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1),
      R(1,0,0,0,1), R(0,1,0,1,0), R(0,0,1,0,0) },                 /* V */
    { R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,0,0,1), R(1,0,1,0,1),
      R(1,0,1,0,1), R(1,1,0,1,1), R(1,0,0,0,1) },                 /* W */
    { R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,0,1,0), R(0,0,1,0,0),
      R(0,1,0,1,0), R(1,0,0,0,1), R(1,0,0,0,1) },                 /* X */
    { R(1,0,0,0,1), R(1,0,0,0,1), R(0,1,0,1,0), R(0,0,1,0,0),
      R(0,0,1,0,0), R(0,0,1,0,0), R(0,0,1,0,0) },                 /* Y */
    { R(1,1,1,1,1), R(0,0,0,0,1), R(0,0,0,1,0), R(0,0,1,0,0),
      R(0,1,0,0,0), R(1,0,0,0,0), R(1,1,1,1,1) },                 /* Z */

    { 0, 0, 0, 0, 0, 0, 0 }                                       /* space */
};

#undef R

#define SPK_GLYPH_SPACE  36

static int spk_glyph(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'Z')
        return 10 + (c - 'A');
    if (c >= 'a' && c <= 'z')
        return 10 + (c - 'a');

    return SPK_GLYPH_SPACE;
}

int spk_text_width(const char *s, int scale)
{
    int n = 0;

    while (s[n])
        n++;

    if (n == 0)
        return 0;

    return n * (SPK_GLYPH_W + SPK_GLYPH_GAP) * scale - SPK_GLYPH_GAP * scale;
}

int spk_text_height(int scale)
{
    return SPK_GLYPH_H * scale;
}

/* One row of a bitmap, a run of set bits at a time -- the same rule the
 * glyphs are drawn by, and the reason drawing chrome every frame is
 * affordable. */
static void spk_bits(int x, int y, int bits, int width, int scale, int grow)
{
    int col = 0;

    while (col < width)
    {
        int run = 0;

        while (col + run < width && (bits & (1 << (width - 1 - col - run))))
            run++;

        if (run > 0)
        {
            lcd_fillrect(x + col * scale - grow, y,
                         run * scale + 2 * grow, scale);
            col += run;
        }
        else
            col++;
    }
}

int spk_crown_width(int scale)
{
    return SPK_CROWN_W * scale;
}

void spk_crown(int x, int y, int scale)
{
    int row;

    for (row = 0; row < SPK_GLYPH_H; row++)
        spk_bits(x, y + row * scale, crown[row], SPK_CROWN_W, scale, 0);
}

void spk_text(int x, int y, const char *s, int scale, bool bold)
{
    int i;

    for (i = 0; s[i]; i++)
    {
        const unsigned char *g = glyphs[spk_glyph(s[i])];
        int gx = x + i * (SPK_GLYPH_W + SPK_GLYPH_GAP) * scale;
        int row;

        /* A row at a time, and a run of set bits at a time within it: a
         * glyph is two or three rectangles a row rather than thirty-five
         * one-pixel ones, which is what makes drawing text every frame
         * affordable at all. */
        for (row = 0; row < SPK_GLYPH_H; row++)
            spk_bits(gx, y + row * scale, g[row], SPK_GLYPH_W, scale,
                     bold ? 1 : 0);
    }
}
