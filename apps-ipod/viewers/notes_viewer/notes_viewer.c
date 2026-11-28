/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The notes as a wiki: one kind of screen, a page, with links between
 * pages. A page is a file someone wrote, or one made up for an artist or an
 * album the library has (notes_source.c), so every link goes somewhere; a
 * [Name] links where notes_links.c says.
 *
 * How it is built:
 *   - A plain core screen, returning a GO_TO_* code. It owns the screen with
 *     the theme off, as the lyric viewer does, so dim text can be a true
 *     blend toward a solid background.
 *   - A page is made whole when it opens: its Markdown read through the ts_*
 *     engine, laid out by text_layout.c, and every link in it found once.
 *     Its banner is the owner's banner art, else a band of the person's
 *     picture or the album's cover, and scrolls with the text; the page's
 *     name is set over it in the theme's heading font, else the biggest font
 *     loaded that sets a name.
 *   - The page takes its own picture's colours, held to a deep or a pale
 *     tint, never the colour itself: it is read against for minutes at a
 *     time. Or the theme's, the text viewer's, or Custom's text, link,
 *     page, dim text, heading and pull line colours (liner_notes_colours);
 *     a picture with no colour in it gives its links the theme's. And
 *     liner_notes_banners off leaves the banner out, liner_notes_progress
 *     off the progress line.
 *   - Everything lives in the app buffer, never core_alloc(): the screen is
 *     used while music plays, and taking memory from the audio buffer stops
 *     the track. One banner is kept, the one on screen, so opening its page
 *     again does not make it again; going back to another page does.
 *
 * Links are drawn in the accent colour, and one is current, drawn inverted:
 * the one nearest the reading line, a third of the way down, or at the top
 * of a page the first, the keyclick sounding only as one becomes current.
 * The wheel scrolls; a click moves between two links on
 * one line before the text moves on, and at the end of a page, or on a page
 * too short to scroll, it moves the current link on instead. Select or Right
 * follows it, Select held offers what can be done with it -- nothing, for a
 * link that can only be opened -- and Left goes back along the last
 * NV_HISTORY pages, each where it was left. Play plays an album page's album
 * when it is not the one playing, and is pause and play anywhere else. Menu
 * leaves; Menu held offers the same for the page itself, Move Banner, and
 * the colour and banner settings, below the other pages of an album's: its
 * own and those whose front matter names it (notes_album_parts()).
 *
 * Parts, in order:
 *   - the buffer, colours and the screen
 *   - banners: banner art, or a band of a portrait or a cover
 *   - a page: its Markdown, its lines and its links
 *   - drawing
 *   - the current link
 *   - Move Banner and Reset Banner
 *   - a link's menu
 *   - opening pages, and going back
 *   - the screen, and its entry points
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "audio.h"
#include "metadata.h"
#include "playlist/playlist.h"
#include "string-extra.h"
#include "settings/settings.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "draw/color.h"
#include "skin/skin_albumart_color.h"   /* dynamic_colors_resolve */
#include "widgets/splash.h"
#include "widgets/menu.h"
#include "widgets/color_picker.h"       /* Chosen Colour */
#include "input/action.h"
#include "audio/sound_feedback.h"   /* the wheel's click */
#include "system/activity.h"
#include "system/app_buffer.h"
#include "system/shutdown.h"            /* default_event_handler */
#include "system/debug_log.h"
#include "root_menu.h"                  /* GO_TO_* */
#include "screens/playback/wps.h"       /* wps_do_playpause */
#include "screens/browse/browser_db.h"  /* Go to Album, Go to Artist */
#include "metadata/art_cache.h"
#include "metadata/art_banner.h"
#include "metadata/art_name.h"
#include "metadata/name_index.h"
#include "database/tagcache.h"
#include "database/db_summary.h"        /* playing an album */
#include "viewers/text_viewer/txt_source.h"
#include "viewers/text_viewer/text_layout.h"
#include "viewers/text_viewer/text_viewer.h"   /* text_viewer_colours */
#include "notes_files.h"
#include "notes_links.h"
#include "notes_source.h"
#include "notes_viewer.h"

#define NV_BAN_W        ART_BANNER_W
#define NV_BAN_H        ART_BANNER_H
#define NV_ROW          28          /* the slim bar */
#define NV_NAME_MAX_H   40          /* the tallest name font taken */
#define NV_MARGIN_L     10
#define NV_MARGIN_R     12
#define NV_PROGRESS     2           /* the progress line along the foot */
#define NV_THUMB        300         /* the art cache's largest size */
#define NV_TEXT_MAX     (40 * 1024)
#define NV_LINES_MAX    1024
#define NV_LINKS_MAX    512
#define NV_SRC_MAX      (40 * 1024) /* the Markdown, pieces filled in */
#define NV_FILE_MAX     (32 * 1024) /* the written file, or the template */
#define NV_NAME_MAX     224         /* a link's name, qualifier and all */
#define NV_HISTORY      8
#define NV_PAGES_MAX    8           /* an album's pages, on the page's menu */
#define NV_READ         (LCD_HEIGHT / 3)    /* the reading line */
#define NV_NEAR         3           /* lines from it a link is current */
#define NV_POLL         (HZ / 2)
#define NV_FADE         92          /* rows over which the picture fades */
#define NV_MOVE_SHOW    240         /* the picture, on Move Banner's screen */
#define NV_MOVE_STEP    4           /* rows of the 300px picture a click */

/* What each laid-out line is */
#define NV_HEAD     1       /* part of a heading */
#define NV_PULL     2       /* part of a pull line: the rule beside it */
#define NV_DIM      4       /* starts inside dim text */
#define NV_MINOR    8       /* part of a `##` heading or deeper: underlined */

struct nv_line
{
    uint32_t off;
    int y;                      /* its top, down from the text's */
    int16_t link;               /* the link it starts inside, or -1 */
    int16_t bullet;             /* where a list item's bullet is, or -1 */
    uint8_t flags;
    struct tl_line ln;
};

/* A link: its TS_NAME_OPEN, the byte after its TS_NAME_CLOSE, the lines it
 * starts and ends on, and where it goes */
struct nv_link
{
    uint32_t off, end;
    int line, last;
    struct notes_target t;
};

/* A page left, to go back to */
struct nv_hist
{
    struct notes_target t;
    int scroll, cur;
};

/* How a page is asked for: by where a link goes, or from outside the screen
 * by a track, an album or a name; or by a link's name, as a page is found
 * again once the library or the pages are renumbered */
enum { NV_BY_TARGET, NV_BY_ALBUM, NV_BY_NAME, NV_BY_LINK };

/* A request's focus: the track an album request's path names */
#define NV_FOCUS_TRACK (-2)

struct nv_req
{
    int by;
    struct notes_target t;
    const struct notes_album *album;    /* its names and a track's path */
    const char *name;
    int focus;                  /* -1, or NV_FOCUS_TRACK */
};

static struct
{
    struct viewport vp;         /* the whole screen */
    struct viewport tvp;        /* the text, inside the margins */
    int font, lh, quote_indent;
    int bold;                   /* the theme's bold font, or -1 */
    int name_font, name_h;      /* the large name's font, and its height */
    unsigned fg, bg, dim, accent, rule, track;
    unsigned head, quote;       /* headings, and a pull line's rule */

    /* the app buffer, carved up */
    char *work;                 /* a page while it is made, then a picture */
    size_t work_size;

    /* The banner on screen, kept under its page's key: a page opened again
     * (after Move Banner, or a link to itself) does not make it again */
    struct
    {
        bool valid, has_banner, movable;
        uint32_t id;
        unsigned int pic_key, band_key;
        int band_default;
    } slot;
    fb_data *banner;
    bool has_banner;
    /* The banner is a band of the person's picture or the album's cover,
     * which Move Banner moves: the art cache key the picture is under, the
     * key its band is kept under in banners.dat, and where the band sits
     * when it has not been moved, in rows of the 300px picture */
    bool movable;
    unsigned int pic_key, band_key;
    int band_default;

    /* the page */
    struct notes_target t;      /* how it was reached */
    struct notes_doc doc;       /* what it is about */
    unsigned char *text;
    size_t text_len;
    struct nv_line *lines;
    int nlines;
    struct nv_link *links;
    int nlinks, cur;            /* the current link, or -1 */
    int text_top, name_y, scroll, max_scroll;
    int focus_line;             /* a track's line to open at, or -1 */
    struct name_index ix;

    /* The page's album rows, tracks and page numbers, and the history's,
     * hold under the database and the page index it was made in; after
     * either changes, the page is found again by this link name */
    int32_t built_commit;
    uint32_t built_gen;
    unsigned int built_pages;
    char refind[NV_NAME_MAX];

    struct nv_hist hist[NV_HISTORY];
    int nhist;

    /* the page's album's other pages, its own first, and their names */
    struct notes_target pages[NV_PAGES_MAX];
    char page_names[NV_PAGES_MAX][64];
    int npages;

    /* an album to play once the screen has let go of the buffer */
    int play_album;
    int play_track;
} nv;

/* ---- the buffer, colours and the screen -------------------------------- */

#define NV_BANNER_BYTES (NV_BAN_W * NV_BAN_H * FB_DATA_SZ)
#define NV_THUMB_BYTES  (NV_THUMB * NV_THUMB * FB_DATA_SZ)
#define NV_FIXED_BYTES  (NV_BANNER_BYTES + ALIGN_UP(NV_TEXT_MAX + 1, 4) \
                         + NV_LINES_MAX * sizeof(struct nv_line) \
                         + NV_LINKS_MAX * sizeof(struct nv_link) + 16)
#define NV_BUILD_BYTES  (NV_SRC_MAX + NV_FILE_MAX + TS_ARENA_PLAIN)
#define NV_BUF_BYTES    (NV_FIXED_BYTES + MAX(NV_THUMB_BYTES, NV_BUILD_BYTES))
/* The 5G's app buffer is 512 KB: past it, the screen would not open. The
 * library's links and the names index take what is left over. */
_Static_assert(NV_BUF_BYTES <= PLUGIN_BUFFER_SIZE,
               "the notes screen's buffer is too big for the app buffer");

/* `s`, copied into a box too small for it, cut back to its last whole
 * character: the copy stops at a byte, which can be inside one */
static void nv_utf8_cut(char *s)
{
    size_t n = strlen(s), i = n, need = 2;
    unsigned char lead;

    while (i > 0 && ((unsigned char)s[i - 1] & 0xc0) == 0x80)
        i--;
    if (i == 0 || !((lead = s[i - 1]) & 0x80))
        return;
    if (lead >= 0xf0)
        need = 4;
    else if (lead >= 0xe0)
        need = 3;
    if (n - (i - 1) < need)
        s[i - 1] = '\0';
}

/* The clock, raised while a page is made and while the wheel turns, and
 * let go once the screen has been still for a second: nothing else raises
 * it for drawing, and the players idle at 30 MHz (5G) and 54 MHz (6G) */
static bool nv_boosted;
static long nv_boost_until;

static void nv_boost(void)
{
    nv_boost_until = current_tick + HZ;
    if (nv_boosted)
        return;
    cpu_boost(true);
    nv_boosted = true;
}

/* Lets it go when the second is up, or at once for `now` */
static void nv_unboost(bool now)
{
    if (nv_boosted && (now || TIME_AFTER(current_tick, nv_boost_until)))
    {
        cpu_boost(false);
        nv_boosted = false;
    }
}

static int nv_luma(unsigned c)
{
    return (RGB_UNPACK_RED(c) * 77 + RGB_UNPACK_GREEN(c) * 150
            + RGB_UNPACK_BLUE(c) * 29) >> 8;
}

/* The last page's picture's colours, which a page without one keeps */
static bool nv_art_valid;
static unsigned nv_art_dominant, nv_art_vivid;

/* Whether the page's colours come from its picture */
static bool nv_uses_art(void)
{
    return global_settings.liner_notes_colours == NOTES_COLOURS_ART
           || (global_settings.liner_notes_colours == NOTES_COLOURS_CUSTOM
               && (global_settings.liner_notes_text == NOTES_FROM_ART
                   || global_settings.liner_notes_links == NOTES_FROM_ART
                   || global_settings.liner_notes_page == NOTES_FROM_ART
                   || global_settings.liner_notes_dim == NOTES_FROM_ART + 1
                   || global_settings.liner_notes_heads == NOTES_FROM_ART + 1
                   || global_settings.liner_notes_quote
                      == NOTES_FROM_ART + 1));
}

/* The picture's vivid colour, or `theme` when the picture has none: a
 * black-and-white cover's most vivid colour is a grey, which links in it
 * would not stand out from the text in */
#define NV_GREY_CHROMA 4588     /* 0.07 in color_chroma()'s units */

static unsigned nv_vivid(unsigned theme)
{
    return color_chroma(nv_art_vivid) >= NV_GREY_CHROMA ? nv_art_vivid
                                                        : theme;
}

/* The page's picture's colours: its album's cover or its person's picture,
 * from the art cache's small thumbnail */
static void nv_art_palette(void)
{
    struct tagcache_album row;
    unsigned int key = 0, dominant, vivid;

    if (!nv_uses_art())
        return;
    if (nv.doc.kind == NOTES_PAGE_ALBUM && nv.doc.album >= 0
        && tagcache_album_get(nv.doc.album, &row))
        key = row.art_hash;
    else if (nv.doc.kind == NOTES_PAGE_PERSON)
        key = nv.doc.artist ? nv.doc.artist : art_name_hash(nv.doc.title);
    if (key && dynamic_colors_palette_of(key, &dominant, &vivid))
    {
        nv_art_dominant = dominant;
        nv_art_vivid = vivid;
        nv_art_valid = true;
    }
}

/* A Custom colour: NOTES_FROM_* `from`, given what Theme, Art and Chosen
 * Colour are */
static unsigned nv_from(int from, unsigned theme, unsigned art,
                        unsigned chosen)
{
    if (from == NOTES_FROM_WHITE)
        return LCD_WHITE;
    if (from == NOTES_FROM_BLACK)
        return LCD_BLACK;
    if (from == NOTES_FROM_CHOSEN)
        return chosen;
    return from == NOTES_FROM_ART && nv_art_valid ? art : theme;
}

/* `c` held to a tone that reads on the page */
static unsigned nv_readable(unsigned c, bool dark)
{
    return dark ? color_tone_bound(c, 70, 1) : color_tone_bound(c, 45, -1);
}

/* The page under Custom. Art's text and links are held to tones that read
 * on the page, and Art's page is a deep tint under light text, a pale one
 * under dark. */
static void nv_custom_colours(unsigned fg, unsigned bg, unsigned ac)
{
    const int *rgb = global_settings.liner_notes_rgb;
    int text = global_settings.liner_notes_text;
    int links = global_settings.liner_notes_links;
    bool dark;

    fg = nv_from(text, fg, nv_art_vivid, rgb[0]);
    ac = nv_from(links, ac, nv_vivid(ac), rgb[1]);
    if (global_settings.liner_notes_page == NOTES_FROM_ART && nv_art_valid)
        bg = text == NOTES_FROM_ART || nv_luma(fg) >= 128
             ? color_blend(color_tone_bound(nv_art_dominant, 14, -1),
                           LCD_BLACK, 80)
             : color_blend(color_tone_bound(nv_art_dominant, 94, 1),
                           LCD_WHITE, 80);
    else
        bg = nv_from(global_settings.liner_notes_page, bg, bg, rgb[2]);
    dark = nv_luma(bg) < 128;
    if (text == NOTES_FROM_ART && nv_art_valid)
        fg = nv_readable(fg, dark);
    if (links == NOTES_FROM_ART && nv_art_valid)
        ac = nv_readable(ac, dark);
    nv.fg = fg;
    nv.bg = bg;
    nv.accent = ac;
}

/* One of Custom's dim text, heading and pull line rows: `v` is
 * NOTES_STYLE_AUTO for `own`, else a NOTES_FROM_* one more than its value */
static unsigned nv_style(int v, unsigned theme, unsigned own, unsigned chosen)
{
    int from = v - 1;

    if (v == NOTES_STYLE_AUTO)
        return own;
    if (from == NOTES_FROM_ART && nv_art_valid)
        return nv_readable(nv_vivid(theme), nv_luma(nv.bg) < 128);
    return nv_from(from, theme, theme, chosen);
}

/* The page's colours, as liner_notes_colours says: the theme's through
 * dynamic colours, as the lyric viewer takes them, with the selector's
 * colour as the accent, or the page's picture's in their place; the text
 * viewer's; or Custom's. The page keeps the art's hue but not its strength:
 * light text gets a deep tint under it and dark text a pale one, and the
 * text and accent are held to tones that read on it. */
static void nv_colours(void)
{
    unsigned fg = dynamic_colors_resolve(global_settings.fg_color);
    unsigned bg = dynamic_colors_resolve(global_settings.bg_color);
    unsigned ac = dynamic_colors_resolve(global_settings.lss_color);

    if (global_settings.liner_notes_colours == NOTES_COLOURS_CUSTOM)
        nv_custom_colours(fg, bg, ac);
    else if (global_settings.liner_notes_colours == NOTES_COLOURS_TEXT_VIEWER)
    {
        text_viewer_colours(&fg, &bg);
        nv.fg = fg;
        nv.bg = bg;
        nv.accent = nv_readable(ac, nv_luma(fg) >= nv_luma(bg));
    }
    else
    {
        bool dark = nv_luma(fg) >= nv_luma(bg);

        if (global_settings.liner_notes_colours == NOTES_COLOURS_ART
            && nv_art_valid)
        {
            bg = nv_art_dominant;
            ac = nv_vivid(ac);
        }
        if (dark)
        {
            nv.bg = color_blend(color_tone_bound(bg, 14, -1), LCD_BLACK, 80);
            nv.fg = color_tone_bound(fg, 90, 1);
        }
        else
        {
            nv.bg = color_blend(color_tone_bound(bg, 94, 1), LCD_WHITE, 80);
            nv.fg = color_tone_bound(fg, 18, -1);
        }
        nv.accent = nv_readable(ac, dark);
    }
    nv.dim = color_blend(nv.fg, nv.bg, 108);
    nv.head = nv.fg;
    nv.quote = nv.accent;
    if (global_settings.liner_notes_colours == NOTES_COLOURS_CUSTOM)
    {
        const int *rgb = global_settings.liner_notes_rgb;

        nv.dim = nv_style(global_settings.liner_notes_dim, fg, nv.dim, rgb[3]);
        nv.head = nv_style(global_settings.liner_notes_heads, fg, nv.head,
                           rgb[4]);
        nv.quote = nv_style(global_settings.liner_notes_quote, ac, nv.quote,
                            rgb[5]);
    }
    nv.rule = color_blend(nv.fg, nv.bg, 218);
    nv.track = color_blend(nv.fg, nv.bg, 210);
    nv.vp.fg_pattern = nv.tvp.fg_pattern = nv.fg;
    nv.vp.bg_pattern = nv.tvp.bg_pattern = nv.bg;
}

/* The theme's `font heading`, else the largest font loaded that sets a name
 * -- in practice one of the theme's skin fonts, its Now Playing title's in
 * nearly every theme -- no taller than NV_NAME_MAX_H. A font sets a name when
 * it has the alphabet, as a clock's digits-only font has not, letters that
 * hang below the line, and an i narrower than its W; an icon font, which
 * often spans the alphabet too, has neither. The UI font when there is none
 * larger. */
static int nv_name_font(void)
{
    int best = nv.font, best_h = nv.lh, id = font_get_ui_heading();
    struct font *pf = id >= 0 ? font_get(id) : NULL;

    if (pf && pf->height <= NV_NAME_MAX_H)
        return id;
    for (id = FONT_FIRSTUSERFONT; id < MAXFONTS; id++)
    {
        pf = font_get(id);

        if (pf && (int)pf->height > best_h && pf->height <= NV_NAME_MAX_H
            && pf->firstchar <= 'A' && pf->firstchar + pf->size > 'z'
            && pf->ascent > 0 && pf->ascent < (int)pf->height
            && font_get_width(pf, 'i') < font_get_width(pf, 'W'))
        {
            best = id;
            best_h = pf->height;
        }
    }
    return best;
}

static void nv_setup_screen(void)
{
    viewportmanager_theme_enable(SCREEN_MAIN, false, &nv.vp);
    nv.font = screens[SCREEN_MAIN].getuifont();
    nv.vp.font = nv.font;
    nv.tvp = nv.vp;
    nv.tvp.x = NV_MARGIN_L;
    nv.tvp.width = LCD_WIDTH - NV_MARGIN_L - NV_MARGIN_R;
    nv.lh = font_get(nv.font)->height;
    nv.bold = font_get_ui_bold();
    if (nv.bold == nv.font)
        nv.bold = -1;
    nv.name_font = nv_name_font();
    nv.name_h = font_get(nv.name_font)->height;
    nv_colours();
}

/* ---- banners ----------------------------------------------------------- */

static void nv_unpack(fb_data p, int *r, int *g, int *b)
{
    *r = RGB_UNPACK_RED(p);
    *g = RGB_UNPACK_GREEN(p);
    *b = RGB_UNPACK_BLUE(p);
}

/* `sw` x `sh` pixels of `src`, rows `stride` apart, scaled to fill
 * `dw` x `dh` at `dst`, bilinear */
static void nv_scale(fb_data *dst, int dw, int dh, const fb_data *src,
                     int sw, int sh, int stride)
{
    /* Each column's source position, worked out once: the 5G divides in
     * software */
    static int col[NV_BAN_W];
    int x, y;

    for (x = 0; x < dw && x < NV_BAN_W; x++)
        col[x] = dw > 1 ? x * ((sw - 1) << 8) / (dw - 1) : 0;
    for (y = 0; y < dh; y++)
    {
        int fy = dh > 1 ? y * ((sh - 1) << 8) / (dh - 1) : 0;
        int y0 = fy >> 8, wy = fy & 255, y1 = MIN(y0 + 1, sh - 1);
        const fb_data *r0 = src + (size_t)y0 * stride;
        const fb_data *r1 = src + (size_t)y1 * stride;

        for (x = 0; x < dw && x < NV_BAN_W; x++)
        {
            int fx = col[x];
            int x0 = fx >> 8, wx = fx & 255, x1 = MIN(x0 + 1, sw - 1);
            int c[4][3], k, out[3];

            nv_unpack(r0[x0], &c[0][0], &c[0][1], &c[0][2]);
            nv_unpack(r0[x1], &c[1][0], &c[1][1], &c[1][2]);
            nv_unpack(r1[x0], &c[2][0], &c[2][1], &c[2][2]);
            nv_unpack(r1[x1], &c[3][0], &c[3][1], &c[3][2]);
            for (k = 0; k < 3; k++)
            {
                int top = c[0][k] * (256 - wx) + c[1][k] * wx;
                int bot = c[2][k] * (256 - wx) + c[3][k] * wx;

                out[k] = (top * (256 - wy) + bot * wy) >> 16;
            }
            dst[(size_t)y * dw + x] = LCD_RGBPACK(out[0], out[1], out[2]);
        }
    }
}

/* Banner art from the art cache, 320 x 132 or narrower, centred */
static bool nv_banner_art(const char *path)
{
    struct art_cache_header hdr;
    int fd = open(path, O_RDONLY), y, i;
    bool ok;

    if (fd < 0)
        return false;
    ok = read(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)
         && hdr.magic == ART_CACHE_MAGIC
         && hdr.version == ART_CACHE_FORMAT_VERSION && hdr.layout == AA_ROWS
         && hdr.width > 0 && hdr.width <= NV_BAN_W
         && hdr.height > 0 && hdr.height <= NV_BAN_H;
    if (ok)
    {
        int x0 = (NV_BAN_W - hdr.width) / 2, y0 = (NV_BAN_H - hdr.height) / 2;
        ssize_t row = (ssize_t)hdr.width * FB_DATA_SZ;

        for (i = 0; i < NV_BAN_W * NV_BAN_H; i++)
            nv.banner[i] = nv.bg;
        for (y = 0; y < hdr.height && ok; y++)
            ok = read(fd, nv.banner + (size_t)(y0 + y) * NV_BAN_W + x0, row)
                 == row;
    }
    close(fd);
    return ok;
}

/* The art cache's 300px picture with key `key` into `px` at `dim` square,
 * unless all it has is the placeholder */
static bool nv_thumb(unsigned int key, fb_data *px, int dim)
{
    static int wps_size = -2;
    char path[MAX_PATH];
    struct bitmap bm;
    bool fallback;
    int fd, rc;

    if (wps_size == -2)
        wps_size = art_cache_size_index("wps");
    if (!key || wps_size < 0
        || !art_cache_lookup_hash(key, wps_size, path, sizeof(path),
                                  &fallback)
        || fallback || art_cache_size_dim(wps_size) != NV_THUMB)
        return false;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    if (dim == NV_THUMB)
    {
        /* At its own size, in one read: the scaler reads it a row at a
         * time, which costs over a second on the 5G */
        struct art_cache_header hdr;
        ssize_t bytes = (ssize_t)NV_THUMB * NV_THUMB * FB_DATA_SZ;
        bool ok = read(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)
                  && hdr.magic == ART_CACHE_MAGIC
                  && hdr.version == ART_CACHE_FORMAT_VERSION
                  && hdr.layout == AA_ROWS && hdr.width == NV_THUMB
                  && hdr.height == NV_THUMB
                  && read(fd, px, bytes) == bytes;

        close(fd);
        return ok;
    }
    memset(&bm, 0, sizeof(bm));
    bm.width = bm.height = dim;
    bm.data = (unsigned char *)px;
    rc = art_cache_load_aat(fd, &bm, dim * dim * FB_DATA_SZ, NULL);
    close(fd);
    return rc > 0;
}

/* The banner as a band of the 300px picture `thumb`: where the owner moved
 * it, kept under `band_key`, else at row `band_default` */
static void nv_banner_band(const fb_data *thumb, unsigned int pic_key,
                           unsigned int band_key, int band_default)
{
    int band = NV_THUMB * NV_BAN_H / NV_BAN_W;
    int off = art_banner_offset(band_key);
    int top = off >= 0 ? off * NV_THUMB / 1024 : band_default;

    top = MAX(0, MIN(top, NV_THUMB - band));
    nv_scale(nv.banner, NV_BAN_W, NV_BAN_H, thumb + (size_t)top * NV_THUMB,
             NV_THUMB, band, NV_THUMB);
    nv.movable = true;
    nv.pic_key = pic_key;
    nv.band_key = band_key;
    nv.band_default = band_default;
}

/* A person's banner: their banner art, else the band of their picture the
 * owner chose, or the top third */
static bool nv_banner_person(const char *person)
{
    char path[MAX_PATH];
    unsigned int key = art_name_hash(person);
    int band = NV_THUMB * NV_BAN_H / NV_BAN_W;

    if (art_banner_person(person, path, sizeof(path)) && nv_banner_art(path))
        return true;
    if (!nv_thumb(key, (fb_data *)nv.work, NV_THUMB))
        return false;
    nv_banner_band((fb_data *)nv.work, key, key,
                   (NV_THUMB - band) * 85 / 256);
    return true;
}

/* An album's banner: its banner art, else a band across the middle of its
 * cover, which the art cache keeps under `pic_key` */
static bool nv_banner_album(const char *artist, const char *album,
                            unsigned int pic_key)
{
    char path[MAX_PATH];
    int band = NV_THUMB * NV_BAN_H / NV_BAN_W;

    if (art_banner_album(artist, album, path, sizeof(path))
        && nv_banner_art(path))
        return true;
    if (!nv_thumb(pic_key, (fb_data *)nv.work, NV_THUMB))
        return false;
    nv_banner_band((fb_data *)nv.work, pic_key,
                   art_name_work_hash(artist, album), (NV_THUMB - band) / 2);
    return true;
}

/* The page's banner, kept when it is the page on screen already. The work
 * area is a picture after this. */
static void nv_banner(void)
{
    const struct notes_doc *d = &nv.doc;
    uint32_t id = d->key * 31 + d->kind + (uint32_t)d->album * 7;

    nv.movable = false;
    if (!global_settings.liner_notes_banners)
    {
        nv.has_banner = false;
        return;
    }
    if (nv.slot.valid && nv.slot.id == id)
    {
        nv.has_banner = nv.slot.has_banner;
        nv.movable = nv.slot.movable;
        nv.pic_key = nv.slot.pic_key;
        nv.band_key = nv.slot.band_key;
        nv.band_default = nv.slot.band_default;
        return;
    }

    nv.has_banner = false;
    if (d->kind == NOTES_PAGE_ALBUM)
    {
        struct tagcache_album row;
        char artist[NV_NAME_MAX], album[NV_NAME_MAX];

        if (d->album >= 0 && tagcache_album_get(d->album, &row)
            && tagcache_seek_string(tag_albumartist, row.artist_seek, artist,
                                    sizeof(artist))
            && tagcache_seek_string(tag_album, row.album_seek, album,
                                    sizeof(album)))
            nv.has_banner = nv_banner_album(strcmp(artist, UNTAGGED)
                                            ? artist : NULL,
                                            album, row.art_hash);
    }
    else if (d->kind == NOTES_PAGE_PERSON && d->title[0])
        nv.has_banner = nv_banner_person(d->title);
    else if (d->kind == NOTES_PAGE_OTHER)
    {
        const struct notes_page *pg = notes_page_get(d->page);
        char path[MAX_PATH];

        nv.has_banner = pg && art_banner_page(pg->key, pg->by, path,
                                              sizeof(path))
                        && nv_banner_art(path);
    }

    nv.slot.valid = true;
    nv.slot.id = id;
    nv.slot.has_banner = nv.has_banner;
    nv.slot.movable = nv.movable;
    nv.slot.pic_key = nv.pic_key;
    nv.slot.band_key = nv.band_key;
    nv.slot.band_default = nv.band_default;
}

/* ---- a page: its Markdown, its lines and its links --------------------- */

/* The Markdown, read from memory */
struct nv_mem
{
    const char *p;
    size_t len, pos;
};

static long nv_mem_read(void *ctx, void *buf, size_t n)
{
    struct nv_mem *m = ctx;

    n = MIN(n, m->len - m->pos);
    memcpy(buf, m->p + m->pos, n);
    m->pos += n;
    return n;
}

static bs_off_t nv_mem_seek(void *ctx, bs_off_t off, int whence)
{
    struct nv_mem *m = ctx;
    bs_off_t at = whence == SEEK_CUR ? (bs_off_t)m->pos + off
                  : whence == SEEK_END ? (bs_off_t)m->len + off : off;

    if (at < 0 || at > (bs_off_t)m->len)
        return -1;
    m->pos = at;
    return at;
}

static bs_off_t nv_mem_size(void *ctx)
{
    return ((struct nv_mem *)ctx)->len;
}

static void nv_mem_close(void *ctx)
{
    (void)ctx;
}

/* `len` bytes of Markdown at `src` through the back end into the text */
static void nv_markdown(const char *src, size_t len, void *arena)
{
    static struct nv_mem m;
    ts_config cfg = ts_config_default();
    bs_io io = { nv_mem_read, nv_mem_seek, nv_mem_size, nv_mem_close, &m };
    ts_source *ts;
    long got;

    nv.text_len = 0;
    m.p = src;
    m.len = len;
    m.pos = 0;
    cfg.force_format = TS_FMT_MARKDOWN;
    if (ts_open(&ts, &io, "page.md", arena, TS_ARENA_PLAIN, &cfg) == BS_OK)
    {
        while (nv.text_len + 8 <= NV_TEXT_MAX
               && (got = ts_read(ts, (char *)nv.text + nv.text_len,
                                 NV_TEXT_MAX - nv.text_len)) > 0)
            nv.text_len += got;
        ts_close(ts);
    }
    nv.text[nv.text_len] = '\0';
}

static void nv_layout_init(struct text_layout *tl)
{
    memset(tl, 0, sizeof(*tl));
    tl->font = nv.font;
    tl->bold = nv.bold;
    tl->width = nv.tvp.width;
    tl->fg = nv.fg;
    tl->dim = nv.dim;
    /* `#` headings bold, or underlined when the theme has no bold font;
     * `##` and deeper underlined */
    tl->head_bold = nv.bold >= 0;
    tl->links = true;
    tl->link = nv.accent;
    tl->link_on = nv.bg;
    tl->current = nv.cur >= 0 ? nv.text + nv.links[nv.cur].off : NULL;
}

/* The page laid out whole: each line's place in the text and what it is */
static void nv_layout(void)
{
    struct text_layout tl;
    uint32_t off = 0;
    int y = 0, item = -1;       /* a list item's indent, while it wraps */
    bool heading = false, quote = false, dim = false, bold = false;
    bool minor = false;

    nv.cur = -1;
    nv_layout_init(&tl);
    nv.quote_indent = tl_literal_width(&tl, "M");
    nv.nlines = 0;
    while (off < nv.text_len && nv.nlines < NV_LINES_MAX)
    {
        unsigned char *s = nv.text + off;
        size_t avail = nv.text_len - off, i;
        struct nv_line *l = &nv.lines[nv.nlines];
        bool head = heading || (avail >= 3 && !memcmp(s, TL_HEAD_OPEN, 3));
        bool lesser = heading ? minor
                      : head && avail >= 6 && s[3] == 0xee && s[4] == 0x80
                        && s[5] == TL_MINOR;
        bool opens = avail >= 3 && !memcmp(s, TL_QUOTE, 3);
        bool pull = quote || opens;
        int indent = pull ? nv.quote_indent : 0, first, bullet = -1;

        if (item >= 0)
            indent += item;
        else
        {
            int skip = opens ? 3 : 0;

            item = tl_item_indent(&tl, s + skip, avail - skip,
                                  nv.quote_indent, &first, &bullet);
            item = item ? item : -1;
            if (bullet >= 0)
                bullet += indent;
            indent += first;
        }
        l->ln.bold = bold || (heading && !minor && tl.head_bold);
        tl_line_break(&tl, s, avail, nv.tvp.width - indent, &l->ln);
        if (l->ln.used == 0)
            break;
        l->ln.indent = indent;
        l->ln.full = false;
        l->ln.dim = dim;
        l->off = off;
        l->y = y;
        l->link = -1;
        l->bullet = bullet;
        y += l->ln.height;
        l->flags = (head ? NV_HEAD : 0) | (pull ? NV_PULL : 0)
                   | (dim ? NV_DIM : 0) | (lesser ? NV_MINOR : 0);

        for (i = 0; i + 3 <= l->ln.used; i++)
            if (s[i] == 0xee && s[i + 1] == 0x80)
            {
                if (s[i + 2] == 0x82 || s[i + 2] == 0x83)
                {
                    heading = s[i + 2] == 0x82;
                    minor = false;
                }
                else if (s[i + 2] == TL_MINOR)
                    minor = true;
                else if (s[i + 2] == TL_DIM_OPEN || s[i + 2] == TL_DIM_CLOSE)
                    dim = s[i + 2] == TL_DIM_OPEN;
                else if (s[i + 2] == TL_BOLD_OPEN
                         || s[i + 2] == TL_BOLD_CLOSE)
                    bold = s[i + 2] == TL_BOLD_OPEN;
            }
        quote = pull && s[l->ln.used - 1] != '\n';
        if (s[l->ln.used - 1] == '\n')
            item = -1;
        off += l->ln.used;
        nv.nlines++;
    }
}

/* The line holding byte `off` of the text */
static int nv_line_of(uint32_t off)
{
    int lo = 0, hi = nv.nlines - 1;

    while (lo < hi)
    {
        int mid = (lo + hi + 1) / 2;

        if (nv.lines[mid].off <= off)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

/* The line the page's heading whose name key is `key` starts on, or -1 */
static int nv_section_line(uint32_t key);

/* The lines each link starts and ends on, after the page is laid out */
static void nv_link_lines(void)
{
    int k, l;

    for (k = 0; k < nv.nlinks; k++)
    {
        struct nv_link *lk = &nv.links[k];

        lk->line = nv_line_of(lk->off);
        lk->last = nv_line_of(lk->end - 3);
        for (l = lk->line + 1; l <= lk->last; l++)
            nv.lines[l].link = k;
    }
}

/* Every name in the text laid out found: a link where it goes somewhere,
 * else plain text, its mark made one that draws nothing. A page longer than
 * NV_LINES_MAX lines has its names past the last line left alone, and names
 * past the NV_LINKS_MAX-th are plain. True when plain text was made wider,
 * and the page needs laying out again. */
#define NV_SHOWN_MAX 64

/* The no-break spaces of the qualifiers shown again, at `shown` (`n` of
 * them, in order), made spaces: the text closed up in one pass, the links
 * after them moved with it */
static void nv_unhide_spaces(const size_t *shown, int n)
{
    unsigned char *t = nv.text;
    size_t r = shown[0], w = r;
    int j = 0, k = 0;
    bool in = false;

    while (k < nv.nlinks && nv.links[k].off < r)
        k++;
    while (r <= nv.text_len)            /* the NUL too */
    {
        if (k < nv.nlinks && nv.links[k].off == r)
        {
            nv.links[k].end = w + (nv.links[k].end - nv.links[k].off);
            nv.links[k].off = w;
            k++;
        }
        if (j < n && r == shown[j])
        {
            in = true;
            j++;
            t[w++] = t[r++];            /* past the mark's first byte */
            continue;
        }
        if (in && t[r] == 0xee && r + 1 < nv.text_len && t[r + 1] == 0x80)
            in = false;
        if (in && t[r] == 0xc2 && t[r + 1] == 0xa0)
        {
            t[w++] = ' ';
            r += 2;
            continue;
        }
        t[w++] = t[r++];
    }
    nv.text_len = w - 1;
}

static bool nv_find_links(void)
{
    static char name[NV_NAME_MAX];
    static size_t shown[NV_SHOWN_MAX];  /* where qualifiers were unhidden */
    unsigned char *t = nv.text;
    size_t off, i, end = 0;
    int nshown = 0;
    bool wider = false;

    nv.nlinks = 0;
    if (nv.nlines)
        end = nv.lines[nv.nlines - 1].off + nv.lines[nv.nlines - 1].ln.used;
    for (off = 0; off + 3 <= end; off++)
    {
        size_t n = 0, close;
        struct nv_link *k = &nv.links[nv.nlinks];

        if (t[off] != 0xee || t[off + 1] != 0x80 || t[off + 2] != TL_NAME_OPEN)
            continue;
        for (close = off + 3; close + 3 <= nv.text_len; close++)
            if (t[close] == 0xee && t[close + 1] == 0x80
                && t[close + 2] == TL_NAME_CLOSE)
                break;
        if (close + 3 > nv.text_len)
            break;
        for (i = off + 3; i < close && n + 1 < sizeof(name); i++)
        {
            if (t[i] == 0xee && t[i + 1] == 0x80)
            {
                i += 2;
                continue;
            }
            /* a qualifier's no-break spaces are spaces in its name */
            if (t[i] == 0xc2 && t[i + 1] == 0xa0)
            {
                name[n++] = ' ';
                i++;
                continue;
            }
            name[n++] = t[i];
        }
        if (nv.nlinks < NV_LINKS_MAX
            && notes_link_find(name, n, nv.doc.artist, nv.doc.album, &k->t)
            && (k->t.to != NOTES_TO_SECTION
                || nv_section_line(k->t.key) >= 0))
        {
            k->off = off;
            k->end = close + 3;
            nv.nlinks++;
        }
        else
        {
            /* plain text, and a trailing "(...)" shown with it: it was
             * not a qualifier that found anything */
            t[off + 2] = TL_NAME_CLOSE;
            for (i = off + 3; i + 3 <= close; i++)
                if (t[i] == 0xee && t[i + 1] == 0x80
                    && (t[i + 2] == TL_HIDE_OPEN || t[i + 2] == TL_HIDE_CLOSE))
                {
                    if (t[i + 2] == TL_HIDE_OPEN && nshown < NV_SHOWN_MAX)
                        shown[nshown++] = i;
                    t[i + 2] = TL_NAME_CLOSE;
                    wider = true;
                }
        }
        off = close + 2;
    }
    if (nshown)
        nv_unhide_spaces(shown, nshown);
    return wider;
}

/* A heading's text from line `i`, its marks left out, into `buf` */
static void nv_heading_text(int i, char *buf, size_t size)
{
    const unsigned char *s = nv.text + nv.lines[i].off;
    size_t n = 0;

    while (*s && *s != '\n' && n + 1 < size)
    {
        if (s[0] == 0xee && s[1] == 0x80 && s[2])
        {
            if (s[2] == 0x83)
                break;                          /* the heading's end */
            if (s[2] == TL_HIDE_OPEN)           /* a name's qualifier */
                while (*s && *s != '\n'
                       && !(s[0] == 0xee && s[1] == 0x80
                            && s[2] == TL_HIDE_CLOSE))
                    s++;
            if (*s && *s != '\n')
                s += 3;
            continue;
        }
        buf[n++] = *s++;
    }
    buf[n] = '\0';
}

/* The disc a heading names -- "Disc 2", "Disk 2", "CD 2" -- or 0 */
static int nv_disc_heading(const char *p)
{
    static const char *const words[] = { "disc", "disk", "cd" };
    size_t i, n = 0;
    int disc = 0;

    for (i = 0; i < ARRAYLEN(words); i++)
    {
        n = strlen(words[i]);
        if (!strncasecmp(p, words[i], n))
            break;
    }
    if (i == ARRAYLEN(words))
        return 0;
    for (p += n; *p == ' ' || *p == '.'; p++)
        ;
    while (*p >= '0' && *p <= '9')
        disc = disc * 10 + (*p++ - '0');
    return *p ? 0 : disc;
}

/* ---- drawing ----------------------------------------------------------- */

/* The rows the progress line takes along the foot, 0 with it off */
static int nv_foot(void)
{
    return global_settings.liner_notes_progress ? NV_PROGRESS : 0;
}

static void nv_clamp_scroll(void)
{
    int content = nv.text_top + 8;

    if (nv.nlines > 0)
        content += nv.lines[nv.nlines - 1].y
                   + nv.lines[nv.nlines - 1].ln.height;
    nv.max_scroll = MAX(0, content - (LCD_HEIGHT - nv_foot()));
    nv.scroll = MAX(0, MIN(nv.scroll, nv.max_scroll));
}

static void nv_draw_progress(bool update)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct mp3entry *id3 = audio_current_track();
    int w = 0, y = LCD_HEIGHT - NV_PROGRESS;

    if (!nv_foot())
        return;
    if (id3 && id3->length > 0)
        w = (int)((long long)id3->elapsed * LCD_WIDTH / id3->length);
    w = MAX(0, MIN(w, LCD_WIDTH));
    d->set_viewport(&nv.vp);
    d->set_foreground(nv.track);
    d->fillrect(w, y, LCD_WIDTH - w, NV_PROGRESS);
    d->set_foreground(nv.accent);
    d->fillrect(0, y, w, NV_PROGRESS);
    d->set_foreground(nv.fg);
    if (update)
        d->update_viewport_rect(0, y, LCD_WIDTH, NV_PROGRESS);
}

/* `text` in font `font` at (x, y), clipped at `right` */
static void nv_put(const char *text, int font, int x, int y, int right,
                   unsigned colour)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport vp = nv.vp;

    vp.x = x;
    vp.width = MAX(0, right - x);
    vp.font = font;
    d->set_viewport(&vp);
    d->set_foreground(colour);
    d->putsxy(0, y, (const unsigned char *)text);
    d->set_viewport(&nv.vp);
}

/* Whether the large name has gone off the top, for the slim bar */
static bool nv_bar_shown(void)
{
    return nv.name_y - nv.scroll + nv.name_h <= NV_ROW - 4;
}

/* The large name, scrolled with the text; or, once it has gone off the top,
 * the slim bar */
static void nv_draw_title(void)
{
    struct screen *d = &screens[SCREEN_MAIN];
    int y = nv.name_y - nv.scroll;

    if (!nv_bar_shown())
    {
        nv_put(nv.doc.title, nv.name_font, NV_MARGIN_L, y,
               LCD_WIDTH - NV_MARGIN_R, nv.fg);
        return;
    }
    d->set_foreground(nv.bg);
    d->fillrect(0, 0, LCD_WIDTH, NV_ROW);
    d->set_foreground(nv.rule);
    d->hline(0, LCD_WIDTH - 1, NV_ROW - 1);
    nv_put(nv.doc.title, nv.font, NV_MARGIN_L, NV_ROW - 4 - nv.lh,
           LCD_WIDTH - NV_MARGIN_R, nv.fg);
}

/* The current link's box on line `i`, at `y` */
static void nv_draw_current(const struct text_layout *tl, int i, int y)
{
    const struct nv_link *k = &nv.links[nv.cur];
    struct nv_line *l = &nv.lines[i];
    uint32_t start = i == k->line ? k->off : l->off;
    uint32_t end = i == k->last ? k->end : l->off + l->ln.draw;
    bool bold = l->ln.bold;
    int x, w;

    if (end <= start)
        return;
    x = l->ln.indent + tl_text_width(tl, nv.text + l->off, start - l->off,
                                     &bold);
    w = tl_text_width(tl, nv.text + start, end - start, &bold);
    screens[SCREEN_MAIN].set_foreground(nv.accent);
    screens[SCREEN_MAIN].fillrect(x - 2, y, w + 4, l->ln.height);
}

static void nv_draw_lines(void)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct text_layout tl;
    int i;

    nv_layout_init(&tl);
    d->set_viewport(&nv.tvp);
    d->set_drawmode(DRMODE_FG);
    for (i = 0; i < nv.nlines; i++)
    {
        struct nv_line *l = &nv.lines[i];
        int y = nv.text_top + l->y - nv.scroll;
        unsigned colour;

        tl.fg = (l->flags & NV_HEAD) ? nv.head : nv.fg;
        colour = (l->flags & NV_DIM) ? nv.dim : tl.fg;
        if (y + l->ln.height <= 0)
            continue;
        if (y >= LCD_HEIGHT - nv_foot())
            break;
        if (nv.cur >= 0 && i >= nv.links[nv.cur].line
            && i <= nv.links[nv.cur].last)
            nv_draw_current(&tl, i, y);
        if (l->link >= 0)
            colour = l->link == nv.cur ? nv.bg : nv.accent;
        if (l->bullet >= 0)
        {
            d->set_foreground((l->flags & NV_DIM) ? nv.dim : tl.fg);
            tl_draw_bullet(&l->ln, l->bullet, y);
        }
        d->set_foreground(colour);
        if (l->ln.draw > 0)
            tl_draw_line(&tl, nv.text + l->off, &l->ln, y);
        if ((l->flags & NV_HEAD) && l->ln.draw > 0
            && (!tl.head_bold || (l->flags & NV_MINOR)))
        {
            d->set_foreground(nv.head);
            d->hline(l->ln.indent, l->ln.indent + l->ln.width - 1,
                     MIN(y + l->ln.ascent + 2, y + l->ln.height - 1));
        }
        if (l->flags & NV_PULL)
        {
            d->set_foreground(nv.quote);
            d->fillrect(0, y, 2, l->ln.height);
        }
    }
    d->set_foreground(nv.fg);
    d->set_viewport(&nv.vp);
}

/* The banner, scrolling with the text and faded into the page over the
 * NV_FADE rows above where the text begins */
static void nv_draw_banner(void)
{
    static fb_data row[NV_BAN_W];
    struct screen *d = &screens[SCREEN_MAIN];
    int back = nv.scroll;
    int edge = nv.text_top - 4 - nv.scroll;     /* where the text begins */
    int rows = MIN(NV_BAN_H - back, edge), solid = MIN(rows, edge - NV_FADE);
    /* The page's colour with its green apart from its red and blue, so one
     * multiply blends all three */
    uint32_t bg = ((uint32_t)nv.bg | ((uint32_t)nv.bg << 16)) & 0x07e0f81f;
    int y, x;

    if (rows <= 0)
        return;
    if (solid > 0)
        d->bitmap_part(nv.banner, 0, back, NV_BAN_W, 0, 0, NV_BAN_W, solid);
    for (y = MAX(0, solid); y < rows; y++)
    {
        const fb_data *src = nv.banner + (size_t)(y + back) * NV_BAN_W;
        uint32_t t = (y - (edge - NV_FADE)) * 32 / NV_FADE;
        uint32_t bgt = bg * t;

        for (x = 0; x < NV_BAN_W; x++)
        {
            uint32_t p = ((uint32_t)src[x] | ((uint32_t)src[x] << 16))
                         & 0x07e0f81f;

            p = ((p * (32 - t) + bgt) >> 5) & 0x07e0f81f;
            row[x] = (fb_data)(p | (p >> 16));
        }
        d->bitmap_part(row, 0, 0, NV_BAN_W, 0, y, NV_BAN_W, 1);
    }
}

static void nv_draw(void)
{
    struct screen *d = &screens[SCREEN_MAIN];

    d->set_viewport(&nv.vp);
    d->set_foreground(nv.fg);
    d->set_background(nv.bg);
    d->set_drawmode(DRMODE_SOLID);
    d->clear_viewport();
    /* Text is drawn over the banner as well as the page, so only its own
     * pixels: no box of background behind it */
    d->set_drawmode(DRMODE_FG);
    if (nv.has_banner)
        nv_draw_banner();
    nv_draw_lines();
    nv_draw_title();
    nv_draw_progress(false);
    d->set_drawmode(DRMODE_SOLID);
    d->update_viewport();
}

/* A message in place of a page */
static void nv_draw_message(int lang_id)
{
    struct screen *d = &screens[SCREEN_MAIN];
    const char *msg = str(lang_id);
    int w, h;

    d->set_viewport(&nv.vp);
    d->set_background(nv.bg);
    d->clear_viewport();
    d->getstringsize((const unsigned char *)msg, &w, &h);
    d->set_foreground(nv.dim);
    d->putsxy((LCD_WIDTH - w) / 2, (LCD_HEIGHT - h) / 2,
              (const unsigned char *)msg);
    d->set_foreground(nv.fg);
    d->update_viewport();
}

/* ---- the current link -------------------------------------------------- */

/* Link `i`'s top and bottom on the screen */
static int nv_link_top(int i)
{
    return nv.text_top + nv.lines[nv.links[i].line].y - nv.scroll;
}

static int nv_link_bottom(int i)
{
    const struct nv_line *l = &nv.lines[nv.links[i].last];

    return nv.text_top + l->y + l->ln.height - nv.scroll;
}

/* Link `i` whole on the screen: below the slim bar, above the progress
 * line */
static bool nv_link_shown(int i)
{
    return i >= 0 && i < nv.nlinks
           && nv_link_top(i) >= (nv_bar_shown() ? NV_ROW : 0)
           && nv_link_bottom(i) <= LCD_HEIGHT - nv_foot();
}

/* The link the reading line falls on, or the nearest within NV_NEAR lines
 * of it; at the top of a page the reading line is where the text begins.
 * Of two on one line, the first, or the last for `back`. -1 for none. */
static int nv_natural(bool back)
{
    int read = MAX(NV_READ, nv.text_top - nv.scroll);
    int i, best = -1, best_d = NV_NEAR * nv.lh + 1;

    for (i = 0; i < nv.nlinks; i++)
    {
        int top = nv_link_top(i), bottom = nv_link_bottom(i), dist;

        if (!nv_link_shown(i))
            continue;
        dist = read < top ? top - read : read >= bottom ? read - bottom + 1
                                                        : 0;
        if (dist < best_d || (back && dist == best_d && best >= 0
                              && nv.links[i].line == nv.links[best].line))
        {
            best = i;
            best_d = dist;
        }
    }
    return best;
}

/* At the end of a page with nothing current: the first link shown at or
 * below the reading line, else the last shown above it, or -1 */
static int nv_first_below(void)
{
    int read = MAX(NV_READ, nv.text_top - nv.scroll);
    int i, above = -1;

    for (i = 0; i < nv.nlinks; i++)
        if (nv_link_shown(i))
        {
            if (nv_link_bottom(i) > read)
                return i;
            above = i;
        }
    return above;
}

static bool nv_dirty;

/* Whether the links `a` and `b` share a line: the later starts on the line
 * the earlier ends on, which a link that wraps may not start on */
static bool nv_same_line(int a, int b)
{
    if (a < 0 || b < 0 || a >= nv.nlinks || b >= nv.nlinks)
        return false;
    return a < b ? nv.links[b].line == nv.links[a].last
                 : nv.links[a].line == nv.links[b].last;
}

/* The link to be current after the text scrolled in `dir`: `nat`, the one
 * the reading line now finds, unless that is behind `cur`, which stays
 * while it is on screen -- the current link never goes back against the
 * wheel */
static int nv_onward(int nat, int cur, int dir)
{
    if (cur >= 0 && nat >= 0 && (nat - cur) * dir < 0 && nv_link_shown(cur))
        return cur;
    return nat;
}

/* The wheel's last move, in pixels: a line a click, and more the faster it
 * turns, as the lists accelerate */
static int nv_wheel_px(void)
{
    return MAX(1, button_apply_acceleration(get_action_data())) * nv.lh;
}

/* A click of the wheel: on to the next link on the line if it is on screen,
 * else the text on, else -- at the end of a page, or a page that does not
 * scroll -- the current link on. A name that wraps joins two lines, so in a
 * list of names the next link "on the line" can be lines off screen. */
static void nv_wheel(int dir)
{
    int was = nv.scroll, cur = nv.cur;

    nv_boost();
    if (dir > 0)
    {
        if (nv_same_line(cur, cur + 1) && nv_link_shown(cur + 1))
            nv.cur++;
        else if (nv.scroll < nv.max_scroll)
        {
            nv.scroll += nv_wheel_px();
            nv_clamp_scroll();
            nv.cur = nv_onward(nv_natural(false), cur, 1);
        }
        else if (nv_link_shown(cur + 1))
            nv.cur++;
        else if (cur < 0 && nv.nlinks)
            nv.cur = nv_first_below();
    }
    else
    {
        int nat = nv_natural(true);

        /* on along the line, or the reading line comes back up */
        if (nv_link_shown(cur - 1)
            && (nv_same_line(cur, cur - 1) || (nat >= 0 && cur > nat)))
            nv.cur--;
        else if (nv.scroll > 0)
        {
            nv.scroll -= nv_wheel_px();
            nv_clamp_scroll();
            nv.cur = nv_onward(nv_natural(true), cur, -1);
        }
        else if (nv_link_shown(cur - 1))
            nv.cur--;
    }
    if (nv.scroll != was || nv.cur != cur)
        nv_dirty = true;
    /* The wheel clicks as a link becomes current, not as the text moves */
    if (nv.cur != cur && nv.cur >= 0)
        keyclick_click(true, BUTTON_SCROLL_FWD);
}

/* The keyclick callback: the wheel's own click left out (nv_wheel() makes
 * its own) */
static bool nv_keyclick(int action, void *data)
{
    (void)data;
    return action != ACTION_WPS_VOLUP && action != ACTION_WPS_VOLDOWN;
}

/* The scroll that puts line `i` on the reading line */
static void nv_scroll_to(int i)
{
    nv.scroll = nv.text_top + nv.lines[i].y - (NV_READ - nv.lh / 2);
    nv_clamp_scroll();
}

/* ---- Move Banner and Reset Banner -------------------------------------- */

/* The band's top in rows of the 300px picture, as banners.dat keeps it in
 * 1/1024ths: rounded up, so reading it back gives the same row */
static int nv_offset_of(int top)
{
    return (top * 1024 + NV_THUMB - 1) / NV_THUMB;
}

/* The whole picture, the band the banner shows outlined and the rest dimmed.
 * The wheel moves the band; Select or Right saves it, Menu or Left leaves it
 * as it was. True when USB was plugged in. */
static bool nv_move_banner(void)
{
    static fb_data row[NV_MOVE_SHOW];
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport vp;
    fb_data *pic = (fb_data *)nv.work;
    unsigned int key = nv.band_key;
    int band = NV_THUMB * NV_BAN_H / NV_BAN_W, off, top;
    int x0 = (LCD_WIDTH - NV_MOVE_SHOW) / 2;
    bool usb = false, done = false;

    if (!nv_thumb(nv.pic_key, pic, NV_MOVE_SHOW))
        return false;
    off = art_banner_offset(key);
    top = off >= 0 ? off * NV_THUMB / 1024 : nv.band_default;

    viewportmanager_theme_enable(SCREEN_MAIN, false, &vp);
    vp.fg_pattern = nv.fg;
    vp.bg_pattern = nv.bg;
    vp.font = nv.font;
    while (!done)
    {
        int b0, bh, y, x, action;

        top = MAX(0, MIN(top, NV_THUMB - band));
        b0 = top * NV_MOVE_SHOW / NV_THUMB;
        bh = band * NV_MOVE_SHOW / NV_THUMB;

        d->set_viewport(&vp);
        d->set_drawmode(DRMODE_SOLID);
        d->clear_viewport();
        for (y = 0; y < NV_MOVE_SHOW; y++)
        {
            const fb_data *src = pic + (size_t)y * NV_MOVE_SHOW;

            if (y >= b0 && y < b0 + bh)
            {
                d->bitmap_part(pic, 0, y, NV_MOVE_SHOW, x0, y, NV_MOVE_SHOW,
                               1);
                continue;
            }
            for (x = 0; x < NV_MOVE_SHOW; x++)
                row[x] = color_blend(src[x], LCD_BLACK, 150);
            d->bitmap_part(row, 0, 0, NV_MOVE_SHOW, x0, y, NV_MOVE_SHOW, 1);
        }
        d->set_foreground(nv.accent);
        d->drawrect(x0, b0, NV_MOVE_SHOW, bh);
        d->drawrect(x0 + 1, b0 + 1, NV_MOVE_SHOW - 2, bh - 2);
        d->update_viewport();

        action = get_action(CONTEXT_STD, TIMEOUT_BLOCK);
        switch (action)
        {
            case ACTION_STD_PREV:
            case ACTION_STD_PREVREPEAT:
                top -= NV_MOVE_STEP;
                break;
            case ACTION_STD_NEXT:
            case ACTION_STD_NEXTREPEAT:
                top += NV_MOVE_STEP;
                break;
            case ACTION_STD_OK:
                art_banner_set_offset(key, nv_offset_of(top));
                done = true;
                break;
            case ACTION_STD_MENU:
            case ACTION_STD_CANCEL:
                done = true;
                break;
            default:
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                    usb = done = true;
                break;
        }
    }
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    return usb;
}

/* ---- the menus: a link's, and the page's ------------------------------- */

/* What the menu is about -- the current link (Select held), or the page
 * itself (Menu held), which offers its banner's rows as well -- and what it
 * chose */
static struct notes_target nv_mt;
static bool nv_mt_page;

enum { NV_DO_NONE, NV_DO_PLAY, NV_DO_NEXT, NV_DO_LAST, NV_DO_GO,
       NV_DO_BANNER, NV_DO_PAGE };
static int nv_do;
static int nv_do_page;          /* NV_DO_PAGE's, into nv.pages */

/* USB plugged in during Move Banner, for nv_run_menu() to leave on */
static bool nv_usb;

static int nv_do_play(void) { nv_do = NV_DO_PLAY; return 1; }
static int nv_do_next(void) { nv_do = NV_DO_NEXT; return 1; }
static int nv_do_last(void) { nv_do = NV_DO_LAST; return 1; }
static int nv_do_go(void)   { nv_do = NV_DO_GO; return 1; }

static int nv_move_run(void)
{
    nv_usb = nv_move_banner();
    nv_do = NV_DO_BANNER;
    return 1;
}

static int nv_reset_run(void)
{
    art_banner_set_offset(nv.band_key, -1);
    nv_do = NV_DO_BANNER;
    return 1;
}

static const struct notes_target *nv_current(void)
{
    return nv.cur >= 0 ? &nv.links[nv.cur].t : NULL;
}

/* Whether the menu about `t` has a row: to play an album or a track, or to
 * go to a library artist; the page's always has the settings */
static bool nv_has_rows(const struct notes_target *t, bool page)
{
    return page || t->album >= 0 || notes_is_artist(t->artist_seek);
}

/* Move Banner on the page's menu, over a band of a picture */
static int nv_move_callback(int action, const struct menu_item_ex *this_item,
                            struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM && !(nv_mt_page && nv.movable))
        return ACTION_EXIT_MENUITEM;
    return action;
}

/* Reset Banner, only once the banner has been moved */
static int nv_reset_callback(int action, const struct menu_item_ex *this_item,
                             struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM
        && !(nv_mt_page && nv.movable && art_banner_offset(nv.band_key) >= 0))
        return ACTION_EXIT_MENUITEM;
    return action;
}

/* Play and the queue's rows: for an album or a track */
static int nv_play_callback(int action, const struct menu_item_ex *this_item,
                            struct gui_synclist *this_list)
{
    const struct notes_target *t = &nv_mt;

    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM && t->album < 0)
        return ACTION_EXIT_MENUITEM;
    return action;
}

/* Go to Album for an album, Go to Artist for a library artist */
static int nv_go_album_callback(int action,
                                const struct menu_item_ex *this_item,
                                struct gui_synclist *this_list)
{
    const struct notes_target *t = &nv_mt;

    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM
        && !(t->album >= 0 && t->to != NOTES_TO_TRACK))
        return ACTION_EXIT_MENUITEM;
    return action;
}

static int nv_go_artist_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list)
{
    const struct notes_target *t = &nv_mt;

    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM
        && !(t->album < 0 && notes_is_artist(t->artist_seek)))
        return ACTION_EXIT_MENUITEM;
    return action;
}

/* The album's pages, at the top of the page's menu */
static int nv_page_run(void *param)
{
    nv_do = NV_DO_PAGE;
    nv_do_page = (intptr_t)param;
    return 1;
}

static char *nv_page_name(int selected_item, void *data, char *buffer,
                          size_t buffer_len)
{
    (void)selected_item;
    (void)buffer;
    (void)buffer_len;
    return nv.page_names[(intptr_t)data];
}

static int nv_page_callback(int action, const struct menu_item_ex *this_item,
                            struct gui_synclist *this_list)
{
    intptr_t i = (intptr_t)this_item->menu_get_name_and_icon
                     ->list_get_name_data;

    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM && !(nv_mt_page && i < nv.npages))
        return ACTION_EXIT_MENUITEM;
    return action;
}

#define NV_PAGE_ITEM(name, i)                                               \
    MENUITEM_FUNCTION_DYNTEXT_W_PARAM(name, MENU_FUNC_CHECK_RETVAL,         \
                                      nv_page_run, (void *)(i),             \
                                      nv_page_name, NULL, (void *)(i),      \
                                      nv_page_callback, Icon_NOICON)
NV_PAGE_ITEM(nv_page_0, 0)
NV_PAGE_ITEM(nv_page_1, 1)
NV_PAGE_ITEM(nv_page_2, 2)
NV_PAGE_ITEM(nv_page_3, 3)
NV_PAGE_ITEM(nv_page_4, 4)
NV_PAGE_ITEM(nv_page_5, 5)
NV_PAGE_ITEM(nv_page_6, 6)
NV_PAGE_ITEM(nv_page_7, 7)
_Static_assert(NV_PAGES_MAX == 8, "a menu row for each of an album's pages");

MENUITEM_FUNCTION(nv_play_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_PLAY),
                  nv_do_play, nv_play_callback, Icon_NOICON);
MENUITEM_FUNCTION(nv_next_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_PLAY_NEXT),
                  nv_do_next, nv_play_callback, Icon_NOICON);
MENUITEM_FUNCTION(nv_last_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_PLAY_LAST),
                  nv_do_last, nv_play_callback, Icon_NOICON);
MENUITEM_FUNCTION(nv_go_album_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_GO_TO_ALBUM), nv_do_go, nv_go_album_callback,
                  Icon_NOICON);
MENUITEM_FUNCTION(nv_go_artist_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_GO_TO_ARTIST), nv_do_go, nv_go_artist_callback,
                  Icon_NOICON);
MENUITEM_FUNCTION(nv_move_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_NOTES_MOVE_BANNER), nv_move_run, nv_move_callback,
                  Icon_NOICON);
MENUITEM_FUNCTION(nv_reset_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_NOTES_RESET_BANNER), nv_reset_run,
                  nv_reset_callback, Icon_NOICON);
/* The settings, on the page's menu and not a link's */
static int nv_setting_callback(int action, const struct menu_item_ex *this_item,
                               struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM && !nv_mt_page)
        return ACTION_EXIT_MENUITEM;
    return action;
}

/* Custom's rows, while Custom is chosen */
static int nv_custom_callback(int action, const struct menu_item_ex *this_item,
                              struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM
        && !(nv_mt_page
             && global_settings.liner_notes_colours == NOTES_COLOURS_CUSTOM))
        return ACTION_EXIT_MENUITEM;
    return action;
}

MENUITEM_SETTING(nv_colours_item, &global_settings.liner_notes_colours,
                 nv_setting_callback);
MENUITEM_SETTING(nv_text_item, &global_settings.liner_notes_text,
                 nv_custom_callback);
MENUITEM_SETTING(nv_links_item, &global_settings.liner_notes_links,
                 nv_custom_callback);
MENUITEM_SETTING(nv_page_item, &global_settings.liner_notes_page,
                 nv_custom_callback);
MENUITEM_SETTING(nv_dim_item, &global_settings.liner_notes_dim,
                 nv_custom_callback);
MENUITEM_SETTING(nv_heads_item, &global_settings.liner_notes_heads,
                 nv_custom_callback);
MENUITEM_SETTING(nv_quote_item, &global_settings.liner_notes_quote,
                 nv_custom_callback);
MENUITEM_SETTING(nv_banners_item, &global_settings.liner_notes_banners,
                 nv_setting_callback);
MENUITEM_SETTING(nv_progress_item, &global_settings.liner_notes_progress,
                 nv_setting_callback);
MAKE_MENU(nv_menu, ID2P(LANG_ONPLAY_MENU_TITLE), NULL, Icon_NOICON,
          &nv_page_0, &nv_page_1, &nv_page_2, &nv_page_3, &nv_page_4,
          &nv_page_5, &nv_page_6, &nv_page_7, &nv_play_item, &nv_next_item, &nv_last_item, &nv_go_album_item,
          &nv_go_artist_item, &nv_move_item, &nv_reset_item, &nv_colours_item,
          &nv_text_item, &nv_links_item, &nv_page_item, &nv_dim_item,
          &nv_heads_item, &nv_quote_item, &nv_banners_item,
          &nv_progress_item);

/* The settings the page's menu offers, to see whether one changed */
static uint32_t nv_settings_sum(void)
{
    const struct user_settings *s = &global_settings;
    uint32_t sum = s->liner_notes_colours
                   | s->liner_notes_text << 3 | s->liner_notes_links << 6
                   | s->liner_notes_page << 9 | s->liner_notes_dim << 12
                   | s->liner_notes_heads << 15 | s->liner_notes_quote << 18
                   | s->liner_notes_banners << 21
                   | s->liner_notes_progress << 22;
    unsigned i;

    for (i = 0; i < ARRAYLEN(s->liner_notes_rgb); i++)
        sum = sum * 31 + s->liner_notes_rgb[i];
    return sum;
}

/* Chosen Colour's picker for Custom's row `which` (liner_notes_rgb's
 * order), when `chosen` */
static void nv_choose(int which, bool chosen)
{
    static const int titles[] = {
        LANG_TEXT_VIEWER_TEXT_COLOUR, LANG_NOTES_LINK_COLOUR,
        LANG_TEXT_VIEWER_PAGE_COLOUR, LANG_NOTES_DIM_COLOUR,
        LANG_NOTES_HEADING_COLOUR, LANG_NOTES_QUOTE_COLOUR,
    };

    if (chosen && !set_color(str(titles[which]),
                             (unsigned *)&global_settings.liner_notes_rgb[which],
                             (unsigned)-1))
        settings_save();
}

void notes_chose_text(int value)
{
    nv_choose(0, value == NOTES_FROM_CHOSEN);
}

void notes_chose_links(int value)
{
    nv_choose(1, value == NOTES_FROM_CHOSEN);
}

void notes_chose_page(int value)
{
    nv_choose(2, value == NOTES_FROM_CHOSEN);
}

void notes_chose_dim(int value)
{
    nv_choose(3, value == NOTES_FROM_CHOSEN + 1);
}

void notes_chose_heads(int value)
{
    nv_choose(4, value == NOTES_FROM_CHOSEN + 1);
}

void notes_chose_quote(int value)
{
    nv_choose(5, value == NOTES_FROM_CHOSEN + 1);
}

/* A menu with the theme on for it, as the lyric viewer's is. True when USB
 * was plugged in. */
static bool nv_run_menu(const struct menu_item_ex *menu)
{
    int sel = 0, ret;

    nv_usb = false;
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    push_current_activity(ACTIVITY_CONTEXTMENU);
    ret = do_menu(menu, &sel, NULL, false);
    pop_current_activity();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    return ret == MENU_ATTACHED_USB || nv_usb;
}

/* ---- opening pages, and going back ------------------------------------- */

/* The page of the track or album a request names by its names and a
 * track's path */
static bool nv_album_target(const struct notes_album *a, struct notes_target *t,
                            int *focus)
{
    struct notes_pages pages;
    int idx = a->path[0] ? tagcache_find_path(a->path) : -1;
    int album = idx >= 0 ? tagcache_album_of(idx) : -1;

    memset(t, 0, sizeof(*t));
    t->page = t->album = t->track = -1;
    t->artist_seek = -1;
    if (album >= 0)
    {
        t->to = NOTES_TO_ALBUM;
        t->album = album;
        if (focus)
            *focus = idx;
        return true;
    }
    /* No database in RAM: the written pages alone */
    if (!notes_find_names(a->albumartist, a->artist, a->album, a->path,
                          &pages))
        return false;
    t->to = NOTES_TO_PAGE;
    t->page = pages.have & NOTES_ALBUM ? pages.album : pages.bio;
    return true;
}

/* The page `r` asks for, made whole into the buffer, its target into
 * nv.t; false when there is none, nv.t and nv.doc left as they were. The
 * work area holds a picture after.
 *
 * The library's albums go first in the work area after the page's own
 * buffers, 24 bytes each, then the names index's cache in what is left: on
 * the 5G the two share about 250 KB. Past about 10,000 albums the albums do
 * not fit, and nothing in the library links. */
/* The page's album's pages for its menu: the album's own, unless this is
 * it, then those whose front matter names the album */
static void nv_album_pages(struct name_index *ix)
{
    int parts[NV_PAGES_MAX], i, n;

    nv.npages = 0;
    n = notes_album_parts(&nv.doc, ix, parts, NV_PAGES_MAX - 1);
    if (nv.doc.part)
    {
        struct notes_target *t = &nv.pages[nv.npages];

        memset(t, 0, sizeof(*t));
        t->to = NOTES_TO_ALBUM;
        t->album = nv.doc.album;
        t->page = t->track = -1;
        t->artist_seek = -1;
        notes_album_title(t->album, nv.page_names[nv.npages],
                          sizeof(nv.page_names[0]));
        nv_utf8_cut(nv.page_names[nv.npages++]);
    }
    for (i = 0; i < n; i++)
    {
        const struct notes_page *pg = notes_page_get(parts[i]);
        struct notes_target *t = &nv.pages[nv.npages];

        if (!pg)
            continue;
        memset(t, 0, sizeof(*t));
        t->to = NOTES_TO_PAGE;
        t->page = parts[i];
        t->album = t->track = -1;
        t->artist_seek = -1;
        t->key = pg->key;
        notes_page_title(pg, nv.page_names[nv.npages],
                         sizeof(nv.page_names[0]));
        nv_utf8_cut(nv.page_names[nv.npages++]);
    }
}

/* The link name that finds the page again: an album by its artist and title,
 * a written page by its file's name, else the name it is shown by */
static void nv_stamp(void)
{
    const struct notes_page *pg = notes_page_get(nv.doc.page);
    char artist[128], title[128];
    const char *qual = "";
    int kind = pg ? pg->kind : nv.doc.kind;

    if (nv.doc.kind == NOTES_PAGE_ALBUM && nv.doc.album >= 0 && !nv.doc.part
        && notes_album_title(nv.doc.album, title, sizeof(title)))
    {
        if (notes_album_artist(nv.doc.album, artist, sizeof(artist)))
            snprintf(nv.refind, sizeof(nv.refind), "%s - %s (album)", artist,
                     title);
        else
            snprintf(nv.refind, sizeof(nv.refind), "%s (album)", title);
        return;
    }
    if (pg)
        notes_page_name(pg, title, sizeof(title));
    else
        strlcpy(title, nv.doc.title, sizeof(title));
    if (kind == NOTES_PAGE_PERSON)
        qual = " (person)";
    else if (kind == NOTES_PAGE_ALBUM)
        qual = " (album)";
    snprintf(nv.refind, sizeof(nv.refind), "%s%s", title, qual);
}

static bool nv_build(const struct nv_req *r)
{
    static struct notes_target t;
    static struct notes_doc doc;
    char *w = nv.work, *src, *file, *arena;
    size_t left = nv.work_size - NV_BUILD_BYTES, need, len;
    void *albums = NULL;
    bool have_ix;
    int focus = -1, i;
    /* What the page is made under, read before anything here yields: an
     * Update committing mid-build then leaves the page stale, not stamped
     * with numbers it was never built on */
    int32_t commit = tagcache_commit_id();
    uint32_t gen = tagcache_generation();
    unsigned int pages = notes_pages_generation();

    nv_boost();
    src = w;
    file = src + NV_SRC_MAX;
    arena = file + NV_FILE_MAX;
    w = arena + TS_ARENA_PLAIN;
    need = ALIGN_UP(notes_links_need(), 4);
    if (need && need <= left)
    {
        albums = w;
        w += need;
        left -= need;
    }
    else if (need)
        debug_log(DEBUG_LOG_TASKS, "notes: %lu bytes of albums, %lu free",
                  (unsigned long)need, (unsigned long)left);
    have_ix = name_index_open(w, left, &nv.ix);
    notes_links_begin(albums, albums ? need : 0, have_ix ? &nv.ix : NULL);

    if (r->by == NV_BY_TARGET)
        t = r->t;
    else if (r->by == NV_BY_ALBUM)
    {
        if (!nv_album_target(r->album, &t,
                             r->focus == NV_FOCUS_TRACK ? &focus : NULL))
            goto none;
    }
    else
    {
        char name[NV_NAME_MAX];

        if (r->by == NV_BY_LINK)
            strlcpy(name, r->name, sizeof(name));
        else
            snprintf(name, sizeof(name), "%s (person)", r->name);
        if (!notes_link_find(name, strlen(name), 0, -1, &t))
            goto none;
    }
    if (t.to == NOTES_TO_TRACK)
        focus = t.track;

    len = notes_source(&t, &doc, have_ix ? &nv.ix : NULL, src, NV_SRC_MAX,
                       file, NV_FILE_MAX);
    if (!len)
        goto none;
    nv.t = t;
    nv.doc = doc;
    nv_utf8_cut(nv.doc.title);
    nv.built_commit = commit;
    nv.built_gen = gen;
    nv.built_pages = pages;
    nv_stamp();
    nv_album_pages(have_ix ? &nv.ix : NULL);
    nv_markdown(src, len, arena);
    nv_layout();
    if (nv_find_links())
        nv_layout();
    nv_link_lines();

    /* A track: its section of the notes, else its row in the track list */
    nv.scroll = 0;
    if (focus >= 0)
    {
        char title[NV_NAME_MAX], text[NV_NAME_MAX];
        uint32_t key = tagcache_entry_string(focus, tag_title, title,
                                             sizeof(title))
                       ? art_name_hash(title) : 0;
        int line = -1, disc = 0;
        long track_disc = 0;

        if (!tagcache_entry_numeric(focus, tag_discnumber, &track_disc)
            || track_disc < 1)
            track_disc = 1;
        for (i = 0; key && i < nv.nlines && line < 0; i++)
            if ((nv.lines[i].flags & NV_HEAD)
                && !memcmp(nv.text + nv.lines[i].off, TL_HEAD_OPEN, 3))
            {
                nv_heading_text(i, text, sizeof(text));
                if (nv_disc_heading(text))
                    disc = nv_disc_heading(text);
                else if ((!disc || disc == track_disc)
                         && art_name_hash(notes_numbered(text)) == key)
                    line = i;
            }
        for (i = 0; line < 0 && i < nv.nlinks; i++)
            if (nv.links[i].t.to == NOTES_TO_TRACK
                && nv.links[i].t.track == focus)
            {
                line = nv.links[i].line;
                nv.cur = i;
            }
        if (line >= 0)
            nv.focus_line = line;
    }
    notes_links_end();
    name_index_close(&nv.ix);
    return true;

  none:
    notes_links_end();
    name_index_close(&nv.ix);
    return false;
}

/* The page `r` asks for, on screen at the top or at its focus; false when
 * there is none, the page on screen left as it was */
static bool nv_open(const struct nv_req *r)
{
    int block;

    nv.focus_line = -1;
    if (!nv_build(r))
        return false;
    nv_art_palette();
    nv_colours();
    nv_banner();
    block = nv.name_h;
    nv.name_y = nv.has_banner ? NV_BAN_H - 6 - block : 8;
    nv.text_top = nv.name_y + block + 10;
    nv.scroll = 0;
    nv_clamp_scroll();
    if (nv.focus_line >= 0)
        nv_scroll_to(nv.focus_line);
    if (nv.cur < 0)
        nv.cur = nv_natural(false);
    return true;
}

static bool nv_open_target(const struct notes_target *t)
{
    struct nv_req r = { NV_BY_TARGET, *t, NULL, NULL, -1 };

    return nv_open(&r);
}

/* Whether the database or the page index has changed since the page was
 * made, so that its album rows, tracks and page numbers no longer hold */
static bool nv_stale(void)
{
    return tagcache_commit_id() != nv.built_commit
           || tagcache_generation() != nv.built_gen
           || notes_pages_generation() != nv.built_pages;
}

/* The page found again by its name, and the history let go, its numbers no
 * longer holding; false when the page is gone */
static bool nv_refind(void)
{
    static char name[NV_NAME_MAX];
    struct nv_req r = { NV_BY_LINK, { 0 }, NULL, name, -1 };

    nv.nhist = 0;
    strlcpy(name, nv.refind, sizeof(name));
    return name[0] && nv_open(&r);
}

/* Before a press acts on the page's numbers: 1 when they hold; 0 when the
 * page was made again and drawn instead, for the press to be made on it;
 * -1 when it is gone and the screen is to be left */
static int nv_fresh(void)
{
    if (!nv_stale())
        return 1;
    if (!nv_refind())
        return -1;
    nv.cur = nv_natural(false);
    nv_draw();
    return 0;
}

/* `h` onto the history, its oldest let go when it is full */
static void nv_push(const struct nv_hist *h)
{
    if (nv.nhist == NV_HISTORY)
    {
        memmove(&nv.hist[0], &nv.hist[1],
                sizeof(nv.hist[0]) * (NV_HISTORY - 1));
        nv.nhist--;
    }
    nv.hist[nv.nhist++] = *h;
}

/* Following a link: the page left goes on the history */
static void nv_follow(const struct notes_target *t)
{
    struct nv_hist h = { nv.t, nv.scroll, nv.cur };

    if (!nv_open_target(t))
        return;
    nv_push(&h);
    nv_draw();
}

static int nv_section_line(uint32_t key)
{
    char text[NV_NAME_MAX];
    int i;

    for (i = 0; i < nv.nlines; i++)
        if ((nv.lines[i].flags & NV_HEAD)
            && !memcmp(nv.text + nv.lines[i].off, TL_HEAD_OPEN, 3))
        {
            nv_heading_text(i, text, sizeof(text));
            if (art_name_hash(text) == key)
                return i;
        }
    return -1;
}

/* A link to a heading of this page: the page left where it is goes on the
 * history, and the heading whose name key is `key` comes to the reading
 * line */
static void nv_to_section(uint32_t key)
{
    struct nv_hist h = { nv.t, nv.scroll, nv.cur };
    int i = nv_section_line(key);

    if (i < 0)
        return;
    nv_push(&h);
    nv_scroll_to(i);
    nv.cur = nv_natural(false);
    nv_draw();
}

/* Back to the page before, where it was left; false when there is none */
static bool nv_back(void)
{
    struct nv_hist h;

    if (!nv.nhist)
        return false;
    h = nv.hist[--nv.nhist];
    if (nv_open_target(&h.t))
    {
        nv.scroll = h.scroll;
        nv_clamp_scroll();
        nv.cur = h.cur >= 0 && h.cur < nv.nlinks ? h.cur : nv_natural(false);
    }
    nv_draw();
    return true;
}

/* Whether the album row `album` is the one playing */
static bool nv_playing_album(int album)
{
    struct mp3entry *id3 = audio_current_track();
    int idx;

    if (!id3 || !id3->path[0] || !(audio_status() & AUDIO_STATUS_PLAY))
        return false;
    idx = tagcache_find_path(id3->path);
    return idx >= 0 && tagcache_album_of(idx) == album;
}

/* The path of track `idx` into `buf` */
static bool nv_track_path(int idx, char *buf, size_t size)
{
    struct tagcache_search tcs;
    bool ok;

    if (idx < 0 || !tagcache_search(&tcs, tag_filename))
        return false;
    ok = tagcache_retrieve(&tcs, idx, tag_filename, buf, size);
    tagcache_search_finish(&tcs);
    return ok;
}

static bool nv_album_data(int album, struct album_data *a)
{
    struct tagcache_album row;

    if (!tagcache_album_get(album, &row))
        return false;
    memset(a, 0, sizeof(*a));
    a->seek = row.album_seek;
    a->artist_seek = row.artist_seek;
    return true;
}

/* An album's tracks, or one track, after the playing one or at the end */
static bool nv_cover_sink(const char *path, void *data)
{
    return playlist_insert_context_add(data, path) >= 0;
}

static void nv_insert(const struct notes_target *t, int position)
{
    char path[MAX_PATH];
    struct album_data a;
    struct playlist_insert_context ctx;

    if (t->to == NOTES_TO_TRACK)
    {
        if (nv_track_path(t->track, path, sizeof(path)))
            playlist_insert_track(NULL, path, position, false, true);
        return;
    }
    if (!nv_album_data(t->album, &a))
        return;
    if (playlist_insert_context_create(NULL, &ctx, position, false,
                                       false) < 0)
    {
        /* create() keeps the playlist lock even when it fails; release()
         * is the only thing that gives it back. */
        playlist_insert_context_release(&ctx);
        return;
    }
    db_summary_album_tracks(&a, nv_cover_sink, &ctx);
    playlist_insert_context_release(&ctx);
}

/* ---- the screen, and its entry points ---------------------------------- */

/* Whether the screen holds the app buffer: a claim that fails after the
 * queue let it go leaves nothing for the screen's end to release */
static bool nv_claimed;

/* The app buffer, carved up. False with no room. */
static bool nv_claim(void)
{
    size_t size;
    char *buf = app_try_claim_buffer(&size, "notes");
    char *p;

    if (!buf || size < NV_BUF_BYTES)
    {
        if (buf)
            app_release_buffer("notes");
        splash(HZ * 2, ID2P(LANG_OUT_OF_MEMORY));
        return false;
    }
    p = (char *)ALIGN_UP((uintptr_t)buf, 4);
    nv.banner = (fb_data *)p;
    p += NV_BANNER_BYTES;
    nv.text = (unsigned char *)p;
    p += ALIGN_UP(NV_TEXT_MAX + 1, 4);
    nv.lines = (struct nv_line *)p;
    p += NV_LINES_MAX * sizeof(struct nv_line);
    nv.links = (struct nv_link *)p;
    p += NV_LINKS_MAX * sizeof(struct nv_link);
    nv.work = p;
    nv.work_size = buf + size - p;
    nv.slot.valid = false;
    nv.nlines = nv.nlinks = 0;
    nv.cur = -1;
    nv_dirty = false;
    nv_claimed = true;
    return true;
}

static void nv_release(void)
{
    nv_unboost(true);
    if (nv_claimed)
        app_release_buffer("notes");
    nv_claimed = false;
}

/* The page on screen made again, where it was: after the buffer was let go
 * for the queue, or a banner was moved. False when it can no longer be
 * made -- its file gone, say -- and the screen is to be left. */
/* Whether a page made while the library was out can be made with it now:
 * once the database is in RAM, or has done nothing for NV_QUIET polls, as
 * it does not go straight from scanning to loading. Asked once a poll. */
#define NV_QUIET 4

static bool nv_library_back(void)
{
    static int quiet;

    if (tagcache_is_in_ram())
        return true;
    quiet = notes_library_waiting() ? 0 : quiet + 1;
    if (quiet < NV_QUIET)
        return false;
    quiet = 0;
    return true;
}

static bool nv_reopen(void)
{
    int scroll = nv.scroll, cur = nv.cur;

    nv.slot.valid = false;
    if (nv_stale() ? !nv_refind() : !nv_open_target(&nv.t))
        return false;
    nv.scroll = scroll;
    nv_clamp_scroll();
    nv.cur = cur >= 0 && cur < nv.nlinks ? cur : nv_natural(false);
    return true;
}

/* The menu of the current link, or of the page for `page`: play what it
 * names, go to it in the library, or move the page's banner, or change the
 * settings. Nothing at all when it would have no rows. A GO_TO_* code when
 * the screen is to be left, else -1. */
static int nv_menu_run(bool page)
{
    const struct notes_target *t = &nv_mt;
    struct tagcache_album row;
    char name[TAGCACHE_BUFSZ];
    uint32_t sum = nv_settings_sum();

    if (page)
    {
        memset(&nv_mt, 0, sizeof(nv_mt));
        nv_mt.to = NOTES_TO_PAGE;
        nv_mt.page = nv.doc.page;
        nv_mt.track = -1;
        nv_mt.album = nv.doc.kind == NOTES_PAGE_ALBUM ? nv.doc.album : -1;
        nv_mt.artist_seek = nv.doc.kind == NOTES_PAGE_PERSON
                            ? nv.doc.artist_seek : -1;
    }
    else if (nv_current())
        nv_mt = *nv_current();
    else
        return -1;
    nv_mt_page = page;
    if (!nv_has_rows(t, page))
        return -1;
    nv_do = NV_DO_NONE;
    if (nv_run_menu(&nv_menu))
        return GO_TO_ROOT;
    /* The library or the pages changed while the menu was up: what it chose
     * may no longer be what it showed */
    if (nv_do != NV_DO_NONE && nv_do != NV_DO_BANNER)
    {
        int fresh = nv_fresh();

        if (fresh < 0)
            return GO_TO_PREVIOUS;
        if (fresh == 0)
            return -1;
    }
    switch (nv_do)
    {
        case NV_DO_PLAY:
            nv.play_album = t->album;
            nv.play_track = t->to == NOTES_TO_TRACK ? t->track : -1;
            return GO_TO_WPS;
        case NV_DO_NEXT:
        case NV_DO_LAST:
        {
            struct notes_target keep = *t;
            int cur = nv.cur;

            /* The queue takes the app buffer for the album's order */
            nv_release();
            nv_insert(&keep, nv_do == NV_DO_NEXT ? PLAYLIST_INSERT_FIRST
                                                 : PLAYLIST_INSERT_LAST);
            if (!nv_claim())
                return GO_TO_PREVIOUS;
            nv.cur = cur;
            if (!nv_reopen())
                return GO_TO_PREVIOUS;
            break;
        }
        case NV_DO_GO:
            if (t->album < 0)
            {
                if (!tagcache_seek_string(tag_albumartist, t->artist_seek,
                                          name, sizeof(name)))
                    name[0] = '\0';
                browser_db_enter_artist_albums_on_next_load(t->artist_seek,
                                                            name);
                return GO_TO_DBBROWSER;
            }
            if (tagcache_album_get(t->album, &row)
                && tagcache_seek_string(tag_album, row.album_seek, name,
                                        sizeof(name)))
            {
                browser_db_enter_album_tracks_on_next_load(row.album_seek,
                                                           name);
                return GO_TO_DBBROWSER;
            }
            break;
        case NV_DO_PAGE:
        {
            struct notes_target to = nv.pages[nv_do_page];

            nv_follow(&to);
            break;
        }
        case NV_DO_BANNER:
            /* At the top, where the banner is, made from where it now is */
            if (!nv_reopen())
                return GO_TO_PREVIOUS;
            nv.scroll = 0;
            nv.cur = nv_natural(false);
            break;
        default:
            /* A setting changed: the page made again in it, where it was */
            if (nv_settings_sum() != sum && !nv_reopen())
                return GO_TO_PREVIOUS;
            break;
    }
    nv_draw();
    return -1;
}

/* Select or Right: the current link followed. A track of this page's own
 * album has nowhere else to go, so it plays the album from that track. A
 * GO_TO_* code when the screen is to be left, else -1. */
static int nv_select(void)
{
    const struct notes_target *t = nv_current();

    if (!t)
        return -1;
    if (t->to == NOTES_TO_SECTION)
    {
        nv_to_section(t->key);
        return -1;
    }
    if (t->to == NOTES_TO_TRACK && t->album == nv.doc.album)
    {
        nv.play_album = t->album;
        nv.play_track = t->track;
        return GO_TO_WPS;
    }
    nv_follow(t);
    return -1;
}

/* After the screen: the album asked for played, from its track, now the
 * buffer is free for its order. Its GO_TO_* code. */
static int nv_play_on_leaving(int ret)
{
    struct album_data a;
    char path[MAX_PATH];
    bool from = nv.play_track >= 0
                && nv_track_path(nv.play_track, path, sizeof(path));

    if (nv.play_album < 0)
        return ret;
    if (!nv_album_data(nv.play_album, &a)
        || db_summary_play_album_at(&a, from ? path : NULL) <= 0)
        ret = GO_TO_PREVIOUS;
    nv.play_album = -1;
    return ret;
}

static int nv_run(const struct nv_req *r)
{
    int ret = GO_TO_PREVIOUS;

    if (!nv_claim())
        return GO_TO_PREVIOUS;
    nv.nhist = 0;
    nv.play_album = nv.play_track = -1;
    push_current_activity(ACTIVITY_TEXTVIEWER);
    nv_setup_screen();

    if (!nv_open(r))
    {
        int action;

        nv_draw_message(LANG_NOTES_NONE);
        while ((action = get_action(CONTEXT_STD, TIMEOUT_BLOCK)) == ACTION_NONE)
            ;
        if (default_event_handler(action) == SYS_USB_CONNECTED)
            ret = GO_TO_ROOT;
        goto done;
    }
    nv_draw();

    while (1)
    {
        int action, go, fresh = 1;

        keyclick_set_callback(nv_keyclick, NULL);
        action = get_action(CONTEXT_WPS, nv_dirty ? TIMEOUT_NOBLOCK : NV_POLL);

        switch (action)
        {
            case ACTION_WPS_BROWSE:
            case ACTION_WPS_SKIPNEXT:
            case ACTION_WPS_CONTEXT:
            case ACTION_WPS_SKIPPREV:
            case ACTION_WPS_PLAY:
            case ACTION_WPS_QUICKSCREEN:
                if ((fresh = nv_fresh()) < 0)
                    goto done;
                break;
        }
        if (!fresh)
            continue;

        switch (action)
        {
            case ACTION_WPS_BROWSE:             /* Select */
            case ACTION_WPS_SKIPNEXT:           /* Right */
                if ((go = nv_select()) >= 0)
                {
                    ret = go;
                    goto done;
                }
                break;

            case ACTION_WPS_CONTEXT:            /* Select held */
                if ((go = nv_menu_run(false)) >= 0)
                {
                    ret = go;
                    goto done;
                }
                break;

            case ACTION_WPS_SKIPPREV:           /* Left: back */
                if (!nv_back())
                    goto done;
                break;

            case ACTION_WPS_MENU:
                goto done;

            case ACTION_WPS_PLAY:
                if (nv.doc.kind == NOTES_PAGE_ALBUM && nv.doc.album >= 0
                    && !nv_playing_album(nv.doc.album))
                {
                    nv.play_album = nv.doc.album;
                    ret = GO_TO_WPS;
                    goto done;
                }
                wps_do_playpause(false);
                break;

            case ACTION_WPS_VOLUP:              /* the wheel, clockwise */
                nv_wheel(1);
                break;

            case ACTION_WPS_VOLDOWN:
                nv_wheel(-1);
                break;

            case ACTION_WPS_QUICKSCREEN:        /* Menu held */
                if ((go = nv_menu_run(true)) >= 0)
                {
                    ret = go;
                    goto done;
                }
                break;

            case ACTION_NONE:
                if (nv_dirty)
                {
                    nv_dirty = false;
                    nv_draw();
                }
                else if (nv.doc.waiting && nv_library_back())
                {
                    /* the database done: the page with its library, its
                     * current link found again, as the links have moved */
                    if (!nv_reopen())
                        goto done;
                    nv.cur = nv_natural(false);
                    nv_draw();
                }
                else
                {
                    nv_draw_progress(true);
                    nv_unboost(false);
                }
                break;

            default:
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                {
                    ret = GO_TO_ROOT;
                    goto done;
                }
                break;
        }
    }

  done:
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    pop_current_activity();
    nv_release();
    return nv_play_on_leaving(ret);
}

int notes_viewer(void)
{
    static struct notes_album a;
    struct mp3entry *id3 = audio_current_track();
    struct nv_req r = { NV_BY_ALBUM, { 0 }, &a, NULL, NV_FOCUS_TRACK };

    if (!id3 || !id3->path[0])
        return GO_TO_PREVIOUS;
    memset(&a, 0, sizeof(a));
    strlcpy(a.path, id3->path, sizeof(a.path));
    strlcpy(a.albumartist, id3->albumartist ? id3->albumartist : "",
            sizeof(a.albumartist));
    strlcpy(a.artist, id3->artist ? id3->artist : "", sizeof(a.artist));
    strlcpy(a.album, id3->album ? id3->album : "", sizeof(a.album));
    return nv_run(&r);
}

int notes_album(const struct notes_album *a)
{
    struct nv_req r = { NV_BY_ALBUM, { 0 }, a, NULL, -1 };

    return nv_run(&r);
}

int notes_person(const char *name)
{
    struct nv_req r = { NV_BY_NAME, { 0 }, NULL, name, -1 };

    return name && name[0] ? nv_run(&r) : GO_TO_PREVIOUS;
}

int notes_bio(const char *name)
{
    return notes_person(name);
}
