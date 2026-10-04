/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.h (colour parsing)
 * GNU General Public License (version 2+)
 *
 * Interface to color.c.
 ****************************************************************************/
#ifndef _COLOR_H_
#define _COLOR_H_

#include <stdbool.h>
#include "draw/screen_access.h"

/* Parse "#rrggbb" (or "rrggbb") into a packed RGB value. Returns 0 on
 * success, -1 if the string is not a valid hex colour. */
int hex_to_rgb(const char* hex, int* color);

/* Set on a parsed colour written with a leading '!', marking it as one the
 * dynamic-colour palette must leave alone. Colours are 16 bits on this
 * display, so the flag rides above them in the same int and is stripped by
 * dynamic_colors_resolve(), the one place every skin colour passes through.
 *
 * It stays set in the stored original, which is what carries the intent: the
 * viewport keeps it in dc_orig_fg/dc_orig_bg, and the tags that default to the
 * viewport's foreground (%Vg's text, %dr's fill) inherit it from there. */
#define COLOR_FIXED (1u << 24)

/* Set on a colour a skin wrote as a palette word: `accent`, `dominant` or
 * `vivid`, with an optional tone bound, shade and fallback. A word with all
 * three does not fit beside a colour in one int, so the bits below hold an
 * index into a table of what was written (color_word()), and
 * dynamic_colors_resolve() turns it into a colour. */
#define COLOR_WORD (1u << 25)
#define COLOR_WORD_INDEX(c) ((c) & 0xffu)

/* Distinct palette words, across every skin loaded at once. The table is
 * emptied when the skins are reloaded (color_words_reset()), and a skin that
 * asks for more fails to load. */
#define COLOR_WORDS_MAX 64

enum color_word_kind
{
    COLOR_WORD_ACCENT,          /* the album's text colour */
    COLOR_WORD_DOMINANT,        /* the album's background colour */
    COLOR_WORD_VIVID,           /* the album's most colourful colour */
};

/* What a skin wrote: `vivid>50.75:5ea8f0` is kind VIVID, bound +1, tone 50,
 * shade 76, fallback 5ea8f0. */
struct color_word
{
    unsigned char kind;         /* enum color_word_kind */
    signed char bound;          /* +1 for `>tone`, -1 for `<tone`, 0 none */
    unsigned char tone;         /* 0..100 */
    unsigned char shade;        /* `.NN` plus one; 0 is none */
    bool has_fallback;          /* `:rrggbb` was written */
    unsigned fallback;          /* that colour, native */
};

/* The word a parsed colour stands for, or NULL if it is a plain colour. */
const struct color_word *color_word(unsigned c);

/* Empty the word table. Only while no skin holds an index into it: the skin
 * engine calls it between unloading every skin and loading them again. */
void color_words_reset(void);

/* Parse a colour for the given screen, accepting the forms theme files and
 * skins use, plus a leading '!' for COLOR_FIXED and the palette words above.
 * Returns true if text held a usable colour. */
bool parse_color(enum screen_type screen, char *text, int *value);

/* Tone is perceptual lightness, CIELAB L*: 0 is black, 100 white. Taken from
 * the same luminance color_contrast() uses, so the contrast between two
 * colours follows from their tones alone, whatever their hues.
 *
 * c moved to tone `tone` or lighter (bound > 0) or darker (bound < 0), as
 * little as that takes: its OKLab lightness changes, its hue does not, and its
 * chroma drops only where the display cannot show it at the new lightness. A
 * colour already within the bound comes back unchanged. */
unsigned color_tone_bound(unsigned c, int tone, int bound);

/* OKLab chroma -- how colourful c is, independent of how light -- in units of
 * 1/65536. Greys are 0, and the most vivid colours a display shows are about
 * 21000. */
int color_chroma(unsigned c);

/* Mix c1 toward c2, per channel. t runs 0..256: 0 leaves c1 alone, 256 gives
 * c2. Used to derive a secondary colour from a foreground/background pair --
 * the only way to do that which holds up for any pair, including the arbitrary
 * ones the album-art colours produce. */
unsigned color_blend(unsigned c1, unsigned c2, int t);

/* A colour in base's family but distinctly not it: base's hue turned by
 * `degrees`, rebuilt at saturation `sat` and brightness `val` (both 0..255).
 * Saturation and brightness are given rather than kept because the colours
 * this is asked about -- theme and album backgrounds -- are typically dark and
 * flat, and keeping theirs would return something as unusable as the input. */
unsigned color_hue_rotate(unsigned base, int degrees, int sat, int val);

/* Where an accent sits from the background it belongs to, as a turn around the
 * colour wheel.
 *
 * 100 degrees rather than a true complement (180): on a navy background 180
 * lands on amber, which is legible but reads as a different design. 100 lands
 * on a pink-mauve -- which is, to within a couple of values, the accent this
 * fork's theme already used by eye before any of it was computed.
 *
 * Also the orientation given to a theme that has no hue of its own to measure
 * from, so its accents land where a derived one would have. */
#define COLOR_ACCENT_ROTATE 100

/* Split a colour into hue (0..359 degrees), saturation and brightness (both
 * 0..255), and put one back together. A grey has no hue and reports 0. */
void color_get_hsv(unsigned c, int *h, int *s, int *v);
unsigned color_from_hsv(int h, int s, int v);

/* c moved to at least `target` contrast (in hundredths) against `against`,
 * keeping its hue and saturation and changing only its brightness. Falls back
 * to white or black when the hue cannot reach the target at any brightness. */
unsigned color_fit_contrast(unsigned c, unsigned against, int target);

/* WCAG contrast between two colours, in hundredths: 450 is the 4.5:1 wanted
 * for body text, 700 the 7:1 of AAA. A ratio rather than a difference, because
 * the same gap between two dark colours reads far weaker than between two
 * light ones. (skin_albumart_color.c carries the same formula over unpacked
 * channels, for use inside its extraction loop.) */
int color_contrast(unsigned c1, unsigned c2);

#endif /* _COLOR_H_ */
