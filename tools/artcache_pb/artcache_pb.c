/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Brings the player's art cache up to date from a desktop, against a mounted
 * player.
 *
 * The player makes a thumbnail of each album's and artist's art in the
 * background, which on a large library takes hours. This compiles apps-ipod's
 * art_cache.c and does one pass of the same work on the computer, decoding
 * and scaling there and reading the images over USB. The thumbnails, stamps
 * and no-art lists are the player's own files, and the pass records the
 * database marks the player would, so the player finds the cache covered and
 * starts no pass of its own.
 *
 * It walks the database as it stands, so a library with new music wants
 * database_pb run first.
 *
 * Parts, in order:
 *   - finding the player
 *   - the player's settings
 *   - the run
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "config.h"
#include "file.h"
#include "dir.h"
#include "pathfuncs.h"
#include "system/library_files.h"
#include "database/libfile.h"
#include "database/tagcache.h"
#include "metadata/art_cache.h"
#include "settings/settings.h"
#include "artcache_pb.h"

/* The simulator's filesystem layer maps every path under this. */
extern const char *sim_root_dir;
const char *sim_root_dir = ".";

int artcache_pb_verbose;
bool artcache_pb_rebuild;

/* ------------------------------------------------------------------ *
 * finding the player                                                 *
 * ------------------------------------------------------------------ */

static void usage(void)
{
    printf(
"artcache_pb -- bring a mounted player's art cache up to date\n"
"\n"
"  artcache_pb [--rebuild] [-v] [player root]\n"
"\n"
"  [player root]  where the player is mounted: the folder holding .rockbox.\n"
"                 Left out, the first drive with a .rockbox folder is used.\n"
"  --rebuild      throw every thumbnail away and make them all again, as\n"
"                 Rebuild Art Cache does on the player.\n"
"  -v             list each folder whose thumbnails are written or removed\n");
}

#ifdef _WIN32
/* A drive with a .rockbox directory at its root is a player */
static const char *find_player(void)
{
    static char found[4];

    for (char c = 'C'; c <= 'Z'; c++)
    {
        char probe[32];
        DWORD attr;

        snprintf(probe, sizeof(probe), "%c:\\.rockbox", c);
        attr = GetFileAttributesA(probe);
        if (attr != INVALID_FILE_ATTRIBUTES
            && (attr & FILE_ATTRIBUTE_DIRECTORY))
        {
            snprintf(found, sizeof(found), "%c:\\", c);
            printf("Found a player on %s\n", found);
            return found;
        }
    }
    printf("No player found. Name where it is mounted, for example:\n"
           "  artcache_pb E:\\\n");
    return NULL;
}
#else
static const char *find_player(void)
{
    printf("Name where the player is mounted, for example:\n"
           "  artcache_pb /media/ipod\n");
    return NULL;
}
#endif

/* The layout format.dat records, or 0 */
static int layout(void)
{
    struct libfile_header h;
    int fd = open(LIB_FORMAT_FILE, O_RDONLY);
    int n;

    if (fd < 0)
        return 0;
    n = read(fd, &h, sizeof(h));
    close(fd);
    return n == (int)sizeof(h) && h.magic == LIB_FORMAT_MAGIC ? h.version : 0;
}

/* ------------------------------------------------------------------ *
 * the player's settings                                              *
 * ------------------------------------------------------------------ */

/* The two that change what a pass makes: where an album's art comes from,
 * and whether small sizes are made from the large one. The defaults are the
 * player's. */
static void read_settings(void)
{
    static const char *const sources[] =
        { "files", "files then embedded", "embedded then files", "embedded" };
    static char text[64 * 1024];
    int fd = open("/.rockbox/config.cfg", O_RDONLY);
    char *line, *next;
    int n;

    global_settings.art_cache_album_source = ART_SOURCE_FILES_FIRST;
    global_settings.art_cache_fast_build = false;
    if (fd < 0)
        return;
    /* open() is the simulator's, whose descriptors are its own: a stdio
     * stream cannot be made from one */
    n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0)
        return;
    text[n] = '\0';
    for (line = text; line != NULL; line = next)
    {
        char *v, *end;

        next = strchr(line, '\n');
        if (next != NULL)
            *next++ = '\0';
        line[strcspn(line, "\r")] = '\0';
        v = strchr(line, ':');
        if (v == NULL)
            continue;
        *v++ = '\0';
        while (*v == ' ')
            v++;
        end = v + strlen(v);
        while (end > v && end[-1] == ' ')
            *--end = '\0';

        if (!strcmp(line, "art cache album source"))
        {
            for (int i = 0; i < 4; i++)
                if (!strcmp(v, sources[i]))
                    global_settings.art_cache_album_source = i;
        }
        else if (!strcmp(line, "art cache fast build"))
            global_settings.art_cache_fast_build = !strcmp(v, "on");
    }
}

/* ------------------------------------------------------------------ *
 * the run                                                            *
 * ------------------------------------------------------------------ */

static void report(long secs, unsigned int written)
{
    struct art_cache_counts c;

    art_cache_get_counts(&c);
    if (secs < 60)
        printf("Done in %ld s. ", secs);
    else
        printf("Done in %ld min %ld s. ", secs / 60, secs % 60);
    printf("%u thumbnails written or removed.\n", written);
    printf("%d album folders, %d with art; %d artist folders, %d with art.\n",
           c.albums, c.album_art, c.artists, c.artist_art);
}

int main(int argc, char **argv)
{
    const char *target = NULL;
    enum bg_result result;
    unsigned int gen0;
    time_t t0;

    setvbuf(stdout, NULL, _IONBF, 0);
    /* As the player does at boot: database paths carry "/<HDD0>", and the
     * thumbnails are keyed on those paths */
    init_volume_names();

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--rebuild"))
            artcache_pb_rebuild = true;
        else if (!strcmp(argv[i], "-v"))
            artcache_pb_verbose = 1;
        else if (argv[i][0] == '-')
        {
            usage();
            return 1;
        }
        else
            target = argv[i];
    }

    if (target == NULL && (target = find_player()) == NULL)
        return 1;
    sim_root_dir = target;

    if (!dir_exists("/.rockbox"))
    {
        printf("No .rockbox in %s -- is that the player's root?\n", target);
        return 1;
    }

    if (layout() != LIBRARY_FORMAT)
    {
        if (layout() < LIBRARY_FORMAT)
            printf("This player has not started on this firmware yet.\n"
                   "Start it once, then run this again.\n");
        else
            printf("This player runs newer firmware than this tool.\n"
                   "Use the artcache_pb.exe from that firmware's zip.\n");
        return 1;
    }

    read_settings();

    if (!tagcache_tool_open())
    {
        printf("The player has no database to walk yet. Let it build one,\n"
               "or run database_pb first.\n");
        return 1;
    }

    printf("%s the art cache on %s\n",
           artcache_pb_rebuild ? "Rebuilding" : "Updating", target);
    t0 = time(NULL);
    gen0 = art_cache_generation();
    result = art_cache_tool_run(artcache_pb_rebuild);
    artcache_pb_progress_end();

    if (result != BG_DONE)
    {
        printf(result == BG_FAILED
               ? "The database could not be read to the end.\n"
               : "The pass stopped before the end.\n");
        return 1;
    }
    report((long)(time(NULL) - t0), art_cache_generation() - gen0);
    printf("Eject the player.\n");
    return 0;
}
