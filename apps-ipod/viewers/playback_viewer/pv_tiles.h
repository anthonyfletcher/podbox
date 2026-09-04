/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to pv_tiles.c -- what Spun's rows are made of.
 ****************************************************************************/
#ifndef _PV_TILES_H
#define _PV_TILES_H

#include <stdbool.h>
#include "draw/card_paint.h"
#include "pv_stats.h"

/* The sections, in the order the row screen walks them.
 *
 * "Newly unlocked" is first and is the only one whose existence is
 * conditional, so the list a screen actually shows is built when Spun opens
 * rather than being this table. With nothing new, Spun opens on In numbers.
 *
 * .specifications/spun-tiles.md is the schedule this implements. */
enum pv_sec
{
    PV_SEC_NEW,
    PV_SEC_NUMBERS,
    PV_SEC_WEEKS,
    PV_SEC_ARTISTS,
    PV_SEC_SONGS,
    PV_SEC_ALBUMS,
    PV_SEC_SKIPS,
    PV_SEC_ACH,
    PV_SEC_COUNT
};

/* The section's own heading, for the title band. */
const char *pv_tiles_section_name(enum pv_sec sec);

/* Whether a section has anything to show.
 *
 * A section with nothing in it is not built at all -- Week by week on a log
 * of eleven days, Top albums where no album name was ever resolved. An absent
 * section is better than a present and empty one, and this is what the screen
 * asks before offering to scroll to one. */
bool pv_tiles_section_present(enum pv_sec sec, const struct pv_totals *t);

/* Expand a section into a row of cards. Returns how many there are.
 *
 * The statistics model must already be built and its buffer still held: every
 * name a card shows is a pointer into the aggregate tables, and nothing here
 * copies one. */
int pv_tiles_build(enum pv_sec sec, const struct pv_totals *t);

/* The row, for card_row_init(). Valid until the next build. */
int                  pv_tiles_count(void);
const short         *pv_tiles_widths(void);
const unsigned char *pv_tiles_flags(void);

/* Where a card carrying artwork gets its colour from: the hue of the picture
 * behind 'key', in degrees, or -1 when there is none to be had.
 *
 * A callback because the pictures belong to the screen and the colours belong
 * to the cards, and neither should have to know about the other. Answered
 * from what is already loaded rather than by loading: a colour is wanted for
 * every card on screen and a load is a file read, so a card whose picture has
 * not arrived keeps its assigned colour and takes the derived one in the same
 * frame the picture itself appears. */
typedef int (*pv_tint_fn)(unsigned key);
void pv_tiles_set_tint(pv_tint_fn fn);

/* Which picture card 'idx' follows, or 0 for a card that follows none. A
 * sub-card answers with its parent's, because it wears its parent's colour.
 *
 * For a caller that has to make sure a picture is in hand BEFORE it resolves
 * the card, which is the order the colour above depends on. */
unsigned pv_tiles_art_key(int idx);

/* Fill in what card 'idx' shows.
 *
 * Strings are either pointers into the aggregate tables or into a small pool
 * this file rotates, so they are valid until the next call -- which is what a
 * caller drawing one card at a time wants, and the only thing it may assume. */
void pv_tiles_content(int idx, struct card_content *out);

/* A run has just been unfolded, named by any card in it -- opening one moves
 * the focus into the run, so a caller passing "the card that was opened"
 * passes a sub-card.
 *
 * A week's top artist, top song and distinct song count are the one thing the
 * day array cannot answer, so they cost a bounded read of the log -- one
 * week's worth, seeked to rather than scanned for. Doing it here means it
 * happens when a week is opened rather than once per card on screen. */
void pv_tiles_open(int idx);

/* ---------------------------------------------------- acting on a card */

/* What a card is about, for a caller that wants to act on it rather than draw
 * it. Only the cards that name a thing answer: a figure, a chart or a badge
 * is about the year, not about anything that could be played. */
enum pv_target_kind
{
    PV_TARGET_NONE = 0,
    PV_TARGET_ARTIST,
    PV_TARGET_ALBUM,
    PV_TARGET_SONG,
};

struct pv_target
{
    enum pv_target_kind kind;
    const char *name;      /* the artist, the album or the song */
    const char *artist;    /* narrows an album or a song; NULL if unknown */
};

/* Where card 'idx' points, false for a card that points nowhere. Any card in
 * a run answers for the run's parent: the sub-cards describe the same thing
 * from behind it.
 *
 * The strings are the aggregate table's own and last as long as it does.
 * They are NAMES, taken from a tag or from a path when the log was written --
 * not database ids, so a caller looking one up again can genuinely miss. */
bool pv_tiles_target(int idx, struct pv_target *out);

#endif /* _PV_TILES_H */
