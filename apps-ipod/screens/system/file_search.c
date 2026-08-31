/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Text search across every filename on the player: type, and the files and
 * folders whose names contain it appear, each saying which folder it sits in.
 * Reached from the row at the top of the file browser's root.
 *
 * The box itself is widgets/search_dialog.c. This is the provider behind it:
 * what a match is, where matches are kept, and what selecting one does.
 *
 * The names come from the directory cache, which already holds every one of
 * them in RAM -- so a scan is a linear sweep of memory rather than a walk of
 * the disk, and there is no index of our own to build, ship or keep current.
 * That is also why the row is absent when the cache is off or still building:
 * there is no second source to fall back to.
 *
 * The sweep matches on the name alone and resolves a path only for the
 * handful of rows it keeps. A path costs a walk to the root of the tree, so
 * building one per entry rather than per hit is the difference between a
 * sweep and a crawl.
 *
 * Parts, in order:
 *   - the matches
 *   - the scan
 *   - the provider
 *   - acting on a result
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"                   /* HZ */
#include "file.h"
#include "dir.h"                      /* ATTR_DIRECTORY */
#include "dircache.h"
#include "string-extra.h"
#include "pathfuncs.h"                /* path_dirname */
#include "settings/settings.h"        /* ID2P */
#include "lang.h"
#include "widgets/search_dialog.h"
#include "widgets/splash.h"
#include "system/activity.h"
#include "root_menu.h"                /* browser_reveal_on_next_load, GO_TO_* */
#include "file_search.h"

/* Far more than fits on screen. A longer list is not more useful: the answer
 * to too many results is another letter. */
#define MAX_MATCHES  100
#define NAME_ARENA   (8 * 1024)

/* Two letters before anything runs. One letter over every name on the player
 * matches most of them, and a screenful of near-everything is worse than
 * nothing. */
#define MIN_LETTERS  2

/* ---- the matches -------------------------------------------------------- */

static struct match {
    int      idx;               /* cache index: what the path is resolved from */
    uint16_t text_off;          /* into name_arena, filled by the second pass */
} matches[MAX_MATCHES];

static int  match_count;
static char name_arena[NAME_ARENA];
static int  arena_used;

/* The query the screen last closed on, so reopening resumes rather than
 * starting blank: a search is usually one of a run, and retyping the stem on a
 * click wheel is most of the work. */
static char last_query[SEARCH_MAX_QUERY + 1];

/* ---- the scan ----------------------------------------------------------- */

/* First pass, called for every entry in the cache. Nothing is resolved here
 * and nothing yields: this runs under the cache's read lock, once per name on
 * the player. */
static bool collect(const char *name, int idx, unsigned int attr, void *ctx)
{
    const char *query = ctx;

    (void)attr;

    /* An index of 0 is an entry the cache could not number for us. Nothing can
     * be resolved from it, so it cannot become a row. */
    if (idx != 0 && strcasestr(name, query))
    {
        matches[match_count].idx = idx;
        match_count++;
    }

    return match_count < MAX_MATCHES;
}

/* Second pass: turn the indexes the sweep kept into the text their rows show.
 *
 * Separate from collect() rather than folded into it because resolving a path
 * takes the cache's read lock, which the sweep is already holding -- and
 * because it is the pass whose cost scales with hits rather than with the size
 * of the library, which is the whole reason the two are split.
 *
 * "song.mp3  (/Music/Album)" -- the name first, since that is what was typed
 * and what is being looked for, and the folder after it, since on a player
 * with a tidy library the name alone is often ambiguous. */
static void resolve_paths(void)
{
    char path[MAX_PATH];
    int kept = 0;

    for (int i = 0; i < match_count; i++)
    {
        const char *leaf, *dir;
        size_t dirlen, leaflen;
        int len;

        if (dircache_get_index_path(matches[i].idx, path, sizeof(path)) < 0)
            continue;           /* gone between the sweep and here */

        dirlen = path_dirname(path, &dir);
        leaflen = path_basename(path, &leaf);

        len = snprintf(&name_arena[arena_used], NAME_ARENA - arena_used,
                       "%.*s  (%.*s)", (int)leaflen, leaf, (int)dirlen, dir);

        if (len < 0 || arena_used + len + 1 > NAME_ARENA)
            break;              /* out of arena: keep what is already built */

        matches[kept].idx = matches[i].idx;
        matches[kept].text_off = arena_used;
        kept++;

        arena_used += len + 1;
    }

    match_count = kept;
}

static int run_search(const char *query, void *ctx)
{
    (void)ctx;

    match_count = 0;
    arena_used = 0;

    /* Too short to bother with, and also the empty-query case. Everything
     * above is already cleared, so a query backspaced below the threshold
     * empties the results rather than leaving the last scan's on screen. */
    if ((int)strlen(query) < MIN_LETTERS)
        return 0;

    if (dircache_foreach_name(collect, (void *)query) < 0)
        return 0;               /* the cache went away under us */

    resolve_paths();

    return match_count;
}

/* ---- the provider ------------------------------------------------------- */

static const char *match_text(int i, void *ctx)
{
    (void)ctx;
    return &name_arena[matches[i].text_off];
}

/* ---- acting on a result ------------------------------------------------- */

int file_search_run(void)
{
    static const struct search_provider provider = {
        .activity = ACTIVITY_FILE_SEARCH,
        .scan     = run_search,
        .row_text = match_text,
        .row_icon = NULL,
    };
    struct search_provider p = provider;
    char path[MAX_PATH];
    int chosen;

    /* The row that opens this is hidden without a cache, so reaching here
     * means one went away between the list being built and the press. */
    if (!dircache_is_ready())
    {
        splash(HZ * 2, "Needs the directory cache");
        return GO_TO_PREVIOUS;
    }

    /* A cache that filled up still reports itself ready, so without this the
     * results would be short of the disk with nothing to say so. */
    {
        struct dircache_info info;
        dircache_get_info(&info);
        if (info.size >= info.size_limit)
            splash(HZ, "Cache full: some files not searched");
    }

    /* str() is not a constant expression, so the title is filled in here
     * rather than in the initialiser above. */
    p.title = str(LANG_DB_SEARCH);

    match_count = 0;
    arena_used = 0;

    chosen = search_dialog_run(&p, NULL, last_query, sizeof(last_query));

    if (chosen == SEARCH_USB)
        return GO_TO_ROOT;        /* USB: root_menu handles the screen */

    if (chosen < 0)
        return GO_TO_PREVIOUS;

    /* Resolved again rather than kept from the scan: the arena holds the text
     * a row shows, which has the folder in brackets and is not a path. */
    if (dircache_get_index_path(matches[chosen].idx, path, sizeof(path)) < 0)
    {
        splash(HZ * 2, ID2P(LANG_FILE_NOT_FOUND));
        return GO_TO_PREVIOUS;
    }

    /* Shown in the browser rather than opened, which is what the context
     * menu's "Show in Files" does with a path and what leaves every one of the
     * browser's own actions a press away -- play it, queue it, look at it. */
    browser_reveal_on_next_load(path);
    return GO_TO_FILEBROWSER;
}
