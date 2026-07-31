/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Cover-flow artist browser. A second carousel.c model, showing artist
 * photos instead of album art.
 ****************************************************************************/

/* Artist Portraits: the coverflow carousel (apps/gui/carousel via
 * album_covers.c) over the album-artist list, showing artist photos from
 * <artist>/folder.jpg. A carousel_model whose data is the album-artist index;
 * selecting an artist opens that album-artist's own album listing. */

#include <string.h>
#include "config.h"
#include "system.h"          /* ALIGN_BUFFER */
#include "settings/settings.h"        /* global_settings */
#include "database/tagcache.h"
#include "draw/screen_access.h"   /* screens[], SCREEN_MAIN */
#include "lcd.h"
#include "font.h"
#include "screens/browse/browser_db.h"         /* browser_db_enter_artist_albums_on_next_load */
#include "root_menu.h"       /* GO_TO_* screen codes, MENU_ATTACHED_USB */
#include "album_covers.h"    /* artist_portraits(), ALBUM_NAME_* */
#include "carousel.h"
#include "database/db_index.h" /* build_artist_index() */

static char *artist_name(int index)
{
    return pf_idx.artist_names + pf_idx.artist_index[index].name_idx;
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
    void *buf = pf_idx.buf;
    size_t buf_size = pf_idx.buf_sz;
    bool by_plays = global_settings.album_covers_sort_artists_by
                        == SORT_ARTISTS_BY_PLAYS;
    int res;

    ALIGN_BUFFER(buf, buf_size, sizeof(long));

    /* Read the saved index first, and only walk the database when there is
     * none to read.
     *
     * This used to rebuild on every open, on the reasoning that artists are
     * few and a rebuild is cheap. Cheap is not free, and it stopped being
     * either once sorting by plays was an option: the figures that sort needs
     * cost a filtered search per artist on the build path, where the saved
     * index simply has them. Reading is faster in both orders, and makes the
     * sort cost nothing at all. */
    res = db_index_load_artists(&pf_idx, &buf, &buf_size);

    /* Only a missing or unusable index falls through to a build. ERROR_USER_ABORT
     * means the user cancelled out of waiting for the background pass, and
     * answering that by starting the very work they declined to wait for
     * would be the opposite of what they asked -- see wait_for_background(). */
    if (res == ERROR_NO_ARTISTS)
    {
        /* The background pass has not produced an index yet, or a rebuild is
         * pending. Ask for the figures here only if the sort will use them. */
        buf = pf_idx.buf;
        buf_size = pf_idx.buf_sz;
        ALIGN_BUFFER(buf, buf_size, sizeof(long));
        res = db_index_build_artists(&pf_idx, &tcs, &buf, &buf_size, by_plays);
    }

    if (res < SUCCESS)
        return res;

    pf_idx.buf = buf;
    pf_idx.buf_sz = buf_size;
    pf_idx.album_ct = 0;   /* artist model has no album list */

    if (by_plays)
        qsort(pf_idx.artist_index, pf_idx.artist_ct,
              sizeof(struct artist_data), compare_artists_by_plays);

    return SUCCESS;
}

static int artist_count(void)
{
    return pf_idx.artist_ct;
}

/* carousel_model.art_key for the artist model: the shared cache's key for this
 * album-artist's own folder, resolved when the index was built. */
static unsigned int artist_art_key(int index)
{
    return pf_idx.artist_index[index].art_hash;
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
 * resume to (shared pf_resume_* state), so backing out lands on the artist just
 * visited rather than the first one. */
static int artist_enter(int index)
{
    pf_resume_album_index = index;
    pf_resume_last_album = true;
    browser_db_enter_artist_albums_on_next_load(pf_idx.artist_index[index].seek,
                                             artist_name(index));
    return GO_TO_ALBUM_COVERS_TRACKS;
}

/* Jump to the next/previous artist whose name starts with a different letter. */
static int artist_jump_next(void)
{
    char *current = artist_name(center_index);
    for (int i = center_index + 1; i < pf_idx.artist_ct; i++)
        if (strncmp(artist_name(i), current, 1))
            return i;
    return pf_idx.artist_ct - 1;
}

static int artist_jump_prev(void)
{
    char *current = artist_name(center_index);
    for (int i = center_index - 1; i > 0; i--)
    {
        if (strncmp(artist_name(i), current, 1))
            current = artist_name(i);
        while (i > 0)
        {
            if (strncmp(artist_name(i - 1), current, 1))
                break;
            i--;
        }
        return i;
    }
    return 0;
}

/* The artist name caption -- a single line, positioned like the album name line
 * (album covers' second, artist/year line has no artist-mode equivalent). */
static void artist_draw_text(void)
{
    static int prev_index = -1;
    int char_height, txt_x, txt_y;
    char *name;

    if (global_settings.album_covers_show_album_name == ALBUM_NAME_HIDE)
        return;

    name = artist_name(center_index);
    struct viewport *saved_vp = carousel_text_begin();
    lcd_set_foreground(pf_fg_color);
    lcd_setfont(pf_bold_font);
    if (center_index != prev_index)
    {
        set_scroll_line(name, PF_SCROLL_ALBUM);
        prev_index = center_index;
    }

    /* This caption is one line where the album carousel's is two, so it is
     * placed where the middle of that pair would be rather than on the first
     * line -- otherwise it sits high in a strip sized for two, which reads as
     * misaligned against the same screen showing albums. The strip itself is
     * unchanged: the engine reserves the same space either way. */
    char_height = screens[SCREEN_MAIN].getcharheight();
    switch (global_settings.album_covers_show_album_name)
    {
        case ALBUM_AND_ARTIST_TOP:
            txt_y = PF_CAPTION_ONE_LINE_Y(char_height);
            break;
        case ALBUM_NAME_BOTTOM:
        case ALBUM_AND_ARTIST_BOTTOM:
            txt_y = pf_height - PF_CAPTION_STRIP(char_height)
                              - PF_CAPTION_LIFT
                              + PF_CAPTION_ONE_LINE_Y(char_height);
            break;
        case ALBUM_NAME_TOP:
        default:
            txt_y = char_height / 2;
            break;
    }

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

    if (carousel_settings_menu() == MENU_ATTACHED_USB)
        return GO_TO_ROOT;

    /* The caption layout decides the text margin, which is computed during
     * init() -- so a change to it needs a full rebuild, not just a redraw. */
    if (global_settings.album_covers_show_album_name != old_show_name)
    {
        if (!carousel_reinit())
            return GO_TO_PREVIOUS;
    }

    /* Re-apply the live geometry settings (zoom / margins / tilt). */
    carousel_refresh();
    return CAROUSEL_MENU_RELOADED;
}

static void artist_set_initial(const char *selected_file)
{
    (void)selected_file;
    if (pf_resume_last_album)
    {
        pf_resume_last_album = false;
        set_current_slide(pf_resume_album_index);
    }
    else
        set_current_slide(0);
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
