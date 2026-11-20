/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A page's Markdown. A written page is its file; a page nobody wrote is a
 * template -- /.rockbox/notes/Artist.md, Album.md or Name.md when the owner
 * has written one, else the built-in one -- and both are made of the same
 * pieces, which are filled in here before the Markdown back end reads them:
 *
 *   {{albums}}     an artist's albums, or a genre's or some years'
 *   {{tracks}}     an album's tracks, an artist's, or a composer's
 *   {{played}}     the tracks of either played last
 *   {{mentions}}   the pages that name it
 *   {{pages}}      the pages of a folder of /Notes
 *   {{contents}}   the page's own headings
 *   {{track|number=5}}, {{artist}}, {{album}}, {{year}}, {{genre}},
 *   {{composer}}, {{grouping}}, {{comment}}, {{tracks.count}}, {{discs}},
 *   {{length}}, {{plays}}
 *
 * A piece takes properties, `{{albums|sort=-plays|limit=5}}`, the first of
 * them after a colon as well: `{{albums: Name}}` is `{{albums|artist=Name}}`.
 * Every list takes limit, style=inline, heading and empty, and the library's
 * lists sort and show too; a sorted list keeps the NS_LIST_MAX rows that
 * sort first, however many there are. A list piece stands alone on its line
 * and becomes lines of links; one that comes out empty takes the heading
 * just above it with it. A value goes where it is
 * written, so `[{{artist}}]` is a link; one the page has none of is left out,
 * with the " · " beside it. Album and track names come through Trim Titles.
 *
 * A written album page gets the line of its artist and year above it, and
 * its tracks below when it does not list them; a written artist's page its
 * albums when it links none of them. Everything here comes from the database's RAM copy and the names
 * index: nothing but the page and its template is read from the disk.
 *
 * Parts, in order:
 *   - what a page is about
 *   - values and lists
 *   - filling in the pieces
 *   - the page
 ****************************************************************************/

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "lang.h"
#include "string-extra.h"
#include "database/tagcache.h"
#include "database/db_featured.h"   /* guest=true */
#include "database/db_names.h"
#include "metadata/art_name.h"
#include "metadata/tag_trim.h"
#include "metadata/name_index.h"
#include "notes_files.h"
#include "notes_links.h"
#include "notes_source.h"

#define NS_LIST_MAX  256            /* an album's tracks, an artist's albums */
#define NS_VALUE_MAX 192
#define NS_TEMPLATES ROCKBOX_DIR "/notes"
#define NS_DOT       " \xc2\xb7 "   /* the separator values are left out with */

/* ---- what a page is about ---------------------------------------------- */

static char ns_buf[NS_VALUE_MAX], ns_trim[NS_VALUE_MAX];

/* The written page of `kind` about the library's album `album`, or about the
 * album artist whose key is `key` */
static int ns_written(int kind, int album, uint32_t key)
{
    const struct notes_page *best = NULL;
    struct tagcache_album row;
    int i, found = -1, n = notes_page_count();

    if (kind == NOTES_PAGE_ALBUM && !tagcache_album_get(album, &row))
        return -1;
    for (i = 0; i < n; i++)
    {
        const struct notes_page *pg = notes_page_get(i);

        if (!pg || pg->kind != kind || !notes_page_better(pg, best))
            continue;
        /* a page beside the music is its album's folder's */
        if (kind == NOTES_PAGE_ALBUM
            ? (pg->dir ? pg->dir == row.dir_key
                       : notes_page_album(i) == album)
            : (pg->key == key || pg->alt == key || pg->part == key))
        {
            best = pg;
            found = i;
        }
    }
    return found;
}

/* The page's name, its artist part left out of an album's and its
 * qualifier out of a person's */
static void ns_page_title(const struct notes_page *pg, char *buf, size_t size)
{
    char *dash;

    notes_page_name(pg, buf, size);
    if (pg->kind == NOTES_PAGE_ALBUM && !pg->dir
        && (dash = strstr(buf, " - ")))
        memmove(buf, dash + 3, strlen(dash + 3) + 1);
    else
        buf[notes_bare_len(buf, strlen(buf))] = '\0';
}

static void ns_doc(const struct notes_target *t, struct notes_doc *d,
                   const struct name_index *ix)
{
    const struct notes_page *pg;
    struct tagcache_album row;

    d->page = t->page;
    d->album = t->album;
    d->artist_seek = t->artist_seek;
    d->artist = 0;
    d->key = t->key;
    d->title[0] = '\0';
    switch (t->to)
    {
        case NOTES_TO_PAGE:
            pg = notes_page_get(t->page);
            d->kind = pg ? pg->kind : 0;
            break;
        case NOTES_TO_ARTIST:
            d->kind = NOTES_PAGE_PERSON;
            break;
        case NOTES_TO_ALBUM:
        case NOTES_TO_TRACK:
            d->kind = NOTES_PAGE_ALBUM;
            break;
        default:
            d->kind = 0;
            break;
    }

    if (d->kind == NOTES_PAGE_ALBUM && d->album >= 0
        && tagcache_album_get(d->album, &row))
    {
        d->artist = row.artist_art_hash;
        d->artist_seek = row.artist_seek;
        if (d->page < 0)
            d->page = ns_written(NOTES_PAGE_ALBUM, d->album, 0);
    }
    pg = notes_page_get(d->page);
    if (d->kind == NOTES_PAGE_PERSON)
    {
        if (pg && !notes_is_artist(d->artist_seek))
            d->artist_seek = notes_artist_seek(pg->key, pg->alt, 0,
                                               &d->artist);
        else if (notes_is_artist(d->artist_seek))
        {
            if (tagcache_seek_string(tag_albumartist, d->artist_seek, ns_buf,
                                     sizeof(ns_buf)))
                d->artist = art_name_hash(ns_buf);
            if (!pg)
                pg = notes_page_get(d->page = ns_written(NOTES_PAGE_PERSON,
                                                         -1, d->artist));
        }
        if (!d->artist && pg)
            d->artist = pg->key;
    }
    if (pg && !d->key)
        d->key = pg->key;

    if (d->kind == NOTES_PAGE_ALBUM && d->album >= 0)
        notes_album_title(d->album, d->title, sizeof(d->title));
    else if (pg)
        ns_page_title(pg, d->title, sizeof(d->title));
    else if (d->kind == NOTES_PAGE_PERSON && notes_is_artist(d->artist_seek))
        tagcache_seek_string(tag_albumartist, d->artist_seek, d->title,
                             sizeof(d->title));
    else if (ix)
        strlcpy(d->title, name_index_name(ix, d->key) ?: "",
                sizeof(d->title));
}

/* ---- values and lists -------------------------------------------------- */

static bool ns_is(const char *name, size_t n, const char *word)
{
    return n == strlen(word) && !strncasecmp(name, word, n);
}

/* ---- a piece and its properties ---------------------------------------- */

#define NS_PROPS_MAX 8

/* `{{name|key=value|key=value}}`, or `{{name: value|...}}` for its first
 * key; values loosely matched where they are names */
struct ns_prop
{
    const char *key, *val;
    size_t klen, vlen;
};

struct ns_piece
{
    const char *name;
    size_t nlen;
    struct ns_prop prop[NS_PROPS_MAX];
    int nprops;
};

/* The key a piece's value after `:` is */
static const char *ns_first_key(const struct ns_piece *pc)
{
    static const char *const keys[][2] = {
        { "albums", "artist" }, { "tracks", "album" }, { "mentions", "name" },
        { "pages", "folder" }, { "track", "number" },
    };
    unsigned i;

    for (i = 0; i < ARRAYLEN(keys); i++)
        if (ns_is(pc->name, pc->nlen, keys[i][0]))
            return keys[i][1];
    return "";
}

static void ns_trim_run(const char **s, size_t *n)
{
    while (*n && (**s == ' ' || **s == '\t'))
    {
        (*s)++;
        (*n)--;
    }
    while (*n && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t'))
        (*n)--;
}

static void ns_add_prop(struct ns_piece *pc, const char *key, size_t klen,
                        const char *val, size_t vlen)
{
    struct ns_prop *p = &pc->prop[pc->nprops];

    if (pc->nprops == NS_PROPS_MAX)
        return;
    ns_trim_run(&key, &klen);
    ns_trim_run(&val, &vlen);
    p->key = key;
    p->klen = klen;
    p->val = val;
    p->vlen = vlen;
    pc->nprops++;
}

/* The piece at `s` into `pc`, and the bytes it takes; 0 when `s` starts
 * none */
static size_t ns_piece(const char *s, struct ns_piece *pc)
{
    const char *end, *at;

    if (s[0] != '{' || s[1] != '{')
        return 0;
    end = strstr(s + 2, "}}");
    if (!end || memchr(s + 2, '\n', end - s - 2))
        return 0;
    pc->name = s + 2;
    for (at = pc->name; at < end && *at != ':' && *at != '|'; at++)
        ;
    pc->nlen = at - pc->name;
    ns_trim_run(&pc->name, &pc->nlen);
    pc->nprops = 0;
    if (at < end && *at == ':')
    {
        const char *v = at + 1, *first = ns_first_key(pc);

        for (at = v; at < end && *at != '|'; at++)
            ;
        ns_add_prop(pc, first, strlen(first), v, at - v);
    }
    while (at < end && *at == '|')
    {
        const char *k = at + 1, *eq;

        for (at = k; at < end && *at != '|'; at++)
            ;
        eq = memchr(k, '=', at - k);
        ns_add_prop(pc, k, (eq ? eq : at) - k, eq ? eq + 1 : at,
                    eq ? (size_t)(at - eq - 1) : 0);
    }
    return end + 2 - s;
}

/* Property `key`'s value, or NULL */
static const char *ns_get(const struct ns_piece *pc, const char *key,
                          size_t *len)
{
    int i;

    for (i = 0; i < pc->nprops; i++)
        if (ns_is(pc->prop[i].key, pc->prop[i].klen, key))
        {
            *len = pc->prop[i].vlen;
            return pc->prop[i].val;
        }
    return NULL;
}

/* Whether the property `key` is `word`, or a list naming it */
static bool ns_has(const struct ns_piece *pc, const char *key,
                   const char *word)
{
    size_t len, wl = strlen(word);
    const char *v = ns_get(pc, key, &len), *end;

    if (!v)
        return false;
    for (end = v + len; v < end; )
    {
        const char *c = memchr(v, ',', end - v);
        size_t n = (c ? c : end) - v;

        ns_trim_run(&v, &n);
        if (n == wl && !strncasecmp(v, word, n))
            return true;
        v = c ? c + 1 : end;
    }
    return false;
}

/* Property `key` as a number, or `def` */
static long ns_int(const struct ns_piece *pc, const char *key, long def)
{
    size_t len;
    const char *v = ns_get(pc, key, &len);
    long n = 0;
    bool neg = false, any = false;

    if (!v)
        return def;
    if (len && *v == '-')
    {
        neg = true;
        v++;
        len--;
    }
    for (; len && *v >= '0' && *v <= '9'; v++, len--, any = true)
        n = n * 10 + (*v - '0');
    return any ? (neg ? -n : n) : def;
}

/* The sort property: its word without a leading `-` into `word`, and
 * whether the list runs the other way from that word's own order */
static bool ns_sort(const struct ns_piece *pc, char *word, size_t size)
{
    size_t len;
    const char *v = ns_get(pc, "sort", &len);
    bool rev = false;

    word[0] = '\0';
    if (!v)
        return false;
    if (len && *v == '-')
    {
        rev = true;
        v++;
        len--;
    }
    snprintf(word, size, "%.*s", (int)len, v);
    return rev;
}

/* ---- values ------------------------------------------------------------ */

/* "3 plays", or "1 play" */
static void ns_plays(char *buf, size_t size, long n)
{
    if (n == 1)
        strlcpy(buf, str(LANG_NOTES_ONE_PLAY), size);
    else
        snprintf(buf, size, str(LANG_NOTES_N_PLAYS), (int)n);
}

/* An album row's running time in milliseconds */
static long ns_album_ms(int album)
{
    struct tagcache_album row;
    long ms = 0, len;
    int idx;

    if (!tagcache_album_get(album, &row))
        return 0;
    for (idx = row.first; idx <= row.last; idx++)
        if (tagcache_album_of(idx) == album
            && tagcache_entry_numeric(idx, tag_length, &len))
            ms += len;
    return ms;
}

/* An album's running time in seconds */
static long ns_album_secs(int album)
{
    return ns_album_ms(album) / 1000;
}

/* A running time of `s` seconds into `buf` */
static bool ns_secs_text(long s, char *buf, size_t size)
{
    if (s <= 0)
        return false;
    if (s >= 3600)
        snprintf(buf, size, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(buf, size, "%ld:%02ld", s / 60, s % 60);
    return true;
}

/* The album a piece's `album` property names, "Artist - Title" or a title
 * (with `artist` beside it, or else the page's artist's), else the page's:
 * the library's album of that name, else the one a page of that name is
 * about */
static int ns_album_of(const struct ns_piece *pc, const struct notes_doc *d)
{
    static char name[NS_VALUE_MAX * 2 + 16];
    struct notes_target t;
    size_t len, alen;
    const char *v = ns_get(pc, "album", &len);
    const char *a = ns_get(pc, "artist", &alen);
    int album;

    if (!v)
        return d->album;
    len = MIN(len, (size_t)NS_VALUE_MAX - 1);
    if (a)
        snprintf(name, sizeof(name), "%.*s - %.*s",
                 (int)MIN(alen, (size_t)NS_VALUE_MAX - 1), a, (int)len, v);
    else
        snprintf(name, sizeof(name), "%.*s", (int)len, v);
    if ((album = notes_album_named(name, strlen(name), d->artist)) >= 0)
        return album;
    /* the qualifier kept, however long the name */
    strlcat(name, " (album)", sizeof(name));
    return notes_link_find(name, strlen(name), d->artist, -1, &t)
           ? t.album : -1;
}

static void ns_track_label(char *buf, size_t size, int idx);

/* The value piece `pc` of the page into `buf` */
static bool ns_value(const struct notes_doc *d, const struct ns_piece *pc,
                     char *buf, size_t size)
{
    static const struct { const char *name; int tag; } strings[] = {
        { "genre", tag_genre }, { "composer", tag_composer },
        { "grouping", tag_grouping }, { "comment", tag_comment },
    };
    struct tagcache_album row;
    bool album = d->album >= 0 && tagcache_album_get(d->album, &row);
    const char *name = pc->name;
    size_t n = pc->nlen;
    unsigned i;

    buf[0] = '\0';
    if (ns_is(name, n, "artist"))
    {
        const struct notes_page *pg = notes_page_get(d->page);
        char *dash;

        if (notes_is_artist(d->artist_seek)
            && tagcache_seek_string(tag_albumartist, d->artist_seek, buf,
                                    size)
            && strcmp(buf, UNTAGGED))
            return true;
        buf[0] = '\0';
        if (!pg || pg->kind != NOTES_PAGE_ALBUM || pg->dir)
            return false;
        notes_page_name(pg, buf, size);
        if (!(dash = strstr(buf, " - ")))
            return buf[0] = '\0', false;
        *dash = '\0';
        return true;
    }
    if (ns_is(name, n, "album"))
        return d->kind == NOTES_PAGE_ALBUM
               && strlcpy(buf, d->title, size) > 0;
    if (ns_is(name, n, "track"))
    {
        /* {{track|number=5}}: a link to that track */
        int a = ns_album_of(pc, d), idx;
        long want = ns_int(pc, "number", 0), disc = ns_int(pc, "disc", 1);

        if (a < 0 || want <= 0 || !tagcache_album_get(a, &row))
            return false;
        for (idx = row.first; idx <= row.last; idx++)
        {
            long dn = 0, tn = 0;

            if (tagcache_album_of(idx) != a)
                continue;
            tagcache_entry_numeric(idx, tag_discnumber, &dn);
            tagcache_entry_numeric(idx, tag_tracknumber, &tn);
            if (tn == want && MAX(dn, 1) == disc)
            {
                ns_track_label(buf, size, idx);
                return buf[0] != '\0';
            }
        }
        return false;
    }
    if (ns_is(name, n, "plays"))
    {
        /* an album's plays, or the album artist's whole */
        long plays = album ? row.playcount : 0;
        struct tagcache_artist ar;

        if (!album && db_names_is_handle(d->artist_seek))
        {
            if (db_names_artist(db_names_index(d->artist_seek), NULL, &ar))
                plays = ar.playcount;
        }
        else
            for (i = 0; !album && (int)i < tagcache_artist_count(); i++)
                if (tagcache_artist_get(i, &ar) && ar.seek == d->artist_seek)
                    plays = ar.playcount;
        return plays > 0 && snprintf(buf, size, "%ld", plays) > 0;
    }
    if (!album)
        return false;
    if (ns_is(name, n, "year"))
    {
        if (row.year <= 0)
            return false;
        if (ns_has(pc, "format", "decade"))
            snprintf(buf, size, str(LANG_NOTES_DECADE), row.year / 10 * 10);
        else
            snprintf(buf, size, "%d", row.year);
        return true;
    }
    if (ns_is(name, n, "tracks.count"))
        return row.tracks > 0 && snprintf(buf, size, "%d", row.tracks) > 0;
    if (ns_is(name, n, "discs"))
    {
        long most = 0, dn;
        int idx;

        for (idx = row.first; idx <= row.last; idx++)
            if (tagcache_album_of(idx) == d->album
                && tagcache_entry_numeric(idx, tag_discnumber, &dn))
                most = MAX(most, dn);
        return snprintf(buf, size, "%ld", MAX(most, 1)) > 0;
    }
    if (ns_is(name, n, "length"))
    {
        long s = ns_album_secs(d->album);

        if (s <= 0)
            return false;
        if (ns_has(pc, "format", "minutes"))
        {
            snprintf(buf, size, str(LANG_NOTES_MINUTES), (int)((s + 30) / 60));
            return true;
        }
        return ns_secs_text(ns_album_secs(d->album), buf, size);
    }
    for (i = 0; i < ARRAYLEN(strings); i++)
        if (ns_is(name, n, strings[i].name))
            return tagcache_entry_string(row.first, strings[i].tag, buf, size)
                   && buf[0];
    return false;
}

/* ---- lists ------------------------------------------------------------- */

/* The output, which stops short rather than overflows */
struct ns_out
{
    char *buf;
    size_t size, len;
};

static void ns_put(struct ns_out *o, const char *s, size_t n)
{
    if (o->len + n + 1 > o->size)
        n = o->size - o->len - 1;
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

static void ns_puts(struct ns_out *o, const char *s)
{
    ns_put(o, s, strlen(s));
}

/* A name as a link's label: brackets in it would end the link */
static void ns_label(struct ns_out *o, const char *name, const char *qual)
{
    const char *c;

    ns_puts(o, "[");
    for (c = name; *c; c++)
        ns_put(o, *c == '[' ? "(" : *c == ']' ? ")" : c, 1);
    if (qual)
        ns_puts(o, qual);
    ns_puts(o, "]");
}

/* A list's items, made before they are written out: each a line, a
 * heading among them starting `#` */
#define NS_ITEMS_BYTES (12 * 1024)

static struct
{
    char buf[NS_ITEMS_BYTES];
    uint16_t off[NS_LIST_MAX + 16];
    int n, count;               /* lines, and of them items */
    size_t used;
    struct ns_out o;            /* the one being made */
} ni;

static void ns_items_begin(void)
{
    ni.n = ni.count = 0;
    ni.used = 0;
}

static struct ns_out *ns_item(void)
{
    ni.o.buf = ni.buf + ni.used;
    ni.o.size = NS_ITEMS_BYTES - ni.used;
    ni.o.len = 0;
    ni.o.buf[0] = '\0';
    return &ni.o;
}

static void ns_item_done(bool heading)
{
    if (ni.n == (int)ARRAYLEN(ni.off) || ni.used + ni.o.len + 2 > NS_ITEMS_BYTES)
        return;
    ni.off[ni.n++] = ni.used;
    ni.used += ni.o.len + 1;
    if (!heading)
        ni.count++;
}

/* The limit a piece has when it gives none: {{played}}'s */
static long ns_limit_default;

/* Whether the list has room for another item: `limit` or its own cap */
static bool ns_room(const struct ns_piece *pc)
{
    long limit = ns_int(pc, "limit", ns_limit_default);

    return ni.count < NS_LIST_MAX && (limit <= 0 || ni.count < limit);
}

/* A dim note after an item: what `show` asks for, joined */
static void ns_suffix(struct ns_out *o, const char *parts[], int n)
{
    int i, any = 0;

    for (i = 0; i < n; i++)
        if (parts[i] && parts[i][0])
        {
            ns_puts(o, any++ ? NS_DOT : "  *");
            ns_puts(o, parts[i]);
        }
    if (any)
        ns_puts(o, "*");
}

/* Whether the list being made is among a line's words, inline */
static bool ns_in_line;

/* Whether the list is written on one line: among words, or for
 * `style=inline` or `commas` */
static bool ns_one_line(const struct ns_piece *pc)
{
    return ns_in_line || ns_has(pc, "style", "inline")
           || ns_has(pc, "style", "commas");
}

/* Whether `show` names `word`; `def` says whether it does when there is no
 * `show`, which lists on one line never do */
static bool ns_show(const struct ns_piece *pc, const char *word, bool def)
{
    size_t len;

    if (ns_get(pc, "show", &len))
        return ns_has(pc, "show", word);
    return def && !ns_one_line(pc);
}

/* What parts a list's items on one line: `style=commas`'s comma, else a
 * dot */
static const char *ns_joiner(const struct ns_piece *pc)
{
    return ns_has(pc, "style", "commas") ? ", " : NS_DOT;
}

/* The items written out, as the list's properties say: under `heading`, on
 * one line for `style=inline` or `commas`, each a bullet's for `bullets`,
 * `empty`'s text when there are none. The items written. */
static int ns_emit(struct ns_out *o, const struct ns_piece *pc)
{
    size_t hl, el;
    const char *heading = ns_get(pc, "heading", &hl);
    const char *empty = ns_get(pc, "empty", &el);
    bool inline_ = ns_one_line(pc);
    bool bullets = ns_has(pc, "style", "bullets"), first = true;
    int i;

    if (!ni.count && !empty)
        return 0;
    if (heading)
    {
        ns_puts(o, "## ");
        ns_put(o, heading, hl);
        ns_puts(o, "\n");
    }
    if (!ni.count)
    {
        ns_put(o, empty, el);
        ns_puts(o, "\n");
        return 1;
    }
    for (i = 0; i < ni.n; i++)
    {
        const char *line = ni.buf + ni.off[i];

        if (inline_)
        {
            if (line[0] == '#')
                continue;
            if (!first)
                ns_puts(o, ns_joiner(pc));
            first = false;
            ns_puts(o, line);
            continue;
        }
        if (bullets && line[0] != '#')
            ns_puts(o, "- ");
        ns_puts(o, line);
        ns_puts(o, "\n");
    }
    if (inline_)
        ns_puts(o, "\n");
    return ni.count;
}

/* Sorting: rows of (what, key), by key; a name sort keeps its names apart,
 * a row's key the index of its name */
static char ns_names[NS_LIST_MAX][48];

/* Whether row `a` comes after row `b` once the rows are sorted */
static bool ns_after(const int32_t *a, const int32_t *b, bool by_name,
                     bool rev)
{
    int c = by_name ? strcasecmp(ns_names[a[1]], ns_names[b[1]])
                    : (a[1] > b[1]) - (a[1] < b[1]);

    return rev ? c < 0 : c > 0;
}

/* Where a list's next row goes: the next of its NS_LIST_MAX, or once they
 * are full the place of the row that sorts last, when `row` sorts before
 * it; -1 when it would not be shown. For a name sort, `row`'s name is in
 * ns_names[NS_LIST_MAX], and is copied to the place given. */
static char ns_spare[48];

static int ns_slot(int32_t (*rows)[2], int n, const int32_t *row,
                   bool by_name, bool rev)
{
    int32_t probe[2] = { row[0], NS_LIST_MAX };
    int i, worst = 0;

    if (n < NS_LIST_MAX)
    {
        if (by_name)
            strlcpy(ns_names[n], ns_spare, sizeof(ns_names[n]));
        return n;
    }
    for (i = 1; i < n; i++)
        if (ns_after(rows[i], rows[worst], by_name, rev))
            worst = i;
    if (by_name)
    {
        /* the new name compared from the spare, then put in place */
        char keep[48];
        int at = rows[worst][1], c;

        strlcpy(keep, ns_names[at], sizeof(keep));
        c = strcasecmp(ns_spare, keep);
        if (rev ? c <= 0 : c >= 0)
            return -1;
        strlcpy(ns_names[at], ns_spare, sizeof(ns_names[at]));
        return worst;
    }
    probe[1] = row[1];
    return ns_after(rows[worst], probe, false, rev) ? worst : -1;
}

static int ns_by_key(const void *a, const void *b)
{
    const int32_t *x = a, *y = b;

    return x[1] < y[1] ? -1 : x[1] > y[1];
}

static int ns_by_name(const void *a, const void *b)
{
    const int32_t *x = a, *y = b;

    return strcasecmp(ns_names[x[1]], ns_names[y[1]]);
}

static void ns_order(int32_t (*rows)[2], int n, bool by_name, bool rev)
{
    int i;

    qsort(rows, n, sizeof(rows[0]), by_name ? ns_by_name : ns_by_key);
    for (i = 0; rev && i < n / 2; i++)
    {
        int32_t a = rows[i][0], b = rows[i][1];

        rows[i][0] = rows[n - 1 - i][0];
        rows[i][1] = rows[n - 1 - i][1];
        rows[n - 1 - i][0] = a;
        rows[n - 1 - i][1] = b;
    }
}

/* Album row `album`'s title as a link, with its artist for a list that
 * holds more than one artist's */
static void ns_album_label(struct ns_out *o, int album, bool with_artist)
{
    char artist[NS_VALUE_MAX];

    if (!notes_album_title(album, ns_buf, sizeof(ns_buf)))
        return;
    if (with_artist && notes_album_artist(album, artist, sizeof(artist)))
    {
        snprintf(ns_trim, sizeof(ns_trim), "%s - %s", artist, ns_buf);
        ns_label(o, ns_trim, " (album)");
        return;
    }
    ns_label(o, ns_buf, " (album)");
}

/* An album row's place in a list's rows, and whether it is one the artist
 * only features on */
#define NS_ALBUM_MASK 0xffffff
#define NS_GUEST      0x1000000

/* The albums the artist `name` features on, from Featured Artists' table
 * when it has been built, into `out`; how many */
static int ns_guest_albums(const char *name, int32_t *out, int max)
{
    static int32_t ids[NS_LIST_MAX];
    int k, j, n = 0, m;

    if (!name[0] || db_featured_count() <= 0)
        return 0;
    m = db_featured_guest_tracks(name, ids, NS_LIST_MAX);
    for (k = 0; k < m && n < max; k++)
    {
        int a = tagcache_album_of(ids[k]);

        for (j = 0; j < n && out[j] != a; j++)
            ;
        if (a >= 0 && j == n)
            out[n++] = a;
    }
    return n;
}

/* A heading among a list's items */
static void ns_item_heading(const char *text)
{
    struct ns_out *o = ns_item();

    ns_puts(o, "### ");
    ns_puts(o, text);
    ns_item_done(true);
}

/* A split artist's album can be a row under each string naming them, one
 * after another as an album name's rows are; the browser shows them as one
 * album. Whether row `album` follows another of its name that is `by`'s. */
static bool ns_split_dup(int album, long by)
{
    struct tagcache_album a, b;
    int k;

    if (by == -1 || !tagcache_album_get(album, &a))
        return false;
    for (k = album - 1; k >= 0 && tagcache_album_get(k, &b)
                        && b.album_seek == a.album_seek; k--)
        if (notes_album_by(k, by))
            return true;
    return false;
}

/* Row `album` with the rows of its name after it that are `by`'s added in:
 * plays, tracks and the latest play into `row`; with `secs`, its seconds
 * returned, else 0 */
static long ns_split_sum(int album, long by, struct tagcache_album *row,
                         bool secs)
{
    struct tagcache_album b;
    long ms = secs ? ns_album_ms(album) : 0;
    int k;

    for (k = album + 1; by != -1 && tagcache_album_get(k, &b)
                        && b.album_seek == row->album_seek; k++)
        if (notes_album_by(k, by))
        {
            row->playcount += b.playcount;
            row->tracks += b.tracks;
            row->lastplayed = MAX(row->lastplayed, b.lastplayed);
            if (secs)
                ms += ns_album_ms(k);
        }
    return ms / 1000;
}

/* {{albums}}: an artist's albums -- with guest=true the ones they feature
 * on as well -- or those of a genre or some years */
static int ns_albums(const struct ns_piece *pc, const struct notes_doc *d)
{
    static int32_t rows[NS_LIST_MAX][2], guests[NS_LIST_MAX];
    static char name[NS_VALUE_MAX];
    struct tagcache_album row;
    char sort[16], v1[16], v2[24], v3[24], v4[24], v5[24];
    size_t len;
    const char *v;
    uint32_t artist = 0, genre = 0;
    long from = 0, to = 0, by = -1;     /* a split artist's handle */
    bool rev = ns_sort(pc, sort, sizeof(sort)), by_name, desc, mixed = false;
    bool by_genre = ns_has(pc, "group", "genre");
    bool by_decade = ns_has(pc, "group", "decade");
    int i, j, n = 0, ng = 0, decade = -1, count = tagcache_album_count();

    name[0] = '\0';
    if ((v = ns_get(pc, "artist", &len)))
    {
        artist = art_name_hash_n(v, len);
        snprintf(name, sizeof(name), "%.*s", (int)len, v);
        by = notes_artist_seek(artist, 0, 0, NULL);
        if (!db_names_is_handle(by))
            by = -1;
    }
    if ((v = ns_get(pc, "genre", &len)))
        genre = art_name_hash_n(v, len);
    if ((v = ns_get(pc, "year", &len)))
    {
        const char *dash = memchr(v, '-', len);

        from = ns_int(pc, "year", 0);
        to = dash ? atoi(dash + 1) : from;
    }
    if (!artist && !genre && !from)
    {
        artist = d->artist;
        if (db_names_is_handle(d->artist_seek))
            by = d->artist_seek;
        if (!notes_is_artist(d->artist_seek)
            || !tagcache_seek_string(tag_albumartist, d->artist_seek, name,
                                     sizeof(name)))
            name[0] = '\0';
    }
    if (!artist && !genre && !from)
        return 0;
    if (artist && ns_has(pc, "guest", "true"))
        ng = ns_guest_albums(name, guests, NS_LIST_MAX);

    by_name = by_genre || !strcasecmp(sort, "name");
    desc = !strcasecmp(sort, "plays") || !strcasecmp(sort, "played")
           || !strcasecmp(sort, "added");
    for (i = 0; i < count; i++)
    {
        bool guest = false;
        long added = 0;

        int32_t r[2] = { 0, 0 };
        int at;

        if (!tagcache_album_get(i, &row))
            continue;
        if (artist && (by != -1 ? !notes_album_by(i, by)
                                : row.artist_art_hash != artist))
        {
            for (j = 0; j < ng && guests[j] != i; j++)
                ;
            if (j == ng)
                continue;
            guest = true;
        }
        if (!guest && ns_split_dup(i, by))
            continue;
        if (from && (row.year < from || row.year > to))
            continue;
        if (genre && !(tagcache_entry_string(row.first, tag_genre, ns_buf,
                                             sizeof(ns_buf))
                       && art_name_hash(ns_buf) == genre))
            continue;
        r[0] = i | (guest ? NS_GUEST : 0);
        if (by_genre)
        {
            if (!tagcache_entry_string(row.first, tag_genre, ns_spare,
                                       sizeof(ns_spare)))
                ns_spare[0] = '\0';
        }
        else if (by_name)
        {
            if (!notes_album_title(i, ns_spare, sizeof(ns_spare)))
                ns_spare[0] = '\0';
        }
        else if (!strcasecmp(sort, "plays"))
        {
            ns_split_sum(i, guest ? -1 : by, &row, false);
            r[1] = row.playcount;
        }
        else if (!strcasecmp(sort, "played"))
        {
            ns_split_sum(i, guest ? -1 : by, &row, false);
            r[1] = row.lastplayed;
        }
        else if (!strcasecmp(sort, "added"))
        {
            tagcache_entry_numeric(row.first, tag_mtime, &added);
            r[1] = added;
        }
        else
            r[1] = row.year > 0 ? MIN(row.year, 9999) : 9999;
        if ((at = ns_slot(rows, n, r, by_name, rev != desc)) < 0)
            continue;
        rows[at][0] = r[0];
        rows[at][1] = by_name ? at : r[1];
        if (at == n)
            n++;
    }
    ns_order(rows, n, by_name, rev != desc);
    for (i = 1; i < n && !mixed && !artist; i++)
    {
        struct tagcache_album first;

        mixed = tagcache_album_get(rows[0][0] & NS_ALBUM_MASK, &first)
                && tagcache_album_get(rows[i][0] & NS_ALBUM_MASK, &row)
                && row.artist_art_hash != first.artist_art_hash;
    }

    for (i = 0; i < n && ns_room(pc); i++)
    {
        int a = rows[i][0] & NS_ALBUM_MASK;
        bool guest = rows[i][0] & NS_GUEST;
        const char *parts[5] = { NULL, NULL, NULL, NULL, NULL };
        struct ns_out *o;
        long secs;

        tagcache_album_get(a, &row);
        secs = ns_split_sum(a, guest ? -1 : by, &row,
                            ns_show(pc, "length", false));
        if (by_genre && (i == 0 || strcasecmp(ns_names[rows[i][1]],
                                              ns_names[rows[i - 1][1]])))
            ns_item_heading(ns_names[rows[i][1]]);
        else if (by_decade && row.year > 0 && row.year / 10 != decade)
        {
            decade = row.year / 10;
            snprintf(v5, sizeof(v5), str(LANG_NOTES_DECADE), decade * 10);
            ns_item_heading(v5);
        }
        o = ns_item();
        ns_album_label(o, a, artist ? guest : mixed);
        if (ns_show(pc, "year", true) && row.year > 0)
        {
            snprintf(v1, sizeof(v1), "%d", row.year);
            parts[0] = v1;
        }
        if (ns_show(pc, "tracks", false) && row.tracks > 0)
        {
            snprintf(v2, sizeof(v2), str(LANG_N_TRACKS_TITLE), row.tracks);
            parts[1] = v2;
        }
        if (ns_show(pc, "length", false) && ns_secs_text(secs, v3, sizeof(v3)))
            parts[2] = v3;
        if (ns_show(pc, "plays", false) && row.playcount > 0)
        {
            ns_plays(v4, sizeof(v4), row.playcount);
            parts[3] = v4;
        }
        if (guest && ns_show(pc, "year", true))
            parts[4] = str(LANG_NOTES_GUEST);
        ns_suffix(o, parts, 5);
        ns_item_done(false);
    }
    return ni.count;
}

/* Track `idx` as a link, which names its entry: a title alone finds the
 * first album holding it */
static void ns_track_label(char *buf, size_t size, int idx)
{
    struct ns_out o = { buf, size, 0 };
    char qual[24];

    buf[0] = '\0';
    snprintf(qual, sizeof(qual), " (track %d)", idx);
    if (tagcache_entry_string(idx, tag_title, ns_buf, sizeof(ns_buf)))
        ns_label(&o, tag_trim(ns_buf, ns_trim, sizeof(ns_trim)), qual);
}

/* The composer tag ns_composed() read last, and whether it credited */
static long ns_comp_seek;
static bool ns_comp_has;

/* Whether track `idx`'s composer tag credits the name whose key is `key`,
 * among names parted by , ; / & or " and " */
static bool ns_composed(int idx, uint32_t key)
{
    static char c[NS_VALUE_MAX];
    long seek = tagcache_entry_seek(idx, tag_composer);
    const char *p, *e, *and;

    if (seek < 0)
        return false;
    if (seek == ns_comp_seek)
        return ns_comp_has;
    ns_comp_seek = seek;
    ns_comp_has = false;
    if (!tagcache_entry_string(idx, tag_composer, c, sizeof(c)))
        return false;
    for (p = c; *p; p = e)
    {
        size_t n;

        e = p + strcspn(p, ",;/&");
        if ((and = strstr(p, " and ")) && and < e)
            e = and;
        while (p < e && *p == ' ')
            p++;
        for (n = e - p; n && p[n - 1] == ' '; n--)
            ;
        if (n && art_name_hash_n(p, n) == key)
            return ns_comp_has = true;
        e += *e == ' ' ? 5 : *e != '\0';
    }
    return false;
}

/* Whether {{tracks}}'s `number=1,2,4` names track number `tn`, or it has
 * no `number` */
static bool ns_number_wanted(const struct ns_piece *pc, long tn)
{
    size_t len;
    const char *v = ns_get(pc, "number", &len), *end;

    if (!v)
        return true;
    for (end = v + len; v < end; v++)
    {
        while (v < end && (*v == ' ' || *v == ','))
            v++;
        if (v < end && atoi(v) == tn && tn > 0)
            return true;
        while (v < end && *v != ',')
            v++;
    }
    return false;
}

/* {{tracks}}: an album's tracks, an artist's, or a composer's; for
 * `played`, {{played}}, those played, the last played first */
static int ns_tracks(const struct ns_piece *pc, const struct notes_doc *d,
                     bool played)
{
    static int32_t rows[NS_LIST_MAX][2];
    static int16_t disc_of[NS_LIST_MAX], num_of[NS_LIST_MAX];
    static char label[NS_VALUE_MAX * 2];
    struct tagcache_album row;
    char sort[16], v1[16], v2[24];
    size_t len;
    const char *v;
    uint32_t artist = 0, composer = 0;
    long only_disc = ns_int(pc, "disc", 0);
    bool rev = ns_sort(pc, sort, sizeof(sort)), by_name, by_plays, in_order;
    bool numbers, group, by_played, all;
    int a, only, n = 0, i, last_disc = 0, count = tagcache_album_count();
    bool multi = false;
    long first_disc = 0;

    if ((v = ns_get(pc, "artist", &len)))
        artist = art_name_hash_n(v, len);
    if ((v = ns_get(pc, "composer", &len)))
        composer = art_name_hash_n(v, len);
    ns_comp_seek = -1;
    by_name = !strcasecmp(sort, "title");
    by_plays = !strcasecmp(sort, "plays");
    by_played = played || !strcasecmp(sort, "played");
    if (by_played)
        by_name = by_plays = false;
    if (played && !artist && d->kind == NOTES_PAGE_PERSON)
        artist = d->artist;
    in_order = !by_name && !by_plays && !by_played;
    /* numbered unless on one line, as other details are, or bulleted */
    numbers = in_order && (ns_has(pc, "numbers", "true")
                           || (!ns_has(pc, "numbers", "false")
                               && !ns_one_line(pc)
                               && !ns_has(pc, "style", "bullets")));
    group = in_order && ns_has(pc, "group", "disc");
    /* with album=, artist= says whose album it is */
    all = (artist || composer) && !ns_get(pc, "album", &len);
    if (!all)
        artist = 0;
    only = all ? -1 : ns_album_of(pc, d);
    if (!all && only < 0)
        return 0;

    for (a = all ? 0 : only; a < count; a = all ? a + 1 : count)
    {
        int idx;

        if (!tagcache_album_get(a, &row)
            || (artist && row.artist_art_hash != artist))
            continue;
        for (idx = row.first; idx <= row.last; idx++)
        {
            long dn = 0, tn = 0, plays = 0, year;
            int32_t r[2] = { 0, 0 };
            int at;

            if (tagcache_album_of(idx) != a
                || (composer && !ns_composed(idx, composer)))
                continue;
            tagcache_entry_numeric(idx, tag_discnumber, &dn);
            tagcache_entry_numeric(idx, tag_tracknumber, &tn);
            dn = MAX(dn, 1);
            if ((only_disc && dn != only_disc) || !ns_number_wanted(pc, tn))
                continue;

            if (by_name)
            {
                if (!tagcache_entry_string(idx, tag_title, ns_spare,
                                           sizeof(ns_spare)))
                    ns_spare[0] = '\0';
            }
            else if (by_plays)
            {
                tagcache_entry_numeric(idx, tag_playcount, &plays);
                r[1] = plays;
            }
            else if (by_played)
            {
                if (!tagcache_entry_numeric(idx, tag_lastplayed, &plays)
                    || plays <= 0)
                    continue;
                r[1] = plays;
            }
            else
            {
                /* year, disc and number in one key, each held to its
                 * width */
                year = row.year > 0 ? MIN(row.year, 2899) - 1900 : 999;
                r[1] = year * 1000000 + MIN(dn, 999) * 1000
                       + (tn > 0 ? MIN(tn, 999) : 999);
            }
            if ((at = ns_slot(rows, n, r, by_name,
                              rev != (by_plays || by_played))) < 0)
                continue;
            disc_of[at] = dn;
            num_of[at] = tn;
            /* the entry and its place among the rows, through the sort */
            rows[at][0] = idx * NS_LIST_MAX + at;
            rows[at][1] = by_name ? at : r[1];
            if (!first_disc)
                first_disc = dn;
            multi = multi || dn != first_disc;
            if (at == n)
                n++;
        }
    }
    ns_order(rows, n, by_name, rev != (by_plays || by_played));

    for (i = 0; i < n && ns_room(pc); i++)
    {
        int idx = rows[i][0] / NS_LIST_MAX, at = rows[i][0] % NS_LIST_MAX;
        struct ns_out *o;
        const char *parts[3] = { NULL, NULL, NULL };
        long secs = 0, plays = 0;

        ns_track_label(label, sizeof(label), idx);
        if (!label[0])
            continue;
        if (group && multi && disc_of[at] != last_disc)
        {
            o = ns_item();
            ns_puts(o, "### ");
            snprintf(ns_buf, sizeof(ns_buf), str(LANG_NOTES_DISC),
                     disc_of[at]);
            ns_puts(o, ns_buf);
            ns_item_done(true);
        }
        last_disc = disc_of[at];
        o = ns_item();
        if (numbers)
        {
            snprintf(ns_buf, sizeof(ns_buf), "*%d*  ",
                     num_of[at] > 0 ? num_of[at] : i + 1);
            ns_puts(o, ns_buf);
        }
        ns_puts(o, label);
        if (ns_show(pc, "length", false)
            && tagcache_entry_numeric(idx, tag_length, &secs) && secs > 0)
        {
            secs /= 1000;
            snprintf(v1, sizeof(v1), "%ld:%02ld", secs / 60, secs % 60);
            parts[0] = v1;
        }
        if (ns_show(pc, "plays", false)
            && tagcache_entry_numeric(idx, tag_playcount, &plays) && plays > 0)
        {
            ns_plays(v2, sizeof(v2), plays);
            parts[1] = v2;
        }
        if (ns_show(pc, "artist", false)
            && tagcache_entry_string(idx, tag_artist, ns_trim,
                                     sizeof(ns_trim)))
            parts[2] = ns_trim;
        ns_suffix(o, parts, 3);
        ns_item_done(false);
    }
    return ni.count;
}

/* Whether the person page `pg` is about a library album artist */
static bool ns_library_artist(const struct notes_page *pg)
{
    return notes_artist_seek(pg->key, pg->alt, pg->part, NULL) != -1;
}

/* A page as a link back to it: by its whole name, a person's and an
 * album's kind said */
static void ns_page_link(struct ns_out *o, int page)
{
    const struct notes_page *pg = notes_page_get(page);
    const char *path;
    size_t len;

    if (!pg)
        return;
    path = notes_page_path(pg);
    if (pg->kind == NOTES_PAGE_ALBUM && pg->dir)
    {
        /* Beside the music: "Artist - Album", as its folders give it */
        const char *leaf = strrchr(path, '/'), *album = leaf, *artist;

        while (album > path && album[-1] != '/')
            album--;
        for (artist = album - 1; artist > path && artist[-1] != '/'; artist--)
            ;
        if (album - 1 > path)
            snprintf(ns_buf, sizeof(ns_buf), "%.*s - %.*s",
                     (int)(album - 1 - artist), artist,
                     (int)(leaf - album), album);
        else
            snprintf(ns_buf, sizeof(ns_buf), "%.*s", (int)(leaf - album),
                     album);
    }
    else
        notes_page_name(pg, ns_buf, sizeof(ns_buf));
    len = strlen(ns_buf);
    ns_label(o, ns_buf, pg->kind == NOTES_PAGE_PERSON
                        && notes_bare_len(ns_buf, len) == len
                        ? " (person)" : NULL);
}

/* Whether page `pg` passes the mentions' `filter` */
static bool ns_kind_ok(const struct ns_piece *pc, const struct notes_page *pg)
{
    size_t len;

    if (!ns_get(pc, "filter", &len))
        return true;
    return (ns_has(pc, "filter", "albums") && pg->kind == NOTES_PAGE_ALBUM)
           || (ns_has(pc, "filter", "people") && pg->kind == NOTES_PAGE_PERSON)
           || (ns_has(pc, "filter", "artists")
               && pg->kind == NOTES_PAGE_PERSON && ns_library_artist(pg))
           || (ns_has(pc, "filter", "other") && pg->kind == NOTES_PAGE_OTHER);
}

/* The page the names index's file `file` is, or -1: by its number in the
 * page index while that is still the same file, else by its path's hash */
static int ns_index_page(const struct name_index *ix, int file)
{
    const struct name_file *f = &ix->files[file];
    const struct notes_page *pg = notes_page_get(f->page);
    int p, pages = notes_page_count();

    if (pg && name_index_path_hash(notes_page_path(pg)) == f->hash)
        return f->page;
    for (p = 0; p < pages; p++)
        if ((pg = notes_page_get(p))
            && name_index_path_hash(notes_page_path(pg)) == f->hash)
            return p;
    return -1;
}

/* {{mentions}}: the pages that name any of the keys, the page itself left
 * out, each once */
static int ns_mentions(const struct ns_piece *pc, const struct name_index *ix,
                       const uint32_t *keys, int nkeys, int self, int kind)
{
    bool headings = ns_has(pc, "headings", "true");
    int i, j, k, p, last = -1;

    if (!ix)
        return 0;
    for (i = 0; i < ix->count && ns_room(pc); i++)
    {
        const struct name_mention *m = &ix->m[i];
        const struct notes_page *pg;
        struct ns_out *o;

        for (k = 0; k < nkeys && (!keys[k] || keys[k] != m->key); k++)
            ;
        if (k == nkeys || m->file == last || m->file >= ix->nfiles)
            continue;
        last = m->file;
        p = ns_index_page(ix, m->file);
        if (p < 0 || p == self)
            continue;
        /* once each, though named under two keys */
        for (j = 0; j < i; j++)
            if (ix->m[j].file == m->file)
            {
                for (k = 0; k < nkeys && keys[k] != ix->m[j].key; k++)
                    ;
                if (k < nkeys)
                    break;
            }
        if (j < i)
            continue;
        if (!(pg = notes_page_get(p)) || !ns_kind_ok(pc, pg)
            || (kind && pg->kind != kind))
            continue;
        o = ns_item();
        ns_page_link(o, p);
        if (headings && m->section != NAME_NONE
            && m->section < ix->nstrings)
        {
            const char *parts[1] = { ix->strings + ix->str_off[m->section] };

            ns_suffix(o, parts, 1);
        }
        ns_item_done(false);
    }
    return ni.count;
}

/* {{pages|folder=Studios}}: the pages of a folder of /Notes, by name */
static int ns_pages(const struct ns_piece *pc)
{
    static int32_t rows[NS_LIST_MAX][2];
    size_t len;
    const char *v = ns_get(pc, "folder", &len);
    int kind = NOTES_PAGE_OTHER, i, n = 0, count = notes_page_count();
    uint32_t key;

    if (!v)
        return 0;
    if (len == 6 && !strncasecmp(v, "People", 6))
        kind = NOTES_PAGE_PERSON;
    else if (len == 6 && !strncasecmp(v, "Albums", 6))
        kind = NOTES_PAGE_ALBUM;
    key = art_name_hash_n(v, len);
    for (i = 0; i < count; i++)
    {
        const struct notes_page *pg = notes_page_get(i);
        int32_t r[2] = { i, 0 };
        int at;

        if (!pg || pg->kind != kind || pg->dir
            || (kind == NOTES_PAGE_OTHER && pg->by != key))
            continue;
        notes_page_name(pg, ns_spare, sizeof(ns_spare));
        if ((at = ns_slot(rows, n, r, true, false)) < 0)
            continue;
        rows[at][0] = i;
        rows[at][1] = at;
        if (at == n)
            n++;
    }
    ns_order(rows, n, true, false);
    for (i = 0; i < n && ns_room(pc); i++)
    {
        ns_page_link(ns_item(), rows[i][0]);
        ns_item_done(false);
    }
    return ni.count;
}

/* ---- filling in the pieces --------------------------------------------- */

struct ns_state
{
    const struct notes_doc *d;
    const struct name_index *ix;
    struct ns_out o;
    long head;                  /* where the heading just written starts */
    const char *text;           /* the page as written, for {{contents}} */
};

/* {{contents}}: the headings of the page as written, each a link to its
 * place */
static int ns_contents(const struct ns_piece *pc, const char *text)
{
    const char *s;

    for (s = text; s && *s && ns_room(pc); )
    {
        const char *e = strchr(s, '\n'), *h = s;
        size_t n = e ? (size_t)(e - s) : strlen(s);

        if (*h == '#')
        {
            while (h < s + n && *h == '#')
                h++;
            if (h < s + n && *h == ' ')
            {
                size_t len = s + n - h;

                ns_trim_run(&h, &len);
                while (len && h[len - 1] == '\r')
                    len--;
                snprintf(ns_buf, sizeof(ns_buf), "%.*s", (int)len, h);
                if (ns_buf[0])
                {
                    ns_label(ns_item(), ns_buf, " (section)");
                    ns_item_done(false);
                }
            }
        }
        s = e ? e + 1 : NULL;
    }
    return ni.count;
}

/* The mentions of `keys`, under a heading a kind of page for group=kind */
static void ns_mentions_grouped(const struct ns_piece *pc,
                                const struct name_index *ix,
                                const uint32_t *keys, int self)
{
    static const int kinds[][2] = {
        { NOTES_PAGE_ALBUM, LANG_ALBUMS },
        { NOTES_PAGE_PERSON, LANG_NOTES_PEOPLE },
        { NOTES_PAGE_OTHER, LANG_NOTES_OTHER },
    };
    unsigned k;

    if (!ns_has(pc, "group", "kind"))
    {
        ns_mentions(pc, ix, keys, 5, self, 0);
        return;
    }
    for (k = 0; k < ARRAYLEN(kinds); k++)
    {
        int lines = ni.n, items = ni.count;
        size_t used = ni.used;

        ns_item_heading(str(kinds[k][1]));
        ns_mentions(pc, ix, keys, 5, self, kinds[k][0]);
        if (ni.count == items)
        {
            ni.n = lines;           /* nothing of this kind: no heading */
            ni.used = used;
        }
    }
}

/* The list piece `pc`'s items made; whether it is one */
static bool ns_list_items(struct ns_state *st, const struct ns_piece *pc)
{
    const struct notes_doc *d = st->d;
    size_t len;
    const char *v;

    ns_items_begin();
    if (ns_is(pc->name, pc->nlen, "albums"))
        ns_albums(pc, d);
    else if (ns_is(pc->name, pc->nlen, "tracks"))
        ns_tracks(pc, d, false);
    else if (ns_is(pc->name, pc->nlen, "played"))
    {
        ns_limit_default = 5;
        ns_tracks(pc, d, true);
        ns_limit_default = 0;
    }
    else if (ns_is(pc->name, pc->nlen, "pages"))
        ns_pages(pc);
    else if (ns_is(pc->name, pc->nlen, "contents"))
        ns_contents(pc, st->text);
    else if (ns_is(pc->name, pc->nlen, "mentions"))
    {
        const struct notes_page *pg = notes_page_get(d->page);
        uint32_t keys[5] = { d->key, d->artist, 0, 0, 0 };

        if ((v = ns_get(pc, "name", &len)))
        {
            keys[0] = art_name_hash_n(v, len);
            keys[1] = art_name_alt_hash_n(v, len);
        }
        else
        {
            if (pg)
            {
                keys[2] = pg->alt;
                keys[3] = pg->part;
            }
            if (d->kind == NOTES_PAGE_ALBUM)
            {
                keys[1] = 0;            /* the artist is not the album */
                keys[4] = art_name_hash(d->title);
            }
        }
        ns_mentions_grouped(pc, st->ix, keys, v ? -1 : d->page);
    }
    else
        return false;
    return true;
}

/* The list piece `pc` on a line of its own; whether it is one */
static bool ns_list(struct ns_state *st, const struct ns_piece *pc)
{
    size_t before = st->o.len;

    if (!ns_list_items(st, pc))
        return false;
    /* An empty list takes the heading just above it with it */
    if (!ns_emit(&st->o, pc) && st->head >= 0 && st->o.len == before)
    {
        st->o.len = st->head;
        st->o.buf[st->o.len] = '\0';
    }
    st->head = -1;
    return true;
}

/* A line with value pieces in it, `n` bytes at `s`: filled in, with what
 * came out empty left out along with its separator */
static void ns_values(struct ns_state *st, const char *s, size_t n)
{
    static char line[4096], value[NS_VALUE_MAX * 2];
    struct ns_out lo = { line, sizeof(line), 0 };
    const char *end, *seg;
    bool first = true;

    /* a file written on Windows ends its lines "\r\n" */
    while (n && s[n - 1] == '\r')
        n--;
    end = s + n;
    /* written text: a heading above keeps it, though a list under it is
     * empty */
    st->head = -1;
    while (s < end)
    {
        struct ns_piece pc;
        size_t used = ns_piece(s, &pc);

        if (used && s + used <= end)
        {
            int i, k = 0;

            /* A list among words is written on their line, inline */
            ns_in_line = true;
            if (ns_list_items(st, &pc))
            {
                ns_in_line = false;
                for (i = 0; i < ni.n; i++)
                    if (ni.buf[ni.off[i]] != '#')
                    {
                        if (k++)
                            ns_puts(&lo, ns_joiner(&pc));
                        ns_puts(&lo, ni.buf + ni.off[i]);
                    }
            }
            else if (ns_value(st->d, &pc, value, sizeof(value)))
                ns_puts(&lo, value);
            ns_in_line = false;
            s += used;
        }
        else
            ns_put(&lo, s++, 1);
    }

    /* Between separators: a part that is empty, or an empty link, goes */
    for (seg = line; seg <= line + lo.len; )
    {
        const char *next = strstr(seg, NS_DOT);
        size_t len = next ? (size_t)(next - seg) : strlen(seg);
        const char *a = seg, *b = seg + len;

        while (a < b && *a == ' ')
            a++;
        while (b > a && b[-1] == ' ')
            b--;
        if (b > a && !(b - a == 2 && a[0] == '[' && a[1] == ']'))
        {
            if (!first)
                ns_puts(&st->o, NS_DOT);
            ns_put(&st->o, seg, len);
            first = false;
        }
        if (!next)
            break;
        seg = next + strlen(NS_DOT);
    }
    /* a line that came out empty goes, leaving no gap */
    if (!first)
        ns_puts(&st->o, "\n");
}

static bool ns_has_piece(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i + 1 < n; i++)
        if (s[i] == '{' && s[i + 1] == '{')
            return true;
    return false;
}

static void ns_expand(struct ns_state *st, const char *text)
{
    const char *s = text;

    while (*s)
    {
        const char *e = strchr(s, '\n'), *p = s;
        size_t n = e ? (size_t)(e - s) : strlen(s), used;
        struct ns_piece pc;

        while (p < s + n && (*p == ' ' || *p == '\t'))
            p++;
        used = ns_piece(p, &pc);
        if (used && p + used <= s + n)
        {
            const char *q = p + used;

            while (q < s + n && (*q == ' ' || *q == '\t' || *q == '\r'))
                q++;
            if (q == s + n && ns_list(st, &pc))
                goto next;
        }
        if (ns_has_piece(s, n))
            ns_values(st, s, n);
        else
        {
            if (*p == '#')
                st->head = st->o.len;
            else if (p < s + n && *p != '\r')
                st->head = -1;
            ns_put(&st->o, s, n);
            ns_puts(&st->o, "\n");
        }
      next:
        s += n + (e != NULL);
    }
}

/* A heading's text past the track number in front of it -- "5. Pink Up",
 * "2-03 Pink Up" -- or all of it */
static const char *ns_numbered(const char *s)
{
    const char *p = s;

    while (*p >= '0' && *p <= '9')
        p++;
    if (p == s)
        return s;
    if ((*p == '-' || *p == '.') && p[1] >= '0' && p[1] <= '9')
        for (p++; *p >= '0' && *p <= '9'; p++)
            ;
    while (*p == '.' || *p == ')' || *p == '-' || *p == ':' || *p == ' ')
        p++;
    return *p ? p : s;
}

/* Whether the written page `text` has a section for one of the tracks of
 * album `album`: a heading named for its title, as the screen opens a
 * track's place by */
static bool ns_track_sections(const char *text, int album)
{
    static uint32_t keys[NS_LIST_MAX];
    struct tagcache_album row;
    const char *s;
    int idx, i, n = 0;

    if (!tagcache_album_get(album, &row))
        return false;
    for (idx = row.first; idx <= row.last && n < NS_LIST_MAX; idx++)
        if (tagcache_album_of(idx) == album
            && tagcache_entry_string(idx, tag_title, ns_buf, sizeof(ns_buf)))
            keys[n++] = art_name_hash(ns_buf);
    for (s = text; *s; s++)
    {
        size_t len = 0;
        uint32_t key;

        if (*s != '#' || (s > text && s[-1] != '\n'))
            continue;
        while (*s == '#' || *s == ' ')
            s++;
        if (!*s)
            break;
        while (s[len] && s[len] != '\n' && s[len] != '\r'
               && len + 1 < sizeof(ns_trim))
            len++;
        memcpy(ns_trim, s, len);
        ns_trim[len] = '\0';
        key = art_name_hash(ns_numbered(ns_trim));
        for (i = 0; key && i < n; i++)
            if (keys[i] == key)
                return true;
        s += len - (len > 0);
    }
    return false;
}

/* Whether `text` uses the list piece `name`, with properties or without */
static bool ns_uses(const char *text, const char *name)
{
    size_t n = strlen(name);
    const char *s;

    for (s = strstr(text, "{{"); s; s = strstr(s + 2, "{{"))
    {
        const char *p = s + 2;

        while (*p == ' ')
            p++;
        if (!strncasecmp(p, name, n))
        {
            for (p += n; *p == ' '; p++)
                ;
            if (*p == '}' || *p == ':' || *p == '|')
                return true;
        }
    }
    return false;
}

/* Whether `text` links to the name whose key is `key` */
static bool ns_links_to(const char *text, uint32_t key)
{
    const char *s, *e;

    for (s = strchr(text, '['); s; s = strchr(s + 1, '['))
    {
        size_t len;

        for (e = s + 1; *e && *e != ']' && *e != '\n'; e++)
            ;
        if (*e != ']')
            continue;
        len = notes_bare_len(s + 1, e - s - 1);
        if (len && art_name_hash_n(s + 1, len) == key)
            return true;
    }
    return false;
}

/* Whether `text` links to an album of the library album artist at `seek`,
 * whose key is `artist` */
static bool ns_links_album_of(const char *text, long seek, uint32_t artist)
{
    struct notes_target t;
    const char *s, *e;

    for (s = strchr(text, '['); s; s = strchr(s + 1, '['))
    {
        for (e = s + 1; *e && *e != ']' && *e != '\n'; e++)
            ;
        if (*e == ']'
            && notes_link_find(s + 1, e - s - 1, artist, -1, &t)
            && (t.to == NOTES_TO_ALBUM || t.to == NOTES_TO_PAGE)
            && notes_album_by(t.album, seek))
            return true;
    }
    return false;
}

/* ---- the page ---------------------------------------------------------- */

/* Whether the line at `s` is `c` three times, spaces aside */
static bool ns_rule_line(const char *s, char c)
{
    int n = 0;

    for (; *s && *s != '\n'; s++)
        if (*s == c)
            n++;
        else if (*s != ' ' && *s != '\r')
            return false;
    return n == 3;
}

/* Whether the front matter line at `s` is YAML: blank, indented, a list's
 * or a comment's, or a key of at most 32 bytes and its colon */
static bool ns_yaml_line(const char *s)
{
    int n;

    if (*s == '\n' || *s == '\r' || !*s || strchr(" \t-#.", *s))
        return true;
    for (n = 0; n < 32; n++, s++)
    {
        unsigned char c = *s;

        if (!((c >= '0' && c <= '9')
              || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')
              || c >= 0x80 || (c && strchr("_- .\"'", c))))
            break;
    }
    return *s == ':';
}

/* `buf` less its front matter and its comments, as the Markdown reader
 * (ts_text.c) leaves them out, so the pieces, the headings and the links in
 * them are not counted either. Front matter: a line of exactly --- opening
 * it, to the next --- or ... line, for as long as its lines are YAML. A
 * comment: <!-- to -->, outside code; one that is all of its line takes
 * the line. */
static void ns_strip(char *buf)
{
    char *w = buf, *r = buf;
    bool fence = false, code = false;

    if (!strncmp(r, "\xef\xbb\xbf", 3))
        r += 3;
    if (!strncmp(r, "---\n", 4) || !strncmp(r, "---\r\n", 5))
    {
        r = strchr(r, '\n') + 1;
        while (*r && !ns_rule_line(r, '-') && !ns_rule_line(r, '.')
               && ns_yaml_line(r))
            r = strchr(r, '\n') ? strchr(r, '\n') + 1 : r + strlen(r);
        if (*r && (ns_rule_line(r, '-') || ns_rule_line(r, '.')))
            r = strchr(r, '\n') ? strchr(r, '\n') + 1 : r + strlen(r);
    }
    while (*r)
    {
        char *end;
        bool bol = w == buf || w[-1] == '\n';

        if (*r == '`')
        {
            const char *p = w;
            int run = 0;

            while (r[run] == '`')
                run++;

            /* a fence opens with three or more at a line's start, spaces
             * before them aside */
            while (p > buf && p[-1] == ' ')
                p--;
            if (run >= 3 && (p == buf || p[-1] == '\n'))
                fence = !fence;
            else if (!fence)
                code = !code;
            while (run--)
                *w++ = *r++;
            continue;
        }
        if (*r == '\n')
            code = false;
        if (fence || code || strncmp(r, "<!--", 4)
            || !(end = strstr(r + 4, "-->")))
        {
            *w++ = *r++;
            continue;
        }
        r = end + 3;
        if (bol && (*r == '\n' || (r[0] == '\r' && r[1] == '\n')))
            r += *r == '\r' ? 2 : 1;
    }
    *w = '\0';
}

/* The file `path` into `buf`, NUL ended and ns_strip()ped; its length, or
 * -1 */
static long ns_read(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    long got;

    if (fd < 0)
        return -1;
    got = read(fd, buf, size - 1);
    close(fd);
    if (got < 0)
        return -1;
    buf[got] = '\0';
    ns_strip(buf);
    return strlen(buf);
}

/* The template `name`: the owner's in NS_TEMPLATES, else the built-in one */
static void ns_template(const char *name, char *buf, size_t size)
{
    char path[MAX_PATH];

    snprintf(path, sizeof(path), NS_TEMPLATES "/%s.md", name);
    if (ns_read(path, buf, size) >= 0)
        return;
    if (!strcmp(name, "Album"))
        snprintf(buf, size,
                 "[{{artist}}]" NS_DOT "{{year}}" NS_DOT "{{genre}}\n\n"
                 "## %s\n{{tracks|group=disc}}\n\n## %s\n{{mentions}}\n",
                 str(LANG_TRACKS), str(LANG_NOTES_MENTIONED_IN));
    else if (!strcmp(name, "Artist"))
        snprintf(buf, size,
                 "## %s\n{{albums|guest=true}}\n\n## %s\n{{mentions}}\n",
                 str(LANG_ALBUMS), str(LANG_NOTES_MENTIONED_IN));
    else
        snprintf(buf, size, "## %s\n{{mentions}}\n",
                 str(LANG_NOTES_MENTIONED_IN));
}

bool notes_library_waiting(void)
{
    return !tagcache_is_in_ram()
           && (tagcache_is_busy() || tagcache_is_loading());
}

size_t notes_source(const struct notes_target *t, struct notes_doc *d,
                    const struct name_index *ix, char *out, size_t size,
                    char *scratch, size_t scratch_size)
{
    struct ns_state st = { d, ix, { out, size, 0 }, -1, scratch };
    const struct notes_page *pg;
    char tail[64];

    out[0] = '\0';
    if (t->to == NOTES_TO_NONE)
        return 0;
    ns_doc(t, d, ix);
    d->waiting = notes_library_waiting();
    if (d->waiting)
    {
        ns_puts(&st.o, "*");
        ns_puts(&st.o, str(LANG_NOTES_LIBRARY_LOADING));
        ns_puts(&st.o, "*\n\n");
    }
    pg = notes_page_get(d->page);
    if (!pg)
    {
        ns_template(d->kind == NOTES_PAGE_ALBUM ? "Album"
                    : d->kind == NOTES_PAGE_PERSON ? "Artist" : "Name",
                    scratch, scratch_size);
        ns_expand(&st, scratch);
        return st.o.len;
    }
    if (ns_read(notes_page_path(pg), scratch, scratch_size) < 0)
        return 0;
    if (d->kind == NOTES_PAGE_ALBUM && !ns_uses(scratch, "artist")
        && !(d->artist && ns_links_to(scratch, d->artist)))
        ns_expand(&st, "[{{artist}}]" NS_DOT "{{year}}\n\n");
    st.head = -1;
    ns_expand(&st, scratch);
    tail[0] = '\0';
    if (d->kind == NOTES_PAGE_ALBUM && d->album >= 0
        && !ns_uses(scratch, "tracks")
        && !ns_track_sections(scratch, d->album))
        snprintf(tail, sizeof(tail), "\n## %s\n{{tracks|group=disc}}\n",
                 str(LANG_TRACKS));
    else if (d->kind == NOTES_PAGE_PERSON && notes_is_artist(d->artist_seek)
             && !ns_uses(scratch, "albums")
             && !ns_links_album_of(scratch, d->artist_seek, d->artist))
        snprintf(tail, sizeof(tail), "\n## %s\n{{albums|guest=true}}\n",
                 str(LANG_ALBUMS));
    ns_expand(&st, tail);
    return st.o.len;
}
