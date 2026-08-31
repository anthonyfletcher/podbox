/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Text search across the names of the saved playlists: type, and the .m3u
 * files in the playlist folder whose names contain it appear. Reached from the
 * row at the top of the playlist catalogue.
 *
 * Names only. A playlist file holds paths rather than titles, so searching
 * inside one finds filenames wearing a song's label -- it would miss every
 * track whose file is named for its track number, which is most of a tidy
 * library. Tracks are searched by title from the database instead
 * (system/db_search.c).
 *
 * Not the same thing as viewer.c's search_playlist(), which looks inside the
 * playlist already open for a track. This one is about which playlist to open.
 *
 * The box itself is widgets/search_dialog.c. This is the provider behind it.
 * The scan walks the playlist folder rather than reading the directory cache,
 * because one folder of a few dozen files is small enough that a walk costs
 * nothing -- and it leaves the row working when the cache is off.
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
#include "file.h"
#include "dir.h"
#include "string-extra.h"
#include "pathfuncs.h"                /* path_basename */
#include "lang.h"
#include "files/filetypes.h"          /* FILE_ATTR_M3U */
#include "widgets/search_dialog.h"
#include "system/activity.h"
#include "playlist/catalog.h"
#include "playlist/viewer.h"
#include "root_menu.h"                /* the GO_TO_* codes returned */
#include "playlist_search.h"

/* Far more than a catalogue is likely to hold, let alone show at once. */
#define MAX_MATCHES  100
#define PATH_ARENA   (8 * 1024)

/* How far below the playlist folder the walk will go. Playlists are filed a
 * folder deep at most; anything deeper is not a catalogue someone browses. */
#define MAX_DEPTH    3

/* One letter is worth acting on here. A catalogue is tens of files, not the
 * thousands the other searches sweep, so a single letter narrows rather than
 * matching nearly everything. */
#define MIN_LETTERS  1

/* ---- the matches -------------------------------------------------------- */

/* The full path, which is both what the viewer is opened with and what the row
 * text is derived from. */
static uint16_t match_off[MAX_MATCHES];
static int      match_count;
static char     path_arena[PATH_ARENA];
static int      arena_used;

/* The query the screen last closed on, so reopening resumes rather than
 * starting blank. */
static char last_query[SEARCH_MAX_QUERY + 1];

static bool add_match(const char *path)
{
    int len = strlen(path) + 1;

    if (match_count >= MAX_MATCHES || arena_used + len > PATH_ARENA)
        return false;

    match_off[match_count] = arena_used;
    match_count++;

    memcpy(&path_arena[arena_used], path, len);
    arena_used += len;
    return true;
}

/* ---- the scan ----------------------------------------------------------- */

/* Walk 'dir' and everything under it, matching playlist names against 'query'.
 * Returns false once there is no more room, which unwinds the recursion.
 *
 * 'dir' is both the directory to read and the buffer the path is rebuilt in,
 * so it must hold MAX_PATH. */
static bool walk(char *dir, int depth, const char *query)
{
    DIR *d;
    struct dirent *entry;
    size_t len = strlen(dir);
    bool go_on = true;

    if (depth > MAX_DEPTH)
        return true;

    d = opendir(dir);
    if (!d)
        return true;

    while (go_on && (entry = readdir(d)))
    {
        struct dirinfo info;
        const char *leaf;

        if (entry->d_name[0] == '.')
            continue;

        if (len + 1 + strlen((char *)entry->d_name) >= MAX_PATH)
            continue;
        snprintf(dir + len, MAX_PATH - len, "%s%s",
                 len > 1 ? "/" : "", (char *)entry->d_name);

        info = dir_get_info(d, entry);

        if (info.attribute & ATTR_DIRECTORY)
            go_on = walk(dir, depth + 1, query);
        else if ((filetype_get_attr(dir) & FILE_ATTR_MASK) == FILE_ATTR_M3U)
        {
            path_basename(dir, &leaf);
            if (strcasestr(leaf, query))
                go_on = add_match(dir);
        }

        dir[len] = '\0';        /* back to the directory this level is walking */
    }

    closedir(d);
    return go_on;
}

static int run_search(const char *query, void *ctx)
{
    char dir[MAX_PATH];

    (void)ctx;

    match_count = 0;
    arena_used = 0;

    /* Also the empty-query case. Everything above is already cleared, so a
     * query backspaced below the threshold empties the results rather than
     * leaving the last scan's on screen. */
    if ((int)strlen(query) < MIN_LETTERS)
        return 0;

    catalog_get_directory(dir, sizeof(dir));
    walk(dir, 0, query);

    return match_count;
}

/* ---- the provider ------------------------------------------------------- */

/* The bare name, without the folder it sits in or the extension it is stored
 * under: that is what the catalogue's own list shows, and what a playlist is
 * called. */
static const char *match_text(int i, void *ctx)
{
    static char buf[MAX_PATH];
    const char *leaf;
    char *dot;

    (void)ctx;

    path_basename(&path_arena[match_off[i]], &leaf);
    strmemccpy(buf, leaf, sizeof(buf));

    dot = strrchr(buf, '.');
    if (dot)
        *dot = '\0';

    return buf;
}

/* ---- acting on a result ------------------------------------------------- */

int playlist_search_run(void)
{
    static const struct search_provider provider = {
        .activity = ACTIVITY_PLAYLIST_SEARCH,
        .scan     = run_search,
        .row_text = match_text,
        .row_icon = NULL,
    };
    struct search_provider p = provider;

    /* str() is not a constant expression, so the title is filled in here
     * rather than in the initialiser above. */
    p.title = str(LANG_DB_SEARCH);

    match_count = 0;
    arena_used = 0;

    /* Back to the results after a playlist is closed rather than out to the
     * catalogue: searching is how it was found, so it is also where the next
     * one is most likely to be found. */
    for (;;)
    {
        char path[MAX_PATH];
        int chosen = search_dialog_run(&p, NULL, last_query,
                                       sizeof(last_query));

        if (chosen == SEARCH_USB)
            return GO_TO_ROOT;    /* USB: root_menu handles the screen */
        if (chosen < 0)
            return GO_TO_PREVIOUS;

        /* Copied out: opening the viewer runs a screen that may rescan, and
         * the arena is rebuilt when it does. */
        strmemccpy(path, &path_arena[match_off[chosen]], sizeof(path));

        switch (playlist_viewer_ex(path, NULL))
        {
            case PLAYLIST_VIEWER_OK:       return GO_TO_WPS;
            case PLAYLIST_VIEWER_USB:      return GO_TO_ROOT;
            case PLAYLIST_VIEWER_MAINMENU: return GO_TO_ROOT;
            case PLAYLIST_VIEWER_CANCEL:   break;   /* back to the results */
        }
    }
}
