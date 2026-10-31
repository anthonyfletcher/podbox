/***************************************************************************
 * Token ids for this build's extra skin tags.
 * GNU General Public License (version 2+)
 *
 * Upstream token ids come from enum skin_token_type in
 * lib/skin_parser/tag_table.h. Ids defined here sit above that enum's range
 * instead of inside it, so the upstream header needs no edit and the two sets
 * cannot collide. struct wps_token stores an id in an unsigned short rather
 * than the enum itself so values from both sets fit.
 *
 * The tags are registered in custom_tags.c and evaluated in skin_tokens.c.
 ****************************************************************************/

#ifndef CUSTOM_TOKENS_H
#define CUSTOM_TOKENS_H

/* The upstream enum currently ends below 200. Basing well above that leaves it
 * ample room to grow before a merge could tread on these. */
#define SKIN_TOKEN_CUSTOM_BASE      1000

#define SKIN_TOKEN_TEXT_WIDTH       (SKIN_TOKEN_CUSTOM_BASE + 0)
#define SKIN_TOKEN_VIEWPORT_WIDTH   (SKIN_TOKEN_CUSTOM_BASE + 1)
#define SKIN_TOKEN_VIEWPORT_HEIGHT  (SKIN_TOKEN_CUSTOM_BASE + 2)
#define SKIN_TOKEN_SELECT           (SKIN_TOKEN_CUSTOM_BASE + 3)
#define SKIN_TOKEN_WORD_WRAP        (SKIN_TOKEN_CUSTOM_BASE + 4)
#define SKIN_TOKEN_MATH             (SKIN_TOKEN_CUSTOM_BASE + 5)
#define SKIN_TOKEN_STRLEN           (SKIN_TOKEN_CUSTOM_BASE + 6)
#define SKIN_TOKEN_STRFIND          (SKIN_TOKEN_CUSTOM_BASE + 7)
#define SKIN_TOKEN_PAD              (SKIN_TOKEN_CUSTOM_BASE + 8)
#define SKIN_TOKEN_TEXT_BOX         (SKIN_TOKEN_CUSTOM_BASE + 9)
#define SKIN_TOKEN_LINE_HEIGHT      (SKIN_TOKEN_CUSTOM_BASE + 10)
#define SKIN_TOKEN_VIEWPORT_TEXTSHADOW (SKIN_TOKEN_CUSTOM_BASE + 11)
#define SKIN_TOKEN_LIST_ITEM_KIND   (SKIN_TOKEN_CUSTOM_BASE + 12)
#define SKIN_TOKEN_LIST_ITEM_POSITION (SKIN_TOKEN_CUSTOM_BASE + 13)
#define SKIN_TOKEN_LIST_ITEM_VALUE  (SKIN_TOKEN_CUSTOM_BASE + 14)
#define SKIN_TOKEN_LIST_ITEM_PLAYING (SKIN_TOKEN_CUSTOM_BASE + 15)
#define SKIN_TOKEN_TRIM             (SKIN_TOKEN_CUSTOM_BASE + 16)
#define SKIN_TOKEN_LIST_ITEM_RANGE  (SKIN_TOKEN_CUSTOM_BASE + 17)
#define SKIN_TOKEN_SLEEP_TIMER_PERCENT (SKIN_TOKEN_CUSTOM_BASE + 18)
#define SKIN_TOKEN_LIST_SELECTED_DESC (SKIN_TOKEN_CUSTOM_BASE + 19)
#define SKIN_TOKEN_LIST_ITEM_MARKED (SKIN_TOKEN_CUSTOM_BASE + 20)
#define SKIN_TOKEN_LIST_HEADER      (SKIN_TOKEN_CUSTOM_BASE + 21)
#define SKIN_TOKEN_SPECTRUM_BARS    (SKIN_TOKEN_CUSTOM_BASE + 22)
#define SKIN_TOKEN_LIST_ITEM_ALBUMART (SKIN_TOKEN_CUSTOM_BASE + 23)
#define SKIN_TOKEN_LIST_HAS_ART_ROWS (SKIN_TOKEN_CUSTOM_BASE + 24)
#define SKIN_TOKEN_VLED_BUILDING    (SKIN_TOKEN_CUSTOM_BASE + 25)
#define SKIN_TOKEN_VLED_WORKING     (SKIN_TOKEN_CUSTOM_BASE + 26)
#define SKIN_TOKEN_LOADING_ANIM     (SKIN_TOKEN_CUSTOM_BASE + 27)

/* %LH's fields: what its word names, held in the token's value */
enum browser_header_field {
    BROWSER_HEADER_KIND,        /* no word */
    BROWSER_HEADER_NAME,
    BROWSER_HEADER_ARTIST,
    BROWSER_HEADER_YEAR,
    BROWSER_HEADER_TRACKS,
    BROWSER_HEADER_LENGTH,
    BROWSER_HEADER_DISCS,
    BROWSER_HEADER_ART,
};

#endif /* CUSTOM_TOKENS_H */
