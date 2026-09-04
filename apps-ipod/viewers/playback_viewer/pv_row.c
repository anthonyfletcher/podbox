/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Spun's row screen: a titled section and a horizontally scrolling row of
 * cards built from the playback log.
 *
 * Parts: the artwork slots, which are the only thing held between frames; the
 * fonts; the section list; the composite, which is one frame; then the loop.
 *
 * This owns the screen outright -- no status bar, no themed viewport, no list
 * widget -- and it owns the working memory while it is up, because every name
 * a card shows is a pointer into the aggregate tables.
 *
 * See .specifications/spun-card-engine.md for the engine and
 * .specifications/spun-tiles.md for what each card says.
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <file.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "timefuncs.h"
#include "settings/settings.h"    /* ID2P */
#include "system/activity.h"
#include "system/app_buffer.h"
#include "system/shutdown.h"      /* default_event_handler */
#include "audio/sound_feedback.h" /* keyclick_click */
#include "widgets/splash.h"
#include "widgets/list.h"
#include "input/action.h"   /* ACTION_STD_MENU */
#include "widgets/menu.h"
#include "screens/settings/exported_settings.h"
#include "draw/viewport.h"
#include "draw/card_row.h"
#include "draw/card_paint.h"
#include "root_menu.h"            /* GO_TO_*, MENU_ATTACHED_USB */
#include "pv_stats.h"
#include "pv_badges.h"
#include "pv_tiles.h"
#include "pv_play.h"
#include "pv_row.h"

#ifdef HAVE_ALBUMART
#include "metadata/art_cache.h"
#include "metadata/art_sizes.h"  /* AA_COLUMNS */
#endif

#define ROW_Y       CARD_ROW_Y
#define ROW_H       CARD_ROW_H
#define ROW_X       CARD_ROW_X
#define ROW_W       (LCD_WIDTH - ROW_X)
#define SUB_H       (ROW_H - CARD_SUB_DROP)
#define MAX_VISIBLE 32

/* Milliseconds a frame is allowed to take.
 *
 * Uncapped the row runs at ninety frames a second, starves the codec and
 * pushes the panel three times for every frame anyone could see. Thirty is
 * both the fix and the saving -- it is a third of the CPU rather than two
 * thirds, and it is what made drawing every card every frame affordable. */
#define FRAME_MS    33

/* Not a year: what year_menu() answers when a setting has changed that
 * divides the working memory differently, so the screen has to be opened
 * again rather than patched. Outside any year the log can hold. */
#define PV_YEAR_REBUILD (-1)

/* ------------------------------------------------------------ the artwork */

/* Squares, because the cache stores squares: the picture band is wider than
 * it is tall and the painter takes the middle rows out of this, which crops
 * rather than squashes.
 *
 * This is the one thing held between frames. Finished cards are not cached --
 * measured on a 5G, holding them cost 387 KB and was slower, because the art
 * had to come out of the same buffer and what was left could not hold a
 * screen's worth of cards (spun-card-engine.md §5.1). */
#define ART_PX      128
#define ART_SLOTS     6

/* Cards past each end of the view whose pictures are fetched anyway.
 *
 * Without this a picture is only ever asked for once its card is ALREADY on
 * screen, so the pattern is always shown first and the picture replaces it a
 * frame or two later however fast the read is. One each way is enough at a
 * card a scroll step, and it is what the slot count is cut for: four cards
 * can be on screen at 128 wide, and six holds those and the two coming. */
#define ART_AHEAD     1
#define ART_BYTES   (ART_SLOTS * ART_PX * ART_PX * (int)sizeof(fb_data))

static fb_data     *art_mem;
static unsigned     art_key[ART_SLOTS];
static bool         art_ok[ART_SLOTS];

/* Hues, kept apart from the pictures and for far longer.
 *
 * Two things follow from keeping the answer rather than the pixels. A card's
 * colour does not change when its picture is evicted -- five slots hold a
 * screenful, and a row scrolled far evicts everything behind it, which would
 * otherwise make a card revert to its assigned colour on the way back. And
 * the same sleeve is the same colour in every section, without a second read.
 *
 * Direct-mapped, and the key is checked: a collision costs one picture its
 * remembered hue, never gives it another picture's. */
#define HUE_CACHE 64
static unsigned     hue_key[HUE_CACHE];
static short        hue_val[HUE_CACHE];
static unsigned     art_used[ART_SLOTS];
static unsigned     art_tick;
static int          art_size_idx = -1;    /* the 300px, scaled down */
static int          art_square_idx = -1;  /* the 128px, read straight */
/* Pictures worth fetching, in the order they are wanted: the cards on screen
 * first, then the ones about to be. While any remain the loop keeps drawing
 * instead of blocking, so they arrive a frame apart with the screen alive.
 *
 * Order is the point. Fetching the look-ahead before the card being looked at
 * leaves a pattern on screen while the reads go to cards nobody can see yet,
 * which is the whole complaint the look-ahead was meant to answer. */
static unsigned     art_want[MAX_VISIBLE + 4];
static int          art_want_n;
static int          art_want_at;

/* Loads allowed this frame. A miss is a file read, and letting every miss in
 * one frame do one is what turns a fast scroll into a stutter: the row is
 * moving, so a slot missed now will be asked for again next frame anyway. */
static int          art_budget;

static void art_init(void *buf)
{
    art_mem = buf;
    art_tick = 0;
    memset(art_key, 0, sizeof(art_key));
    memset(art_ok, 0, sizeof(art_ok));
    memset(art_used, 0, sizeof(art_used));
    memset(hue_key, 0, sizeof(hue_key));

#ifdef HAVE_ALBUMART
    /* By name, never by index: the size table is edited over time and the
     * indices move with it. "wps" is the 300px square, the only cached size
     * bigger than this and stored by rows -- the carousel's is transposed and
     * cannot be read back. */
    art_size_idx = art_cache_size_index("wps");
    art_square_idx = art_cache_size_index("coverflow");
#endif
}

/* Read a thumbnail that is already the size we want, straight into the slot.
 *
 * The carousel's cache size is 128 x 128 -- exactly the picture band's width
 * -- so there is no scaling at all, and the file is 32 KB against the 300px
 * size's 180 KB. Measured on a 5G, a load through the 300px path cost 211 ms
 * and was 87% of every frame's time while scrolling; almost all of that is
 * reading the file.
 *
 * It is stored COLUMN-major, because the carousel draws slides column by
 * column, and art_cache_load_aat() refuses such a file for that reason.
 * Reading it here means transposing in place afterwards, which is sixteen
 * thousand moves and nothing beside the read it saves.
 *
 * A source too elongated to crop square is stored by ROWS instead (the
 * CONTAIN fallback), and is not this size at all -- so the header is checked
 * rather than assumed, and anything unexpected falls back to the 300px path.
 */
static bool art_read_square(unsigned key, fb_data *dst)
{
    struct art_cache_header hdr;
    char path[MAX_PATH];
    int fd;
    bool ok = false;

    if (art_square_idx < 0)
        return false;

    art_cache_thumb_path(key, art_square_idx, path, sizeof(path));
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;

    if (read(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)
        && hdr.magic == ART_CACHE_MAGIC
        && hdr.version == ART_CACHE_FORMAT_VERSION
        && hdr.layout == AA_COLUMNS
        && hdr.width == ART_PX && hdr.height == ART_PX
        && read(fd, dst, ART_PX * ART_PX * FB_DATA_SZ)
           == (ssize_t)(ART_PX * ART_PX * FB_DATA_SZ))
    {
        for (int y = 0; y < ART_PX; y++)
            for (int x = y + 1; x < ART_PX; x++)
            {
                fb_data t = dst[y * ART_PX + x];

                dst[y * ART_PX + x] = dst[x * ART_PX + y];
                dst[x * ART_PX + y] = t;
            }
        ok = true;
    }

    close(fd);
    return ok;
}

/* Whether this picture's slot has been settled -- loaded, or looked for and
 * not there. A key that has never been tried is what keeps the loop awake. */
static bool art_resolved(unsigned key)
{
    for (int i = 0; i < ART_SLOTS; i++)
        if (art_key[i] == key)
            return true;
    return false;
}

/* Note a picture worth fetching, once, and only if it is not in hand. */
static void art_want_add(unsigned key)
{
    if (!key || art_want_n >= (int)ARRAYLEN(art_want) || art_resolved(key))
        return;
    for (int i = 0; i < art_want_n; i++)
        if (art_want[i] == key)
            return;
    art_want[art_want_n++] = key;
}

static const fb_data *art_get(unsigned key, int *stride, int *w, int *h)
{
#ifdef HAVE_ALBUMART
    char path[MAX_PATH];
    struct bitmap bm;
    int lru = 0, fd, rc;

    if (!art_mem || art_size_idx < 0 || key == 0)
        return NULL;

    for (int i = 0; i < ART_SLOTS; i++)
    {
        if (art_key[i] == key)
        {
            art_used[i] = ++art_tick;
            if (!art_ok[i])
                return NULL;
            *stride = ART_PX;
            *w = *h = ART_PX;
            return art_mem + (size_t)i * ART_PX * ART_PX;
        }
        if (art_used[i] < art_used[lru])
            lru = i;
    }

    if (art_budget <= 0)
        return NULL;
    art_budget--;

    art_key[lru]  = key;
    art_ok[lru]   = false;
    art_used[lru] = ++art_tick;

    /* Ask where the thumbnail would be and open it, rather than asking
     * whether it exists and then opening it -- the open is the existence
     * test. */
    rc = art_read_square(key, art_mem + (size_t)lru * ART_PX * ART_PX) ? 1 : 0;

    if (!rc)
    {
        /* No square for this folder, so scale the big one down. */
        art_cache_thumb_path(key, art_size_idx, path, sizeof(path));
        fd = open(path, O_RDONLY);
        if (fd >= 0)
        {
            bm.width  = ART_PX;
            bm.height = ART_PX;
            bm.format = FORMAT_NATIVE;
            bm.data   = (unsigned char *)(art_mem
                                          + (size_t)lru * ART_PX * ART_PX);
            rc = art_cache_load_aat(fd, &bm,
                                    ART_PX * ART_PX * (int)sizeof(fb_data),
                                    NULL);
            close(fd);
        }
    }

    if (rc <= 0)
        return NULL;

    art_ok[lru] = true;
    hue_key[key % HUE_CACHE] = key;
    hue_val[key % HUE_CACHE] =
        (short)card_paint_dominant_hue(art_mem + (size_t)lru * ART_PX * ART_PX,
                                       ART_PX, ART_PX, ART_PX);
    *stride = ART_PX;
    *w = *h = ART_PX;
    return art_mem + (size_t)lru * ART_PX * ART_PX;
#else
    (void)key; (void)stride; (void)w; (void)h;
    return NULL;
#endif
}

/* What colour a card carrying this picture should be.
 *
 * Answered from the remembered hue, never by loading: the compositor makes
 * sure a visible card's picture is in hand before it resolves the card, which
 * is the whole of what keeps a colour from arriving a frame late. */
static int art_tint(unsigned key)
{
    int i = (int)(key % HUE_CACHE);

    return hue_key[i] == key ? hue_val[i] : -1;
}

/* -------------------------------------------------------------- the fonts */

/* Three sans faces and an icon face, and no others: the zip carries only what
 * the Scrim theme uses, because bundle-theme.sh copies that theme's fonts
 * folder wholesale. Naming a size the build does not ship is silent --
 * font_load() fails, the system font stands in, and the screen looks nothing
 * like it should while still working. The simulator hides it, because a
 * simdisk accumulates every font it has ever been given.
 *
 * The three carry the whole hierarchy, so the steps between them have to do
 * real work: 26 against 18 is a size apart, and bold against regular is a
 * weight apart. A figure is therefore unmistakable next to the words around
 * it without either being small.
 *
 * The badge face is ours -- apps-ipod/fonts/, shipped beside the binary --
 * for the reason the serif is: a face the core cannot do without must not
 * arrive inside a theme. Missing, the plates are simply empty. */
#define FONT_TITLE_PATH ROCKBOX_DIR "/fonts/26-noto-sans-medium.fnt"
#define FONT_NAME_PATH  ROCKBOX_DIR "/fonts/18-noto-sans-bold.fnt"
#define FONT_BODY_PATH  ROCKBOX_DIR "/fonts/18-noto-sans.fnt"
#define FONT_FIG_PATH   ROCKBOX_DIR "/fonts/30-noto-serif-figures.fnt"
#define FONT_ICON_PATH  ROCKBOX_DIR "/fonts/24-spun-badges.fnt"

static int font_title = FONT_SYSFIXED;
static int font_name  = FONT_SYSFIXED;
static int font_body  = FONT_SYSFIXED;
static int font_fig   = FONT_SYSFIXED;
static int font_icon  = FONT_SYSFIXED;

/* The serif fills two roles -- a card's hero figure and the number in a
 * plate, which is also a figure -- so it is loaded once and named twice. */
static void fonts_apply(void)
{
    struct card_fonts f;

    f.title  = font_title;
    f.tag    = font_fig;
    f.figure = font_fig;
    /* A table's figures stay sans: the serif is for the one hero number on a
     * card, and a column of them in a table is a list, not a headline. It
     * also has to be a face with letters in it, because a table's right-hand
     * column carries dates. */
    f.value  = font_name;
    f.card   = font_name;
    f.body   = font_body;
    f.icon   = font_icon;
    card_paint_set_fonts(&f);
}

/* A missing font is not worth reporting: the system font is uglier and
 * everything still lays out. */
static void fonts_load(void)
{
    int id;

    if ((id = font_load(FONT_TITLE_PATH)) >= 0) font_title = id;
    if ((id = font_load(FONT_NAME_PATH))  >= 0) font_name  = id;
    if ((id = font_load(FONT_BODY_PATH))  >= 0) font_body  = id;
    if ((id = font_load(FONT_FIG_PATH))   >= 0) font_fig   = id;
    if ((id = font_load(FONT_ICON_PATH))  >= 0) font_icon  = id;

    fonts_apply();
}

static void fonts_unload(void)
{
    if (font_title != FONT_SYSFIXED) font_unload(font_title);
    if (font_name  != FONT_SYSFIXED) font_unload(font_name);
    if (font_body  != FONT_SYSFIXED) font_unload(font_body);
    if (font_fig   != FONT_SYSFIXED) font_unload(font_fig);
    if (font_icon  != FONT_SYSFIXED) font_unload(font_icon);
    font_title = font_name = font_body = FONT_SYSFIXED;
    font_fig = font_icon = FONT_SYSFIXED;
    fonts_apply();
}

/* ----------------------------------------------------------- the sections */

/* Only the sections that have something in them, in schedule order. Newly
 * unlocked is first and conditional, so this is built when Spun opens rather
 * than being a fixed table -- with nothing new, Spun opens on In numbers. */
static unsigned char sec_list[PV_SEC_COUNT];
static int sec_n, sec_at;

/* Where the user was in each section, so walking away and back does not put
 * them at the front of a row they had scrolled halfway along. Kept by section
 * rather than by list position, because a year switch rebuilds the list. */
static short sec_focus[PV_SEC_COUNT];

static void sections_build(const struct pv_totals *t)
{
    memset(sec_focus, 0, sizeof(sec_focus));
    sec_n = 0;
    for (int s = 0; s < PV_SEC_COUNT; s++)
        if (pv_tiles_section_present((enum pv_sec)s, t))
            sec_list[sec_n++] = (unsigned char)s;
    sec_at = 0;
}

/* --------------------------------------------------------------- the row */

/* A section change slides: the row leaves in the direction of travel and the
 * next arrives from the other side.
 *
 * Sequential, not a cross-fade. A cross-fade needs both sections' pictures
 * live at once, and on the 5G the artwork slots are the scarcest thing there
 * is. The title swaps at the halfway point, where the band is empty, so it
 * costs no animation of its own. */
#define SLIDE_MS 140

enum { SLIDE_NONE, SLIDE_OUT, SLIDE_IN };

static int slide_phase = SLIDE_NONE;
static int slide_dir;
static int slide_t;
static int band_shift;      /* added to every card's x this frame */

static short         cur_w[512];
static short         from_w[512];
static struct card_row row;
static unsigned band_bg;

static void row_load(const struct pv_totals *t)
{
    int n = pv_tiles_build((enum pv_sec)sec_list[sec_at], t);

    if (n > (int)ARRAYLEN(cur_w))
        n = (int)ARRAYLEN(cur_w);
    card_row_init(&row, pv_tiles_widths(), pv_tiles_flags(),
                  cur_w, from_w, n, ROW_W, 0, CARD_GAP);
    card_row_set_fold_w(&row, CARD_FOLD_W);
    card_row_set_focus(&row, sec_focus[sec_list[sec_at]]);
}

/* -------------------------------------------------------------- the frame */

static int card_h(int idx)
{
    return (pv_tiles_flags()[idx] & CARD_ROW_SUB) ? SUB_H : ROW_H;
}

/* Fill every part of the band no card is about to cover.
 *
 * The plan says where the cards are, so the compositor owns the complement --
 * and a card's complement is not just the gap beside it. A sub-card is
 * bottom-aligned and shorter than the band, so it leaves the rows above it
 * uncovered too, and those hold the last frame until something clears them.
 *
 * Done from the plan rather than by clearing the whole band first: a
 * full-band fill is a third of the composite budget and the uncovered region
 * is usually a few pixels. */
static void fill_uncovered(const struct card_row_item *it, int n)
{
    int x = 0;

    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(band_bg);
    lcd_fillrect(0, ROW_Y, ROW_X, ROW_H);

    /* Mid-slide the row does not start at the margin, so the complement is
     * taken in shifted coordinates and clamped to the band. */
    for (int i = 0; i < n; i++)
    {
        int h = card_h(it[i].index);
        int cx = it[i].dst_x + band_shift;

        if (cx > x)
            lcd_fillrect(ROW_X + x, ROW_Y, cx - x, ROW_H);
        if (h < ROW_H && cx + it[i].w > 0)
            lcd_fillrect(ROW_X + (cx < 0 ? 0 : cx), ROW_Y, it[i].w, ROW_H - h);
        if (cx + it[i].w > x)
            x = cx + it[i].w;
    }

    if (x < 0)
        x = 0;
    if (x < ROW_W)
        lcd_fillrect(ROW_X + x, ROW_Y, ROW_W - x, ROW_H);
}

static void composite(void)
{
    struct card_row_item it[MAX_VISIBLE];
    const short *full_w = pv_tiles_widths();
    int n = card_row_plan(&row, it, MAX_VISIBLE);

    /* Pictures first, cards second.
     *
     * A card's colour is derived from its picture (pv_tiles.h), and the card
     * is resolved before it is drawn -- so a picture fetched during the draw
     * arrives one frame too late to colour the card that asked for it. Worse
     * at rest than in motion: the loop blocks until a button arrives, so
     * "next frame" can be minutes away and the card simply looks wrong until
     * the row is moved.
     *
     * At rest every miss is allowed to load, because there is no motion for a
     * disk read to interrupt. In motion the budget stands: a fast scroll
     * misses several slots a frame and doing every read would stutter, and a
     * card passing at speed is one frame from being asked for again. */
    /* Nothing is READ here. This only looks at what is already in a slot and
     * notes what is missing; the reading happens after the frame has been
     * flushed, out of what is left of the frame's own time (see the loop).
     *
     * A file read inside the drawing is what made this screen slow: on a 5G
     * one load measured 211 ms, so a load a frame was not a small cost paid
     * smoothly but every frame taking a fifth of a second. */
    art_budget = 0;
    art_want_n = art_want_at = 0;
    for (int i = 0; i < n; i++)
    {
        unsigned key = pv_tiles_art_key(it[i].index);
        int stride, aw, ah;

        if (!key)
            continue;
        art_get(key, &stride, &aw, &ah);
        art_want_add(key);
    }

    /* Then the cards either side of the view, so a picture is in hand before
     * its card arrives rather than a frame or two after. After the visible
     * ones, never before them. */
    for (int a = 1; a <= ART_AHEAD && n > 0; a++)
    {
        art_want_add(pv_tiles_art_key(it[n - 1].index + a));
        art_want_add(pv_tiles_art_key(it[0].index - a));
    }

    fill_uncovered(it, n);

    for (int i = 0; i < n; i++)
    {
        struct card_content c;
        struct viewport vp, *old;
        int h = card_h(it[i].index);
        int y = ROW_Y + ROW_H - h;      /* sub-cards are bottom-aligned */
        int x = ROW_X + it[i].dst_x + band_shift;
        int src = it[i].src_x;
        int w = it[i].w;

        /* The slide clips at the band's own edges, which is the same
         * clipping the plan already did at the view's -- so a card halfway
         * off the side lays out exactly as an unclipped one does. */
        if (x < ROW_X)
        {
            src += ROW_X - x;
            w   -= ROW_X - x;
            x    = ROW_X;
        }
        if (x + w > LCD_WIDTH)
            w = LCD_WIDTH - x;
        if (w <= 0)
            continue;

        pv_tiles_content(it[i].index, &c);

        memset(&vp, 0, sizeof(vp));
        vp.x = x;
        vp.y = y;
        vp.width  = w;
        vp.height = h;
        vp.font = FONT_SYSFIXED;
        vp.drawmode = DRMODE_SOLID;
        vp.fg_pattern = LCD_DEFAULT_FG;
        vp.bg_pattern = LCD_DEFAULT_BG;

        /* The card is drawn at its own full width with the origin shifted
         * left by whatever the screen edge cuts off, so a card clipped at
         * either edge lays out exactly as an unclipped one does. */
        old = lcd_set_viewport(&vp);
        /* A card down to a spine is drawn as one: laying out a whole card to
         * show nine pixels of its left edge costs a card's work for a fill. */
        if (w <= CARD_FOLD_W)
            card_paint_spine(&c, w, h);
        else
            card_paint_draw(&c, full_w[it[i].index], h, -src);
        lcd_set_viewport(old);
    }
}

/* A triangle at the band's edge, pointing the way the section list runs.
 *
 * Drawn rather than set from an icon font: two triangles are not worth a
 * face, and a face is a thing a theme can stop shipping. */
#define ARROW_W 7

static void draw_arrow(int cx, int cy, int dir, unsigned col)
{
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(col);
    for (int i = 0; i < ARROW_W; i++)
        lcd_vline(cx + dir * (ARROW_W - 1 - i) / 1, cy - i, cy + i);
}

/* A year as text, for a title that has to say either a year or "All time". */
static const char *pfmt_year(int year)
{
    static char buf[8];

    snprintf(buf, sizeof(buf), "%d", year);
    return buf;
}

static void draw_title(int year)
{
    enum pv_sec sec = (enum pv_sec)sec_list[sec_at];
    char line[48];
    int tw, th, cy = ROW_Y / 2;

    /* The year belongs to the title, not to a corner of the screen: it
     * applies to everything below and "2026 in numbers" is a heading, where
     * a number on the right is a label nobody asked for. */
    if (sec == PV_SEC_NUMBERS)
        snprintf(line, sizeof(line), "%s in numbers",
                 year == PV_YEAR_ALL ? "All time" : pfmt_year(year));
    else
        snprintf(line, sizeof(line), "%s", pv_tiles_section_name(sec));

    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(band_bg);
    lcd_fillrect(0, 0, LCD_WIDTH, ROW_Y);

    lcd_setfont(font_title);
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(LCD_RGBPACK(240, 239, 235));
    /* Left, on the row's own margin: the title is the heading of what is
     * under it, and a heading that does not share an edge with the thing it
     * heads is floating. */
    font_getstringsize((const unsigned char *)line, &tw, &th, font_title);
    lcd_putsxy(ROW_X, cy - th / 2, line);

    /* One each side, and only where there is somewhere to go: an arrow that
     * does nothing is worse than no arrow. */
    if (sec_at > 0)
        draw_arrow(ROW_X / 2 - 2, cy, -1, LCD_RGBPACK(120, 120, 128));
    if (sec_at < sec_n - 1)
        draw_arrow(LCD_WIDTH - ROW_X / 2 + 1, cy, +1,
                   LCD_RGBPACK(120, 120, 128));

    lcd_update_rect(0, 0, LCD_WIDTH, ROW_Y);
}

/* The whole screen, from scratch. Whatever drew over it -- a menu, a splash,
 * a yes/no -- left the frame buffer as it found it, so the clear comes first
 * and the row is composited on top of a known ground. */
static void repaint(int year)
{
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(band_bg);
    lcd_fillrect(0, 0, LCD_WIDTH, LCD_HEIGHT);
    lcd_update();

    draw_title(year);
    composite();
    lcd_update_rect(0, ROW_Y, LCD_WIDTH, ROW_H);
}

/* --------------------------------------------------------------- the loop */

/* Pick a year, or all time. Returns the choice, or 'cur' if nothing was
 * picked; PV_YEAR_ALL is the last row.
 *
 * A list rather than a cycle. Six years of history makes a hold-to-cycle six
 * presses deep with nothing on screen saying how far round it has got, and a
 * cycle has no good place for "All time" -- which is not a year and does not
 * belong in a sequence of them. All time has to be reachable at all, because
 * Spun opens on the clock's year and that is what the badges are scored
 * against.
 *
 * The theme comes back for the list and the screen is handed to our own
 * drawing again after, the way the deck's own menu does it. */
/* MENU dismisses this list; it does not leave for the root menu.
 *
 * simplelist_show_list() reads MENU as "out to the root", which is right for
 * a browser and wrong for a menu opened by HOLDING Menu inside a screen: the
 * gesture that opened it is the one that should close it. Turning it into a
 * cancel is how a caller says so, and it is what every other viewer here
 * amounts to -- they call do_menu(), which also returns GO_TO_ROOT for a
 * MENU press, and act on MENU_ATTACHED_USB alone.
 *
 * The cancel path is reached because the widget only treats a callback's
 * ACTION_STD_CANCEL as "the callback asked us to exit" when the action it was
 * given was ACTION_STD_OK; anything else falls through to its own handling,
 * which for CANCEL leaves without the root-menu return. */
static int year_menu_action(int action, struct gui_synclist *lists)
{
    (void)lists;
    return action == ACTION_STD_MENU ? ACTION_STD_CANCEL : action;
}

static int year_menu(int cur, bool *to_root, bool *reload)
{
    struct simplelist_info info;
    int lo = 0, hi = 0, y, n = 0, at = 0, pick;
    bool rebuild = false;
    bool was_art = global_settings.spun_artwork;
    int  was_top = global_settings.spun_top_count;
    int  was_ord = global_settings.spun_badge_order;
    int  was_rank = global_settings.spun_rank_by;

    pv_stats_year_span(&lo, &hi);

    /* Trap: simplelist_info_init() zeroes the line COUNT but not the buffer
     * position, so without this the lines are written into a full buffer with
     * nothing remaining and every row comes out empty. Every caller that adds
     * lines resets first. */
    simplelist_info_init(&info, "Playback Report", 0, NULL);
    simplelist_reset_lines();

    for (y = hi; y >= lo && lo != 0; y--, n++)
    {
        simplelist_addline("%d", y);
        if (y == cur)
            at = n;
    }
    simplelist_addline("All time");
    if (cur == PV_YEAR_ALL)
        at = n;

    /* Settings on the same list rather than behind a second one. Both things
     * a held Menu is for are then one press away, and the alternative -- a
     * menu whose two rows are "which year" and "settings" -- makes the year,
     * which is the reason this gesture exists, two presses deep. */
    simplelist_addline("Settings");

    info.selection = at;
    info.hide_theme = false;
    info.action_callback = year_menu_action;

    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    push_current_activity(ACTIVITY_CONTEXTMENU);
    if (simplelist_show_list(&info))
        *to_root = true;
    pick = info.selection;

    if (pick == n + 1)
    {
        if (do_menu(&spun_menu, NULL, NULL, false) == MENU_ATTACHED_USB)
            *to_root = true;

        /* Top Rows and Achievement Order change what the row is made of, so
         * the row is rebuilt. Artwork changes how the working memory is
         * divided, which is settled when Spun opens -- so it costs the same
         * re-read a year change does, and is handled the same way. */
        if (global_settings.spun_top_count != was_top
            || global_settings.spun_badge_order != was_ord
            || global_settings.spun_rank_by != was_rank)
            *reload = true;
        rebuild = global_settings.spun_artwork != was_art;
    }

    /* Trap: the activity and the theme are pushed above the block, so a
     * return from inside it leaks one activity entry a visit -- on a stack of
     * twelve that push_current_activity() does not bound. Every way out of
     * here goes through these three lines; what the caller is told comes
     * after them. */
    pop_current_activity();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    lcd_set_viewport(NULL);

    if (rebuild)
        return PV_YEAR_REBUILD;
    if (pick < 0 || pick > n)
        return cur;
    if (pick == n)
        return PV_YEAR_ALL;
    return hi - pick;
}

/* One visit. Sets *again when a setting has changed that divides the working
 * memory differently: the model is built inside that division, so the only
 * honest way to apply it is to start over. */
static int row_session(bool *again)
{
    struct pv_totals totals;
    enum pv_build_result r;
    void *buf, *stats_buf;
    size_t bufsz, stats_sz;
    unsigned long prev;
    int ret = GO_TO_PREVIOUS;
    int year;
    bool moving = false, was_moving = false;
    bool held = false;
    bool art_on;

    band_bg = LCD_RGBPACK(8, 8, 12);

    buf = app_claim_buffer(&bufsz, "spun row");

    /* Two consumers share this, and dividing it is the screen's job because
     * the screen is the only thing that knows both exist. The artwork takes a
     * fixed slice off the front; the model sizes itself to what is left.
     *
     * With artwork switched off the slice is not taken at all, which is the
     * second reason that setting exists: it is the largest fixed claim Spun
     * makes, and a large library loses rows to it.
     *
     * Trap: 'bufsz' is a size_t, so a region too small for the slice does not
     * give a negative remainder -- it wraps, and the model is handed a size
     * larger than the machine. Too small to divide is answered the way the
     * setting being off is answered, which is a path the screen already
     * takes: patterns instead of sleeves, and the whole region to the
     * model. */
    art_on = global_settings.spun_artwork && bufsz > (size_t)ART_BYTES;
    art_init(art_on ? buf : NULL);
    stats_buf = (char *)buf + (art_on ? ART_BYTES : 0);
    stats_sz  = bufsz - (art_on ? ART_BYTES : 0);

    /* The year Spun opens on comes from the clock, not from the log.
     *
     * The log's own span would be the better answer and cannot be had: it is
     * not known until the log has been read, and reading it for a different
     * year than the one asked for means reading it twice -- a full pass, on a
     * spinning disk, before anything is drawn. A player whose year has no
     * plays in it opens on an empty In numbers and a held Menu walks back to
     * one that has; a player with no clock set gets the whole log, which is
     * the only honest answer when nothing is dated. */
    {
        struct tm *tm = get_time();

        year = valid_time(tm) ? tm->tm_year + 1900 : PV_YEAR_ALL;
    }

    splash(0, ID2P(LANG_WAIT));
    r = pv_stats_build(stats_buf, stats_sz, &totals, year);

    if (r != PV_BUILD_OK)
    {
        if (r == PV_BUILD_NO_LOG)
            splash(HZ * 3, ID2P(LANG_PV_NO_LOG));
        else
            splash(HZ * 2, ID2P(LANG_PV_NO_MEMORY));
        app_release_buffer("spun row");
        return ret;
    }

    /* Decide what is newly unlocked while the model is still in front of us,
     * and commit it before anything is drawn: seen the moment Spun opens, so
     * leaving by pulling the USB cable still counts as having looked.
     *
     * Once, and only here. Classifying again under the Achievements section
     * would clear is_new beneath a section the user can still scroll back to,
     * and saving from a past-year view would write year-scoped state over the
     * real one -- earned dates cannot be recovered once lost. */
    pv_badges_classify();
    pv_badges_save();

    push_current_activity(ACTIVITY_PLAYBACKVIEWER);
    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, false, NULL);
    lcd_set_viewport(NULL);

    fonts_load();
    card_paint_set_art(art_on ? art_get : NULL);
    pv_tiles_set_tint(art_on ? art_tint : NULL);
    sections_build(&totals);
    row_load(&totals);

    repaint(year);
    button_clear_queue();
    prev = USEC_TIMER;

    /* One frame loop, not a wait followed by a run to rest.
     *
     * Two things depend on it. The frame interval has to be measured across
     * the whole screen rather than from the top of each animation, or every
     * tick reports the microseconds since the animation began -- which is
     * none of them -- and a motion made of one-frame animations never
     * advances its clock at all. And the input has to be drained rather than
     * taken one event per frame: a held wheel repeats faster than frames can
     * be drawn, so a loop that consumes one leaves the rest queued and falls
     * further behind for as long as the button is down. */
    while (1)
    {
        unsigned long now;
        int dt;

        do
        {
            /* Anything still to draw -- a motion, or a picture waiting to be
             * read -- means the queue is polled rather than waited on. */
            bool busy = moving || art_want_at < art_want_n;
            int btn = busy ? button_get(false) : button_get(true);
            int code;

            if (btn == BUTTON_NONE)
                break;

            if (btn == SYS_USB_CONNECTED)
            {
                /* Acknowledge it rather than swallowing it: an
                 * unacknowledged handover is what the host reports as a
                 * malfunctioning device. */
                default_event_handler(btn);
                ret = GO_TO_ROOT;
                goto done;
            }
            if (btn == SYS_POWEROFF)
            {
                default_event_handler(btn);
                goto done;
            }

            /* Raw button codes rather than an action context, the way the
             * rest of Spun does it. CONTEXT_STD maps Right and Select to one
             * action, and here they mean different things.
             *
             * Trap: BUTTON_REL rides in the same word as the button, so a
             * switch that masks off only BUTTON_REPEAT never matches a
             * release -- which is how Menu stopped being a way out. Menu is
             * the one button that cares about the release, so it is taken
             * first and every other release is dropped; without that, the
             * release of Select would toggle a second time. */
            code = btn & ~(BUTTON_REPEAT | BUTTON_REL);

            if (code != BUTTON_MENU && (btn & BUTTON_REL))
                continue;

            switch (code)
            {
            /* A click when the row actually moves, not when a button
             * arrives: the wheel sends far more events than it makes steps,
             * and a click per event is a rattle rather than feedback.
             * keyclick_click() honours the user's own keyclick settings, so
             * this is silent for anyone who has turned it off. */
            case BUTTON_SCROLL_FWD:
                if (card_row_step(&row, +1))
                    keyclick_click(true, btn);
                break;
            case BUTTON_SCROLL_BACK:
                if (card_row_step(&row, -1))
                    keyclick_click(true, btn);
                break;

            case BUTTON_RIGHT:
                if (sec_at < sec_n - 1 && slide_phase == SLIDE_NONE)
                {
                    slide_phase = SLIDE_OUT;
                    slide_dir = +1;
                    slide_t = 0;
                    keyclick_click(true, btn);
                }
                break;
            case BUTTON_LEFT:
                if (sec_at > 0 && slide_phase == SLIDE_NONE)
                {
                    slide_phase = SLIDE_OUT;
                    slide_dir = -1;
                    slide_t = 0;
                    keyclick_click(true, btn);
                }
                break;

            case BUTTON_SELECT:
                if (card_row_toggle(&row))
                {
                    pv_tiles_open(row.focus);
                    keyclick_click(true, btn);
                }
                break;

            case BUTTON_PLAY:
            {
                /* Play what the card is about, where it is about something
                 * that can be played. A card naming nothing -- a figure, a
                 * chart, a badge -- is not an error and says nothing. */
                struct pv_target target;

                if (!pv_tiles_target(row.focus, &target))
                    break;

                keyclick_click(true, btn);
                splash(0, ID2P(LANG_WAIT));

                switch (pv_play_target(&target))
                {
                case 0:
                    /* The library has nothing under that name. Spun's rows
                     * are names rather than database ids, so this is an
                     * ordinary outcome, not a fault. */
                    splash(HZ * 2, ID2P(LANG_PV_NOT_IN_LIBRARY));
                    repaint(year);
                    break;
                case -1:
                    /* Already spoken for: the database was busy, or the
                     * listener declined to lose the playlist they had. */
                    repaint(year);
                    break;
                default:
                    ret = GO_TO_WPS;
                    goto done;
                }
                break;
            }

            case BUTTON_MENU:
                /* Menu held switches year; Menu pressed and let go leaves.
                 * Telling them apart means waiting for the release, so the
                 * press itself does nothing and 'held' remembers whether a
                 * switch happened in between -- otherwise the release that
                 * ends the hold would also be read as "leave". */
                if (btn & BUTTON_REPEAT)
                {
                    bool root = false, reload = false;
                    int y2;

                    held = true;
                    y2 = year_menu(year, &root, &reload);
                    if (root)
                    {
                        ret = GO_TO_ROOT;
                        goto done;
                    }
                    button_clear_queue();
                    /* Asking the keypad rather than assuming: the list
                     * usually eats the release as well, and a stuck 'held'
                     * would swallow the next press to leave. */
                    held = (button_status() & BUTTON_MENU) != 0;

                    if (y2 == PV_YEAR_REBUILD)
                    {
                        /* Artwork was switched: the whole buffer is divided
                         * differently, so the screen is opened again from
                         * the top rather than patched. */
                        ret = GO_TO_PREVIOUS;
                        *again = true;
                        goto done;
                    }

                    if (y2 != year || reload)
                    {
                        /* A year is not a slice of a finished total, so
                         * switching means re-reading the log. A settings
                         * change that only reshapes the row does not. */
                        if (y2 != year)
                        {
                            splash(0, ID2P(LANG_WAIT));
                            year = y2;
                            pv_stats_build(stats_buf, stats_sz, &totals,
                                           year);
                            memset(art_key, 0, sizeof(art_key));
                            memset(hue_key, 0, sizeof(hue_key));
                        }
                        sections_build(&totals);
                        if (sec_at >= sec_n)
                            sec_at = 0;
                        row_load(&totals);
                    }

                    repaint(year);
                }
                else if (btn & BUTTON_REL)
                {
                    if (!held)
                        goto done;
                    held = false;
                }
                break;

            default:
                break;
            }
        }
        while (button_queue_count() > 0);

        now = USEC_TIMER;

        /* The first frame after a wait measures nothing: the interval would
         * be however long the screen sat blocked, and handing that to the row
         * would spend the whole motion at once. */
        dt = was_moving ? (int)((now - prev) / 1000) : 0;
        prev = now;
        if (dt > 100)
            dt = 100;

        moving = card_row_tick(&row, dt);

        /* The row leaves in the direction of travel and the next arrives
         * from the other side, with the section swapped at the turn -- which
         * is the one moment the band is empty, so the title costs no
         * animation of its own. */
        if (slide_phase != SLIDE_NONE)
        {
            slide_t += dt;
            if (slide_phase == SLIDE_OUT)
            {
                if (slide_t >= SLIDE_MS)
                {
                    sec_focus[sec_list[sec_at]] = (short)row.focus;
                    sec_at += slide_dir;
                    row_load(&totals);
                    draw_title(year);
                    slide_phase = SLIDE_IN;
                    slide_t = 0;
                    band_shift = slide_dir * ROW_W;
                }
                else
                    band_shift = -slide_dir * ROW_W * slide_t / SLIDE_MS;
            }
            else if (slide_t >= SLIDE_MS)
            {
                slide_phase = SLIDE_NONE;
                band_shift = 0;
            }
            else
                band_shift = slide_dir * ROW_W * (SLIDE_MS - slide_t)
                           / SLIDE_MS;
            moving = true;
        }

        /* Braced: cpu_boost() is a no-op macro where the CPU frequency is
         * not adjustable, and an unbraced body then reads as an empty
         * statement. */
        if (moving && !was_moving)
        {
            cpu_boost(true);
        }

        composite();
        lcd_update_rect(0, ROW_Y, LCD_WIDTH, ROW_H);

        if (!moving && was_moving)
        {
            cpu_boost(false);
        }

        /* Hand the CPU back, every frame, always. Rockbox schedules
         * cooperatively: a screen that draws flat out never lets the codec
         * run, and the audio stops. */
        if (moving || art_want_at < art_want_n)
        {
            int spent = (int)((USEC_TIMER - now) / 1000);
            int left = FRAME_MS - spent;

            /* Read one picture with what is left of the frame rather than
             * sleeping through it.
             *
             * The cap is a frame's worth of time and a composite spends about
             * a third of it, so most frames end by waiting twenty
             * milliseconds; a thumbnail read very nearly fits in that.
             *
             * The gate is "is there any slack at all", not "is there enough
             * for the whole read". Waiting for enough means reading nothing
             * during a scroll on a device where a read costs more than the
             * slack -- and then the row shows patterns until it stops, which
             * is the thing this exists to prevent. One long frame in exchange
             * for a picture arriving on time is the right way round, and one
             * a frame is what keeps it to that. */
            while (art_want_at < art_want_n && left > 0)
            {
                unsigned key = art_want[art_want_at++];
                int stride, aw, ah;

                if (art_resolved(key))
                    continue;       /* arrived since the plan was made */

                art_budget = 1;
                art_get(key, &stride, &aw, &ah);

                left = FRAME_MS - (int)((USEC_TIMER - now) / 1000);
                break;              /* one a frame; the screen stays alive */
            }

            if (left > 0)
                sleep(left * HZ / 1000);
            else
                yield();
        }

        was_moving = moving;
    }

done:
    /* Braced: cpu_boost() is a no-op macro where the CPU frequency is not
     * adjustable, and an unbraced body then reads as an empty statement. */
    if (was_moving)
    {
        cpu_boost(false);
    }
    card_paint_set_art(NULL);
    pv_tiles_set_tint(NULL);
    fonts_unload();
    lcd_setfont(FONT_UI);
    FOR_NB_SCREENS(i)
        viewportmanager_theme_undo(i, false);
    pop_current_activity();
    app_release_buffer("spun row");

    return ret;
}

int pv_row_screen(void)
{
    bool again;
    int ret;

    do
    {
        again = false;
        ret = row_session(&again);
    }
    while (again);

    return ret;
}
