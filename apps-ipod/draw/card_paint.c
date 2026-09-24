/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * What one card looks like.
 *
 * Parts: the ink, derived from a card's one assigned colour; the palette a
 * card is assigned from; the generators and patterns that stand in for a
 * picture; the grid; then the measure and the painter.
 *
 * It knows nothing about where a card's content came from. Everything it
 * draws arrives in one struct card_content, which is what lets the same
 * routine serve the device screen, the measuring harness and the host
 * renderer without any of them being a special case.
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "card_text.h"
#include "card_paint.h"

static card_paint_art_fn art_fn;

void card_paint_set_art(card_paint_art_fn fn)
{
    art_fn = fn;
}

static int font_title  = FONT_SYSFIXED;
static int font_tag    = FONT_SYSFIXED;
static int font_card   = FONT_SYSFIXED;
static int font_body   = FONT_SYSFIXED;
static int font_figure = FONT_SYSFIXED;
static int font_value  = FONT_SYSFIXED;
static int font_icon   = FONT_SYSFIXED;

void card_paint_set_fonts(const struct card_fonts *f)
{
    font_title  = f->title;
    font_tag    = f->tag;
    font_card   = f->card;
    font_body   = f->body;
    font_figure = f->figure;
    font_value  = f->value;
    font_icon   = f->icon;
}

int card_paint_title_font(void)
{
    return font_title;
}

int card_paint_card_font(void)
{
    return font_card;
}

int card_paint_body_font(void)
{
    return font_body;
}

int card_paint_tag_font(void)
{
    return font_tag;
}

int card_paint_figure_font(void)
{
    return font_figure;
}

int card_paint_value_font(void)
{
    return font_value;
}

int card_paint_icon_font(void)
{
    return font_icon;
}

/* A codepoint as the UTF-8 the text routines take. Two bytes is enough for
 * every glyph in an icon font, which sits inside the first 2048. */
static void icon_utf8(char *b, unsigned cp)
{
    if (cp < 0x80)
    {
        b[0] = (char)cp;
        b[1] = 0;
    }
    else
    {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        b[2] = 0;
    }
}

/* One row of a table: a label, a figure against the right edge, and a rule
 * under both. */
static int table_line_h(void)
{
    int a = (int)font_get(font_body)->height;
    int b = (int)font_get(font_value)->height;

    return a > b ? a : b;
}

static int table_row_h(void)
{
    return table_line_h() + CARD_TABLE_GAP;
}

/* What a table actually covers: n rows of ink with the gap only between them.
 *
 * Not n * row_h. That counts a gap under the last rule which is never drawn,
 * so a caller centring the table places a block taller than the thing in it
 * and everything sits high by half a gap. */
static int table_block_h(int n)
{
    return n > 0 ? (n - 1) * table_row_h() + table_line_h() + 2 : 0;
}

static void table_draw(const struct card_kv *kv, int n, int x, int y, int w)
{
    int rh = table_row_h();
    int lh = table_line_h();
    int asc = font_get(font_body)->ascent;
    int va  = font_get(font_value)->ascent;

    if (va > asc)
        asc = va;

    for (int i = 0; i < n; i++)
    {
        int tw = 0, top = y + i * rh;
        int vf = font_value;

        lcd_set_drawmode(DRMODE_FG);
        lcd_setfont(font_body);
        lcd_putsxy(x, top + asc - font_get(font_body)->ascent, kv[i].label);

        lcd_setfont(vf);
        font_getstringsize((const unsigned char *)kv[i].value, &tw, NULL, vf);
        lcd_putsxy(x + w - tw, top + asc - font_get(vf)->ascent,
                   kv[i].value);

        /* The rule sits just under its own row rather than midway to the
         * next: a line floating between two rows belongs to neither, and the
         * last one has to close the table rather than leave it hanging. */
        lcd_set_drawmode(DRMODE_SOLID);
        lcd_hline(x, x + w - 1, top + lh + 1);
    }
}


/* ------------------------------------------------------------- the ink */

/* From draw/color.c. Declared here rather than included, because color.h
 * pulls in screen_access.h and with it most of the application: this file has
 * to link into a host renderer with no device behind it, and two prototypes
 * are a smaller price than that. */
void     color_get_hsv(unsigned c, int *h, int *s, int *v);
unsigned color_from_hsv(int h, int s, int v);


/* One base per card, everything else derived with a fixed step rather than
 * driven at a contrast target -- a target met by lightening whitens every
 * accent on a light card. */
/* Trap: every component has to be pulled into a signed int BEFORE it is
 * subtracted. RGB_UNPACK_*() of an unsigned is unsigned, so a difference that
 * should be negative -- any mix toward a darker colour -- wraps to about four
 * billion instead, and the result is a saturated colour with no relation to
 * either end. It is invisible in the mixes that happen to brighten. */
static unsigned mix(unsigned a, unsigned b, int t)
{
    int ar = RGB_UNPACK_RED(a),   br = RGB_UNPACK_RED(b);
    int ag = RGB_UNPACK_GREEN(a), bg = RGB_UNPACK_GREEN(b);
    int ab = RGB_UNPACK_BLUE(a),  bb = RGB_UNPACK_BLUE(b);

    return LCD_RGBPACK(ar + (br - ar) * t / 256,
                       ag + (bg - ag) * t / 256,
                       ab + (bb - ab) * t / 256);
}

/* Every colour on a card is the card's own hue at a different lightness.
 *
 * Not black or white, and not one or the other depending on the ground. Two
 * things are wrong with that, and only the second is obvious:
 *
 * White or black chosen per card is a decision made one card at a time while
 * the reader sees a whole row of them, so the ink flips partway along and the
 * row reads as two designs -- and it flips at whichever colours happen to sit
 * near the threshold, which is nowhere meaningful. Forcing it to one by
 * darkening the palette fixes that and costs the palette its life.
 *
 * A near-white carrying the card's own hue is light enough to read on any of
 * them and still belongs to the card it is on, so the row is consistent
 * without being flat and the palette stays as bright as it likes. What has to
 * recede goes the other way, toward the step a sub-card takes. */
void card_paint_ink(unsigned base, struct card_ink *ink)
{
    int h, sat, val;
    unsigned lift;

    color_get_hsv(base, &h, &sat, &val);
    /* A grey card's own hue is a trace, and a near-white carrying it reads
     * as tinted glass. Slate takes a plainer white. */
    lift = color_from_hsv(h, sat < 128 ? 10 : 30, 255);

    ink->bg     = base;
    ink->text   = lift;
    ink->dim    = mix(lift, base, 80);
    ink->accent = mix(base, lift, 70);
    /* The same colour a sub-card of this card would be. The unfilled part of
     * a progress bar is the card's own ground showing through, and making it
     * the shade that already means "one step behind this" ties the bar to the
     * run underneath rather than giving the card a second scheme. */
    ink->track  = card_paint_step(base, 1);
    /* The plate belongs to the card, so it is a shade of the card and not a
     * block of something else: one short step toward the text colour, which
     * reads as the same colour caught in a different light. */
    ink->plate  = mix(base, lift, 40);
    /* A pattern is drawn in the card's own pair, so it reads as a texture on
     * the card rather than as a second card behind it. Further from the base
     * than the accent, because a fill covering half the tile at the accent's
     * step is barely a pattern at all. */
    ink->pat    = mix(base, lift, 110);
}

/* A card given an accent of its own wears it on everything that is not text:
 * the bar, the progress fill, the plate and the pattern. The plate and the
 * pattern are mixed toward it rather than filled with it, so they stay
 * shades of the card. */
static void ink_accent(struct card_ink *ink, unsigned a)
{
    if (!a)
        return;
    ink->accent = a;
    ink->plate  = mix(ink->bg, a, 120);
    ink->pat    = mix(ink->bg, a, 90);
}

/* --------------------------------------------------------- the palette */

/* The ground: four slates, close enough to read as one surface and far
 * enough apart that two neighbours still part without a gap between them.
 * Blue-grey rather than grey, so the accents sit on something that is a
 * colour, not on an absence of one. */
#define N_SLATE 4

static const unsigned slate[N_SLATE] =
{
    LCD_RGBPACK( 36,  42,  54),
    LCD_RGBPACK( 46,  54,  68),
    LCD_RGBPACK( 30,  34,  44),
    LCD_RGBPACK( 54,  62,  78),
};

/* One accent a section, in the order sections are numbered, and no two
 * neighbours near each other on the wheel. Each is as light as the ink rule above lets it be: an
 * accent is also a whole card where a card is lit, and keeps its near-white
 * text at CARD_INK_FLOOR or better there.
 *
 * Trap: amber is the only yellow, and it has to stay this deep. sRGB's most
 * chromatic colours between about 50 and 130 degrees are also its lightest,
 * so a hue there held down to where the ink reads is khaki. The eight share
 * the rest of the wheel, about thirty-five degrees apart. */
#define N_ACCENT 8

static const unsigned accent[N_ACCENT] =
{
    LCD_RGBPACK(196,  56,  48),   /* coral   */
    LCD_RGBPACK(  0, 124, 116),   /* teal    */
    LCD_RGBPACK(120,  72, 210),   /* violet  */
    LCD_RGBPACK(166,  88,   0),   /* amber   */
    LCD_RGBPACK(  0, 106, 190),   /* azure   */
    LCD_RGBPACK(184,  40, 128),   /* magenta */
    LCD_RGBPACK( 28, 122,  66),   /* green   */
    LCD_RGBPACK( 72,  82, 210),   /* indigo  */
};

/* Every fifth summary tile, from the first: often enough that a section's
 * colour is never out of sight for long, rarely enough that it still marks
 * something. */
#define ACCENT_EVERY 5

unsigned card_paint_slate(int idx)
{
    return slate[(unsigned)idx % N_SLATE];
}

unsigned card_paint_accent(int section)
{
    return accent[(unsigned)section % N_ACCENT];
}

bool card_paint_accent_slot(int idx)
{
    return idx % ACCENT_EVERY == 0;
}

/* A card does not choose its colour; it is assigned one.
 *
 * A sub-card takes its parent's, stepped further from the text colour once
 * per position in the run -- so an open run reads as one block in one family
 * rather than as four unrelated tiles, which is the same thing the ten-pixel
 * height difference is saying. */
unsigned card_paint_step(unsigned base, int depth)
{
    if (depth == 0)
        return base;

    /* Darker per position, never lighter: the ink is a near-white and has to
     * keep reading on the deepest card in a run, and a run that darkens as it
     * goes also reads as standing behind the card that owns it.
     *
     * A wide step, because a run is short -- the tile schedule's longest is
     * three. A step sized for a run of ten leaves four cards nobody can tell
     * apart. */
    return mix(base, LCD_RGBPACK(16, 16, 20), 26 + depth * 22);
}

/* ------------------------------------------------------ derived colour */

/* Fifteen degrees a bin. Finer splits one picture's hue across neighbouring
 * bins and lets a smaller, purer patch of colour outvote it. */
#define HUE_BINS   24

/* What a pixel has to have before its hue is worth anything: not nearly
 * black, not nearly white, and actually coloured. */
#define HUE_MIN_V  40
#define HUE_MIN_S  60

int card_paint_dominant_hue(const fb_data *px, int stride, int w, int h)
{
    long bin[HUE_BINS];
    int step = (w * h) / 2048;
    int best = -1;
    long best_w = 0;

    if (!px || w <= 0 || h <= 0)
        return -1;
    if (step < 1)
        step = 1;

    for (int i = 0; i < HUE_BINS; i++)
        bin[i] = 0;

    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x += step)
        {
            int hue, sat, val;

            color_get_hsv(px[y * stride + x], &hue, &sat, &val);
            if (val < HUE_MIN_V || sat < HUE_MIN_S)
                continue;
            /* Weighted by how strongly the pixel holds its hue, so a wash of
             * pale blue does not outvote a smaller field of real colour. */
            bin[(hue / (360 / HUE_BINS)) % HUE_BINS] += sat;
        }
    }

    for (int i = 0; i < HUE_BINS; i++)
    {
        if (bin[i] > best_w)
        {
            best_w = bin[i];
            best = i;
        }
    }

    if (best < 0)
        return -1;
    return best * (360 / HUE_BINS) + (360 / HUE_BINS) / 2;
}

/* The least contrast a card's text may have against the card, in tenths:
 * 4.1 to 1. The palette meets it by design; a derived tint is darkened until
 * it does. */
#define CARD_INK_FLOOR 41

/* Each channel's share of relative luminance, sRGB-decoded, in 1/65536ths --
 * indexed by the channel's RGB565 bits, which is every value the panel has. */
static const unsigned short lum_r[32] =
{
        0,    34,    72,   127,   212,   309,   428,   570,
      759,   954,  1176,  1425,  1738,  2049,  2389,  2760,
     3215,  3654,  4126,  4632,  5242,  5822,  6438,  7090,
     7868,  8600,  9369, 10177, 11134, 12026, 12959, 13933,
};
static const unsigned short lum_g[64] =
{
        0,    57,   114,   172,   243,   328,   428,   544,
      677,   827,   995,  1181,  1385,  1610,  1854,  2118,
     2478,  2789,  3123,  3478,  3857,  4258,  4682,  5131,
     5603,  6100,  6621,  7168,  7740,  8338,  8962,  9612,
    10463, 11174, 11912, 12679, 13473, 14295, 15146, 16026,
    16935, 17873, 18841, 19839, 20867, 21926, 23015, 24135,
    25579, 26769, 27992, 29246, 30532, 31851, 33203, 34587,
    36004, 37455, 38939, 40457, 42009, 43596, 45216, 46871,
};
static const unsigned short lum_b[32] =
{
        0,    11,    25,    43,    72,   105,   145,   194,
      258,   324,   399,   484,   590,   696,   811,   937,
     1092,  1241,  1401,  1573,  1780,  1977,  2186,  2408,
     2672,  2920,  3182,  3456,  3781,  4084,  4401,  4732,
};

static long luminance(unsigned c)
{
    return (long)lum_r[RGB_UNPACK_RED(c) >> 3]
         + lum_g[RGB_UNPACK_GREEN(c) >> 2]
         + lum_b[RGB_UNPACK_BLUE(c) >> 3];
}

/* Whether a card of this colour keeps its text at the floor. The ink is
 * lighter than any card, so the ratio is text over ground, each with the
 * 0.05 flare term (3277 in these units) added. */
static bool ink_reads(unsigned base)
{
    struct card_ink ink;

    card_paint_ink(base, &ink);
    return 10 * (luminance(ink.text) + 3277)
           >= CARD_INK_FLOOR * (luminance(base) + 3277);
}

unsigned card_paint_tint(int hue)
{
    static int pal_s, pal_v;
    unsigned c;
    int v;

    if (hue < 0)
        return 0;

    /* The accents' own register, measured from them rather than written
     * down, so editing an accent above cannot leave a sleeve's in a family the
     * designed ones have left. */
    if (pal_v == 0)
    {
        long sum_s = 0, sum_v = 0;

        for (int i = 0; i < N_ACCENT; i++)
        {
            int h, s, v;

            color_get_hsv(accent[i], &h, &s, &v);
            sum_s += s;
            sum_v += v;
        }
        pal_s = (int)(sum_s / N_ACCENT);
        pal_v = (int)(sum_v / N_ACCENT);
    }

    /* Trap: one brightness is not one lightness. At the same brightness a
     * yellow or green is far lighter than a blue, and text on it falls well
     * under the floor -- so the brightness comes down per hue until the text
     * reads. Still a function of the hue alone, so a sleeve keeps one colour
     * everywhere. */
    for (v = pal_v; ; v -= 4)
    {
        c = color_from_hsv(hue, pal_s, v);
        if (v <= 4 || ink_reads(c))
            return c;
    }
}

/* ------------------------------------------------------- the generators */

/* Five bands a sky, and the share of the card's height each takes, top down.
 * The top three are dark enough for the card's ink to read on, and together
 * they cover the title and the line under it; the bright two sit behind the
 * chart, whose bars are what stands on the horizon. */
#define SKY_BANDS 5

static const unsigned char sky_pc[SKY_BANDS] = { 30, 18, 16, 17, 19 };

/* Each sky starts in the slate the row is made of and ends in a horizon of
 * the palette's own accents, so the four are the row's colours and not a
 * second scheme. Amber, teal and coral are sunrise, noon and dusk; night
 * starts darker than any slate and ends in violet. */
static const unsigned sky[CARD_GEN_COUNT][SKY_BANDS] =
{
    [CARD_GEN_SUNRISE] = {
        LCD_RGBPACK( 36,  42,  54), LCD_RGBPACK( 46,  50,  66),
        LCD_RGBPACK( 72,  64,  80), LCD_RGBPACK(140,  96,  50),
        LCD_RGBPACK(166,  88,   0) },
    [CARD_GEN_NOON] = {
        LCD_RGBPACK( 36,  42,  54), LCD_RGBPACK( 36,  54,  66),
        LCD_RGBPACK( 30,  72,  82), LCD_RGBPACK( 10, 100, 104),
        LCD_RGBPACK(  0, 124, 116) },
    [CARD_GEN_DUSK] = {
        LCD_RGBPACK( 36,  42,  54), LCD_RGBPACK( 54,  42,  60),
        LCD_RGBPACK( 92,  46,  60), LCD_RGBPACK(150,  52,  52),
        LCD_RGBPACK(196,  56,  48) },
    [CARD_GEN_NIGHT] = {
        LCD_RGBPACK( 22,  26,  36), LCD_RGBPACK( 28,  32,  46),
        LCD_RGBPACK( 40,  38,  64), LCD_RGBPACK( 64,  48, 110),
        LCD_RGBPACK( 90,  60, 160) },
};

/* Night's stars, as thousandths of the card across and down. Between the
 * text and the chart, which is the only part of the sky left showing. */
#define N_STARS 9

static const short star_x[N_STARS] =
    { 740, 135, 540, 915, 310, 665,  85, 830, 455 };
static const short star_y[N_STARS] =
    { 400, 430, 470, 500, 520, 560, 590, 620, 650 };

unsigned card_paint_gen_base(enum card_gen gen)
{
    if (gen <= CARD_GEN_NONE || gen >= CARD_GEN_COUNT)
        return LCD_RGBPACK(8, 8, 12);
    return sky[gen][0];
}

unsigned card_paint_gen_ink(enum card_gen gen)
{
    struct card_ink ink;

    /* The same rule as any other card, taken from the top band, which is
     * where the text is. */
    card_paint_ink(card_paint_gen_base(gen), &ink);
    return ink.text;
}

void card_paint_gen(enum card_gen gen, int x, int y, int w, int h)
{
    int top = y;

    if (gen <= CARD_GEN_NONE || gen >= CARD_GEN_COUNT || w <= 0 || h <= 0)
        return;

    lcd_set_drawmode(DRMODE_SOLID);

    /* The last band takes whatever the rounding left, so the sky always
     * reaches the card's foot. */
    for (int i = 0; i < SKY_BANDS; i++)
    {
        int bh = (i == SKY_BANDS - 1) ? y + h - top : h * sky_pc[i] / 100;

        lcd_set_foreground(sky[gen][i]);
        lcd_fillrect(x, top, w, bh);
        top += bh;
    }

    if (gen == CARD_GEN_NIGHT)
    {
        lcd_set_foreground(LCD_RGBPACK(210, 214, 240));
        for (int i = 0; i < N_STARS; i++)
            lcd_fillrect(x + w * star_x[i] / 1000, y + h * star_y[i] / 1000,
                         1, 1);
    }
}

/* --------------------------------------------------------- the patterns */

/* The period of every pattern, in pixels. One number for the set so two
 * patterns side by side share a rhythm.
 *
 * Sixteen, so the tile below is 32 and an art card's 128-pixel band is four
 * whole tiles across: no half a stripe where the card ends, and twelve blits
 * to fill it instead of twenty-eight. Ten was chosen for the density alone
 * and cost both. */
#define PAT_STEP 16

/* Which of the two colours pixel (x, y) takes.
 *
 * A predicate rather than a shape: each pattern is a test cheap enough to run
 * per pixel, and the caller turns runs of equal answers into spans, so the
 * cost is a fill per run and not a call per pixel. */
static int pat_at(enum card_pat pat, int x, int y)
{
    int u = x % PAT_STEP, v = y % PAT_STEP;
    int half = PAT_STEP / 2;

    switch (pat)
    {
    case CARD_PAT_STRIPE:
        return (x / PAT_STEP) & 1;
    case CARD_PAT_CHECK:
        return ((x / PAT_STEP) ^ (y / PAT_STEP)) & 1;
    case CARD_PAT_DIAG:
        return ((x + y) / PAT_STEP) & 1;
    case CARD_PAT_CHEVRON:
    {
        /* The diagonal, folded: y is reflected every period, which turns a
         * run of parallel bands into a run of arrowheads. */
        int m = y % (PAT_STEP * 2);
        int t = (m < PAT_STEP) ? m : PAT_STEP * 2 - m;

        return ((x + t) / PAT_STEP) & 1;
    }
    case CARD_PAT_DIAMOND:
    {
        int du = u < half ? half - u : u - half;
        int dv = v < half ? half - v : v - half;

        return (du + dv) < half;
    }
    case CARD_PAT_TRI_TL:
        return (u + v) < PAT_STEP;
    case CARD_PAT_TRI_BR:
        return (u + v) >= PAT_STEP;
    default:
        return 0;
    }
}

/* One period of a pattern, drawn once and then tiled over the card.
 *
 * Every pattern above repeats on a 2 * PAT_STEP grid in both axes -- the
 * widest period any of them has -- so one small tile serves a whole card,
 * and at PAT_STEP 16 that tile divides the picture band's width exactly.
 *
 * Trap: the obvious way round is to evaluate the predicate per pixel and turn
 * runs of equal answers into spans, and that is what this did. On a 128 x 76
 * picture band it is ten thousand predicate calls and about a thousand
 * one-pixel-tall fills, PER CARD PER FRAME -- which is why a row of tiles
 * with no artwork behind it ran badly on a 5G while every other card was
 * fine. Building the tile is four hundred predicate calls, once, and the fill
 * is then a couple of dozen blits. */
#define PAT_TILE (2 * PAT_STEP)

static fb_data  pat_tile[PAT_TILE * PAT_TILE];
static int      pat_tile_id = -1;
static unsigned pat_tile_a, pat_tile_b;

static void pat_build(enum card_pat pat, unsigned a, unsigned b)
{
    if (pat_tile_id == (int)pat && pat_tile_a == a && pat_tile_b == b)
        return;

    for (int ty = 0; ty < PAT_TILE; ty++)
        for (int tx = 0; tx < PAT_TILE; tx++)
            pat_tile[ty * PAT_TILE + tx] =
                (fb_data)(pat_at(pat, tx, ty) ? b : a);

    pat_tile_id = (int)pat;
    pat_tile_a  = a;
    pat_tile_b  = b;
}

void card_paint_pattern(enum card_pat pat, int x, int y, int w, int h,
                        unsigned a, unsigned b)
{
    if (w <= 0 || h <= 0)
        return;

    pat_build(pat, a, b);

    for (int ty = 0; ty < h; ty += PAT_TILE)
    {
        int bh = (h - ty < PAT_TILE) ? h - ty : PAT_TILE;

        for (int tx = 0; tx < w; tx += PAT_TILE)
        {
            int bw = (w - tx < PAT_TILE) ? w - tx : PAT_TILE;

            /* Placed on the tile's own grid, so the phase is right without
             * the blit having to know where the card sits on screen. */
            lcd_bitmap_part(pat_tile, 0, 0, PAT_TILE, x + tx, y + ty, bw, bh);
        }
    }
}

/* -------------------------------------------------------------- the grid */

void card_paint_grid(int x, int y, int w, int h,
                     const unsigned char *level, int n, int rows,
                     unsigned base, unsigned ink)
{
    /* Whether the caller fixed the shape, captured before 'rows' is filled
     * in below: it is what decides the reading order, and inferring that
     * later from the cell proportions would switch layout the day a year's
     * cells happened to come out square. */
    bool by_column = (rows > 0);
    int cols, cw, ch, gw, gh, ox, oy, gx, gy;

    if (n <= 0 || w <= 0 || h <= 0)
        return;

    if (by_column)
    {
        /* A year fixes its rows at seven, and fifty-three columns then cap
         * the cell width at about four pixels on this screen. Square cells
         * would leave most of the card empty, so the year's cells are
         * allowed to be taller than they are wide -- up to a limit, past
         * which a calendar stops reading as one and starts reading as a
         * barcode. */
        cols = (n + rows - 1) / rows;
        cw = w / cols;
        ch = h / rows;
        if (ch > cw * 3)
            ch = cw * 3;
    }
    else
    {
        /* A wall of badges has no natural shape, so it takes the largest
         * square that squares the whole set up inside the box. */
        cw = 0;
        for (int c = (h < w ? h : w); c >= 2; c--)
        {
            int co = w / c;
            int r = co ? (n + co - 1) / co : 0;

            if (co >= 1 && r * c <= h)
            {
                cw = c;
                break;
            }
        }
        if (cw < 2)
            return;
        ch = cw;
        cols = w / cw;
        rows = (n + cols - 1) / cols;
    }

    if (cw < 2 || ch < 2 || cols < 1)
        return;

    /* A cell keeps a pixel of daylight around it only if it can afford one.
     *
     * Per axis, because a year's cells are four pixels wide and twelve tall:
     * a gap between weeks would be a quarter of the cell and the calendar
     * reads as a comb, while a gap between days is a twelfth and is what
     * stops a busy week merging into one stripe. A badge wall's cells are
     * square and large enough for both. */
    gx = cw >= 6 ? 1 : 0;
    gy = ch >= 6 ? 1 : 0;

    gw = cols * cw;
    gh = rows * ch;
    ox = x + (w - gw) / 2;
    oy = y + (h - gh) / 2;

    lcd_set_drawmode(DRMODE_SOLID);

    for (int i = 0; i < n; i++)
    {
        /* Down a column then across for a year, so it reads as weeks left to
         * right with one week's days stacked -- the shape a calendar heat map
         * has, and the only one a date can be found in. A wall of badges has
         * no dates and reads across. */
        int cx = by_column ? (i / rows) : (i % cols);
        int cy = by_column ? (i % rows) : (i / cols);
        int lv = level[i];

        if (lv > CARD_GRID_MAX)
            lv = CARD_GRID_MAX;
        if (cx >= cols || cy >= rows)
            continue;

        lcd_set_foreground(mix(base, ink, 30 + lv * (226 / CARD_GRID_MAX)));
        lcd_fillrect(ox + cx * cw, oy + cy * ch, cw - gx, ch - gy);
    }
}

/* ------------------------------------------------------------ the measure */

/* The widths a card is allowed to settle at, narrowest first.
 *
 * "As wide as it needs to be for the number" means as narrow as it can be
 * without the text running out of the bottom, so the answer is the first
 * width whose wrapped block fits -- not the width of the longest line, which
 * would make every card holding a sentence the widest one allowed. */
static const short auto_w[] = { 104, 128, 152, 180, 210, CARD_W_MAX };

/* Lines a card aims to wrap to before it widens instead.
 *
 * Three is the balance between two things pulling opposite ways: a card
 * squeezed into the narrowest width that holds it wraps a sentence into a
 * column of fragments, and a card given a line target of two grows so wide
 * that barely one is on screen at a time -- which stops a row being a row. */
#define AUTO_LINES 3

void card_paint_clear(struct card_content *c)
{
    memset(c, 0, sizeof(*c));
    c->prog = -1;
    c->base = slate[0];
}

static int text_w_for(int card_w)
{
    return card_w - 2 * CARD_INSET;
}

int card_paint_measure(const struct card_content *c, int h)
{
    int w = CARD_W_MIN, room, tw, th, i;
    int n_auto = (int)(sizeof(auto_w) / sizeof(auto_w[0]));

    /* A picture is blitted at its own size rather than rescaled per card, and
     * a grid wants every pixel it can have. Neither is negotiable. */
    if (c->art)
        return CARD_ART_W;
    if (c->level)
        return CARD_W_MAX;

    /* A table is as wide as its widest label and its widest figure, with room
     * between them: wrapping a label would defeat the point of the table.
     *
     * A card carrying text as well is at least as wide as that text wants,
     * because the two share a column. */
    if (c->table && c->n_table > 0)
    {
        int lw = 0, vw = 0;

        for (i = 0; i < c->n_table; i++)
        {
            int t = 0;

            font_getstringsize((const unsigned char *)c->table[i].label, &t,
                               NULL, font_body);
            if (t > lw) lw = t;
            font_getstringsize((const unsigned char *)c->table[i].value, &t,
                               NULL, font_value);
            if (t > vw) vw = t;
        }
        /* Two insets and a gutter between the columns. A table squeezed to
         * its content reads as two lists that happen to be beside each other;
         * the space between is what makes a row scan across. */
        w = lw + vw + 5 * CARD_INSET;

        if (c->text.n)
        {
            card_text_measure(&c->text, text_w_for(w), 0, &tw, &th);
            if (tw + 2 * CARD_INSET + CARD_GUTTER > w)
                w = tw + 2 * CARD_INSET + CARD_GUTTER;
        }

        if (w < CARD_W_MIN) w = CARD_W_MIN;
        if (w > CARD_W_MAX) w = CARD_W_MAX;
        return w;
    }

    /* A card with no plate has no head band to leave clear: its content runs
     * the full height. Reserving one everywhere lays every tile out as though
     * it had a title row, which is a third of the card given to nothing. */
    room = h - ((c->tag || c->tag_icon) ? CARD_PLATE_H : CARD_INSET)
         - CARD_INSET;
    if (c->prog >= 0)
        room -= CARD_PROG_H;
    if (c->series)
        room -= c->series_h ? c->series_h : h / 3;
    if (c->title)
        room -= font_get(font_card)->height + CARD_TITLE_GAP;

    /* Not merely "does it fit": a sentence squeezed into the narrowest card
     * that holds it wraps four or five times and reads as a column of
     * fragments. Three lines is the shape a tile is legible at, and a card
     * with more to say widens instead of stacking. */
    for (i = 0; i < n_auto; i++)
    {
        int lines = card_text_measure(&c->text, text_w_for(auto_w[i]), 0,
                                      &tw, &th);

        if ((lines <= AUTO_LINES && th <= room) || i == n_auto - 1)
        {
            /* Wider than the text by a clear margin, not by the inset alone:
              * a figure that stops a hair short of the card's edge reads as
              * having been squeezed in. */
            w = tw + 2 * CARD_INSET + CARD_GUTTER;
            if (w > auto_w[i])
                w = auto_w[i];
            break;
        }
    }

    if (c->title)
    {
        int t = 0;

        font_getstringsize((const unsigned char *)c->title, &t, NULL,
                           font_card);
        t += 2 * CARD_INSET;

        if (t > w)
            w = t;
    }

    if ((c->tag || c->tag_icon) && w < CARD_PLATE_W + CARD_PAD)
        w = CARD_PLATE_W + CARD_PAD;

    if (w < CARD_W_MIN) w = CARD_W_MIN;
    if (w > CARD_W_MAX) w = CARD_W_MAX;
    return w;
}

/* ------------------------------------------------------------ the painter */

/* Flat colour to all four edges, no border and no rounding: the colour is the
 * only thing separating one card from the next, which is why CARD_GAP is
 * zero.
 *
 * Trap: a panel or a strip at the foot has to be measured from the text that
 * will go in it, including the title, and the body has to stop above it. Size
 * the panel from the body alone and the title pushes the last line off the
 * bottom of the card. */
void card_paint_draw(const struct card_content *c, int w, int h, int x_off)
{
    struct card_ink ink;
    int ox = x_off;
    int tfh, tw, fh, y;
    /* A card with no plate has no head band to leave clear, so its content
     * runs the full height -- and a grid with neither plate nor heading runs
     * to the card's own edges, because a year of days wants every pixel and
     * the section title above already says what it is. */
    int body_top = (c->tag || c->tag_icon) ? CARD_PLATE_H
                 : (c->level && c->title)  ? CARD_PLATE_H
                 : c->level                ? 0
                                           : CARD_INSET;
    int body_bot = h;
    int text_w = text_w_for(w);
    int text_x = ox + CARD_INSET;
    int text_h = 0;
    int fit_bot;
    int room, lines;

    card_paint_ink(c->base, &ink);

    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(ink.bg);
    lcd_fillrect(ox, 0, w, h);

    /* A chart tile's art is a sky over the whole card, with the series drawn
     * on it -- which is what the tile schedule asks for, and why the ink
     * comes from the sky rather than from the card. */
    if (c->gen != CARD_GEN_NONE)
    {
        card_paint_gen(c->gen, ox, 0, w, h);
        /* Everything on the card now derives from the sky rather than from
         * the colour it was assigned -- that colour is under the sky and
         * invisible, so a plate taken from it is a shade of something nobody
         * can see. */
        card_paint_ink(card_paint_gen_base(c->gen), &ink);
        ink.text = ink.dim = card_paint_gen_ink(c->gen);
    }
    ink_accent(&ink, c->accent);

    tfh = font_get(font_card)->height;
    if (c->prog >= 0)
        body_bot = h - CARD_PROG_H;

    /* The room the text HAS, and then how many of its lines fit in it. Text
     * past that is not shortened by anything downstream -- it is simply drawn
     * off the bottom edge, which is what a four-line album title did to its
     * tile.
     *
     * An art card is bounded by the picture rather than by the card: the
     * panel may grow only until the picture is down to CARD_ART_MIN_H, so
     * that is the most room a caption can ever be given whatever it says. */
    room = c->art
         ? body_bot - CARD_ART_MIN_H - 2 * CARD_INSET
         : body_bot - ((c->tag || c->tag_icon) ? CARD_PLATE_H : CARD_INSET)
           - CARD_INSET
           - (c->series ? (c->series_h ? c->series_h : h / 3) : 0)
           - (c->title ? tfh + CARD_TITLE_GAP : 0)
           - (c->n_table ? CARD_TABLE_GAP + table_block_h(c->n_table) : 0);

    lines = card_text_lines_in(&c->text, text_w, room);
    card_text_measure(&c->text, text_w, lines, NULL, &text_h);

    /* An art card is a picture with a panel over its foot, and the panel is
     * as tall as its own contents need -- title, body and nothing else. It is
     * the one place a card's layout is driven by content rather than by the
     * template, which is exactly why its height has to be measured and not
     * assumed. */
    if (c->art)
    {
        const fb_data *art = NULL;
        int a_stride = 0, a_w = 0, a_h = 0;
        int panel_h = 2 * CARD_INSET + (c->title ? tfh + CARD_TITLE_GAP : 0)
                    + text_h;
        int art_h;

        if (panel_h > body_bot - CARD_ART_MIN_H)
            panel_h = body_bot - CARD_ART_MIN_H;
        art_h = body_bot - panel_h;

        if (art_fn && c->art_key)
            art = art_fn(c->art_key, &a_stride, &a_w, &a_h);

        if (art)
        {
            /* Cropped rather than squashed: the cache stores squares and
             * the band is wider than it is tall, so the blit takes part of
             * the picture and it keeps its proportions.
             *
             * From the TOP, not the middle. A sleeve's subject sits above its
             * centre far more often than below it, and the band is the top of
             * the card, so taking the top of the picture is also what lines
             * the two up. */
            int sy = 0;
            int sx = (a_w - w) / 2;

            if (sx < 0) sx = 0;
            lcd_bitmap_part(art, sx, sy, a_stride, ox, 0,
                            a_w < w ? a_w : w, a_h < art_h ? a_h : art_h);
        }
        else
        {
            card_paint_pattern((enum card_pat)(c->pat % CARD_PAT_COUNT),
                               ox, 0, w, art_h, ink.bg, ink.pat);
        }

        lcd_set_drawmode(DRMODE_SOLID);
        lcd_set_foreground(ink.bg);
        lcd_fillrect(ox, art_h, w, body_bot - art_h);

        /* The panel's top inset, matching the one fit_bot takes off the
         * bottom. Leaving it out skews the centring by half an inset -- five
         * pixels, which lands the caption hard under the picture with a gap
         * beneath it, and reads as odd without looking obviously wrong. */
        body_top = art_h + CARD_INSET;
    }

    /* The progress strip: full bleed along the card's foot, with the figure
     * on whichever side of the fill it fits. */
    if (c->prog >= 0)
    {
        int pct = c->prog > 100 ? 100 : c->prog;
        int fill = w * pct / 100;
        const char *label = c->prog_label;

        lcd_set_drawmode(DRMODE_SOLID);
        lcd_set_foreground(ink.track);
        lcd_fillrect(ox, h - CARD_PROG_H, w, CARD_PROG_H);
        lcd_set_foreground(ink.accent);
        lcd_fillrect(ox, h - CARD_PROG_H, fill, CARD_PROG_H);

        /* Only where one was asked for. The bar is a proportion, and a
         * proportion of what is already the card's own figure needs no second
         * number on top of it. */
        if (label)
        {
            lcd_setfont(FONT_SYSFIXED);
            lcd_set_drawmode(DRMODE_FG);
            lcd_set_foreground(ink.text);
            font_getstringsize((const unsigned char *)label, &tw, &fh,
                               FONT_SYSFIXED);
            lcd_putsxy(ox + (fill > tw + 8 ? fill - tw - 4 : fill + 4),
                       h - CARD_PROG_H + (CARD_PROG_H - fh) / 2, label);
        }
    }

    /* The plate, where a card has one. On an art card it is punched out of
     * the picture, so it is filled with the text colour and lettered in the
     * card's own. */
    if (c->tag || c->tag_icon)
    {
        char glyph[4];
        const char *mark = c->tag;
        int face = font_tag;

        if (!mark)
        {
            icon_utf8(glyph, c->tag_icon);
            mark = glyph;
            face = font_icon;
        }

        lcd_set_drawmode(DRMODE_SOLID);
        lcd_set_foreground(c->art ? ink.text : ink.plate);
        lcd_fillrect(ox, 0, CARD_PLATE_W, CARD_PLATE_H);

        lcd_set_drawmode(DRMODE_FG);
        lcd_set_foreground(c->art ? ink.bg : ink.text);
        font_getstringsize((const unsigned char *)mark, &tw, &fh, face);
        lcd_setfont(face);
        lcd_putsxy(ox + CARD_BAR_W + (CARD_PLATE_W - CARD_BAR_W - tw) / 2,
                   (CARD_PLATE_H - fh) / 2, mark);
    }

    /* The bar: the card's accent, the plate's height and in its place, so a
     * card without a plate still has something in that corner. It says
     * nothing about the focus -- the card at the focus point is the current
     * one, and position is the whole of that. */
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(ink.accent);
    lcd_fillrect(ox, 0, CARD_BAR_W, CARD_PLATE_H);

    /* The two grids: a year of days and a wall of badges. Same drawing, and
     * the data is what tells them apart. */
    if (c->level && c->n_level > 0)
    {
        /* Bled to the card's edges, the way a chart is: a grid is content,
         * not text, and a year of days wants every pixel there is. Only a
         * head band it actually uses is kept clear. */
        int gap = body_top ? CARD_PAD : 0;

        card_paint_grid(ox, body_top + gap, w, body_bot - body_top - gap,
                        c->level, c->n_level, c->grid_rows, ink.bg,
                        c->accent ? ink.accent : ink.text);
        body_bot = body_top;    /* the grid is the body */
    }

    /* A series bleeds to the card's sides and to the top of whatever is below
     * it, so the body has to stop above that. It arrives normalised to 1000,
     * because only the caller knows what the tallest bar of the group should
     * be -- four charts cut from one histogram share a scale, and normalising
     * each to its own would flatten the very difference they exist to show. */
    if (c->series && c->n_series > 0)
    {
        int n = c->n_series;
        int band = c->series_h ? c->series_h : h / 3;

        /* Black, whatever the card is. Against a sky they read as a skyline
         * cut out of it, which is the one place the screen's own background
         * shows through the design rather than around it. */
        lcd_set_drawmode(DRMODE_SOLID);
        lcd_set_foreground(LCD_RGBPACK(16, 16, 20));
        for (int i = 0; i < n; i++)
        {
            /* Each bar spans from its own boundary to the next, so the set
             * tiles the card exactly: no gaps between bars, and no strip of
             * card left over on the right when the width does not divide. */
            int x0 = i * w / n;
            int x1 = (i + 1) * w / n;
            int v = c->series[i] * band / 1000;

            if (v < 2)
                v = 2;
            lcd_fillrect(ox + x0, body_bot - v, x1 - x0, v);
        }
        body_bot -= band;
    }

    /* Where content is centred, which is not where it is clipped.
     *
     * 'body_top' already carries the inset -- or the head band, which stands
     * in for it -- and 'body_bot' does not, so centring between the two lands
     * everything low by half an inset. Every card was doing that. */
    fit_bot = body_bot - CARD_INSET;

    /* A card with no plate puts its title beside the bar, level with it.
     * A card with one puts the title at the top of the body block, so the
     * plate is not competing with two things. */
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(ink.text);
    lcd_setfont(font_card);

    if (c->level)
    {
        /* A grid owns the body outright, so its card's title stays in the
         * head band -- beside the plate where there is one, beside the bar
         * where there is not. Put it in the body block instead and it is
         * drawn over the grid. */
        int tx = (c->tag || c->tag_icon) ? ox + CARD_PLATE_W + CARD_PAD
                                         : text_x;

        if (c->title)
            lcd_putsxy(tx, (CARD_PLATE_H - tfh) / 2, c->title);
        y = body_bot;
    }
    else
    {
        /* Centred in the space the card has left, with the heading part of
         * the block rather than a row of its own at the top: a card is a
         * whole, not a title and a body.
         *
         * A table counts toward that block. A card may carry both -- a line
         * naming what the figures are about, and then the figures -- and
         * centring the text alone would put the pair off centre by half the
         * table. */
        int block = (c->title ? tfh + CARD_TITLE_GAP : 0) + text_h;

        if (c->table && c->n_table > 0)
            block += (text_h ? CARD_TABLE_GAP : 0)
                   + table_block_h(c->n_table);

        y = body_top + (fit_bot - body_top - block) / 2;
        if (y < body_top)
            y = body_top;

        if (c->title)
        {
            lcd_putsxy(text_x, y, c->title);
            y += tfh + CARD_TITLE_GAP;
        }
    }

    /* Text first, then the table under it. 'y' is already where the block
     * starts, and both were counted into it. */
    if (c->text.n)
    {
        card_text_draw(&c->text, text_x, y, text_w, lines);
        y += text_h + CARD_TABLE_GAP;
    }

    if (c->table && c->n_table > 0)
    {
        lcd_set_foreground(ink.dim);
        table_draw(c->table, c->n_table, text_x, y, text_w);
    }
}

void card_paint_spine(const struct card_content *c, int w, int h)
{
    struct card_ink ink;

    card_paint_ink(c->base, &ink);
    ink_accent(&ink, c->accent);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(ink.bg);
    lcd_fillrect(0, 0, w, h);
    lcd_set_foreground(ink.accent);
    lcd_fillrect(0, 0, CARD_BAR_W < w ? CARD_BAR_W : w, CARD_PLATE_H);
}
