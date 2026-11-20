/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to text_layout.c: breaking the ts_* engine's text into lines and
 * drawing them, for any screen that shows it.
 ****************************************************************************/

#ifndef _TEXT_LAYOUT_H_
#define _TEXT_LAYOUT_H_

#include <stdbool.h>
#include <stddef.h>

/* What a layout measures and draws with. Lines are drawn on the main screen's
 * current viewport, whose font must be `font`. */
struct text_layout
{
    int font;           /* font id */
    int bold;           /* bold text's font id, or -1 to set it in `font` */
    int width;          /* the lines' width, pixels */
    bool justify;       /* a full line's leftover shared out between its
                         * spaces */
    unsigned fg;        /* the text's colour */
    unsigned dim;       /* dim text's colour */
    bool head_bold;     /* headings set in `bold`, as bold text is, but for
                         * those TS_HEADING_MINOR marks */
    bool links;         /* names drawn as links: in `link`, and the one
                         * whose TS_NAME_OPEN is at `current` in `link_on`,
                         * the caller having filled a box of `link` behind
                         * it. Off, names are plain text. */
    unsigned link, link_on;
    const unsigned char *current;
};

/* One line, as tl_line_break() found it */
struct tl_line
{
    size_t used;    /* bytes the line consumes, its break included */
    size_t draw;    /* bytes it draws */
    int width;      /* pixels they take */
    bool hyphen;    /* broken at a soft hyphen: a '-' follows */
    bool full;      /* ended because the next word did not fit */
    int indent;     /* pixels before it, set by the caller */
    bool bold;      /* starts inside bold text: set by the caller before the
                     * break, which keeps it */
    bool dim;       /* starts inside dim text, which a link ends back in;
                     * read only with tl->links, set by the caller */
    int height;     /* pixels: the tallest font the line draws in */
    int ascent;     /* its baseline, down from the top */
};

/* The engine's marks (txt_source.h), as they sit in the text: three bytes of
 * UTF-8 each, the last of which tells them apart */
#define TL_HEAD_OPEN  "\xee\x80\x82"    /* TS_HEADING_OPEN */
#define TL_QUOTE      "\xee\x80\x88"    /* TS_QUOTE */
#define TL_DIM_OPEN   0x86              /* TS_DIM_OPEN's last byte */
#define TL_DIM_CLOSE  0x87
#define TL_NAME_OPEN  0x89
#define TL_NAME_CLOSE 0x8a
#define TL_BOLD_OPEN  0x8b
#define TL_BOLD_CLOSE 0x8c
#define TL_MD_LAST    0x8c              /* the last Markdown mark */
#define TL_HIDE_OPEN  0x8d              /* TS_HIDE_OPEN's: never drawn */
#define TL_HIDE_CLOSE 0x8e
#define TL_BULLET     0x8f              /* TS_BULLET's */
#define TL_NUMBERED   0x90
#define TL_MINOR      0x91              /* TS_HEADING_MINOR's */

/* Width of the `n` bytes at `s`, which must have a byte after them that can
 * be written: it is set to NUL while they are measured */
int tl_width(const struct text_layout *tl, unsigned char *s, size_t n);

/* tl_width() of what is visible in the `n` bytes at `s`, in bold where it
 * is: `*bold` says whether they start in bold and is left saying whether
 * they end so */
int tl_text_width(const struct text_layout *tl, unsigned char *s, size_t n,
                  bool *bold);

/* Width of a string constant */
int tl_literal_width(const struct text_layout *tl, const char *s);

/* What the bytes at `s` start with: a soft hyphen; a heading's mark; a
 * Markdown mark (dim, a pull line, a name); a note's mark, its length or 0 */
bool tl_is_shy(const unsigned char *s, size_t n);
bool tl_is_heading_mark(const unsigned char *s, size_t n);
bool tl_is_style_mark(const unsigned char *s, size_t n);
size_t tl_note_mark(const unsigned char *s, size_t n);

/* The bytes of what is invisible and takes no width at `s`, or 0 */
size_t tl_hidden(const unsigned char *s, size_t n);

/* A list item's line at `s`: how far in its lines start, `*first` for its
 * first and the return for those it wraps onto, with `step` pixels a level;
 * `*bullet` where its bullet is drawn, or -1 for a numbered one. 0 and
 * `*first` 0 for a line that starts no item. */
int tl_item_indent(const struct text_layout *tl, unsigned char *s, size_t n,
                   int step, int *first, int *bullet);

/* A list item's bullet, at `x` on the line `ln` whose top is at y */
void tl_draw_bullet(const struct tl_line *ln, int x, int y);

/* The bytes of the `avail` at `s` that fit in `width`, into `ln` */
void tl_line_break(const struct text_layout *tl, unsigned char *s,
                   size_t avail, int width, struct tl_line *ln);

/* Draws the line `ln` of `s` with its top at y. A dim mark sets the
 * foreground, which stays set for the next line; the caller puts tl->fg back
 * after a page. */
void tl_draw_line(const struct text_layout *tl, unsigned char *s,
                  const struct tl_line *ln, int y);

#endif /* _TEXT_LAYOUT_H_ */
