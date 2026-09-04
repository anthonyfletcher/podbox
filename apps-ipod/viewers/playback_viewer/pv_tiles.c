/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * What Spun's rows are made of: the tile schedule, expanded into cards and
 * resolved against the statistics model.
 *
 * Parts: the tile ids; the string pool every formatted figure comes out of;
 * the section expansion, which is where a row's widths and colours are
 * decided; the week scan, the one figure the day array cannot answer; then
 * the resolver, one case per tile.
 *
 * This is the whole of the join between the engine and the data.
 * draw/card_paint.c knows nothing about a playback log, and nothing below
 * knows how a card is laid out -- a tile says what it shows and the painter
 * decides where it goes.
 *
 * .specifications/spun-tiles.md is the schedule; every id below is a row of
 * one of its tables.
 ****************************************************************************/

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <string-extra.h>
#include "config.h"
#include "draw/card_row.h"
#include "draw/card_paint.h"
#include "draw/card_text.h"
#include "pv_log.h"
#include "pv_names.h"
#include "pv_stats.h"
#include "pv_badges.h"
#include "settings/settings.h"
#include "pv_tiles.h"

/* The wall of badges is the longest section there is -- one card per badge
 * plus one sub-card each -- and it is what sizes this. A section that would
 * overrun it stops early rather than growing the array: the tail of a
 * two-hundred-card row is not somewhere anyone arrives. */
#define TILE_MAX   512

/* How many rows a top-N section can show, and how many the two skip lists do.
 * The schedule asks for ten and five; the first is a setting now, and this is
 * the ceiling the arrays are cut to. */
#define TOP_N       25
#define SKIP_N       5

/* Badges announced in one pass. Everything unlocked at once carries the same
 * timestamp -- the log can say a badge is earned now, never when it was
 * crossed -- so "the latest few" means the tail of the table. The rest are on
 * the wall, dated, and the first tile says how many. */
#define CROWN_MAX    5

/* The wall, at its widest. Only used to hold the order the rows are emitted
 * in, so it costs two bytes a badge. */
#define PV_BADGE_MAX 256

/* spun_badge_order, in the order settings_list.c names them. */
enum { PV_BADGE_ORDER_TABLE = 0, PV_BADGE_ORDER_EARNED,
       PV_BADGE_ORDER_RECENT, PV_BADGE_ORDER_CLOSEST };

/* Glyphs of the theme's icon font, which is what every icon tag in the
 * schedule draws.
 *
 * Trap: the font ships as /.rockbox/fonts/24x24-icons.fnt but its table does
 * not ship anywhere -- the face is generated from a folder of PNGs whose
 * filenames carry the codepoints, and that folder is not in this tree. A
 * wrong entry here draws the wrong picture rather than failing, so check one
 * against the generator's folder rather than against the screen.
 *
 * It also arrives inside the theme rather than beside the binary:
 * bundle-theme.sh copies Scrim's fonts folder wholesale, so a theme that
 * stops using this face takes Spun's icons with it. */

/* Glyphs of 24-spun-badges.fnt, which the badge tiles use for their plate.
 *
 * This face is ours -- apps-ipod/fonts/ -- so unlike the theme's icon font
 * the mapping is in the tree beside it: .build/fonts/mkbadges.py lists the
 * same order, and the codepoints run from 0x21 with no gaps.
 *
 * Badges only. Every other tile's plate holds a number or the card has none,
 * and spreading an icon across the whole schedule reads as decoration rather
 * than as a plate saying what its card is. */
#define ICON_AWARD        0x21
#define ICON_TROPHY       0x22
#define ICON_STARS        0x23
#define ICON_MEDAL        0x24
#define ICON_SHIELD       0x25
#define ICON_LICENSE      0x26
#define ICON_CHOICE       0x27
#define ICON_CROWN        0x28
#define ICON_DIAMOND      0x29
#define ICON_CELEBRATION  0x2A
#define ICON_CHEER        0x2B
#define ICON_HEART        0x2C
#define ICON_STARSHINE    0x2D

/* The metal a badge wears, once it is earned.
 *
 * Trap: a locked badge must NOT show this. The tier is part of what a badge
 * is worth, and pv_badges.h keeps it hidden until the badge is won -- an icon
 * that leaks it undoes that from the outside. Anything not yet earned wears
 * the same neutral award instead. */
static unsigned short tier_icon(int tier)
{
    switch (tier)
    {
    case PV_TIER_GOLD:  return ICON_TROPHY;
    case PV_TIER_ICE:   return ICON_DIAMOND;
    case PV_TIER_VIO:   return ICON_STARS;
    case PV_TIER_EMBER: return ICON_SHIELD;
    case PV_TIER_OMEGA: return ICON_CROWN;
    case PV_TIER_PINK:  return ICON_HEART;
    default:            return ICON_AWARD;
    }
}

enum tile_id
{
    T_NONE = 0,
    /* In numbers */
    T_YEARMINS, T_YEARPLAYS, T_YEARTRACKS, T_YEARSKIPPED, T_YEARTIME,
    T_CHART, T_NIGHTPLAYS,
    /* Week by week */
    T_ACTIVEWEEKS, T_LONGESTDAY, T_STREAK, T_SUBSTREAK, T_CALTITLE,
    T_CALWEEK, T_SUBWEEK1, T_SUBWEEK2,
    /* Top artists, songs, albums */
    T_TOPARTIST, T_SUBARTIST1, T_SUBARTIST2,
    T_TOPSONG, T_SUBSONG1, T_SUBSONG2,
    T_TOPALBUM, T_SUBALBUM1, T_SUBALBUM2,
    /* Skips */
    T_SKIPPERCENT, T_TOPSKIP, T_SUBSKIP,
    T_SKIPNEVER, T_TOPLOYAL, T_SUBLOYAL,
    /* Badges */
    T_NEWCOUNT, T_NEWBADGE, T_SUBNEWBADGE, T_BADGE, T_SUBBADGE
};

/* The row, as the core wants it plus what the resolver needs to answer for
 * each card. 'arg' is the repeat index -- which week, which rank, which
 * badge -- and is the whole of what one template needs to render week 1 and
 * week 52. */
static unsigned char tile_id[TILE_MAX];
static short         tile_arg[TILE_MAX];
static short         tile_w[TILE_MAX];
static unsigned char tile_flags[TILE_MAX];
/* The colour a card was assigned before any picture had a say, and how far
 * into a run it sits. Kept apart rather than as one finished colour, because
 * a derived tint replaces the first and is then stepped by the second -- tint
 * an already-stepped colour and a sub-card is stepped twice. */
static unsigned      tile_root[TILE_MAX];
static unsigned char tile_depth[TILE_MAX];
/* The picture this card's colour follows. A sub-card carries its parent's, so
 * an open run stays in the family the picture put it in. */
static unsigned      tile_art[TILE_MAX];
static int           tile_n;

/* Where in the palette this section starts.
 *
 * Without it every section opens on the same colour and reads as the same
 * page with different words on it. Seven is coprime with twelve, so the eight
 * sections all start somewhere different. */
static int           tile_hue0;

static pv_tint_fn tint_fn;

void pv_tiles_set_tint(pv_tint_fn fn)
{
    tint_fn = fn;
}

static const struct pv_totals *totals;

/* The rankings, resolved once when the row is built rather than per card:
 * pv_stats_top() sorts, and a card is drawn every frame. */
static const struct pv_agg *top_artist[TOP_N];
static const struct pv_agg *top_song[TOP_N];
static const struct pv_agg *top_album[TOP_N];
static const struct pv_agg *top_skip[SKIP_N];
static const struct pv_agg *top_loyal[SKIP_N];
static int n_artist, n_song, n_album, n_skip, n_loyal;

/* What the child queries answer for each ranked row, resolved once when the
 * section is built.
 *
 * Trap: pv_stats_child_count() and pv_stats_best_child() each walk a WHOLE
 * table -- thousands of rows on a real library -- and a card is resolved
 * every frame it is on screen. Asking them there is three table walks a
 * frame, which is where a 5G loses its frame rate. pv_stats.h says as much
 * where it declares them: the answer is wanted for a handful of rows, so it
 * is walked rather than kept. Walking it once is the other half of that. */
static short                 kid_songs[TOP_N];
static const struct pv_agg  *kid_album[TOP_N];
static const struct pv_agg  *kid_song[TOP_N];
static short                 alb_songs[TOP_N];
static const struct pv_agg  *alb_song[TOP_N];

static void resolve_children(enum pv_sec sec)
{
    for (int i = 0; i < TOP_N; i++)
    {
        kid_songs[i] = 0;
        alb_songs[i] = 0;
        kid_album[i] = kid_song[i] = alb_song[i] = NULL;
    }

    if (sec == PV_SEC_ARTISTS)
    {
        for (int i = 0; i < n_artist; i++)
        {
            int ai = pv_stats_index(PV_T_ARTIST, top_artist[i]);

            if (ai == PV_ROW_NONE)
                continue;
            kid_songs[i] = (short)pv_stats_child_count(PV_T_TITLE,
                                                       PV_T_ARTIST, ai);
            kid_album[i] = pv_stats_best_child(PV_T_ALBUM, PV_T_ARTIST, ai);
            kid_song[i]  = pv_stats_best_child(PV_T_TITLE, PV_T_ARTIST, ai);
        }
    }
    else if (sec == PV_SEC_ALBUMS)
    {
        for (int i = 0; i < n_album; i++)
        {
            int ai = pv_stats_index(PV_T_ALBUM, top_album[i]);

            if (ai == PV_ROW_NONE)
                continue;
            alb_songs[i] = (short)pv_stats_child_count(PV_T_TITLE,
                                                       PV_T_ALBUM, ai);
            alb_song[i]  = pv_stats_best_child(PV_T_TITLE, PV_T_ALBUM, ai);
        }
    }
}

/* ------------------------------------------------------------- the pool */

/* Formatted figures live here until the next card is resolved. A card is
 * drawn immediately after it is filled in, so a rotating pool costs nothing
 * and saves every tile owning a buffer it uses once a frame. */
#define POOL_N  12
#define POOL_W  64

static char pool[POOL_N][POOL_W];
static int  pool_at;

static const char *pfmt(const char *fmt, ...)
{
    char *b = pool[pool_at];
    va_list ap;

    pool_at = (pool_at + 1) % POOL_N;
    va_start(ap, fmt);
    vsnprintf(b, POOL_W, fmt, ap);
    va_end(ap);
    return b;
}

/* Grouped in threes, because these figures reach six digits and a run of
 * them is what the figasis is for. */
static const char *num(long v)
{
    char digits[24];
    char *b = pool[pool_at];
    int n, i, o = 0;
    bool neg = v < 0;

    pool_at = (pool_at + 1) % POOL_N;
    if (neg)
        v = -v;

    n = snprintf(digits, sizeof(digits), "%ld", v);
    if (neg)
        b[o++] = '-';

    for (i = 0; i < n && o < POOL_W - 2; i++)
    {
        if (i > 0 && ((n - i) % 3) == 0)
            b[o++] = ',';
        b[o++] = digits[i];
    }
    b[o] = 0;
    return b;
}

static const char *date_of(long day)
{
    int y, m, d;

    pv_civil_from_days(day, &y, &m, &d);
    return pfmt("%d %s", d, pv_month_abbr[m]);
}

/* --------------------------------------------------------- the sections */

const char *pv_tiles_section_name(enum pv_sec sec)
{
    switch (sec)
    {
    case PV_SEC_NEW:     return "Newly unlocked";
    case PV_SEC_NUMBERS: return "In numbers";
    case PV_SEC_WEEKS:   return "Week by week";
    case PV_SEC_ARTISTS: return "Top artists";
    case PV_SEC_SONGS:   return "Top songs";
    case PV_SEC_ALBUMS:  return "Top albums";
    case PV_SEC_SKIPS:   return "Skips";
    case PV_SEC_ACH:     return "Achievements";
    default:             return "";
    }
}

/* Week by week, the year grid and the peak hour all need dated entries. A
 * player whose clock was never set logs plays with no date at all, so those
 * tiles are absent rather than zero. */
static bool has_dates(const struct pv_totals *t)
{
    return t->days > 0;
}

bool pv_tiles_section_present(enum pv_sec sec, const struct pv_totals *t)
{
    const struct pv_agg *rows;
    int n = 0;

    switch (sec)
    {
    case PV_SEC_NEW:
        return pv_badges_new_count() > 0;
    case PV_SEC_NUMBERS:
        return true;
    case PV_SEC_WEEKS:
        return has_dates(t) && pv_stats_week_count() > 0;
    case PV_SEC_ARTISTS:
        rows = pv_stats_rows(PV_T_ARTIST, &n);
        return rows && n > 0 && t->plays > 0;
    case PV_SEC_SONGS:
        rows = pv_stats_rows(PV_T_TITLE, &n);
        return rows && n > 0 && t->plays > 0;
    case PV_SEC_ALBUMS:
        rows = pv_stats_rows(PV_T_ALBUM, &n);
        return rows && n > 0 && t->plays > 0;
    case PV_SEC_SKIPS:
        return t->plays > 0 || t->skips > 0;
    case PV_SEC_ACH:
        return pv_badges_count() > 0;
    default:
        return false;
    }
}

/* ------------------------------------------------------------ the build */

/* A card's width comes from its finished content, which is the only thing
 * that knows how much text there is. */
static void remeasure(int i)
{
    struct card_content c;
    int h = CARD_ROW_H
          - ((tile_flags[i] & CARD_ROW_SUB) ? CARD_SUB_DROP : 0);

    pv_tiles_content(i, &c);
    tile_w[i] = (short)card_paint_measure(&c, h);

    /* Which picture this card follows, taken from the content rather than
     * declared: the resolver is the only thing that knows whether a tile has
     * artwork at all. A sub-card has none of its own and takes its parent's. */
    if (tile_flags[i] & CARD_ROW_SUB)
    {
        int p = i;

        while (p > 0 && (tile_flags[p] & CARD_ROW_SUB))
            p--;
        tile_art[i] = tile_art[p];
    }
    else
        tile_art[i] = c.art_key;
}

static int add(enum tile_id id, int arg, unsigned char flags, int depth,
               int parent)
{
    int i = tile_n;

    if (i >= TILE_MAX)
        return -1;

    tile_id[i]    = (unsigned char)id;
    tile_arg[i]   = (short)arg;
    tile_flags[i] = flags;
    tile_root[i]  = card_paint_palette(tile_hue0 + parent, 0);
    tile_depth[i] = (unsigned char)depth;
    tile_art[i]   = 0;
    tile_n++;

    remeasure(i);
    return i;
}

/* A plain tile: no run under it, its own colour. */
static void tile(enum tile_id id, int arg)
{
    add(id, arg, 0, 0, tile_n);
}

/* A tile that owns the run of sub-cards started immediately after it. The
 * parent's position is what its whole run takes its colour from, so an open
 * run reads as one block in one family. */
static int parent(enum tile_id id, int arg)
{
    return add(id, arg, CARD_ROW_PARENT, 0, tile_n);
}

static void sub(enum tile_id id, int arg, int par, int depth)
{
    add(id, arg, CARD_ROW_SUB, depth, par);
}

static void build_numbers(const struct pv_totals *t)
{
    tile(T_YEARMINS, 0);
    tile(T_YEARPLAYS, 0);
    tile(T_YEARTRACKS, 0);
    tile(T_YEARSKIPPED, 0);
    if (has_dates(t))
    {
        tile(T_YEARTIME, 0);
        for (int q = 0; q < 4; q++)
            tile(T_CHART, q);
    }
    tile(T_NIGHTPLAYS, 0);
}

static void build_weeks(void)
{
    int p, weeks = pv_stats_week_count();

    tile(T_ACTIVEWEEKS, 0);
    tile(T_LONGESTDAY, 0);

    p = parent(T_STREAK, 0);
    sub(T_SUBSTREAK, 0, p, 1);

    tile(T_CALTITLE, 0);

    for (int i = 0; i < weeks && tile_n + 3 <= TILE_MAX; i++)
    {
        struct pv_week w;

        if (!pv_stats_week(i, &w) || w.plays == 0)
            continue;           /* a week nothing was played in is not a card */

        p = parent(T_CALWEEK, i);
        sub(T_SUBWEEK1, i, p, 1);
        sub(T_SUBWEEK2, i, p, 2);
    }
}

static void build_top(enum tile_id head, enum tile_id s1, enum tile_id s2,
                      int n)
{
    for (int i = 0; i < n; i++)
    {
        int p = parent(head, i);

        if (p < 0)
            return;
        sub(s1, i, p, 1);
        if (s2 != T_NONE)
            sub(s2, i, p, 2);
    }
}

static void build_skips(const struct pv_totals *t)
{
    /* A log with no plays at all is a division by zero rather than an empty
     * state, so the tile is absent rather than showing 0% or 100%. */
    if (t->plays > 0)
        tile(T_SKIPPERCENT, 0);

    build_top(T_TOPSKIP, T_SUBSKIP, T_NONE, n_skip);
    tile(T_SKIPNEVER, 0);
    build_top(T_TOPLOYAL, T_SUBLOYAL, T_NONE, n_loyal);
}

/* How far along an unearned badge is, 0..1000.
 *
 * An earned one scores BELOW every unearned one, which looks backwards until
 * you read the label: "Closest First" is what you sort by when you are
 * hunting, and putting the sixty you have already won at the top is exactly
 * the walk it exists to save. */
static int badge_progress(int i)
{
    const struct pv_badge *b = pv_badges_get(i);
    long v;

    if (pv_badges_unlocked(i))
        return -1;
    if (!b || b->threshold <= 0)
        return 0;
    v = pv_badges_value(i);
    if (v <= 0)
        return 0;
    if (v >= b->threshold)
        return 1000;
    return (int)(v * 1000 / b->threshold);
}

/* Which row sorts before which, under the chosen order. */
static bool badge_before(int a, int b, int order)
{
    switch (order)
    {
    case PV_BADGE_ORDER_EARNED:
        if (pv_badges_unlocked(a) != pv_badges_unlocked(b))
            return pv_badges_unlocked(a);
        return a < b;
    case PV_BADGE_ORDER_RECENT:
    {
        unsigned long wa = pv_badges_when(a), wb = pv_badges_when(b);

        /* An earned badge with no date -- won before the file existed --
         * still belongs above one that is not earned at all. */
        if (pv_badges_unlocked(a) != pv_badges_unlocked(b))
            return pv_badges_unlocked(a);
        if (wa != wb)
            return wa > wb;
        return a < b;
    }
    case PV_BADGE_ORDER_CLOSEST:
    {
        int pa = badge_progress(a), pb = badge_progress(b);

        if (pa != pb)
            return pa > pb;
        return a < b;
    }
    default:
        return a < b;
    }
}

static short badge_seq[PV_BADGE_MAX];

static void build_badges(void)
{
    int order = global_settings.spun_badge_order;
    int n = 0;

    for (int i = 0; i < pv_badges_count() && n < PV_BADGE_MAX; i++)
        if (pv_badges_vis(i) != PV_BV_HIDDEN)
            badge_seq[n++] = (short)i;

    /* Insertion sort: the wall is a couple of hundred rows and this runs once
     * when the section is built, so the simple one is the right one. */
    if (order != PV_BADGE_ORDER_TABLE)
    {
        for (int i = 1; i < n; i++)
        {
            short v = badge_seq[i];
            int j = i;

            while (j > 0 && badge_before(v, badge_seq[j - 1], order))
            {
                badge_seq[j] = badge_seq[j - 1];
                j--;
            }
            badge_seq[j] = v;
        }
    }

    for (int k = 0; k < n && tile_n + 2 <= TILE_MAX; k++)
    {
        int p = parent(T_BADGE, badge_seq[k]);

        if (p < 0)
            return;
        sub(T_SUBBADGE, badge_seq[k], p, 1);
    }
}

static void build_new(void)
{
    int n = pv_badges_new_count();

    tile(T_NEWCOUNT, 0);
    if (n > CROWN_MAX)
        n = CROWN_MAX;

    for (int k = 0; k < n; k++)
    {
        int p = parent(T_NEWBADGE, k);

        if (p < 0)
            return;
        sub(T_SUBNEWBADGE, k, p, 1);
    }
}

int pv_tiles_build(enum pv_sec sec, const struct pv_totals *t)
{
    int want;

    tile_n = 0;
    tile_hue0 = (int)sec * 7;
    totals = t;

    want = global_settings.spun_top_count;
    if (want < 1)      want = 1;
    if (want > TOP_N)  want = TOP_N;

    /* Set here, beside the ranking it governs, rather than once as the screen
     * opens: every list below and every "best child" behind them has to agree
     * about what "most played" counts, and this is the only place they are
     * all taken. */
    pv_stats_rank_by_time(global_settings.spun_rank_by != 0);

    n_artist = pv_stats_top(PV_T_ARTIST, PV_RANK_PLAYS, top_artist, want);
    n_song   = pv_stats_top(PV_T_TITLE,  PV_RANK_PLAYS, top_song,   want);
    n_album  = pv_stats_top(PV_T_ALBUM,  PV_RANK_PLAYS, top_album,  want);
    n_skip   = pv_stats_top(PV_T_TITLE,  PV_RANK_SKIPS, top_skip,   SKIP_N);
    n_loyal  = pv_stats_top(PV_T_TITLE,  PV_RANK_LOYAL, top_loyal,  SKIP_N);

    resolve_children(sec);

    switch (sec)
    {
    case PV_SEC_NEW:     build_new(); break;
    case PV_SEC_NUMBERS: build_numbers(t); break;
    case PV_SEC_WEEKS:   build_weeks(); break;
    case PV_SEC_ARTISTS: build_top(T_TOPARTIST, T_SUBARTIST1, T_SUBARTIST2,
                                   n_artist); break;
    case PV_SEC_SONGS:   build_top(T_TOPSONG, T_SUBSONG1, T_SUBSONG2,
                                   n_song); break;
    /* Who it is by comes before how much of it there was: the album card
     * carries only the album's own name, so the first thing behind it should
     * finish the sentence rather than start a different one. */
    case PV_SEC_ALBUMS:  build_top(T_TOPALBUM, T_SUBALBUM2, T_SUBALBUM1,
                                   n_album); break;
    case PV_SEC_SKIPS:   build_skips(t); break;
    case PV_SEC_ACH:     build_badges(); break;
    default: break;
    }

    return tile_n;
}

unsigned pv_tiles_art_key(int idx)
{
    return (idx >= 0 && idx < tile_n) ? tile_art[idx] : 0;
}

int                  pv_tiles_count(void)  { return tile_n; }
const short         *pv_tiles_widths(void) { return tile_w; }
const unsigned char *pv_tiles_flags(void)  { return tile_flags; }

/* --------------------------------------------------------- the week scan */

/* What one opened week costs: a read of that week's bytes of the log, seeked
 * to through the day array's per-day offsets. Everything else about a week
 * comes from the day array, which is already in memory. */
#define WK_SLOTS 48

struct wk_named
{
    char name[PV_NAME_MAX];
    int  c;
};

static struct wk_named wk_art[WK_SLOTS], wk_trk[WK_SLOTS];
static int  wk_art_n, wk_trk_n;
static int  wk_songs;
static int  wk_week = -1;       /* which week the three above describe */
static char wk_top_artist[PV_NAME_MAX];
static char wk_top_track[PV_NAME_MAX];

struct wk_ctx
{
    long d_lo, d_hi;
};

static void wk_add(struct wk_named *arr, int *n, const char *name)
{
    for (int i = 0; i < *n; i++)
    {
        if (strcmp(arr[i].name, name) == 0)
        {
            arr[i].c++;
            return;
        }
    }
    if (*n < WK_SLOTS)
    {
        strlcpy(arr[*n].name, name, PV_NAME_MAX);
        arr[*n].c = 1;
        (*n)++;
    }
}

static void wk_entry(const struct pv_entry *e, void *ctx)
{
    struct wk_ctx *c = ctx;
    char artist[PV_NAME_MAX], title[PV_NAME_MAX], album[PV_NAME_MAX];
    long d;

    if (!e->valid_ts || !e->listened)
        return;

    d = (long)(e->ts / 86400UL);
    if (d < c->d_lo || d >= c->d_hi)
        return;

    /* Names are resolved only for entries inside the week, which is what
     * keeps this affordable: the read is one week, the naming is what is in
     * it. */
    if (e->artist)
    {
        wk_add(wk_art, &wk_art_n, e->artist);
        wk_add(wk_trk, &wk_trk_n, e->title);
    }
    else
    {
        pv_names_resolve(e->path, artist, title, album);
        wk_add(wk_art, &wk_art_n, artist);
        wk_add(wk_trk, &wk_trk_n, title);
    }
}

/* The byte range of the log covering days [d_lo, d_hi).
 *
 * The day array carries the offset of each day's first entry, so the range is
 * where the window's first day starts to where the first day after it starts.
 *
 * This assumes a day's entries sit together in the log, which append-only
 * writing guarantees for everything except a clock moved backwards
 * mid-history. A day split that way loses its later half from this one week's
 * figures; nothing else, and no total, is affected. */
static bool wk_range(long d_lo, long d_hi, unsigned long *from,
                     unsigned long *to)
{
    const struct pv_day *days;
    int n = 0;
    bool found = false;

    *from = *to = 0;
    days = pv_stats_days(&n);
    if (!days)
        return false;

    for (int i = 0; i < n; i++)
    {
        if (!found && days[i].day >= d_lo && days[i].day < d_hi)
        {
            *from = days[i].offset;
            found = true;
        }
        if (days[i].day >= d_hi)
        {
            *to = days[i].offset;
            break;
        }
    }

    return found;
}

static void wk_scan(int wi)
{
    struct pv_week w;
    struct wk_ctx ctx;
    unsigned long from, to;
    int bi;

    wk_week = wi;
    wk_art_n = wk_trk_n = wk_songs = 0;
    wk_top_artist[0] = wk_top_track[0] = 0;

    if (!totals || !pv_stats_week(wi, &w))
        return;

    ctx.d_lo = w.start_day;
    ctx.d_hi = w.start_day + 7;
    if (!wk_range(ctx.d_lo, ctx.d_hi, &from, &to))
        return;

    pv_log_read_range(totals->source, from, to, wk_entry, &ctx);
    wk_songs = wk_trk_n;

    bi = -1;
    for (int i = 0; i < wk_art_n; i++)
        if (bi < 0 || wk_art[i].c > wk_art[bi].c)
            bi = i;
    if (bi >= 0)
        strlcpy(wk_top_artist, wk_art[bi].name, PV_NAME_MAX);

    bi = -1;
    for (int i = 0; i < wk_trk_n; i++)
        if (bi < 0 || wk_trk[i].c > wk_trk[bi].c)
            bi = i;
    if (bi >= 0)
        strlcpy(wk_top_track, wk_trk[bi].name, PV_NAME_MAX);
}

void pv_tiles_open(int idx)
{
    if (idx < 0 || idx >= tile_n)
        return;

    /* Any card in the run identifies the run. Opening one moves the focus
     * into it, so the caller's idea of "the card that was opened" is the
     * first sub-card rather than the parent -- and the parent is the only one
     * that knows which week this is. */
    while (idx > 0 && (tile_flags[idx] & CARD_ROW_SUB))
        idx--;

    if (tile_id[idx] != T_CALWEEK || tile_arg[idx] == wk_week)
        return;

    wk_scan(tile_arg[idx]);

    /* The run's widths were measured before the scan, when two of its cards
     * had nothing to say. Measured again here rather than left alone: a card
     * that gains a name it was not sized for would draw it off its own edge.
     * The unfold is already animating each width from zero, so the new target
     * costs no extra motion. */
    for (int i = idx + 1; i < tile_n && (tile_flags[i] & CARD_ROW_SUB); i++)
        remeasure(i);
}

/* --------------------------------------------------------- the resolver */

/* The four charts are told apart by their colourway as much as by their bars,
 * so each names its own rather than taking the card's. */
static const struct
{
    const char *name;
    enum card_gen gen;
} quarter[4] =
{
    { "Morning",   CARD_GEN_SUNRISE },
    { "Afternoon", CARD_GEN_NOON    },
    { "Evening",   CARD_GEN_DUSK    },
    { "Night",     CARD_GEN_NIGHT   },
};

/* One card's series, grid and table. Filled per resolve and handed over by
 * pointer, which is safe for exactly as long as every other string here. */
static short series[24];
static struct card_kv kv[6];

static void chart_series(int q)
{
    long peak = 1;

    for (int h = 0; h < 24; h++)
        if (totals->hour_hist[h] > peak)
            peak = totals->hour_hist[h];

    /* One scale across all four, because the four exist to be compared:
     * normalising each to its own peak would flatten the difference between
     * a quiet morning and a loud night. */
    for (int i = 0; i < 6; i++)
        series[i] = (short)(totals->hour_hist[q * 6 + i] * 1000 / peak);
}

/* A row's rank in its own list, as the plate shows it. */
static const char *rank_tag(int i)
{
    return pfmt("%d", i + 1);
}

static const char *artist_of(const struct pv_agg *row)
{
    const struct pv_agg *a;

    if (!row || row->artist == PV_ROW_NONE)
        return NULL;
    a = pv_stats_row(PV_T_ARTIST, row->artist);
    return a ? a->name : NULL;
}

bool pv_tiles_target(int idx, struct pv_target *out)
{
    const struct pv_agg *r = NULL;
    enum pv_target_kind kind;
    int arg;

    if (idx < 0 || idx >= tile_n)
        return false;

    /* A sub-card is about its parent's subject, so a run answers as one. */
    while (idx > 0 && (tile_flags[idx] & CARD_ROW_SUB))
        idx--;

    arg = tile_arg[idx];
    switch (tile_id[idx])
    {
    case T_TOPARTIST:
        kind = PV_TARGET_ARTIST;
        r = arg < n_artist ? top_artist[arg] : NULL;
        break;
    case T_TOPALBUM:
        kind = PV_TARGET_ALBUM;
        r = arg < n_album ? top_album[arg] : NULL;
        break;
    case T_TOPSONG:
        kind = PV_TARGET_SONG;
        r = arg < n_song ? top_song[arg] : NULL;
        break;
    case T_TOPSKIP:
        kind = PV_TARGET_SONG;
        r = arg < n_skip ? top_skip[arg] : NULL;
        break;
    case T_TOPLOYAL:
        kind = PV_TARGET_SONG;
        r = arg < n_loyal ? top_loyal[arg] : NULL;
        break;
    default:
        return false;
    }

    if (!r || !r->name[0])
        return false;

    out->kind   = kind;
    out->name   = r->name;
    out->artist = kind == PV_TARGET_ARTIST ? NULL : artist_of(r);
    return true;
}

/* How full a progress bar is: this row against the best of its own list, by
 * whatever the list was ranked on. */
static int share(const struct pv_agg *row, const struct pv_agg *best)
{
    long top = pv_stats_weight(best);

    if (!row || top <= 0)
        return -1;
    return (int)(pv_stats_weight(row) * 100 / top);
}

/* The shape nearly every tile in "In numbers" has: a figure, and the word it
 * is a figure of underneath.
 *
 * Underneath rather than beside, because the two are different sizes and one
 * line between them would be a compromise between two rhythms. And short: on
 * an iPod, in a section called "In numbers", "8,970 minutes" says everything
 * that "You've listened to 8,970 minutes of music" does. */
static void figure(struct card_content *c, const char *value,
                   const char *label)
{
    struct card_ink ink;

    card_paint_ink(c->base, &ink);
    card_text_add(&c->text, value, card_paint_figure_font(), ink.text);
    card_text_add_line(&c->text, label, card_paint_body_font(), ink.dim);
}

/* A named row on an art card: the picture, the rank and the name.
 *
 * The name and nothing else. An art card's panel is two lines under a
 * picture, so everything else a row has to say -- who it is by, how many
 * minutes -- belongs on the sub-card behind it. Four sections are this card
 * with a different table. */
static void name_card(struct card_content *c, const struct pv_agg *row,
                      const struct pv_agg *best, int i)
{
    struct card_ink ink;

    card_paint_ink(c->base, &ink);
    c->art     = true;
    c->art_key = row ? row->art_hash : 0;
    c->pat     = (unsigned char)(row ? row->art_hash : 0);
    c->tag     = rank_tag(i);
    c->prog    = share(row, best);

    if (!row || !row->name[0])
    {
        card_text_add(&c->text, row ? "No name" : "No plays yet",
                      card_paint_body_font(), ink.dim);
        return;
    }

    card_text_add(&c->text, row->name, card_paint_card_font(), ink.text);
}

/* "by [ARTIST]" on its own line, opening the sub-card of a row that belongs
 * to somebody -- which the art card in front of it has no room for. */
static void by_artist(struct card_content *c, const struct pv_agg *row)
{
    struct card_ink ink;
    const char *artist = artist_of(row);

    if (!artist)
        return;
    card_paint_ink(c->base, &ink);
    card_text_add_line(&c->text, "by", card_paint_body_font(), ink.dim);
    card_text_add(&c->text, artist, card_paint_card_font(), ink.text);
}

static void kv_add(struct card_content *c, const char *label,
                   const char *value)
{
    if (c->n_table >= (int)(sizeof(kv) / sizeof(kv[0])))
        return;
    kv[c->n_table].label = label;
    kv[c->n_table].value = value;
    c->n_table++;
    c->table = kv;
}

/* Minutes, plays, songs and skips: the table three sections share. */
static void figures(struct card_content *c, const struct pv_agg *row,
                    int songs)
{
    if (!row)
        return;
    kv_add(c, "Minutes", num((long)(row->y_seconds / 60)));
    kv_add(c, "Plays", num(row->y_count));
    if (songs >= 0)
        kv_add(c, "Songs", num(songs));
    kv_add(c, "Skips", num(row->y_skips));
}

void pv_tiles_content(int idx, struct card_content *out)
{
    struct card_ink ink;
    int name, body;
    int arg;
    enum tile_id id;

    card_paint_clear(out);
    if (idx < 0 || idx >= tile_n || !totals)
        return;

    id  = (enum tile_id)tile_id[idx];
    arg = tile_arg[idx];

    /* A card carrying artwork takes the hue of the picture and the lightness
     * and saturation of the colour it was assigned, then the step its place
     * in a run calls for. Until the picture is loaded there is no hue to be
     * had and the assigned colour stands. */
    out->base = tile_root[idx];
    if (tint_fn && tile_art[idx])
    {
        unsigned t = card_paint_tint(tint_fn(tile_art[idx]));

        if (t)
            out->base = t;
    }
    out->base = card_paint_step(out->base, tile_depth[idx]);

    card_paint_ink(out->base, &ink);

    /* The whole of the hierarchy: a figure is the largest thing on a card, a
     * name is what the card is about, and the words between are dim. */
    name = card_paint_card_font();
    body = card_paint_body_font();

    switch (id)
    {
    case T_YEARMINS:
        figure(out, num(totals->seconds / 60), "minutes");
        break;

    case T_YEARPLAYS:
        figure(out, num(totals->plays), "plays");
        break;

    case T_YEARTRACKS:
        figure(out, num(totals->titles), "tracks");
        break;

    case T_YEARSKIPPED:
        figure(out, num(totals->skips), "plays skipped");
        break;

    case T_YEARTIME:
    {
        /* The peak hour, not the peak minute: hour_hist has 24 bins and
         * there is nothing finer in the model to report. */
        int best = 0;

        for (int h = 1; h < 24; h++)
            if (totals->hour_hist[h] > totals->hour_hist[best])
                best = h;
        figure(out, pfmt("%02d:00", best), "peak hour");
        break;
    }

    case T_CHART:
    {
        long n = 0;

        /* The quarter's own total, so the card says something as well as
         * showing it. Four bursts and four bar charts with nothing but a
         * label on them are four pictures; a figure makes each one a fact. */
        for (int i = 0; i < 6; i++)
            n += totals->hour_hist[arg * 6 + i];

        chart_series(arg);
        out->gen      = quarter[arg].gen;
        out->series   = series;
        out->n_series = 6;
        out->title    = quarter[arg].name;
        card_text_add(&out->text, num(n), card_paint_figure_font(),
                      card_paint_gen_ink(out->gen));
        card_text_add_line(&out->text, "plays", body,
                           card_paint_gen_ink(out->gen));
        break;
    }

    case T_NIGHTPLAYS:
        if (totals->night == 0)
        {
            card_text_add(&out->text, "Your nights are quiet. Respect.",
                          body, ink.text);
            break;
        }
        figure(out, num(totals->night), "plays after midnight");
        break;

    /* ------------------------------------------------------ week by week */

    case T_ACTIVEWEEKS:
        figure(out, num(pv_stats_active_weeks()), "weeks with music");
        break;

    case T_LONGESTDAY:
    {
        struct pv_day d;

        if (!pv_stats_longest_day(&d))
            break;
        figure(out, num(d.secs / 60), "minutes on your best day");
        break;
    }

    case T_STREAK:
        figure(out, num(totals->streak), "day streak");
        break;

    case T_SUBSTREAK:
    {
        long from, to;
        unsigned secs;

        if (!pv_stats_streak_span(&from, &to, &secs))
            break;
        kv_add(out, "From", date_of(from));
        kv_add(out, "To", date_of(to));
        kv_add(out, "Minutes", num(secs / 60));
        break;
    }

    case T_CALTITLE:
        out->title = "By week";
        break;

    case T_CALWEEK:
    {
        struct pv_week w;
        unsigned peak = 1;

        if (!pv_stats_week(arg, &w))
            break;
        out->tag = pfmt("%d", arg + 1);
        figure(out, num(w.secs / 60), "minutes");

        /* The week's days along its own foot, squashed to a strip.
         *
         * On the card rather than behind it: the shape of a week is the first
         * thing anyone wants from a week, and a card that has to be opened to
         * show it is a card that says less than it could. Short, because it
         * is a sparkline under a figure and not a chart in its own right. */
        for (int i = 0; i < 7; i++)
            if (w.day_secs[i] > peak)
                peak = w.day_secs[i];
        for (int i = 0; i < 7; i++)
            series[i] = (short)(w.day_secs[i] * 1000 / peak);
        out->series   = series;
        out->n_series = 7;
        out->series_h = 20;
        break;
    }

    case T_SUBWEEK1:
    {
        struct pv_week w;

        if (!pv_stats_week(arg, &w))
            break;
        kv_add(out, "Minutes", num(w.secs / 60));
        kv_add(out, "Plays", num(w.plays));
        if (wk_week == arg)
            kv_add(out, "Songs", num(wk_songs));
        kv_add(out, "Skips", num(w.skips));
        break;
    }

    case T_SUBWEEK2:
        /* Filled by the scan that runs when the week is opened, which is
         * before this card can be on screen -- so an empty one here means the
         * week's entries carried no names, and the catch-all at the end says
         * so better than a progress message nobody sees. */
        if (wk_week != arg || !wk_top_artist[0])
            break;
        card_text_add(&out->text, "Top artist", body, ink.dim);
        card_text_add_line(&out->text, wk_top_artist, name, ink.text);
        if (wk_top_track[0])
        {
            card_text_add_line(&out->text, "Top song", body, ink.dim);
            card_text_add_line(&out->text, wk_top_track, name, ink.text);
        }
        break;

    /* --------------------------------------------------------- the tops */

    case T_TOPARTIST:
        name_card(out, arg < n_artist ? top_artist[arg] : NULL,
                  n_artist ? top_artist[0] : NULL, arg);
        break;

    case T_SUBARTIST1:
        figures(out, arg < n_artist ? top_artist[arg] : NULL,
                arg < n_artist ? kid_songs[arg] : -1);
        break;

    case T_SUBARTIST2:
    {
        const struct pv_agg *al = arg < n_artist ? kid_album[arg] : NULL;
        const struct pv_agg *so = arg < n_artist ? kid_song[arg] : NULL;

        if (al)
        {
            card_text_add(&out->text, "Top album", body, ink.dim);
            card_text_add_line(&out->text, al->name, name, ink.text);
        }
        if (so)
        {
            card_text_add_line(&out->text, "Top song", body, ink.dim);
            card_text_add_line(&out->text, so->name, name, ink.text);
        }
        break;
    }

    case T_TOPSONG:
        name_card(out, arg < n_song ? top_song[arg] : NULL,
                  n_song ? top_song[0] : NULL, arg);
        break;

    case T_SUBSONG1:
    {
        /* Who it is by and what it is from, before any figure. The song card
         * in front carries only the song's own name, so the first thing
         * behind it should finish that sentence. */
        const struct pv_agg *r = arg < n_song ? top_song[arg] : NULL;
        const struct pv_agg *al;

        by_artist(out, r);
        al = (r && r->album != PV_ROW_NONE)
           ? pv_stats_row(PV_T_ALBUM, r->album) : NULL;
        if (al && al->name[0])
        {
            card_text_add_line(&out->text, "from", body, ink.dim);
            card_text_add(&out->text, al->name, name, ink.text);
        }
        break;
    }

    case T_SUBSONG2:
        figures(out, arg < n_song ? top_song[arg] : NULL, -1);
        break;

    case T_TOPALBUM:
        name_card(out, arg < n_album ? top_album[arg] : NULL,
                  n_album ? top_album[0] : NULL, arg);
        break;

    case T_SUBALBUM1:
        figures(out, arg < n_album ? top_album[arg] : NULL,
                arg < n_album ? alb_songs[arg] : -1);
        break;

    case T_SUBALBUM2:
    {
        const struct pv_agg *so = arg < n_album ? alb_song[arg] : NULL;

        by_artist(out, arg < n_album ? top_album[arg] : NULL);
        if (so)
        {
            card_text_add_line(&out->text, "Top song", body, ink.dim);
            card_text_add_line(&out->text, so->name, name, ink.text);
        }
        break;
    }

    /* -------------------------------------------------------- the skips */

    case T_SKIPPERCENT:
    {
        int pct = (int)(totals->skips * 100 / (totals->plays + totals->skips));

        figure(out, pfmt("%d%%", pct), "of plays skipped");
        break;
    }

    case T_TOPSKIP:
    {
        const struct pv_agg *r = arg < n_skip ? top_skip[arg] : NULL;

        out->tag = rank_tag(arg);
        if (!r)
            break;
        out->art     = true;
        out->art_key = r->art_hash;
        out->pat     = (unsigned char)r->art_hash;
        out->prog    = n_skip && top_skip[0]->y_skips > 0
                     ? (int)((long)r->y_skips * 100 / top_skip[0]->y_skips)
                     : -1;
        card_text_add(&out->text, r->name, name, ink.text);
        break;
    }

    case T_SUBSKIP:
    {
        const struct pv_agg *r = arg < n_skip ? top_skip[arg] : NULL;

        if (!r)
            break;
        by_artist(out, r);
        kv_add(out, "Skips", num(r->y_skips));
        kv_add(out, "Plays", num(r->y_count));
        break;
    }

    case T_SKIPNEVER:
        out->title = n_loyal ? "Never skipped" : "You skip everything?!";
        break;

    case T_TOPLOYAL:
    {
        const struct pv_agg *r = arg < n_loyal ? top_loyal[arg] : NULL;

        out->tag = rank_tag(arg);
        if (!r)
            break;
        out->art     = true;
        out->art_key = r->art_hash;
        out->pat     = (unsigned char)r->art_hash;
        out->prog    = share(r, top_loyal[0]);
        card_text_add(&out->text, r->name, name, ink.text);
        break;
    }

    case T_SUBLOYAL:
    {
        const struct pv_agg *r = arg < n_loyal ? top_loyal[arg] : NULL;

        if (!r)
            break;
        by_artist(out, r);
        kv_add(out, "Plays", num(r->y_count));
        kv_add(out, "Minutes", num((long)(r->y_seconds / 60)));
        break;
    }

    /* ------------------------------------------------------- the badges */

    case T_NEWCOUNT:
        out->tag_icon = ICON_CELEBRATION;
        figure(out, num(pv_badges_new_count()), "new badges");
        break;

    case T_NEWBADGE:
    case T_SUBNEWBADGE:
    {
        int bi = pv_badges_new_index(arg);
        const struct pv_badge *b = bi >= 0 ? pv_badges_get(bi) : NULL;

        if (!b)
            break;
        if (id == T_NEWBADGE)
        {
            /* Newly unlocked, so its metal is no longer a secret. */
            out->tag_icon = tier_icon(b->tier);
            card_text_add(&out->text, b->name, name, ink.text);
            break;
        }
        card_text_add(&out->text, b->desc, body, ink.text);
        break;
    }

    case T_BADGE:
    {
        const struct pv_badge *b = pv_badges_get(arg);

        if (!b)
            break;
        if (pv_badges_vis(arg) == PV_BV_GOAL && b->threshold > 0)
        {
            long v = pv_badges_value(arg);

            out->prog = (int)(v * 100 / b->threshold);
            if (out->prog > 100)
                out->prog = 100;
        }
        out->tag_icon = pv_badges_vis(arg) == PV_BV_DONE
                      ? tier_icon(b->tier) : ICON_AWARD;
        card_text_add(&out->text, b->name, name,
                      pv_badges_vis(arg) == PV_BV_DONE ? ink.text : ink.dim);
        break;
    }

    case T_SUBBADGE:
    {
        const struct pv_badge *b = pv_badges_get(arg);
        unsigned long when = pv_badges_when(arg);

        if (!b)
            break;
        card_text_add(&out->text, b->desc, body, ink.text);
        if (pv_badges_unlocked(arg) && when)
        {
            card_text_add_line(&out->text, "Earned:", body, ink.dim);
            card_text_add(&out->text, date_of((long)(when / 86400UL)), name,
                          ink.text);
        }
        break;
    }

    default:
        break;
    }

    /* A tile with nothing in it says so.
     *
     * A year of listening is not a guaranteed shape -- a month of history, a
     * player that skips everything, a week the log has no names for -- and a
     * card that resolves to nothing has to say something rather than draw an
     * empty field. The wordings of spun-tiles.md §5a are better than this
     * wherever they apply, and the cases above set them. */
    if (!out->text.n && !out->title && !out->level && !out->series
        && !out->art && !out->n_table)
        card_text_add(&out->text, "Nothing to show yet", body, ink.dim);
}
