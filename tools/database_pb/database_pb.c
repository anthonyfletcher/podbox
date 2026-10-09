/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Brings the player's database up to date from a desktop, against a mounted
 * player.
 *
 * The player does this itself -- Library > Maintenance > Update Now -- but a
 * scan reads every new file's tags over its own disk, which on a large
 * library takes a long while. This is the same code doing the same work: it
 * compiles apps-ipod's tagcache.c, so the files it writes are the files the
 * player would have written, play counts and all, and the player loads them
 * as its own.
 *
 * File paths go through the simulator's filesystem layer, rooted at the
 * player, so "/Music/x.flac" here is the string the player stores. The
 * player's own settings decide what is scanned and how, so they are read from
 * its config.cfg rather than taken from the command line.
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
#include "system/library_files.h"
#include "database/libfile.h"
#include "database/tagcache.h"
#include "settings/settings.h"
#include "database_pb.h"

/* The simulator's filesystem layer maps every path under this. */
extern const char *sim_root_dir;
const char *sim_root_dir = ".";

int database_pb_verbose;

/* ------------------------------------------------------------------ *
 * finding the player                                                 *
 * ------------------------------------------------------------------ */

static void usage(void)
{
    printf(
"database_pb -- bring a mounted player's database up to date\n"
"\n"
"  database_pb [--rebuild] [-v] [player root]\n"
"\n"
"  [player root]  where the player is mounted: the folder holding .rockbox.\n"
"                 Left out, the first drive with a .rockbox folder is used.\n"
"  --rebuild      build the database again from nothing, as Rebuild does on\n"
"                 the player. Play counts, ratings and positions are kept.\n"
"  -v             say what is being done\n");
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
           "  database_pb E:\\\n");
    return NULL;
}
#else
static const char *find_player(void)
{
    printf("Name where the player is mounted, for example:\n"
           "  database_pb /media/ipod\n");
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

/* Only the two that change what a scan stores: where it looks, and whether a
 * folder's year overrides the tag. Anything else in the file is the player's
 * business. */
static void read_settings(void)
{
    static char text[64 * 1024];
    int fd = open("/.rockbox/config.cfg", O_RDONLY);
    char *line, *next;
    int n;

    strcpy((char *)global_settings.tagcache_scan_paths, "/");
    global_settings.year_from_folder = false;
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

        if (!strcmp(line, "database scan paths") && *v)
            snprintf((char *)global_settings.tagcache_scan_paths,
                     sizeof(global_settings.tagcache_scan_paths), "%s", v);
        else if (!strcmp(line, "year from folder"))
            global_settings.year_from_folder = !strcmp(v, "on");
    }
}

/* The tracks the database now holds. The master's own count, and a search of
 * the filenames, both include the entries an update marked deleted; a numeric
 * search walks the master and skips them. */
static int count_tracks(void)
{
    static char buf[TAGCACHE_BUFSZ];
    struct tagcache_search tcs;
    int n = 0;

    if (!tagcache_search(&tcs, tag_length))
        return 0;
    while (tagcache_get_next(&tcs, buf, sizeof(buf)))
        n++;
    tagcache_search_finish(&tcs);
    return n;
}

/* ------------------------------------------------------------------ *
 * the run                                                            *
 * ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *target = NULL;
    bool rebuild = false;
    time_t t0;
    int tracks;
    bool ok;

    setvbuf(stdout, NULL, _IONBF, 0);

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--rebuild"))
            rebuild = true;
        else if (!strcmp(argv[i], "-v"))
            database_pb_verbose = 1;
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

    /* A player still on an older layout moves its database at its next
     * boot, over whatever this would write; one on a newer layout keeps its
     * files in a way this build does not know. */
    if (layout() != LIBRARY_FORMAT)
    {
        if (layout() < LIBRARY_FORMAT)
            printf("This player has not started on this firmware yet.\n"
                   "Start it once, then run this again.\n");
        else
            printf("This player runs newer firmware than this tool.\n"
                   "Use the database_pb.exe from that firmware's zip.\n");
        return 1;
    }

    read_settings();
    if (database_pb_verbose)
        printf("Scanning %s%s\n", global_settings.tagcache_scan_paths,
               global_settings.year_from_folder ? ", year from folder" : "");

    mkdir(LIB_DIR);
    mkdir(LIB_DB_DIR);

    printf("%s the database on %s\n", rebuild ? "Rebuilding" : "Updating",
           target);
    t0 = time(NULL);
    ok = tagcache_tool_run(rebuild);
    tracks = ok ? count_tracks() : 0;
    database_pb_progress_end();

    if (!ok)
    {
        printf("The database could not be brought up to date.\n");
        return 1;
    }
    printf("Done in %ld s: %d tracks.\n", (long)(time(NULL) - t0), tracks);
    /* The player holds the old database's header and track numbers until it
     * starts again, and would write them back over these files */
    printf("Eject the player, then restart it (hold MENU and SELECT)\n"
           "before playing anything.\n");
    return 0;
}
