/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Back end for loose markup: HTML and XHTML read directly from the file.
 ****************************************************************************/

#include "ts_internal.h"

static int open_markup(ts_ctx *c, ts_tag_profile p, bs_stream **out)
{
    ts_charset cs = ts_charset_decl(c->head, c->hlen, c->detected);
    bs_stream *st = bs_file_stream(c->arena, c->io, 0, -1);

    if (c->cfg->charset != TS_CS_AUTO) cs = c->cfg->charset;
    if (!st) return BS_ERR_NOMEM;

    st = ts_charset_stream(c->arena, st, cs);
    if (!st) return BS_ERR_NOMEM;
    st = ts_tags_stream(c->arena, st, p, c->cfg);
    if (!st) return BS_ERR_NOMEM;

    c->detected = cs;
    *out = st;
    return BS_OK;
}

int ts_open_html(ts_ctx *c, bs_stream **out)
{
    return open_markup(c, TS_TAGS_HTML, out);
}

int ts_open_fb2(ts_ctx *c, bs_stream **out)
{
    /* FB2 is XML; a zipped .fb2.zip arrives at the container back end
     * instead, and shows no pictures. */
    int rc = open_markup(c, TS_TAGS_FB2, out);

    if (rc == BS_OK) ts_tags_pictures(*out, 1);
    return rc;
}
