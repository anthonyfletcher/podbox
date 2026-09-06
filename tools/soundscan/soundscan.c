/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Builds the player's sound index from a desktop, against a mounted player.
 *
 * The player can do this itself -- Library > Maintenance > Analyse Library --
 * but at about real time, because the analysis costs roughly what decoding
 * does and the CPU is what it is. A desktop does the same work about a
 * hundred times faster, which turns an overnight job into a coffee break.
 *
 * It is the same code doing it. This compiles the tree's own codec loader,
 * decoder harness, analysis and index, so the numbers it writes are the
 * numbers the player would have written -- and it loads the player's own
 * codecs, built for the host by the simulator, so every format the player can
 * play this can measure.
 *
 * Two things make that possible and are worth knowing before changing
 * anything here:
 *
 *   - SIMULATOR is defined. Not because there is a simulator, but because
 *     struct codec_api's layout depends on it and the codecs were built that
 *     way. See system-sdl.h for what that drags in and what is done about it.
 *
 *   - File paths go through the simulator's filesystem layer, rooted at the
 *     player. So "/Music/x.flac" here is the same string the player would
 *     use, which matters because the index is keyed by a hash of it. A tool
 *     keying by "E:\Music\x.flac" would write an index in which not one
 *     record is ever found again.
 *
 * Parts, in order:
 *   - where things are
 *   - measuring one track
 *   - walking the library
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <setjmp.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "config.h"
#include "codecs.h"
#include "dir.h"
#include "file.h"
#include "pathfuncs.h"
#include "metadata.h"
#include "audio/beat_probe.h"
#include "audio/track_decode.h"
#include "database/sound_index.h"
#include "soundscan.h"

/* The window, as apps-ipod/screens/system/sound_scan.c takes it. These must
 * match: a record measured over a different span is not comparable with one
 * measured over this one, and both front ends write the same file. */
#define START_PCT      15
#define START_MIN_MS   5000
#define START_MAX_MS   30000
#define WINDOW_MS      40000
#define MIN_LENGTH_MS  25000

#define FILE_WINDOW    (256 * 1024)

/* Deeper than any music library and shallow enough that a directory loop
 * ends rather than exhausting the stack. */
#define WALK_MAX_DEPTH 24

/* The simulator's filesystem layer maps every path under this. */
extern const char *sim_root_dir;
const char *sim_root_dir = ".";

static char  codec_dir[1024];
static char *file_window;

static int   opt_verbose;
static int   opt_dry;
static int   opt_codectest;

static int   n_seen, n_done, n_current, n_short, n_failed;

const char *soundscan_codec_dir(void)
{
    return codec_dir;
}


/** Surviving a file that kills a codec **/

/* A malformed file can take a decoder down with it, and a decoder dying takes
 * the tool with it. The records already written survive -- the index is
 * appended to and its header rewritten per record -- so a second run resumes.
 * It then reaches the same file and dies again, and a library with one bad
 * track in it can never be finished.
 *
 * So the path about to be attempted is written down first. A run that starts
 * and finds one already there knows the last attempt did not come back, and
 * files that track as unreadable rather than trying it a second time.
 *
 * Three lines of file handling to make the difference between a tool that
 * finishes and one that cannot. */
#define BUSY_FILE  ROCKBOX_DIR "/db_sound.busy"

static void busy_set(const char *path)
{
    int fd = open(BUSY_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (fd < 0)
        return;

    write(fd, path, strlen(path));
    fsync(fd);
    close(fd);
}

static void busy_clear(void)
{
    remove(BUSY_FILE);
}

/* Catching the fault, so one run gets through a library rather than one bad
 * file per run.
 *
 * A decoder that walks off the end of a malformed file raises SIGSEGV, and
 * the default action ends the tool. Jumping back out of the handler is not
 * something to do in a program that has state worth protecting -- but this
 * one's state is a file it appends to and closes, and the alternative is a
 * library that takes as many runs to finish as it has bad tracks in it.
 *
 * The marker above stays as the backstop, for the faults this cannot catch. */
static jmp_buf crash_jmp;
static volatile sig_atomic_t crash_armed;

static void on_crash(int sig)
{
    /* Re-arm first. signal() here has SysV semantics: delivering the signal
     * puts the default action back, so without this the first bad file is
     * survived and the second ends the run -- which reads as the recovery
     * not working at all, rather than working exactly once. */
    signal(sig, on_crash);

    if (!crash_armed)
        _exit(3);           /* Not inside a decode: nothing to recover to */

    crash_armed = 0;
    longjmp(crash_jmp, 1);
}

/* Record a track as unreadable, so no run tries it a second time. */
static void file_failed(const char *path)
{
    struct sound_record rec;
    struct track_sound none;
    uint32_t sz = 0;
    int fd = open(path, O_RDONLY);

    if (fd >= 0)
    {
        off_t n = filesize(fd);

        sz = n > 0 ? (uint32_t)n : 0;
        close(fd);
    }

    memset(&none, 0, sizeof (none));
    sound_index_fill(&rec, sound_index_key(path), 0, sz, 0, 0, &none,
                     TRACK_DECODE_FAILED);

    if (!opt_dry)
        sound_index_add(&rec);

    n_failed++;
}

/* The path the last run died on, or NULL. */
static const char *busy_get(void)
{
    static char path[MAX_PATH];
    int fd = open(BUSY_FILE, O_RDONLY);
    ssize_t n;

    if (fd < 0)
        return NULL;

    n = read(fd, path, sizeof (path) - 1);
    close(fd);

    if (n <= 0)
        return NULL;

    path[n] = '\0';

    /* Whatever wrote it may have left a line ending on the end. A path with
     * one hashes to a key that will never match anything. */
    while (n > 0 && (path[n - 1] == '\n' || path[n - 1] == '\r' ||
                     path[n - 1] == ' '))
    {
        path[--n] = '\0';
    }

    return n > 0 ? path : NULL;
}


/** One track **/

static bool measure(const char *path, uint32_t size)
{
    struct mp3entry id3;
    struct track_sound s;
    struct sound_record rec;
    unsigned long start_ms, analysed = 0;
    uint64_t key = sound_index_key(path);
    int fd, rc;

    /* mtime stays zero: it is the player's FAT directory time read as local
     * time, which cannot be reproduced from here across timezones. The size
     * is what stands in -- see sound_index.h. */
    if (sound_index_done(key, 0, size))
    {
        n_current++;
        return true;
    }

    /* The same metadata reader the player uses, so genre and year are the
     * values its own database would hold rather than a second opinion. */
    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        n_failed++;
        return true;
    }

    memset(&id3, 0, sizeof (id3));
    if (!get_metadata(&id3, fd, path))
    {
        close(fd);
        n_failed++;
        if (opt_verbose)
            printf("  %-50.50s  no metadata\n", path);
        return true;
    }
    close(fd);

    if (id3.length < MIN_LENGTH_MS)
    {
        n_short++;
        return true;
    }

    start_ms = id3.length / 100 * START_PCT;
    if (start_ms < START_MIN_MS)
        start_ms = START_MIN_MS;
    if (start_ms > START_MAX_MS)
        start_ms = START_MAX_MS;
    if (id3.length < start_ms + 5000)
        start_ms = 0;


    beat_probe_start();

    /* The whole window, never stopping early -- see the note on SS_WINDOW_MS
     * in sound_scan.c. The player and this must measure identically or their
     * records are not comparable. */
    rc = track_decode_run(path, start_ms, WINDOW_MS, file_window, FILE_WINDOW,
                          beat_probe_sink, NULL, NULL,
                          &analysed);


    beat_probe_result(&s);

    if (rc != TRACK_DECODE_OK && rc != TRACK_DECODE_ABORTED)
    {
        n_failed++;
        if (opt_verbose)
            printf("  %-50.50s  decode failed (%d)\n", path, rc);
    }

    sound_index_fill(&rec, key, 0, size,
                     sound_index_genre_key(id3.genre_string),
                     id3.year, &s, rc);

    if (analysed < MIN_LENGTH_MS / 2)
        rec.flags |= SOUND_F_SHORT;

    n_done++;

    if (opt_verbose)
    {
        printf("  %-50.50s %3u bpm %4d dB %s\n", path,
               s.period_ms ? 60000 / s.period_ms : 0,
               s.loudness_db10 / 10,
               s.chroma.margin >= CHROMA_MARGIN_MIN
               ? (s.chroma.minor ? "min" : "maj") : "-");
    }

    return opt_dry ? true : sound_index_add(&rec);
}


/** Walking **/

/* Anything the player has a codec for. get_metadata() is the real test --
 * this only avoids opening artwork and text files to ask. */
static bool looks_like_audio(const char *name)
{
    static const char *ext[] = {
        ".mp3", ".flac", ".m4a", ".m4b", ".mp4", ".aac", ".ogg", ".oga",
        ".opus", ".wma", ".wav", ".ape", ".mpc", ".wv", ".aiff", ".aif",
        ".spx", ".ac3", ".shn", ".adx", ".mod", ".sid", ".nsf", ".au",
        ".vox", ".w64", ".tta", ".mp2", ".mp1", ".asf", ".rm", ".ra", NULL
    };
    const char *dot = strrchr(name, '.');
    int i;

    if (dot == NULL)
        return false;

    for (i = 0; ext[i] != NULL; i++)
    {
        if (!strcasecmp(dot, ext[i]))
            return true;
    }

    return false;
}

static bool walk(const char *dir, bool counting, int *total, int depth)
{
    DIR *d;
    struct dirent *e;
    bool ok = true;


    d = opendir(dir);


    if (d == NULL)
        return true;

    while (ok && (e = readdir(d)) != NULL)
    {
        struct dirinfo info;
        char path[MAX_PATH];

        if (e->d_name[0] == '.')
            continue;

        info = dir_get_info(d, e);

        /* A path that will not fit is not walked. Truncating it would build a
         * name that opens something else or nothing, and the depth cap below
         * is the other half of the same guard: a directory that contains
         * itself -- which a damaged FAT can produce -- would otherwise
         * recurse until the stack ends. */
        if (snprintf(path, sizeof (path), "%s%s%s", dir,
                     strcmp(dir, "/") == 0 ? "" : "/", e->d_name)
            >= (int)sizeof (path))
        {
            continue;
        }

        if (info.attribute & ATTR_DIRECTORY)
        {
            if (depth < WALK_MAX_DEPTH)
                ok = walk(path, counting, total, depth + 1);
            continue;
        }

        if (!looks_like_audio(e->d_name))
            continue;

        if (counting)
        {
            (*total)++;
            continue;
        }

        n_seen++;

        if (!opt_verbose)
        {
            /* The name as well as the count, so a run that dies says what it
             * died on without having to be run again under -v. */
            printf("\r  %d/%d  %d done  %-40.40s",
                   n_seen, *total, n_done, e->d_name);
            fflush(stdout);
        }

        busy_set(path);

        if (setjmp(crash_jmp) == 0)
        {
            crash_armed = 1;
            ok = measure(path, (uint32_t)info.size);
            crash_armed = 0;
        }
        else
        {
            /* Back from a fault inside a decoder.
             *
             * Deliberately no codec_close(). Unloading a shared object that
             * was interrupted part way through its own code is what raises
             * "R6031 - attempt to initialize the CRT more than once", which
             * ends the run just as surely as the fault would have. The next
             * load overwrites the handle instead: that leaks one module per
             * bad file, which for a handful of files is a trade worth making
             * against not finishing at all. */
            printf("\n  %s -- decoder crashed, skipped\n", path);
            file_failed(path);
            ok = true;
        }

        busy_clear();
    }

    closedir(d);

    return ok;
}

/* Load every codec in turn and say which ones will not.
 *
 * A codec that fails to load takes the whole run with it and says only which
 * track it was on, which names a file rather than the thing at fault. This
 * asks the question directly. */
static int codec_check(void)
{
    int afmt, bad = 0;

    printf("Codecs in %s\n", codec_dir);

    for (afmt = 0; afmt < AFMT_NUM_CODECS; afmt++)
    {
        const char *name = audio_formats[afmt].codec_root_fn;
        char path[MAX_PATH];
        void *h;

        if (name == NULL)
            continue;

        /* Several formats share one codec; only report each once. */
        if (afmt > 0 && audio_formats[afmt - 1].codec_root_fn != NULL &&
            !strcmp(audio_formats[afmt - 1].codec_root_fn, name))
        {
            continue;
        }

        codec_get_full_path(path, name);

        h = lc_open(path, NULL, 0);
        if (h == NULL)
        {
            printf("  %-12s FAILED\n", name);
            bad++;
        }
        else
        {
            printf("  %-12s ok%s\n", name,
                   lc_get_header(h) != NULL ? "" : " (no header!)");
            lc_close(h);
        }
    }

    printf("%d of the codecs would not load.\n", bad);

    return bad > 0 ? 1 : 0;
}

static void usage(void)
{
    printf(
"soundscan -- build a PodBox sound index from a mounted player\n"
"\n"
"  soundscan [-v] [-n] [player root]\n"
"\n"
"  [player root]  where the player is mounted: the folder holding .rockbox,\n"
"                 for example E:\\ or /media/ipod. Left out,\n"
"                 the connected drives are searched and you are asked\n"
"                 to confirm.\n"
"  -v             one line per track\n"
"  -n             measure but write nothing\n"
"\n"
"Reads every format the player does, using the player's own codecs. Run it\n"
"again after adding music: tracks already measured are left alone.\n");
}

/* Finding the player when nobody said where it is.
 *
 * Every fixed and removable drive is looked at, not just removable ones: an
 * iPod in disk mode shows up as either depending on the machine and the cable.
 * A drive with a .rockbox directory at its root is a player; anything else is
 * somebody's disk and is left alone.
 *
 * Answered rather than assumed, because the tool writes to what it finds and
 * the wrong answer here writes to the wrong disk. */
#ifdef _WIN32

static int probe_root(const char *root)
{
    char path[64];
    DWORD attr;

    snprintf(path, sizeof (path), "%s.rockbox", root);
    attr = GetFileAttributesA(path);

    return attr != INVALID_FILE_ATTRIBUTES
           && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* The volume label, purely so the prompt can say which disk it means. */
static void root_label(const char *root, char *out, size_t outsz)
{
    char name[MAX_PATH];

    if (GetVolumeInformationA(root, name, sizeof (name), NULL, NULL, NULL,
                              NULL, 0)
        && name[0] != '\0')
    {
        snprintf(out, outsz, " (%s)", name);
    }
    else
    {
        out[0] = '\0';
    }
}

static const char *find_player(void)
{
    static char found[8];
    char label[MAX_PATH + 4];
    DWORD mask = GetLogicalDrives();
    int candidates = 0;
    int c;

    for (c = 'A'; c <= 'Z'; c++)
    {
        char root[4];

        if (!(mask & (1UL << (c - 'A'))))
            continue;

        snprintf(root, sizeof (root), "%c:\\", c);

        if (!probe_root(root))
            continue;

        candidates++;
        root_label(root, label, sizeof (label));
        printf("Found a player on %s%s\n", root, label);
        snprintf(found, sizeof (found), "%c:\\", c);
    }

    if (candidates == 0)
    {
        printf("No player found. Connect it, wait for the drive to appear,\n"
               "or name the drive: soundscan E:\\\n");
        return NULL;
    }

    if (candidates > 1)
    {
        printf("More than one player is connected -- name the one you mean:\n"
               "  soundscan E:\\\n");
        return NULL;
    }

    printf("Analyse the music on %s? [y/N] ", found);
    fflush(stdout);

    c = getchar();

    return (c == 'y' || c == 'Y') ? found : NULL;
}

#else

static const char *find_player(void)
{
    printf("Name where the player is mounted, for example:\n"
           "  soundscan /media/ipod\n");
    return NULL;
}

#endif

int main(int argc, char **argv)
{
    const char *target = NULL;
    char self[1024];
    time_t t0;
    int total = 0;
    int i, rc;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* A fault inside a decoder is recoverable here; see on_crash(). */
    signal(SIGSEGV, on_crash);
    signal(SIGILL, on_crash);
    signal(SIGFPE, on_crash);

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-v"))
            opt_verbose = 1;
        else if (!strcmp(argv[i], "-n"))
            opt_dry = 1;
        else if (!strcmp(argv[i], "--codecs"))
            opt_codectest = 1;
        else if (argv[i][0] == '-')
        {
            usage();
            return 1;
        }
        else
            target = argv[i];
    }

    if (target == NULL && !opt_codectest)
    {
        target = find_player();

        if (target == NULL)
            return 1;
    }

    /* Codecs come from beside the executable. The player's own are ARM. */
    snprintf(self, sizeof (self), "%s", argv[0]);
    {
        char *slash = strrchr(self, '/');
#ifdef _WIN32
        char *bs = strrchr(self, '\\');
        if (bs > slash)
            slash = bs;
#endif
        if (slash != NULL)
            *slash = '\0';
        else
            strcpy(self, ".");
    }
    snprintf(codec_dir, sizeof (codec_dir), "%s/codecs", self);

    if (opt_codectest)
        return codec_check();

    sim_root_dir = target;

    if (!dir_exists("/.rockbox"))
    {
        printf("No .rockbox in %s -- is that the player's root?\n", target);
        return 1;
    }

    if (!file_exists("/.rockbox/database_idx.tcd"))
        printf("Note: no tag database on the player. Measuring anyway.\n");

    file_window = malloc(FILE_WINDOW);
    if (file_window == NULL)
        return 1;

    printf("Player %s\n", target);
    printf("Counting...");
    fflush(stdout);
    walk("/", true, &total, 0);
    printf(" %d tracks\n", total);

    if (total == 0)
        return 0;

    rc = sound_index_begin(total + 1, false);
    if (rc != SOUND_OK)
    {
        printf("Could not open the index (%d)\n", rc);
        return 1;
    }

    /* A file the last run did not come back from is filed as unreadable now,
     * before the walk reaches it again. Without this a single bad track stops
     * the library from ever being finished. */
    {
        const char *bad = busy_get();

        if (bad != NULL)
        {
            file_failed(bad);
            busy_clear();
            printf("Skipping %s -- it stopped the previous run.\n", bad);
        }
    }

    t0 = time(NULL);
    walk("/", false, &total, 0);
    busy_clear();

    printf("\r%-79s\r", "");
    printf("%d tracks: %d measured, %d already current, %d too short, "
           "%d unreadable\n",
           n_seen, n_done, n_current, n_short, n_failed);

    if (opt_dry)
    {
        sound_index_close();
        printf("Dry run: nothing written.\n");
        return 0;
    }

    if (sound_index_finish() != SOUND_OK)
    {
        printf("Could not write the index.\n");
        return 1;
    }

    printf("Wrote .rockbox/db_sound.dat in %lds.\n",
           (long)(time(NULL) - t0));

    return 0;
}
