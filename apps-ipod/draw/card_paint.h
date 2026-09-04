/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to card_paint.c -- what one card looks like.
 ****************************************************************************/
#ifndef _CARD_PAINT_H
#define _CARD_PAINT_H

#include <stdbool.h>
#include "lcd.h"
#include "font.h"
#include "card_text.h"

/* Drawing only. It knows nothing about the row, the cache, the screen or
 * where a card sits: it is handed a size and draws into whatever viewport is
 * current, which is a card's own bitmap when the cache is calling and the
 * band when it is not.
 *
 * That is what lets the same painter run on a host with no device
 * (.build/cardrender), so a change to how a card looks can be looked at
 * without building firmware for it. */


/* The head block: a bar at the card's top left, and on some cards a plate
 * beside it. The bar is the plate's height and sits in the plate's place, so
 * a card without one still has something in that corner.
 *
 * The height is what every card pays, plate or not -- it is where the body
 * starts -- so it is kept to what the tallest thing in a plate needs and no
 * more. A grid is what makes that visible: the band comes off the top of the
 * drawing rather than off a paragraph that can reflow.
 *
 * Wider than it is tall because a rank or a week number reaches two digits
 * and the bar takes four pixels of the width. */
#define CARD_PLATE_W 40
#define CARD_PLATE_H 38
#define CARD_BAR_W    4
#define CARD_PAD      6

/* The margin text keeps on every side.
 *
 * One number for all four, so a card's content sits in an even frame: the
 * left inset clears the accent bar, and the right and bottom match it rather
 * than being whatever was left over. Uneven margins are the thing that reads
 * as "not designed" even when nothing is obviously wrong. */
#define CARD_INSET   (CARD_BAR_W + CARD_PAD)

/* Between one ruled table row and the next. Wider than a text line's leading,
 * because a rule between them makes each row a thing of its own and things of
 * their own need air. */
#define CARD_TABLE_GAP 11

/* Clear space a card keeps to the right of its longest line, on top of the
 * inset. Without it an automatic width lands one pixel past the text and the
 * card reads as having been cut to fit. */
#define CARD_GUTTER   14

/* Between a card's heading and the text under it. Related things sit closer
 * together than unrelated ones, and this is the only gap on a card that has
 * to say "these two belong together". */
#define CARD_TITLE_GAP 6

/* A strip along the card's foot, and no more than that. The figure in it is
 * set in the system font: it is a footnote to the card, not a thing to read,
 * and a strip tall enough for a real face is a strip that competes with the
 * content above it. */
#define CARD_PROG_H  13

/* An art card is as wide as the cached artwork, so a picture is blitted at
 * its own size rather than rescaled per card. */
#define CARD_ART_W  128

/* And its picture is never shorter than this.
 *
 * The panel over the foot is measured from the caption, so the picture takes
 * what is left -- which puts every art card's caption on the row's shared
 * bottom line, the same one every other card sits on. The floor is what stops
 * a long caption from squeezing the picture down to a stripe; a caption that
 * would need more than this leaves is clipped, which is the right way round,
 * because a card's picture is not negotiable and its third line is.
 *
 * It is affordable only because a caption is a NAME. Everything else a row
 * has to say belongs on the sub-card behind it: a row is glanced along, and
 * the detail is one press away. */
#define CARD_ART_MIN_H  56

/* What an automatic width is allowed to settle between. The floor keeps a
 * one-word card from becoming a stripe; the ceiling stops a long sentence
 * from becoming a card nothing else fits beside on a 320-pixel screen. */
#define CARD_W_MIN   96
#define CARD_W_MAX  250

/* The band, shared so the device screen and the host renderer cannot drift.
 *
 * The title band above the row is what is left of the screen, and the title
 * is centred in it. */
#define CARD_ROW_X    30    /* the row's left margin, aligned to the title */
#define CARD_ROW_Y    56
#define CARD_ROW_H   155

/* Cards touch: the colour change is the edge. */
#define CARD_GAP       0

/* What a folded sub-card leaves showing. Enough to read as a stack of spines
 * behind the card that owns them, and no more. */
#define CARD_FOLD_W    9

/* A sub-card is barely shorter than its parent: the run reads as one block
 * rather than as a step down. */
#define CARD_SUB_DROP 10

/* The named backgrounds a tile asks for when it has no artwork.
 *
 * Not a placeholder for a missing picture: a generator is what those tiles
 * are meant to look like. The four time-of-day charts are told apart by
 * theirs as much as by their bars, which is why each carries its own
 * colourway rather than taking the card's. */
enum card_gen
{
    CARD_GEN_NONE = 0,
    CARD_GEN_SUNRISE,   /* from the bottom -- red and orange */
    CARD_GEN_NOON,      /* from the top -- yellow and off-white */
    CARD_GEN_DUSK,      /* from the bottom -- orange, pink and purple */
    CARD_GEN_NIGHT,     /* from the top -- blue and deep blue */
    CARD_GEN_COUNT
};

/* Fill a rect with a burst. Flat wedges, hard edges, no gradient: the palette
 * is flat, so a wash would be the only soft thing on screen. */
void card_paint_gen(enum card_gen gen, int x, int y, int w, int h);

/* The repeating patterns, for a tile whose artwork is simply missing -- an
 * artist or an album with no picture. Unlike a burst these carry no colours
 * of their own: they are drawn in the card's own pair, so a patterned tile
 * still belongs to the row it sits in.
 *
 * A set rather than one, because a row of twenty artists with no art is
 * twenty tiles of the same fill otherwise. Which one a tile gets is decided
 * by its identity, so an artist keeps the same pattern between visits. */
enum card_pat
{
    CARD_PAT_STRIPE = 0,   /* upright bands */
    CARD_PAT_CHECK,        /* squares */
    CARD_PAT_DIAG,         /* bands at 45 degrees */
    CARD_PAT_CHEVRON,      /* the same, folded back on themselves */
    CARD_PAT_DIAMOND,      /* a lattice of diamonds */
    CARD_PAT_TRI_TL,       /* half-squares, split top-left */
    CARD_PAT_TRI_BR,       /* half-squares, split bottom-right */
    CARD_PAT_COUNT
};

void card_paint_pattern(enum card_pat pat, int x, int y, int w, int h,
                        unsigned a, unsigned b);

/* A cell per item, each at an intensity of 0..CARD_GRID_MAX.
 *
 * The year of days and the wall of badges are one drawing with two sets of
 * data: a day's intensity is how much was heard on it, a badge's is whether
 * it is earned, in progress or not yet in reach. 'rows' fixes the shape --
 * seven for a year, laid out by weekday -- or zero to let the cell size
 * choose it. */
#define CARD_GRID_MAX 4
void card_paint_grid(int x, int y, int w, int h,
                     const unsigned char *level, int n, int rows,
                     unsigned base, unsigned ink);

/* What reads over that generator. A colourway carries its own answer because
 * the two colours of one are near enough in lightness that a card-level
 * choice would be wrong for half of them. */
unsigned card_paint_gen_ink(enum card_gen gen);

/* The colour a burst is mostly made of.
 *
 * A card wearing one takes its accent and its plate from this rather than
 * from the colour it was assigned: the assigned colour is under the burst and
 * invisible, so anything derived from it lands on the card as a shade of
 * something that is not there. */
unsigned card_paint_gen_base(enum card_gen gen);

/* Where a card's artwork comes from.
 *
 * Keyed by the row's own art hash rather than by a card index, because the
 * cache behind this holds a few pictures and the row holds hundreds of cards:
 * the key is what a slot is looked up by. Returning NULL means this card has
 * no picture and takes a pattern instead -- which is the common case and not
 * a failure.
 *
 * Artwork is handed over already decoded and already scaled, because neither
 * can happen while a card is being drawn: a card may be redrawn every frame,
 * and a decode is orders of magnitude too slow for that. **This is why art
 * needs a cache of its own whether or not finished cards get one** -- the two
 * hold different things, and only one of them is optional. */
typedef const fb_data *(*card_paint_art_fn)(unsigned key, int *stride,
                                            int *w, int *h);

void card_paint_set_art(card_paint_art_fn fn);

/* What a card shows.
 *
 * The painter reads this and nothing else: it has no idea whether the figures
 * came from a playback log or were invented by a measuring harness, which is
 * what lets the same routine run on a device, on a host renderer and under
 * the harness. Which slots are filled is what decides a card's shape -- there
 * is no kind field, because "has a picture" and "has a grid" already say it.
 *
 * Every pointer is borrowed for the duration of one draw. A caller formatting
 * a sentence into a static buffer per card is doing the expected thing.
 *
 * Text runs are laid out flush left in the body block. Which face and which
 * colour each run gets is the caller's decision -- see card_text.h. */
struct card_kv
{
    const char *label;
    const char *value;
};

struct card_content
{
    unsigned base;              /* the colour the row assigned this card */
    enum card_gen gen;          /* a burst over the whole card, or NONE */

    /* The plate: a number, or a glyph from the theme's icon font. Either
     * fills it; a card with neither has no plate. */
    const char    *tag;
    unsigned short tag_icon;    /* a codepoint, 0 for none */
    const char *title;          /* one line; NULL for none */
    struct card_text text;      /* the body */

    /* Or a table instead of it: a label and a figure per row, ruled.
     *
     * The sub-cards carrying four figures read as one long phrase when they
     * are set as prose, and a reader after a single number has to find it
     * inside a sentence. Ruled rows put every label under the one before it
     * and every figure against the same right edge, so the number wanted is
     * where the eye already is. */
    const struct card_kv *table;
    int                   n_table;

    /* A picture band across the card's top. 'art_key' is looked up through
     * card_paint_set_art(); a key with no picture behind it falls to the
     * pattern named by 'pat', which is what most rows get. */
    bool          art;
    unsigned      art_key;
    unsigned char pat;

    const short *series;        /* a chart, normalised 0..1000 */
    int          n_series;
    /* The chart's band height; 0 for a third of the card. */
    short        series_h;

    const unsigned char *level; /* a grid, 0..CARD_GRID_MAX per cell */
    int                  n_level;
    int                  grid_rows;

    int         prog;           /* 0..100 along the foot, or -1 for none */
    const char *prog_label;     /* NULL draws the bar and no figure */

};

/* Start from nothing: no plate, no title, no art, no progress. */
void card_paint_clear(struct card_content *c);

/* The three faces, by whatever id the caller's font system uses. A caller
 * with no font system at all can leave them alone and get the system font.
 *
 * Acquiring them is the caller's job on purpose: the device loads .fnt files
 * through font_load(), and the host renderer reads them itself. */
/* The faces, by whatever id the caller's font system uses.
 *
 * Six names, three or four files: the same face serves more than one role,
 * and which file is which is the caller's decision. What the roles mean is
 * fixed, and it is the whole of the type hierarchy:
 *
 *   figure  a number, and only a number -- the largest thing on a card
 *   name    an artist, an album, a song, a badge, a date: what a card is ABOUT
 *   card    a card's own heading, where it has one
 *   body    the words between, set dim
 *
 * A caller with no font system leaves them alone and gets the system font
 * throughout; one with no icon font passes FONT_SYSFIXED for it and gets no
 * icons rather than wrong ones. */
struct card_fonts
{
    int title;      /* the section heading, above the row */
    int tag;        /* a number in the plate */
    int card;       /* a card's heading, and every name */
    int body;
    int figure;     /* the hero number on a card -- a SUBSET face, see §4.6 */
    int value;      /* a figure in a table, which is a smaller thing */
    int icon;       /* the theme's icon font, for a plate that holds a glyph */
};

/* Trap: 'figure' may be a face with no letterforms in it -- Spun's is Noto
 * Serif cut to seventeen glyphs -- so only a number may be set in it. 'value'
 * must be a full face, because a table's right-hand column carries dates. */

void card_paint_set_fonts(const struct card_fonts *f);
int  card_paint_title_font(void);
int  card_paint_card_font(void);
int  card_paint_body_font(void);
int  card_paint_tag_font(void);
int  card_paint_figure_font(void);
int  card_paint_value_font(void);
int  card_paint_icon_font(void);

/* The colour a card at position 'idx' is assigned, and the step a sub-card
 * 'depth' places into a run takes from its parent's.
 *
 * A card does not choose its colour, so this is not a slot: the row owner
 * calls it while building and puts the answer in 'base'. */
unsigned card_paint_palette(int idx, int depth);

/* That step on its own, for a run whose parent's colour did not come from the
 * palette at all. */
unsigned card_paint_step(unsigned base, int depth);

/* The hue a picture is mostly made of, in degrees, or -1 for one that has
 * none to speak of.
 *
 * A histogram rather than an average: averaging opposite hues gives grey,
 * which is the one answer a picture is never mostly made of. Near-black,
 * near-white and washed-out pixels do not vote -- they carry no hue to
 * contribute and there are usually a great many of them.
 *
 * Sampled, not walked: this runs when a picture is loaded, which is inside a
 * frame. */
int card_paint_dominant_hue(const fb_data *px, int stride, int w, int h);

/* A card carrying artwork takes the hue of the picture and the lightness and
 * saturation of the palette itself. Returns 0 for a picture with no hue.
 *
 * Keeping the picture's own dominant colour is what produces the muddy
 * near-blacks and blown-out neons that make a derived palette look unrelated
 * to the designed one. Keeping only its hue is what makes a derived tile look
 * like a member of the same family as a painted one.
 *
 * The hue is the ONLY thing that varies, and that is the point: an album is
 * the same colour wherever it appears. Take the lightness and saturation from
 * the card's own assigned colour instead and the same sleeve is olive in Top
 * artists and mustard in Top songs, because the two cards sit at different
 * positions in the row. */
unsigned card_paint_tint(int hue);

/* What reads over a colour -- white or near-black, whichever is further from
 * it. Exposed because a caller filling text runs has to choose their colours
 * before the painter sees them. */
struct card_ink
{
    unsigned bg, text, dim, accent, track, pat, plate;
};
void card_paint_ink(unsigned base, struct card_ink *ink);

/* How wide this card wants to be, which is what PV_W_AUTO resolves to. The
 * measurement lives beside the painter so the two cannot disagree about what
 * fits. */
int card_paint_measure(const struct card_content *c, int h);

/* Draw it at 'w' x 'h' into the current viewport.
 *
 * 'x_off' shifts the content left, for a caller drawing a card that is
 * clipped by the screen edge and cannot use a viewport at a negative x. */
void card_paint_draw(const struct card_content *c, int w, int h, int x_off);

/* Just the edge of one: its colour and its accent bar, and none of its
 * content.
 *
 * For a card folded down to a spine, where the content is behind the card in
 * front of it anyway -- drawing the whole thing and letting the viewport clip
 * it to eight pixels costs a full card's layout for eight pixels of fill. */
void card_paint_spine(const struct card_content *c, int w, int h);

#endif /* _CARD_PAINT_H */
