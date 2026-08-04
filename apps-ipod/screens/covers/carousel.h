/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The carousel model vtable and shared render state -- the interface
 * between the engine and its two screens.
 ****************************************************************************/

/* Coverflow "carousel" engine shared by Album Covers (apps/gui/album_covers.c)
 * and Artist Portraits (apps/gui/artist_portraits.c). The engine (rendering,
 * slide cache, scroll, input loop, thread) is generic; each screen supplies a
 * carousel_model describing what the slides are. This header is the interface
 * between the two: the model vtable, the shared index/rendering state the
 * models read, and the engine helpers they call. */

#ifndef _CAROUSEL_H_
#define _CAROUSEL_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"
#include "lcd.h"        /* fb_data */
#include "rbpaths.h"    /* ROCKBOX_DIR, for CACHE_PREFIX */
#include "database/tagcache.h"   /* struct tagcache_search */

typedef fb_data pix_t;

/* Fixed-point type used for slide positions and angles throughout the engine.
 * See carousel.c for the shift and the arithmetic helpers. */
#define PFreal long

/* The on-disk slide cache, shared because a model writes into it during
 * build_index() and the engine reads it back while rendering. Bumping
 * CACHE_VERSION invalidates every cached slide; CACHE_REBUILD is the stored
 * value meaning "no valid cache". */
#define CACHE_PREFIX     ROCKBOX_DIR "/carousel"
#define EMPTY_SLIDE      CACHE_PREFIX "/emptyslide.pfraw"
#define CACHE_VERSION    6
#define CACHE_REBUILD    0

/* Number of slide slots the engine keeps decoded at once. */
#define SLIDE_CACHE_SIZE 100

/* Caption geometry, in one place because three pieces of code have to agree on
 * it: each model's draw_text() places the text, and the engine reserves a strip
 * of the same size for it (see the text_margin switch in carousel.c). They were
 * three copies of the same expression, and moving the text without moving the
 * strip lets the slides run underneath it.
 *
 * PF_CAPTION_STRIP is the room a bottom caption needs -- two lines at 0.75
 * spacing plus descenders. PF_CAPTION_LIFT holds it clear of the very bottom
 * edge, which read as sitting too low.
 *
 * PF_CAPTION_ONE_LINE_Y offsets a caption that has only one line to write, so
 * it sits where the middle of a two-line caption would rather than on the
 * first line: half the 0.75 line spacing. */
#define PF_CAPTION_STRIP(char_height)  ((char_height) * 9 / 4)
#define PF_CAPTION_LIFT                10
#define PF_CAPTION_ONE_LINE_Y(char_height) ((char_height) * 3 / 8)

/* build_index / count return codes */
#define SUCCESS              0
#define ERROR_NO_ALBUMS     -1
#define ERROR_BUFFER_FULL   -2
#define ERROR_NO_ARTISTS    -3
#define ERROR_USER_ABORT    -4

/* Index data built into the shared buffer by a model's build_index(). Album
 * covers fills album_index[]/album_names; artist portraits fills
 * artist_index[]/artist_names via build_artist_index(). */
struct album_data {
    int name_idx;     /* offset to the album name */
    int artist_idx;   /* offset to the artist name */
    /* Identity that survives a database commit, hashed from the two names
     * above. The seeks below do not survive one: any commit that adds a track
     * re-sorts the whole album tagfile and moves every seek in it. So this is
     * what an entry is matched by when figures are carried from one build of
     * the index to the next. */
    uint32_t key;
    int year;         /* album year */
    /* Playback history, summarised over the album's tracks by
     * assign_album_stats(). The album charts sort on these; nothing else
     * reads them. Both are 0 for an album that has never been played, which
     * is why the charts exclude 0 rather than showing it at one end. */
    int playcount;    /* total plays across the album */
    long lastplayed;  /* the most recent of its tracks */
    long artist_seek; /* artist taglist position */
    long seek;        /* album taglist position */
    /* art_cache key for the folder holding this album's tracks, resolved once
     * during the build. A slide load is then a file open, where finding the
     * folder from the seeks above would be a database search per slide. 0 if
     * the build could not resolve it. */
    unsigned int art_hash;
    /* That folder's parent, which is the artist's own. Carried here because it
     * comes from the same path for free, and the artist list is filled by
     * rolling the album list up rather than by walking the database again. */
    unsigned int artist_art_hash;
};

struct artist_data {
    int name_idx; /* offset to the artist name */
    uint32_t key; /* as struct album_data's, from the name alone */
    /* As struct album_data's pair, summarised over everything by this artist.
     * "Artist" here is the album artist -- the tag this list is built from
     * (see build_artist_index()) -- so a guest appearance counts towards the
     * record's artist, not the guest. */
    int playcount;
    long lastplayed;
    long seek;    /* artist taglist position */
    /* As struct album_data's field, for the artist's own folder -- the parent
     * of its albums', under the <artist>/<album>/<track> layout this list
     * assumes. */
    unsigned int art_hash;
};

struct pf_index_t {
    uint32_t            header; /*INDEX_HDR*/
    uint16_t            artist_ct;
    uint16_t            album_ct;
    /* What the database looked like when this index was built. Both come from
     * tagcache_get_marks(): commitid moves when tracks are added, serial once
     * per logged play. They say what has happened since, so a later build can
     * tell which entries still need their figures recomputed. */
    int32_t             commitid;
    int32_t             serial;
    /* Entries tagcache had flagged deleted. A deletion moves figures that
     * cannot be matched back to an album, so a build that finds this changed
     * summarises everything rather than carrying anything across. */
    int32_t             deleted;

    char               *artist_names;
    struct artist_data *artist_index;
    size_t              artist_len;

    unsigned int        album_untagged_idx;
    char               *album_names;
    struct album_data  *album_index;
    size_t              album_len;
    long                album_untagged_seek;

    void * buf;
    size_t buf_sz;
};

enum pf_scroll_line_type {
    PF_SCROLL_TRACK = 0,
    PF_SCROLL_ALBUM,
    PF_SCROLL_ARTIST,
    PF_MAX_SCROLL_LINES
};

/* on_menu() return values. Non-negative values are GO_TO_* screen codes the
 * engine loop returns (exits the carousel); these two sentinels mean "handled,
 * stay in the carousel". They are negative so they never collide with a
 * (non-negative) GO_TO_* enum value. */
#define CAROUSEL_MENU_STAY     (-1)  /* menu closed; just resume drawing */
#define CAROUSEL_MENU_RELOADED (-2)  /* index was rebuilt; engine resets its display */

/* The seam between the generic coverflow engine and the data it shows. */
struct carousel_model {
    int  (*build_index)(void);                         /* build the slide data; SUCCESS/ERROR_* */
    int  (*count)(void);                               /* number of slides */
    unsigned int (*art_key)(int index);                /* art_cache key, or 0 */
    bool (*legacy_art)(int index, char *path, int len);/* pre-cache fallback art, or false */
    int  (*enter)(int index);                          /* select: drill in; returns GO_TO_* */
    int  (*jump_prev)(void);                           /* jump to prev section (letter/year) */
    int  (*jump_next)(void);                           /* jump to next section */
    void (*draw_text)(void);                           /* center-slide caption */
    void (*sort_next)(void);                           /* cycle sort order forward + resort */
    void (*sort_prev)(void);                           /* cycle sort order backward + resort */
    void (*set_initial)(const char *selected_file);    /* position on entry (resume/now-playing) */
    /* Optional (NULL = not provided). Keep the engine loop/init free of any
     * direct calls into a specific model's code. */
    int  (*on_menu)(void);   /* MENU-hold: run the in-screen menu; GO_TO_* or CAROUSEL_MENU_* */
    void (*prepare)(void);   /* one-off post-init setup (e.g. cache-version bookkeeping) */
    bool has_pfraw_cache;                              /* this screen's own pfraw thumbnail cache */
    const char *title;                                 /* status-bar title */
};

/* Engine-owned persistent config for the album carousel (cache version + resume
 * slide). Not user settings -- those live in global_settings; this is internal
 * cache/resume state the engine persists and the album model reads/writes. */
struct pf_config_t
{
     int cache_version;
     bool update_albumart;
     int last_album;
};

/* --- Shared engine state the models read/write (owned by the engine) -------- */
extern struct pf_config_t pf_cfg;  /* engine's persistent cache/resume config */
extern struct pf_index_t pf_idx;   /* the current carousel's index buffer */
extern int   center_index;         /* engine's current slide */
extern int   pf_bold_font;         /* caption bold font (draw_text) */
extern int   pf_height;            /* drawable viewport height (draw_text) */
extern pix_t pf_fg_color;          /* caption colour (draw_text) */
/* Transient "resume to this slide on next open" signal, set by a model's
 * enter() and consumed by its set_initial(). */
extern int   pf_resume_album_index;
extern bool  pf_resume_last_album;

/* --- Engine entry + helpers the models call -------------------------------- */
/* Run the coverflow carousel over `m`. The thin public entry points
 * album_covers()/artist_portraits() wrap this, selecting the model. */
int  carousel_run(const struct carousel_model *m, const char *selected_file);
void set_current_slide(int index);
void set_scroll_line(const char *str, enum pf_scroll_line_type type);
int  get_scroll_line_offset(enum pf_scroll_line_type type);
/* Clip drawing to the inset caption box; offsets from get_scroll_line_offset()
 * are relative to it. Enter before setting the font or colour, and pass the
 * returned viewport back to carousel_text_end() when the caption is drawn. */
struct viewport *carousel_text_begin(void);
void carousel_text_end(struct viewport *saved);
/* build_artist_index() belongs to the index builder -- see
 * database/db_index.h. */
/* Persist the engine's pf_cfg to its config file (album model calls this after
 * changing last_album / triggering a cache rebuild). */
void pf_config_save(void);
/* True once the current index's art has all been inspected (background art
 * cache fully populated); the album model gates its "please wait" splashes on
 * this instead of touching engine buffer state directly. */
bool carousel_cache_ready(void);

/* Engine restart operations the album model drives (re-sort, in-screen rebuild).
 * These wrap the render thread / slide cache / buffer lifecycle so model code
 * never touches those internals directly. */
/* Settle any in-progress scroll animation to the idle state. Call before
 * capturing the current slide, since a mid-scroll settle changes which slide is
 * current. */
void carousel_settle(void);
/* Restart the render pipeline over the current index buffer. If `compare` is
 * non-NULL, the album index is re-sorted with it in the safe window after the
 * loader thread is stopped and before the slide cache is rebuilt. */
void carousel_reload(int (*compare)(const void *, const void *));
/* Full teardown (cleanup) + rebuild (init) of the engine. Returns false if the
 * rebuild failed. */
bool carousel_reinit(void);
/* Re-apply the geometry settings (zoom / margins / tilt) to the slide layout
 * in place, keeping the current slide -- for a visual settings change that
 * doesn't need an index rebuild. */
void carousel_refresh(void);

#endif /* _CAROUSEL_H_ */
