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

/* The upstream enum currently ends at 195. Basing well above that leaves it
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

#endif /* CUSTOM_TOKENS_H */
