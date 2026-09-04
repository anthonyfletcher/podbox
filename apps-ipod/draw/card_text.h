/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to card_text.c -- a line of mixed-size text.
 ****************************************************************************/
#ifndef _CARD_TEXT_H
#define _CARD_TEXT_H

/* Almost every tile emphasises part of a sentence -- "You've listened to
 * [12,345] minutes of music", the figure larger than the words around it, a
 * name larger than the label before it. So a text slot is a sequence of runs
 * rather than a string: the runs wrap together to one width, and each line
 * sets whatever runs land on it against one baseline.
 *
 * A run carries its own face and colour and nothing else. Which face and
 * which colour is the caller's decision, so this file has no opinion about
 * emphasis and no dependency on a card's palette. */

#define CARD_RUN_MAX 10

struct card_run
{
    const char   *text;
    short         font;
    unsigned      colour;
    unsigned char brk;      /* starts a line of its own */
};

struct card_text
{
    struct card_run run[CARD_RUN_MAX];
    int n;
};

/* Runs are appended in reading order. Anything past CARD_RUN_MAX is dropped
 * rather than truncating the sentence somewhere less obvious. */
void card_text_reset(struct card_text *t);
void card_text_add(struct card_text *t, const char *s, int font,
                   unsigned colour);

/* The same, but starting a line of its own.
 *
 * A figure and the word it belongs to do not sit well on one line: they are
 * different sizes, so their spacing is a compromise between two rhythms and
 * looks like neither. Set on separate lines they read as a number with a
 * label, which is what they are. */
void card_text_add_line(struct card_text *t, const char *s, int font,
                        unsigned colour);

/* Wrapped to 'w' and to at most 'max_lines' of them (0 for no limit), how
 * wide the block actually came out and how tall.
 *
 * A block that runs out of lines ends with an ellipsis on the last one, with
 * as many trailing words dropped as it takes to fit -- because the caller's
 * limit is what the card HAS, and text past it is not shortened but simply
 * drawn off the bottom edge.
 *
 *
 * The width back is the longest line rather than 'w', which is what a card
 * asking to be only as wide as it needs measures itself from.
 *
 * Returns the number of lines, which is not the height divided by anything:
 * a line is as tall as the tallest run on it, so a sentence with one
 * emphasised figure has one line taller than the rest. A caller choosing a
 * width wants the count. */
int card_text_measure(const struct card_text *t, int w, int max_lines,
                      int *out_w, int *out_h);

/* Draw it wrapped to 'w' with its top left at (x, y) in the current
 * viewport. Leaves the drawmode at DRMODE_FG and the font unset -- callers
 * here all set their own. */
void card_text_draw(const struct card_text *t, int x, int y, int w,
                    int max_lines);

/* The most of this text's own lines that fit in 'room' pixels when wrapped to
 * 'w'. Never fewer than one: a card with room for nothing still has to show
 * something.
 *
 * Measured, not divided. A line is set from the real metrics of whichever
 * runs land on it, so a block of mixed sizes has no one line height to divide
 * 'room' by -- and dividing by the tallest face the caller owns charges every
 * line for a face most of them do not contain. */
int card_text_lines_in(const struct card_text *t, int w, int room);

#endif /* _CARD_TEXT_H */
