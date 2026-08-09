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
#include "database/db_summary.h" /* build_artist_index() */

/* "Resume to this slide on next open", set by artist_enter() and consumed by
 * artist_set_initial(). The album model's pf_resume_* pair is deliberately not
 * reused: the two indices address different arrays (albums here, artists
 * there), nothing in the shared pair records which model wrote it, and either
 * model's set_initial() consumes whatever is pending -- so an album index left
 * behind by Album Covers used to open this screen on an unrelated artist. */
static int  artist_resume_index;
static bool artist_resume_valid = false;

static char *artist_name(int index)
{
    return carousel_idx.artist_names
         + carousel_idx.artist_index[index].name_idx;
}

/* The slide for the album-artist of `id3`, or -1 if this list has no such
 * artist. Matched by name rather than by any position carried in the database:
 * artist_build_index() re-sorts the list when "Sort Artists By" is set to
 * plays, so index order is not stable between opens. */
static int artist_find_index(const struct mp3entry *id3)
{
    const char *current = UNTAGGED;

    if (!id3)
        return -1;

    /* This list is built from the album-artist tag, so prefer that; a file
     * carrying only a track artist is filed under that instead. */
    if (id3->albumartist)
        current = id3->albumartist;
    else if (id3->artist)
        current = id3->artist;

    for (int i = 0; i < carousel_idx.artist_ct; i++)
        if (!strcasecmp(artist_name(i), current))
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

static int artist_build_index(void)
{
    struct tagcache_search tcs;   /* local; the engine's shared tcs stays private */
    void *buf = carousel_idx.buf;
    size_t buf_size = carousel_idx.buf_sz;
    bool by_plays = global_settings.album_covers_sort_artists_by
                        == SORT_ARTISTS_BY_PLAYS;
    int res;

    ALIGN_BUFFER(buf, buf_size, sizeof(long));

    /* Read the saved index first, and only walk the database when there is
     * none to read.
     *
     * Rebuilding here instead looks cheap -- artists are few -- and is not.
     * Sorting by plays needs each artist's figures, which cost a filtered
     * search per artist on the build path and come free with the saved index.
     * Reading wins in both sort orders, and makes the by-plays one free. */
    res = db_summary_load_artists(&carousel_idx, &buf, &buf_size);

    /* Only a missing or unusable index falls through to a build. ERROR_USER_ABORT
     * means the user cancelled out of waiting for the background pass, and
     * answering that by starting the very work they declined to wait for
     * would be the opposite of what they asked -- see wait_for_background(). */
    if (res == ERROR_NO_ARTISTS)
    {
        /* The background pass has not produced an index yet, or a rebuild is
         * pending. Ask for the figures here only if the sort will use them. */
        buf = carousel_idx.buf;
        buf_size = carousel_idx.buf_sz;
        ALIGN_BUFFER(buf, buf_size, sizeof(long));
        res = db_summary_build_artists(&carousel_idx, &tcs, &buf, &buf_size,
                                       by_plays);
    }

    if (res < SUCCESS)
        return res;

    carousel_idx.buf = buf;
    carousel_idx.buf_sz = buf_size;
    carousel_idx.album_ct = 0;   /* artist model has no album list */

    if (by_plays)
        qsort(carousel_idx.artist_index, carousel_idx.artist_ct,
              sizeof(struct artist_data), compare_artists_by_plays);

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

/* Artists have no per-screen pfraw cache; a missing photo just shows the empty
 * slide. */
static bool no_legacy_art(int index, char *path, int len)
{
    (void)index; (void)path; (void)len;
    return false;
}

/* Select an artist: open that album-artist's own album listing in the database
 * browser (armed for the next load; BACK returns here). Records the slide to
 * resume to, so backing out lands on the artist just visited rather than on
 * the first one. */
static int artist_enter(int index)
{
    artist_resume_index = index;
    artist_resume_valid = true;
    browser_db_enter_artist_albums_on_next_load(
                                carousel_idx.artist_index[index].seek,
                                artist_name(index));
    return GO_TO_ALBUM_COVERS_TRACKS;
}

/* Jump to the next/previous artist whose name starts with a different letter. */
static int artist_jump_next(void)
{
    char *current = artist_name(center_index);
    for (int i = center_index + 1; i < carousel_idx.artist_ct; i++)
        if (strncmp(artist_name(i), current, 1))
            return i;
    return carousel_idx.artist_ct - 1;
}

/* Step back to the first artist of a letter run. Same walk as the album
 * model's jmp_idx_prev() -- see its comment for why the two cases collapse
 * into these three lines, and for where the shape came from. */
static int artist_jump_prev(void)
{
    char *current = artist_name(center_index);
    int i = center_index - 1;

    if (i > 0)
    {
        if (strncmp(artist_name(i), current, 1))
            current = artist_name(i);
        while (i > 0 && strncmp(artist_name(i - 1), current, 1) == 0)
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
    char *name;

    if (global_settings.album_covers_show_album_name == ALBUM_NAME_HIDE)
        return;

    name = artist_name(center_index);
    struct viewport *saved_vp = carousel_text_begin();
    lcd_set_foreground(pf_fg_color);
    lcd_setfont(pf_bold_font);
    /* Nothing but the slide decides this caption, hence the 0 variant. */
    if (carousel_caption_changed(center_index, 0))
        set_scroll_line(name, PF_SCROLL_ALBUM);

    /* One line where the album carousel draws two. The engine reserves the
     * same band either way and centres whatever it is given in it, so this
     * lands in the middle of that band rather than sitting high in space
     * sized for a pair -- which is what made it read as misaligned against
     * the same screen showing albums. */
    carousel_caption_layout(false, &cap);
    txt_y = cap.y1;

    txt_x = get_scroll_line_offset(PF_SCROLL_ALBUM);
    lcd_putsxy(txt_x, txt_y, name);
    lcd_setfont(screens[SCREEN_MAIN].getuifont());
    carousel_text_end(saved_vp);
}

static void carousel_sort_noop(void)
{
}

/* carousel_model.on_menu: the same Carousel settings menu the album carousel
 * opens. Only the caption-layout setting needs acting on here -- artists have
 * no sort order (sort_next/prev are no-ops) and no pfraw cache, so the album
 * model's sort and cache_version follow-ups have no artist equivalent. */
static int artist_on_menu(void)
{
    int old_show_name = global_settings.album_covers_show_album_name;
    bool old_statusbar = global_settings.album_covers_statusbar;

    if (carousel_settings_menu() == MENU_ATTACHED_USB)
        return GO_TO_ROOT;

    /* The caption layout decides the text margin and the status bar decides
     * the viewport, both computed during init() -- so a change to either needs
     * a full rebuild, not just a redraw. */
    if (global_settings.album_covers_show_album_name != old_show_name
        || global_settings.album_covers_statusbar != old_statusbar)
    {
        if (!carousel_reinit())
            return GO_TO_PREVIOUS;
    }

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
    .legacy_art  = no_legacy_art,
    .enter       = artist_enter,
    .jump_prev   = artist_jump_prev,
    .jump_next   = artist_jump_next,
    .draw_text   = artist_draw_text,
    .sort_next   = carousel_sort_noop,
    .sort_prev   = carousel_sort_noop,
    .set_initial = artist_set_initial,
    .on_menu     = artist_on_menu,
    .has_pfraw_cache = false,
    .title       = "Artist Portraits",
};

int artist_portraits(const char *selected_file)
{
    return carousel_run(&artist_model, selected_file);
}
