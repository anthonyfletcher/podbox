/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Text search across the tag database: type, and matching titles, albums and
 * album artists appear. Reached from the root menu (off by default) and from
 * Music; both rows are hidden unless the database is held in RAM.
 *
 * A dialog rather than a screen: a screen must take its input box out of the
 * theme's list rectangle, and themes decorate that rectangle from viewports at
 * absolute coordinates -- Themify_2's Sub_Menu_Corners spans y=52..64 over a
 * list starting at y=55, so a shortened list loses its own rounded corners.
 * Dialog styling is self-contained.
 *
 * The scan passes no filter and no clause, so tagcache_get_next() crawls the
 * tag file sequentially instead of walking the master index. It runs when the
 * query settles (SETTLE_TICKS), not per keystroke.
 *
 * Selecting a result returns a GO_TO_* code for root_menu.c to dispatch.
 *
 * Three settings shape it, under General Settings > Search: how many results
 * are kept, how many letters must be typed before a scan runs, and which of
 * the three tags leads.
 *
 * Parts, in order:
 *   - the matches
 *   - the scan
 *   - the dialog: measure, draw, actions
 *   - acting on a result
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "string-extra.h"
#include "settings/settings.h"
#include "lang.h"
#include "input/action.h"
#include "widgets/dialog.h"
#include "widgets/edit_line.h"
#include "widgets/splash.h"
#include "draw/screen_access.h"
#include "database/tagcache.h"
#include "playlist/playlist.h"
#include "screens/browse/browser_db.h"   /* the two enter_..._on_next_load */
#include "system/activity.h"
#include "system/shutdown.h"      /* default_event_handler */
#include "root_menu.h"                /* the GO_TO_* codes returned */
#include "db_search.h"

/* The "bitmaps/" prefix is required, not cosmetic: dependencies are generated
 * with -MG, so a header that does not exist yet is recorded at the path as
 * written. Only this spelling matches the rule bitmaps.make gives it. */
#include "bitmaps/podbox_icon_album.h"
#include "bitmaps/podbox_icon_artist.h"
#include "bitmaps/podbox_icon_track.h"

/* How long the query must stand still before the scan runs. */
#define SETTLE_TICKS (HZ)

/* How often the dialog loop wakes with ACTION_NONE. Fast enough for a smooth
 * marquee while one is running; slow enough to be idle otherwise, since every
 * wake repaints the whole box. */
#define POLL_ANIM    (HZ/20)
#define POLL_IDLE    (HZ/5)

/* The box, as a fraction of the display. Most of the screen, not all of it --
 * enough of the theme stays visible that this reads as something on top of the
 * UI rather than a screen of its own. */
#define BOX_W_PCT     92
/* Clearance from the top and bottom of the display. Stated as pixels rather
 * than a percentage because it is the thing being looked at -- the strip of
 * theme left showing around the box -- and a percentage turns whatever the row
 * arithmetic leaves over into extra gap on top of it. */
#define BOX_MARGIN_Y  10
#define BOX_PAD       6    /* inside the border, all round */
#define MIN_ROWS      3    /* however large the font, show at least this many */

#define MAX_QUERY    32
#define MAX_MATCHES  200
#define NAME_ARENA   (8 * 1024)
#define ROW_PAD_Y    3

/* Which tags are searched, one row per value of the order setting. Results are
 * appended in scan order and never sorted, so the row read is also the order
 * they appear in. Title is per track and dominates the cost; the other two are
 * unique-valued tags whose blobs hold one entry per distinct value, so they are
 * nearly free.
 *
 * tag_artist is deliberately absent. It is the per-track artist, so on any
 * compilation it produces a row per guest that leads to the same albums the
 * album artist already does -- duplicates that push real results off a list
 * this short. What a search means by "artist" is the album artist. */
static const int search_tags[DB_SEARCH_ORDER_COUNT][3] = {
    { tag_title,       tag_album,       tag_albumartist },
    { tag_title,       tag_albumartist, tag_album       },
    { tag_album,       tag_title,       tag_albumartist },
    { tag_album,       tag_albumartist, tag_title       },
    { tag_albumartist, tag_album,       tag_title       },
    { tag_albumartist, tag_title,       tag_album       },
};

/* Both count settings are indices into evenly spaced choice lists, so the
 * value is arithmetic rather than a second table to hold in step with
 * settings_list.c. */
static int setting_max_rows(void)
{
    return 25 * (global_settings.db_search_max_rows + 1);
}

static int setting_min_letters(void)
{
    return global_settings.db_search_min_letters + 1;
}

/* The query the screen last closed on, so reopening resumes rather than
 * starting blank. Deliberately not a setting: it is a convenience within a
 * session, not something worth persisting across a boot. */
static char last_query[MAX_QUERY + 1];

/* ---- the matches -------------------------------------------------------- */

static struct match {
    uint16_t name_off;          /* into name_arena */
    uint8_t  tag;
    int32_t  seek;              /* tag seek: what the browser is armed with */
    int32_t  idx_id;            /* master index id: what a track is played by */
} matches[MAX_MATCHES];

static int  match_count;
static char name_arena[NAME_ARENA];
static int  arena_used;
static int  row_limit;          /* the max-rows setting, taken once per scan */

static bool add_match(int tag, const char *name, int32_t seek, int32_t idx_id)
{
    int len = strlen(name) + 1;

    if (match_count >= row_limit || arena_used + len > NAME_ARENA)
        return false;

    matches[match_count].name_off = arena_used;
    matches[match_count].tag = tag;
    matches[match_count].seek = seek;
    matches[match_count].idx_id = idx_id;
    match_count++;

    memcpy(&name_arena[arena_used], name, len);
    arena_used += len;
    return true;
}

/* ---- the scan ----------------------------------------------------------- */

/* Re-run the whole search for `query`.
 *
 * The loop does not yield. What keeps this off the critical path is the settle
 * delay in front of it, not chunking: a scan only starts once the query has
 * stood still, and on a library this size it is over in a fraction of that.
 *
 * The clause machinery is deliberately unused. A clause makes tagcache walk
 * the master index -- one 96-byte index_entry per track, plus a scattered read
 * into the string blob for each. With no filter and no clause,
 * tagcache_get_next() instead crawls the tag file sequentially: the same
 * strings for a fraction of the memory traffic, and already sorted. */
static void run_search(const char *query)
{
    char buf[TAGCACHE_BUFSZ];
    struct tagcache_search tcs;
    const int *tags = search_tags[global_settings.db_search_order];
    unsigned int i;

    match_count = 0;
    arena_used = 0;

    /* The setting is a second, lower ceiling; MAX_MATCHES and NAME_ARENA stay
     * the hard array bounds, so no setting can overrun them. */
    row_limit = MIN(setting_max_rows(), MAX_MATCHES);

    /* Too short to bother with -- and, since the threshold is at least one
     * letter, this is also the empty-query case. Everything above is already
     * cleared, so a query backspaced below the threshold empties the results
     * rather than leaving the last scan's on screen. */
    if ((int)strlen(query) < setting_min_letters())
        return;

    for (i = 0; i < ARRAYLEN(search_tags[0]); i++)
    {
        if (!tagcache_search(&tcs, tags[i]))
            continue;

        while (tagcache_get_next(&tcs, buf, sizeof(buf)))
        {
            if (strcasestr(buf, query)
                && !add_match(tags[i], buf, tcs.result_seek, tcs.idx_id))
                break;
        }

        tagcache_search_finish(&tcs);
    }
}

/* ---- the dialog --------------------------------------------------------- */

struct search_dlg {
    struct edit_line ed;
    char   query[MAX_QUERY + 1];
    long   last_edit_tick;
    bool   pending;             /* the query has changed and not been scanned */
    int    selected;            /* row index, or -1 while the focus is the query */
    int    top_row;             /* first visible row */
    int    visible_rows;        /* set by draw(), read by the action handler */

    /* Marquee for the selected row, driven from the poll tick rather than the
     * shared scroll engine -- see dialog_draw_button_ex(). */
    int    scroll_px;
    int    scroll_max;          /* what the last draw said overflows */
    int    scroll_dir;          /* +1 running left, -1 coming back */
    int    scroll_wait;         /* polls to hold still at each end */
    int    scroll_row;          /* the row the marquee belongs to */
    int    chosen;              /* the row SELECT accepted, or -1 */
};

#define SCROLL_STEP  3          /* pixels per poll */
#define SCROLL_PAUSE (HZ / POLL_ANIM)   /* about a second at each end */

/* The icon says which tag matched, so the row is just the text. Album artist
 * shares the artist icon -- the distinction is not worth a third glyph. */
static const struct bitmap *row_icon(int tag)
{
    switch (tag)
    {
        case tag_album:       return &bm_podbox_icon_album;
        case tag_artist:
        case tag_albumartist: return &bm_podbox_icon_artist;
        default:              return &bm_podbox_icon_track;
    }
}

/* Gap between result rows, so adjacent buttons read as separate. */
#define ROW_GAP 2

/* Air above and below the rule, as a fraction of the font. */
#define RULE_AIR(ch_h) ((ch_h) / 4)

/* Where the rule sits, and where the rows start just under it. Both derived
 * here so measure() and draw() cannot disagree: they did, and the head then
 * reserved a whole character of space around a rule drawn a quarter of one
 * below the query -- a dead band under the rule, and a row's worth of height
 * spent on nothing. */
static int rule_offset(struct screen *display)
{
    return edit_line_height(display) + RULE_AIR(display->getcharheight());
}

static int head_height(struct screen *display)
{
    return rule_offset(display) + 1 + RULE_AIR(display->getcharheight());
}

static int row_height(struct screen *display)
{
    return display->getcharheight() + 2 * ROW_PAD_Y;
}

/* Size the box to a whole number of rows rather than a percentage, so the
 * bottom of the box is the bottom of a row at every font size instead of
 * clipping one arbitrarily. */
static void search_measure(struct dialog *d, struct screen *display,
                           struct viewport *box, void *data)
{
    struct search_dlg *s = data;
    struct dialog_insets in;
    int row_h = row_height(display);
    int chrome, limit, rows, box_h;

    dialog_get_insets(&d->style, &in);
    chrome = in.top + in.bottom + head_height(display);

    /* One row short of what actually fits: taking every row the margins allow
     * left the box near enough full-screen that it stopped reading as a dialog
     * over the browser and started reading as a screen of its own. Only when
     * there is a row to spare, so at a large font the row goes to showing
     * results rather than to whitespace. */
    limit = display->lcdheight - 2 * BOX_MARGIN_Y;
    rows = (limit - chrome + ROW_GAP) / (row_h + ROW_GAP);
    if (rows > MIN_ROWS)
        rows--;
    if (rows < MIN_ROWS)
        rows = MIN_ROWS;

    box_h = chrome + rows * (row_h + ROW_GAP) - ROW_GAP;
    if (box_h > display->lcdheight)
        box_h = display->lcdheight;

    /* draw() reads this rather than working it out from the height it was
     * given, so a rounding difference cannot leave a half row visible. */
    s->visible_rows = rows;

    box->width = display->lcdwidth * BOX_W_PCT / 100;
    box->height = box_h;
    box->x = (display->lcdwidth - box->width) / 2;
    box->y = (display->lcdheight - box_h) / 2;
}

static void search_draw(struct dialog *d, struct screen *display,
                        struct viewport *content, void *data)
{
    struct search_dlg *s = data;
    int row_h = row_height(display);
    int line_h = edit_line_height(display);
    char buf[MAX_PATH];

    /* --- the query, in a clipped sub-viewport so long text can scroll --- */
    struct viewport tvp = *content;
    tvp.height = line_h;
    display->set_viewport(&tvp);
    edit_line_draw(display, &s->ed, tvp.width);

    /* --- a rule between the query and what it found --- */
    display->set_viewport(content);
    display->set_drawmode(DRMODE_FG);
    display->hline(0, content->width - 1, rule_offset(display));
    display->set_drawmode(DRMODE_SOLID);

    /* --- the results, each row a button so the selection carries the accent
     * colour the rest of the dialog world uses --- */
    int rows_y = head_height(display);

    if (s->selected >= 0)
    {
        if (s->selected < s->top_row)
            s->top_row = s->selected;
        else if (s->selected >= s->top_row + s->visible_rows)
            s->top_row = s->selected - s->visible_rows + 1;
    }
    if (s->top_row > match_count - s->visible_rows)
        s->top_row = match_count - s->visible_rows;
    if (s->top_row < 0)
        s->top_row = 0;

    /* The marquee belongs to one row; moving the selection starts it over.
     * Done here rather than at every place the selection changes, so no path
     * can leave the new row mid-scroll. */
    if (s->scroll_row != s->selected)
    {
        s->scroll_row = s->selected;
        s->scroll_px = 0;
        s->scroll_max = 0;
        s->scroll_dir = 1;
        s->scroll_wait = SCROLL_PAUSE;
    }

    for (int r = 0; r < s->visible_rows; r++)
    {
        int item = s->top_row + r;
        bool sel;

        if (item >= match_count)
            break;

        sel = (item == s->selected);
        strlcpy(buf, &name_arena[matches[item].name_off], sizeof(buf));

        dialog_draw_button_ex(display, &d->style, 0,
                              rows_y + r * (row_h + ROW_GAP),
                              content->width, row_h, buf, sel,
                              DIALOG_BTN_LEFT, row_icon(matches[item].tag),
                              sel ? s->scroll_px : 0,
                              sel ? &s->scroll_max : NULL);
    }

    display->set_drawmode(DRMODE_SOLID);
}

static int search_on_action(struct dialog *d, int action, void *data)
{
    struct search_dlg *s = data;
    (void)d;

    /* The wheel means the query while the query has focus, and the results
     * once it does not -- the same handover the text-input dialog makes to its
     * button row.
     *
     * The wheel and the caret taps never cross between the two: inside the
     * query they are the only way to change, advance or delete a character, so
     * giving any of them a second meaning would cost an edit. Moving between
     * the two is PLAY (down) and MENU (up), the physical buttons either side
     * of the wheel, which do nothing else here. */
    if (edit_line_owns_action(action))
    {
        if (s->selected < 0)
        {
            if (edit_line_action(&s->ed, action))
            {
                edit_line_get(&s->ed, s->query, sizeof(s->query), false);
                s->last_edit_tick = current_tick;
                s->pending = true;
            }
        }
        else switch (action)
        {
            case ACTION_KBD_DOWN:
                if (s->selected + 1 < match_count)
                    s->selected++;
                break;
            case ACTION_KBD_UP:
                if (s->selected > 0)
                    s->selected--;
                break;
        }
        return DIALOG_CONTINUE;
    }

    switch (action)
    {
        case ACTION_KBD_DONE:      /* PLAY (down): into the results */
            if (s->selected < 0 && match_count > 0)
                s->selected = 0;
            break;

        case ACTION_KBD_SELECT:
            if (s->selected < 0)
            {
                if (match_count > 0)
                    s->selected = 0;
                break;
            }
            s->chosen = s->selected;
            return DIALOG_ACCEPT;

        case ACTION_KBD_ABORT:     /* MENU (up): results -> query -> out */
            if (s->selected >= 0)
            {
                s->selected = -1;
                break;
            }
            return DIALOG_CANCEL;

        case ACTION_NONE:
            /* The settle timer. Nothing else drives the scan. */
            if (s->pending
                && current_tick - s->last_edit_tick >= SETTLE_TICKS)
            {
                run_search(s->query);
                s->pending = false;
                s->top_row = 0;
                if (s->selected >= match_count)
                    s->selected = match_count - 1;
            }

            /* Only tick fast while something is actually moving. Every pass is
             * a full repaint of the box -- the status bar renders into the
             * framebuffer under it on each get_action, so there is no smaller
             * update to make -- and at rest that is pure waste. */
            d->poll_ticks = (s->scroll_max > 0) ? POLL_ANIM : POLL_IDLE;

            /* One step of the selected row's marquee: out to the end of the
             * text, back to the start, pausing at both. */
            if (s->scroll_max > 0)
            {
                if (s->scroll_wait > 0)
                    s->scroll_wait--;
                else
                {
                    s->scroll_px += s->scroll_dir * SCROLL_STEP;
                    if (s->scroll_px >= s->scroll_max)
                    {
                        s->scroll_px = s->scroll_max;
                        s->scroll_dir = -1;
                        s->scroll_wait = SCROLL_PAUSE;
                    }
                    else if (s->scroll_px <= 0)
                    {
                        s->scroll_px = 0;
                        s->scroll_dir = 1;
                        s->scroll_wait = SCROLL_PAUSE;
                    }
                }
            }
            break;

        default:
            if (default_event_handler(action) == SYS_USB_CONNECTED)
                return DIALOG_ABORT;
            break;
    }
    return DIALOG_CONTINUE;
}

/* ---- acting on a result ------------------------------------------------- */

/* A title row is one track, so it plays rather than browsing to itself. The
 * path is not held in the match -- only the master index id is -- because the
 * filename is the one tag the ramcache does not keep as a string. */
static int play_track(const struct match *m)
{
    struct tagcache_search tcs;
    char path[MAX_PATH];
    bool got;

    if (!tagcache_search(&tcs, tag_filename))
        return GO_TO_PREVIOUS;
    got = tagcache_retrieve(&tcs, m->idx_id, tag_filename, path, sizeof(path));
    tagcache_search_finish(&tcs);

    if (!got || playlist_create(NULL, NULL) < 0)
        return GO_TO_PREVIOUS;
    if (playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST, false, true) < 0)
        return GO_TO_PREVIOUS;

    playlist_start(0, 0, 0);
    return GO_TO_WPS;
}

static int act_on(const struct match *m)
{
    const char *name = &name_arena[m->name_off];

    switch (m->tag)
    {
        case tag_album:
            /* straight into that album's tracks, as Album covers does */
            browser_db_enter_album_tracks_on_next_load(m->seek, name);
            return GO_TO_ALBUM_COVERS_TRACKS;
        case tag_albumartist:
            /* straight into that artist's albums, as Artist portraits does */
            browser_db_enter_artist_albums_on_next_load(m->seek, name);
            return GO_TO_ALBUM_COVERS_TRACKS;
        default:
            return play_track(m);
    }
    return GO_TO_PREVIOUS;
}

int db_search_run(void)
{
    static const struct dialog_callbacks cb = {
        .measure   = search_measure,
        .draw      = search_draw,
        .on_action = search_on_action,
    };
    static struct search_dlg s;
    struct dialog d;
    struct dialog_style style;

    /* Both gates the real feature would need. A commit holds tagcache's
     * read_lock for its whole length and tagcache_search() waits on it with
     * sleep(1), so calling in from a foreground screen during one is a freeze
     * with no way out; and off the ramcache every entry costs a seek and a
     * read, which on a disk is not slow but unusable. */
    if (!tagcache_is_usable() || tagcache_get_commit_step() != 0)
    {
        splash(HZ * 2, "Database busy");
        return GO_TO_PREVIOUS;
    }
    if (!tagcache_is_in_ram())
    {
        splash(HZ * 2, "Needs the database in RAM");
        return GO_TO_PREVIOUS;
    }

    /* Reopen on the last thing searched for. A search is usually one of a run
     * -- play something, come back, refine it -- and retyping the stem on a
     * click wheel is most of the work. The results come back on their own: the
     * query is marked pending, so the settle timer re-runs it a moment after
     * the screen appears rather than making the caller wait for a scan. */
    edit_line_init(&s.ed, last_query, MAX_QUERY);
    strlcpy(s.query, last_query, sizeof(s.query));
    s.pending = (s.query[0] != '\0');
    s.last_edit_tick = current_tick;
    s.selected = -1;
    s.top_row = 0;
    s.visible_rows = 1;
    s.scroll_row = -1;
    s.scroll_px = 0;
    s.scroll_max = 0;
    s.scroll_dir = 1;
    s.scroll_wait = SCROLL_PAUSE;
    s.chosen = -1;

    match_count = 0;
    arena_used = 0;

    /* Rows are buttons, but a list of forty outlined boxes reads as clutter.
     * An unselected row is therefore borderless and takes the box's own fill,
     * so it is just text and an icon on the dialog; only the selected one
     * becomes a shape, in the accent. */
    style = *dialog_get_default_style();
    style.box_margin = BOX_PAD;     /* the default 10 costs a row of height */
    style.button_border_width = 0;
    style.button_bg = DIALOG_COLOR_INHERIT;
    style.button_fg = DIALOG_COLOR_INHERIT;
    style.button_bg_selected = DIALOG_COLOR_ACCENT;
    style.button_fg_selected = DIALOG_COLOR_ON_ACCENT;

    push_current_activity(ACTIVITY_DB_SEARCH);
    dialog_init(&d, CONTEXT_KEYBOARD, str(LANG_DB_SEARCH), &style, &cb, &s);

    int res = dialog_run(&d, POLL_IDLE);

    strlcpy(last_query, s.query, sizeof(last_query));

    pop_current_activity();

    if (res == DIALOG_ABORT)
        return GO_TO_ROOT;        /* USB: root_menu handles the screen */

    if (s.chosen >= 0 && s.chosen < match_count)
        return act_on(&matches[s.chosen]);

    return GO_TO_PREVIOUS;
}
