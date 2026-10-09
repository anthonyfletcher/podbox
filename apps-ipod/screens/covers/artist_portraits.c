/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Artist Portraits: the coverflow carousel (screens/covers/carousel.c) over
 * the album-artist list, showing artist photos from <artist>/folder.jpg. A
 * second carousel_model beside album_covers.c, whose data is the album-artist
 * index; selecting an artist opens that album-artist's own album listing.
 ****************************************************************************/

#include <string.h>
#include "string-extra.h"    /* strcasecmp */
#include "config.h"
#include "system.h"          /* ALIGN_BUFFER */
#include "settings/settings.h"        /* global_settings */
#include "database/tagcache.h"   /* UNTAGGED */
#include "metadata.h"        /* struct mp3entry */
#include "audio.h"           /* audio_status, audio_current_track */
#include "draw/screen_access.h"   /* screens[], SCREEN_MAIN */
#include "lcd.h"
#include "font.h"
#include "screens/browse/browser_db.h"         /* browser_db_enter_artist_albums_on_next_load */
#include "root_menu.h"       /* GO_TO_* screen codes, MENU_ATTACHED_USB */
#include "album_covers.h"    /* artist_portraits(), ALBUM_NAME_* */
#include "carousel.h"
#include "draw/line.h"              /* TEXT_FIT_BUF */
#include "lang.h"
#include "database/db_summary.h" /* build_artist_index() */

/* "Resume to this slide on next open", set by artist_enter() and consumed by
 * artist_set_initial(). The album model's pf_resume_* pair is deliberately not
 * reused: the two indices address different arrays (albums here, artists
 * there), nothing in the shared pair records which model wrote it, and either
 * model's set_initial() consumes whatever is pending -- so an album index left
 * behind by Album Covers used to open this screen on an unrelated artist. */
static int  artist_resume_index;
static bool artist_resume_valid = false;

static char *artist_name(int index, char *buf, size_t size)
{
    return db_summary_name(&carousel_idx, tag_albumartist,
                           carousel_idx.artist_index[index].seek, buf, size);
}

/* The slide for the album-artist of `id3`, or -1 if this list has no such
 * artist. Matched by name rather than by any position carried in the database:
 * artist_build_index() re-sorts the list when "Sort Artists By" is set to
 * plays, so index order is not stable between opens. */
static int artist_find_index(const struct mp3entry *id3)
{
    const char *current = UNTAGGED;
    char name[TAGCACHE_BUFSZ];

    if (!id3)
        return -1;

    /* This list is built from the album-artist tag, so prefer that; a file
     * carrying only a track artist is filed under that instead. */
    if (id3->albumartist)
        current = id3->albumartist;
    else if (id3->artist)
        current = id3->artist;

    for (int i = 0; i < carousel_idx.artist_ct; i++)
        if (!strcasecmp(artist_name(i, name, sizeof(name)), current))
            return i;

    return -1;
}

/* carousel_model.build_index: build the persistent album-artist list into the
 * shared buffer (reuses build_artist_index, which album covers' own
 * create_album_index otherwise uses only as transient scaffolding). No on-disk
 * cache -- artists are few, so a rebuild each open is cheap. */
/* Most played first. Ties keep the order the database gave, which is by name,
 * so a library where nothing has been played looks exactly as it did before
 * the option existed. */
static int compare_artists_by_plays(const void *a_v, const void *b_v)
{
    const struct artist_data *a = a_v;
    const struct artist_data *b = b_v;

    return b->playcount - a->playcount;
}

/* By name past a leading article, for Sort Ignoring The/A/An. Off, the
 * database's own order is already by name and nothing is sorted. */
static int compare_artists_by_name(const void *a_v, const void *b_v)
{
    const struct artist_data *a = a_v;
    const struct artist_data *b = b_v;
    char an[TAGCACHE_BUFSZ], bn[TAGCACHE_BUFSZ];

    return strcasecmp(
        tagcache_skip_article(db_summary_name(&carousel_idx, tag_albumartist,
                                              a->seek, an, sizeof(an))),
        tagcache_skip_article(db_summary_name(&carousel_idx, tag_albumartist,
                                              b->seek, bn, sizeof(bn))));
}

static int artist_build_index(void)
{
    void *buf = carousel_idx.buf;
    size_t buf_size = carousel_idx.buf_sz;
    bool by_plays = carousel_artist_order() == SORT_ARTISTS_BY_PLAYS;
    int res;

    ALIGN_BUFFER(buf, buf_size, sizeof(long));

    res = db_summary_load_artists(&carousel_idx, &buf, &buf_size);
    if (res < SUCCESS)
        return res;

    carousel_idx.buf = buf;
    carousel_idx.buf_sz = buf_size;
    carousel_idx.album_ct = 0;   /* artist model has no album list */

    if (by_plays)
        qsort(carousel_idx.artist_index, carousel_idx.artist_ct,
              sizeof(struct artist_data), compare_artists_by_plays);
    else if (carousel_skips_articles())
        qsort(carousel_idx.artist_index, carousel_idx.artist_ct,
              sizeof(struct artist_data), compare_artists_by_name);

    return SUCCESS;
}

static int artist_count(void)
{
    return carousel_idx.artist_ct;
}

/* carousel_model.art_key for the artist model: the shared cache's key for this
 * album-artist's own folder, resolved when the index was built. */
static unsigned int artist_art_key(int index)
{
    return carousel_idx.artist_index[index].art_hash;
}

/* Select an artist: open that album-artist's own album listing in the database
 * browser (armed for the next load; BACK returns here). Records the slide to
 * resume to, so backing out lands on the artist just visited rather than on
 * the first one. */
static int artist_enter(int index)
{
    char name[TAGCACHE_BUFSZ];

    artist_resume_index = index;
    artist_resume_valid = true;
    browser_db_enter_artist_albums_on_next_load(
                                carousel_idx.artist_index[index].seek,
                                artist_name(index, name, sizeof(name)));
    return GO_TO_ALBUM_COVERS_TRACKS;
}

/* The first letter of the name an artist sorts by, which is what the letter
 * jumps compare. */
static char artist_initial(int index)
{
    char name[TAGCACHE_BUFSZ];

    return carousel_sort_name(artist_name(index, name, sizeof(name)))[0];
}

/* Jump to the next/previous artist whose name starts with a different letter. */
static int artist_jump_next(void)
{
    char current = artist_initial(center_index);
    for (int i = center_index + 1; i < carousel_idx.artist_ct; i++)
        if (artist_initial(i) != current)
            return i;
    return carousel_idx.artist_ct - 1;
}

/* Step back to the first artist of a letter run. Same walk as the album
 * model's jmp_idx_prev() -- see its comment for why the two cases collapse
 * into these three lines, and for where the shape came from. */
static int artist_jump_prev(void)
{
    char current = artist_initial(center_index);
    int i = center_index - 1;

    if (i > 0)
    {
        if (artist_initial(i) != current)
            current = artist_initial(i);
        while (i > 0 && artist_initial(i - 1) == current)
            i--;
        return i;
    }
    return 0;
}

/* The artist name caption -- a single line, positioned like the album name line
 * (album covers' second, artist/year line has no artist-mode equivalent). */
static void artist_draw_text(void)
{
    struct pf_caption cap;
    int txt_x, txt_y;
    char name[TAGCACHE_BUFSZ], fit[TEXT_FIT_BUF];
    const char *line;

    if (global_settings.album_covers_show_album_name == ALBUM_NAME_HIDE)
        return;

    artist_name(center_index, name, sizeof(name));
    struct viewport *saved_vp = carousel_text_begin();
    lcd_set_foreground(pf_fg_color);
    lcd_setfont(pf_bold_font);
    line = carousel_caption_fit(name, fit, sizeof(fit));
    /* Nothing but the slide decides this caption, hence the 0 variant. */
    if (carousel_caption_changed(center_index, 0))
        set_scroll_line(line, PF_SCROLL_ALBUM);

    /* One line where the album carousel draws two. The engine reserves the
     * same band either way and centres whatever it is given in it, so this
     * lands in the middle of that band rather than sitting high in space
     * sized for a pair -- which is what made it read as misaligned against
     * the same screen showing albums. */
    carousel_caption_layout(false, &cap);
    txt_y = cap.y1;

    txt_x = get_scroll_line_offset(PF_SCROLL_ALBUM);
    lcd_putsxy(txt_x, txt_y, line);
    lcd_setfont(screens[SCREEN_MAIN].getuifont());
    carousel_text_end(saved_vp);
}

static void carousel_sort_noop(void)
{
}

static void artist_set_initial(const char *selected_file);

/* carousel_model.on_menu: the same Carousel settings menu the album carousel
 * opens. Artists have no pfraw cache, so the album model's cache_version
 * follow-up has no artist equivalent. */
static int artist_on_menu(void)
{
    int old_show_name = global_settings.album_covers_show_album_name;
    bool old_statusbar = global_settings.album_covers_statusbar;
    int old_sort = carousel_artist_order();
    bool old_articles = carousel_skips_articles();
    int old_filter[CAROUSEL_FILTER_SLOTS];
    char old_chain[CAROUSEL_FILTER_MAX];
    long seek = carousel_idx.artist_index[center_index].seek;
    int32_t commitid = carousel_idx.commitid;
    uint32_t generation = carousel_idx.generation;

    memcpy(old_filter, global_settings.album_covers_filter, sizeof(old_filter));
    strmemccpy(old_chain, global_settings.album_covers_filter_chain,
               sizeof(old_chain));

    if (carousel_settings_menu() == MENU_ATTACHED_USB)
        return GO_TO_ROOT;

    /* The caption layout decides the text margin and the status bar decides
     * the viewport, both computed during init(), and init() sorts the index
     * -- so a change to any of them needs a full rebuild, not just a redraw.
     * The artist on screen stays on screen. */
    if (global_settings.album_covers_show_album_name != old_show_name
        || global_settings.album_covers_statusbar != old_statusbar
        || carousel_artist_order() != old_sort
        || carousel_skips_articles() != old_articles)
    {
        if (!carousel_reinit())
            return GO_TO_PREVIOUS;
        /* The seek names the same artist only within one commit */
        if (carousel_idx.commitid != commitid
            || carousel_idx.generation != generation)
            artist_set_initial(NULL);
        else
            for (int i = 0; i < carousel_idx.artist_ct; i++)
                if (carousel_idx.artist_index[i].seek == seek)
                {
                    set_current_slide(i);
                    break;
                }
    }

    /* A treatment reaches a slide only as it is loaded, so the decoded ones
     * have to go */
    if (memcmp(old_filter, global_settings.album_covers_filter,
               sizeof(old_filter))
        || strcmp(old_chain, global_settings.album_covers_filter_chain))
        carousel_drop_slides();

    /* Re-apply the live geometry settings (zoom / margins / tilt). */
    carousel_refresh();
    return CAROUSEL_MENU_RELOADED;
}

/* Where to open: the artist just visited, else whoever is playing, else the
 * top of the list. The same order the album model uses, minus its
 * selected_file case -- nothing enters this screen with a file in hand. */
static void artist_set_initial(const char *selected_file)
{
    int index;

    (void)selected_file;

    if (artist_resume_valid)
    {
        artist_resume_valid = false;
        set_current_slide(artist_resume_index);
        return;
    }

    index = audio_status() ? artist_find_index(audio_current_track()) : -1;
    set_current_slide(index >= 0 ? index : 0);
}

static const struct carousel_model artist_model = {
    .build_index = artist_build_index,
    .count       = artist_count,
    .art_key     = artist_art_key,
    .enter       = artist_enter,
    .jump_prev   = artist_jump_prev,
    .jump_next   = artist_jump_next,
    .draw_text   = artist_draw_text,
    .sort_next   = carousel_sort_noop,
    .sort_prev   = carousel_sort_noop,
    .set_initial = artist_set_initial,
    .on_menu     = artist_on_menu,
    .owns_cache_version = false,
    .title       = (const char *)ID2P(LANG_ARTIST_PORTRAITS),
};

int artist_portraits(const char *selected_file)
{
    return carousel_run(&artist_model, selected_file);
}
