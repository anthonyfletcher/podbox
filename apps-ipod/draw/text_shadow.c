/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The soft drop shadow behind a line of text, drawn from the text's own
 * coverage.
 ****************************************************************************/

/* An anti-aliased glyph already IS a coverage mask -- 4 bits a pixel, and
 * inverted -- so the shadow is built by walking the string's glyphs into a
 * coverage mask, spreading that with a box blur, expanding it into the 4bpp
 * format lcd_alpha_bitmap_part() reads and blending it once in the shadow's
 * colour. A 1bpp font comes through the same walk as full or no coverage.
 *
 * The mask is kept at half the screen's resolution, one byte to a 2x2 block.
 * A shadow is a blurred blob and the blur is what the eye reads, so the grid
 * it was computed on does not show; the blur, the pack and three quarters of
 * the mask's memory go with the resolution. A hard-edged shadow would show
 * it, which is why blur 0 takes the plain path instead.
 *
 * Cost is a glyph walk, two blur passes over a quarter of the pixels, the
 * expand and one composite over the text's bounding box, per line drawn -- a
 * scrolling line pays it again at every scroll step. The blur's radius is
 * free: both passes carry a running sum, so a wider window costs only its
 * border.
 *
 * One mask serves every caller, the scroll thread included. Nothing here
 * yields and threads switch only where something does, so the buffers need no
 * lock -- keep it that way.
 *
 * Trap: the shadow is clipped by the viewport but not by the line, so a
 * shadow reaching past the line's slack over the font height spills into the
 * neighbouring line, where that line's own clear cuts it off. %Vy is the
 * lever: a line taller than its font by the offset plus the radius keeps the
 * whole shadow inside its own row.
 *
 * Parts: the two buffers, then the glyph walk that fills the coverage, the
 * blur passes, the expand to a full-resolution mask, and text_shadow_draw()
 * which drives them.
 *
 * Callers: put_text() (draw/line.c), for a line whose style carries
 * STYLE_SHADOW -- the skin's %Vt.
 */

#include <string.h>
#include "draw/text_shadow.h"
#include "system.h"
#include "lcd.h"
#include "font.h"
#include "gcc_extensions.h"
#include "bidi.h"

/* The mask holds one line of text at screen width, plus the blur's reach
 * above and below. A taller line takes the plain path. */
#define SHADOW_MAX_TEXT_H  48
#define SHADOW_MAX_H       (SHADOW_MAX_TEXT_H + 2 * TEXT_SHADOW_MAX_BLUR)
#define SHADOW_HALF_W      (LCD_WIDTH / 2)
#define SHADOW_HALF_H      ((SHADOW_MAX_H + 1) / 2)

/* Coverage, one byte to a 2x2 block of screen, holding the sum of the four
 * pixels in it -- so full coverage is four times a pixel's own. */
#define SHADOW_COV_FULL    (4 * LCD_BLEND_OPAQUE)

static unsigned char shadow_cov[SHADOW_HALF_W * SHADOW_HALF_H];

/* The same thing at screen resolution and packed two pixels to a byte, which
 * is what the blitter takes. */
static unsigned char shadow_mask[SHADOW_HALF_W * SHADOW_MAX_H];

/* ---------------------------------------------------------------------- *
 * Coverage                                                                *
 * ---------------------------------------------------------------------- */

/* The coverage of the nibble at `idx` of a 4bpp glyph. Nibbles run along the
 * row and rows share bytes, so a row's first pixel is not always the low
 * nibble of its byte.
 *
 * Forced inline: -Os leaves a plain `inline` out of line, and a call for two
 * instructions of work doubles the walk. */
static FORCE_INLINE unsigned glyph_nibble(const unsigned char *bits, size_t idx)
{
    unsigned byte = bits[idx >> 1];

    return LCD_BLEND_OPAQUE - ((idx & 1) ? (byte >> 4) : (byte & 0x0f));
}

/* One glyph's coverage into the mask, `skip` pixels in from its own left edge
 * and `dst_x` in from the mask's. A mask byte is a 2x2 block of screen, and
 * carries the sum of the four -- 0 to SHADOW_COV_FULL -- so that halving the
 * resolution lightens a thin stroke the way covering less of a pixel does,
 * rather than fattening it the way a maximum would.
 *
 * The sum is why this adds to what is there rather than storing: the four
 * arrive in two passes, one row apart. A combining mark drawn over the glyph
 * it follows adds a second time, which the clamp catches. */
static void glyph_to_mask(const struct font *pf, const unsigned char *bits,
                          int width, int skip, int dst_x,
                          int hw, int hh, int top)
{
    int rows = MIN((int)pf->height, 2 * hh - top);
    int last = MIN(width, skip + 2 * hw - dst_x);

    if (dst_x < 0 || last <= skip)
        return;

    for (int py = 0; py < rows; py++)
    {
        unsigned char *dst = &shadow_cov[((top + py) >> 1) * hw + (dst_x >> 1)];
        size_t idx = (size_t)py * width + skip;
        int px = skip;

        if (pf->depth)
        {
            const unsigned char *s = &bits[idx >> 1];

            /* An odd mask column finishes the block the previous glyph
             * started; after it the blocks line up with pixel pairs. */
            if (dst_x & 1)
            {
                *dst = MIN(*dst + glyph_nibble(bits, idx), SHADOW_COV_FULL);
                dst++;
                idx++;
                px++;
                s = &bits[idx >> 1];
            }

            /* From here the nibble index steps by two, so which half of a
             * byte a block starts in is settled for the whole row. Two loops
             * rather than a test and a shift on every pixel. */
            if (!(idx & 1))
            {
                for (; px + 1 < last; px += 2)
                {
                    unsigned byte = *s++;
                    unsigned cov = 2 * LCD_BLEND_OPAQUE -
                                   (byte & 0x0f) - (byte >> 4);

                    *dst = MIN(*dst + cov, SHADOW_COV_FULL);
                    dst++;
                }
            }
            else
            {
                for (; px + 1 < last; px += 2)
                {
                    unsigned cov = 2 * LCD_BLEND_OPAQUE -
                                   (s[0] >> 4) - (s[1] & 0x0f);

                    s++;
                    *dst = MIN(*dst + cov, SHADOW_COV_FULL);
                    dst++;
                }
            }

            if (px < last)
            {
                unsigned nibble = (idx & 1) ? (*s >> 4) : (*s & 0x0f);

                *dst = MIN(*dst + LCD_BLEND_OPAQUE - nibble,
                           SHADOW_COV_FULL);
            }
        }
        else
        {
            /* 1bpp glyphs are columns of eight vertical pixels. */
            const unsigned char *col = &bits[(py >> 3) * width + skip];
            unsigned shift = py & 7;

            if (dst_x & 1)
            {
                if ((*col++ >> shift) & 1)
                    *dst = MIN(*dst + LCD_BLEND_OPAQUE, SHADOW_COV_FULL);
                dst++;
                px++;
            }

            for (; px + 1 < last; px += 2)
            {
                unsigned cov = (((col[0] >> shift) & 1) +
                                ((col[1] >> shift) & 1)) * LCD_BLEND_OPAQUE;

                col += 2;
                *dst = MIN(*dst + cov, SHADOW_COV_FULL);
                dst++;
            }

            if (px < last && ((*col >> shift) & 1))
                *dst = MIN(*dst + LCD_BLEND_OPAQUE, SHADOW_COV_FULL);
        }
    }
}

/* The string's coverage, walked the way lcd_putsxyofs() walks it: the same
 * visual order, the same widths, the same left-to-right advance and the same
 * stop at the viewport's edge, so the shadow lands under the text rather than
 * beside it.
 *
 * Combining diacritics are the one divergence. The renderer centres a mark
 * over the glyph it follows and this leaves it at its own box, which can put
 * a stacked accent's shadow a pixel or two off. */
static void mask_from_text(struct screen *display, struct viewport *vp,
                           const char *text, int x, int ofs,
                           int mx, int hw, int hh, int top)
{
    struct font *pf;
    ucschar_t *ucs;

    if (vp->flags & VP_FLAG_ALIGNMENT_MASK)
    {
        int w;

        display->getstringsize((const unsigned char *)text, &w, NULL);
        if (vp->flags & VP_FLAG_ALIGN_CENTER)
        {
            x = ((vp->width - w) / 2) + x;
            if (x < 0)
                x = 0;
        }
        else
        {
            x = vp->width - w - x + ofs;
            ofs = 0;
        }
    }

    font_lock(vp->font, true);
    pf = font_get(vp->font);

    for (ucs = bidi_l2v((const unsigned char *)text, 1); *ucs; ucs++)
    {
        int width = font_get_width(pf, *ucs);

        if (x >= vp->width)
            break;

        if (ofs > width)
        {
            ofs -= width;
            continue;
        }

        glyph_to_mask(pf, font_get_bits(pf, *ucs), width, ofs, x - mx,
                      hw, hh, top);
        x += width - ofs;
        ofs = 0;
    }

    font_lock(vp->font, false);
}

/* ---------------------------------------------------------------------- *
 * Blur                                                                    *
 * ---------------------------------------------------------------------- */

/* A box blur, separated into a pass along each axis. The window runs
 * [i-r, i+r] with everything outside the mask counted as zero, which is what
 * a shadow wants: coverage falls away at the edges rather than smearing the
 * border pixel outwards.
 *
 * The divisor is a reciprocal multiply. These CPUs have no divide
 * instruction, and a division per pixel per pass costs more than the rest of
 * the blur put together. */
#define BOX_RECIP(n)      (((1u << 16) + (n) / 2) / (n))
#define BOX_DIV(sum, rcp) (((sum) * (rcp) + (1u << 15)) >> 16)

static void blur_rows(unsigned char *cov, int hw, int hh, int r)
{
    unsigned char tmp[SHADOW_HALF_W];
    unsigned rcp = BOX_RECIP(2 * r + 1);

    for (int row = 0; row < hh; row++)
    {
        unsigned char *p = &cov[row * hw];
        unsigned sum = 0;

        memcpy(tmp, p, hw);
        for (int i = 0; i < r && i < hw; i++)
            sum += tmp[i];

        for (int i = 0; i < hw; i++)
        {
            if (i + r < hw)
                sum += tmp[i + r];
            p[i] = BOX_DIV(sum, rcp);
            if (i >= r)
                sum -= tmp[i - r];
        }
    }
}

static void blur_columns(unsigned char *cov, int hw, int hh, int r)
{
    unsigned char tmp[SHADOW_HALF_H];
    unsigned rcp = BOX_RECIP(2 * r + 1);

    for (int col = 0; col < hw; col++)
    {
        unsigned sum = 0;

        for (int i = 0; i < hh; i++)
            tmp[i] = cov[i * hw + col];
        for (int i = 0; i < r && i < hh; i++)
            sum += tmp[i];

        for (int i = 0; i < hh; i++)
        {
            if (i + r < hh)
                sum += tmp[i + r];
            cov[i * hw + col] = BOX_DIV(sum, rcp);
            if (i >= r)
                sum -= tmp[i - r];
        }
    }
}

/* ---------------------------------------------------------------------- *
 * Mask                                                                    *
 * ---------------------------------------------------------------------- */

/* Coverage to the format lcd_alpha_bitmap_part() reads: 4 bits per pixel, two
 * pixels to a byte with the first in the low nibble, 0 fully opaque and 15
 * fully transparent.
 *
 * One coverage byte is a 2x2 block, which is exactly one byte of the packed
 * row and the same byte again in the row below -- so the opacity, the
 * inversion and the horizontal doubling all fold into one 16-entry table and
 * the second row is a copy of the first. */
static void expand_mask(const unsigned char *cov, int hw, int hh,
                        unsigned char *mask, int mh, int opacity)
{
    unsigned char pair[SHADOW_COV_FULL + 1];

    for (int i = 0; i <= SHADOW_COV_FULL; i++)
    {
        unsigned a = LCD_BLEND_OPAQUE -
                     (i * opacity + SHADOW_COV_FULL / 2) / SHADOW_COV_FULL;

        pair[i] = a | (a << 4);
    }

    for (int row = 0; row < hh; row++)
    {
        const unsigned char *src = &cov[row * hw];
        unsigned char *dst = &mask[2 * row * hw];

        for (int i = 0; i < hw; i++)
            dst[i] = pair[src[i]];

        if (2 * row + 1 < mh)
            memcpy(dst + hw, dst, hw);
    }
}

/* Every pixel fully transparent, in the packed form: both nibbles 15. */
static bool row_is_clear(const unsigned char *row, int bytes)
{
    for (int i = 0; i < bytes; i++)
        if (row[i] != 0xff)
            return false;
    return true;
}

/* The first and last row of the packed mask with anything in it. A font box
 * carries ascender and descender space most strings never reach into, and a
 * row of nothing still costs the blitter a pass along it. */
static void shadow_ink_rows(const unsigned char *mask, int stride, int mh,
                            int *first, int *last)
{
    int top = 0, bottom = mh - 1;

    while (top <= bottom && row_is_clear(&mask[top * stride], stride))
        top++;
    while (bottom > top && row_is_clear(&mask[bottom * stride], stride))
        bottom--;

    *first = top;
    *last = bottom;
}

/* ---------------------------------------------------------------------- *
 * Drawing                                                                 *
 * ---------------------------------------------------------------------- */

/* The string again, offset and in the shadow colour, with the glyph edges it
 * already has. This is what a hard-edged shadow is, so an unblurred one is
 * drawn this way rather than through the mask -- exact, and cheaper than any
 * of it. It also stands in for a line too tall for the mask. */
static void plain_shadow(struct screen *display, int x, int y,
                         const char *text, int text_skip_pixels,
                         const struct line_desc *line)
{
    unsigned saved_fg = display->get_foreground();

    display->set_drawmode(DRMODE_FG);
    display->set_foreground(line->shadow_color);
    display->putsxy_scroll_func(x + line->shadow_x, y + line->shadow_y,
                                (const unsigned char *)text, NULL, NULL,
                                text_skip_pixels);
    display->set_foreground(saved_fg);
}

void text_shadow_draw(struct screen *display, int x, int y,
                      const char *text, int text_skip_pixels,
                      const struct line_desc *line)
{
    struct viewport *vp = *display->current_viewport;
    int blur = MIN(line->shadow_blur, TEXT_SHADOW_MAX_BLUR);
    int mh = display->getcharheight() + 2 * blur;
    int mx, mw, tw;

    /* An aligned line hands its empty halves through here as "" */
    if (!text || !*text)
        return;

    /* Nothing for the mask to add: an opaque unblurred shadow is the glyphs
     * themselves, and a line taller than the mask cannot use it anyway. */
    if ((blur == 0 && line->shadow_opacity >= LCD_BLEND_OPAQUE) ||
        mh > SHADOW_MAX_H)
    {
        plain_shadow(display, x, y, text, text_skip_pixels, line);
        return;
    }

    /* An aligned viewport places the string itself, so its box is the whole
     * viewport rather than anything measured from x. */
    if (vp->flags & VP_FLAG_ALIGNMENT_MASK)
    {
        mx = 0;
        mw = vp->width;
    }
    else
    {
        display->getstringsize((const unsigned char *)text, &tw, NULL);
        mx = MAX(0, x - blur);
        mw = MIN(vp->width - mx, x + tw - text_skip_pixels + blur - mx);
    }

    mw &= ~1;                       /* even rows, one byte per pixel pair */
    if (mw < 2)
        return;

    int hw = mw / 2;
    int hh = (mh + 1) / 2;
    /* Half the radius, since it is spread over half the pixels, but never
     * none: a blur the theme asked for has to soften something. */
    int hr = MAX(1, (blur + 1) / 2);

    memset(shadow_cov, 0, (size_t)hw * hh);
    mask_from_text(display, vp, text, x, text_skip_pixels, mx, hw, hh, blur);

    blur_rows(shadow_cov, hw, hh, hr);
    blur_columns(shadow_cov, hw, hh, hr);
    expand_mask(shadow_cov, hw, hh, shadow_mask, mh, line->shadow_opacity);

    int top, bottom;
    shadow_ink_rows(shadow_mask, hw, mh, &top, &bottom);
    if (top > bottom)
        return;

    unsigned saved_fg = display->get_foreground();
    display->set_drawmode(DRMODE_FG);
    display->set_foreground(line->shadow_color);
    display->alpha_bitmap_part(shadow_mask, 0, top, mw,
                               mx + line->shadow_x,
                               y - blur + line->shadow_y + top,
                               mw, bottom - top + 1);
    display->set_foreground(saved_fg);
}
