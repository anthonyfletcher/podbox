/***************************************************************************
 * This build's extra skin tags, kept out of lib/skin_parser/tag_table.c so
 * that file can track RockBox unchanged. find_tag() falls back to
 * find_custom_tag() (weak-linked there) when a tag isn't in the upstream
 * table.
 * GNU General Public License (version 2+)
 *
 * Registry of this fork's non-upstream skin tags, kept separate so the
 * additions are visible in one place.
 ****************************************************************************/

#include <string.h>
#include "tag_table.h"
#include "custom_tokens.h"

/* Same row layout as legal_tags[] in tag_table.c: param_pos is the name's
 * length including the '\0', and name holds "name\0params". */
#define TAG(type,name,param,flag) {(type),sizeof(name),(name "\0" param),(flag)}
static const struct tag_info custom_tags[] =
{
    TAG(SKIN_TOKEN_VLED_BUILDING,      "lb", "",     SKIN_REFRESH_DYNAMIC),
    TAG(SKIN_TOKEN_VLED_WORKING,       "lw", "",     SKIN_REFRESH_DYNAMIC),
    TAG(SKIN_TOKEN_LOADING_ANIM,       "la", "",     SKIN_REFRESH_DYNAMIC),
    /* The alignment argument is lowercase 's', not 'S', so it accepts '-'.
     * Arguments are positional and the parser rejects '-' on an uppercase type
     * outright, so an uppercase one here would make `gap` unreachable without
     * also writing an alignment. */
    TAG(SKIN_TOKEN_SPECTRUM_BARS,      "Sb", "i|sii",SKIN_REFRESH_SPECTRUM),
    TAG(SKIN_TOKEN_LIST_ITEM_ALBUMART, "La", "|IS",  SKIN_REFRESH_DYNAMIC),

    /* Shadows upstream's %Cl ("[iP][iP][iP][iP]|ss"): the same token with one
     * more optional string, the filter chain (draw/img_filter.h). The token
     * id is unchanged, so parse_albumart_load() still handles it and only
     * grows a clause.
     *
     * The one name here that deliberately collides with an upstream tag.
     * find_tag() consults this table first, which is what makes overriding a
     * tag possible at all rather than only adding to the set. The cost is
     * that a theme using the seventh parameter parses here and is rejected by
     * upstream Rockbox -- true of every tag in this file, and not new. */
    TAG(SKIN_TOKEN_ALBUMART_LOAD, "Cl", "[iP][iP][iP][iP]|sss", 0|NOBREAK),

    /* Shadows upstream's %dr ("[IP][IP][ip][ip]|ss"): the same token with one
     * more optional parameter, opacity 0..15, absent meaning opaque. Optional
     * parameters are positional, so reaching it means writing the two colours
     * as '-' -- parse_drawrectangle() accepts that and keeps the viewport's
     * foreground, which is what a panel tinted by the dynamic-colour palette
     * needs. Only useful inside a %VB viewport; see lcd_blendrect(). */
    TAG(SKIN_TOKEN_DRAWRECTANGLE, "dr", "[IP][IP][ip][ip]|ssi",
        SKIN_REFRESH_STATIC),

    /* %tw/%Vw/%Vh transform or report their arguments and are consumed by an
     * enclosing %if/%sel, whose conditional render path drives the redraw, so
     * flag 0 (inherit) is right: the line's refresh follows its arguments.
     *
     * %sel is different -- its result is drawn directly. Render-time redraw
     * (skin_render.c: needs_update) reads only the top-level tag's OWN flag,
     * NOT its nested arguments, so a directly-drawn flag-0 tag would never mark
     * its line dirty and would redraw only erratically. %ss, the visible tag
     * that also takes a T arg, is DYNAMIC for exactly this reason; match it. */
    TAG(SKIN_TOKEN_TEXT_WIDTH,         "tw", "T|i",    0),
    TAG(SKIN_TOKEN_VIEWPORT_WIDTH,     "Vw", "",       0),
    TAG(SKIN_TOKEN_VIEWPORT_HEIGHT,    "Vh", "",       0),
    TAG(SKIN_TOKEN_SELECT,             "sel","T[ITS]*",SKIN_REFRESH_DYNAMIC),

    /* %wr(n, text): the nth word-wrapped line of text, wrapped to the current
     * viewport width. Drawn directly, so DYNAMIC for the same reason as %sel. */
    TAG(SKIN_TOKEN_WORD_WRAP,          "wr", "I[ITS]", SKIN_REFRESH_DYNAMIC),

    /* String/arithmetic helpers. Flag rationale as in %tw vs %sel: %sl/%sf are
     * measurement fed to a conditional (the conditional drives redraw -> 0);
     * %ma/%pd can be drawn directly, so they carry DYNAMIC to redraw reliably. */
    TAG(SKIN_TOKEN_MATH,               "ma", "[IT]S[IT]", SKIN_REFRESH_DYNAMIC),
    TAG(SKIN_TOKEN_STRLEN,             "sl", "[ITS]",      0),
    TAG(SKIN_TOKEN_STRFIND,            "sf", "[ITS][ITS]", 0),
    TAG(SKIN_TOKEN_PAD,                "pd", "I[ITS]",     SKIN_REFRESH_DYNAMIC),

    /* %wt(text[,align]): draw text word-wrapped and aligned to fill the current
     * viewport, ellipsised on overflow. A drawing tag (renders directly like
     * %Sb), so DYNAMIC to redraw as the text changes. */
    TAG(SKIN_TOKEN_TEXT_BOX,           "wt", "T|ST",       SKIN_REFRESH_DYNAMIC),

    TAG(SKIN_TOKEN_UNKNOWN,            "",   "",      0)   /* terminator */
};
#undef TAG

/* Mirrors search_tag() in tag_table.c: n is the candidate length (name length
 * plus the '\0'); the name is truncated to n-1 characters for the compare. */
static const struct tag_info* search_custom(const char* name, int n)
{
    const struct tag_info* current = custom_tags;
    while (current->param_pos > 1
        && (current->param_pos != n || strncmp(current->name, name, n - 1) != 0))
    {
        current++;
    }
    return (current->param_pos <= 1) ? NULL : current;
}

const struct tag_info* find_custom_tag(const char *name)
{
    const struct tag_info *tag = NULL;
    int i = MAX_TAG_LENGTH;
    while (!tag && i > 1)
    {
        tag = search_custom(name, i);
        i--;
    }
    return tag;
}
