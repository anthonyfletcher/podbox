/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Debug > Codec on COP Check: a codec run on the 5G's second core must give
 * the samples it gives on the main core. For each audio file in the folder
 * of the track playing, the first 30 seconds are decoded twice through
 * track_decode.c, on the CPU and then on the COP (codec_cop.c), and a hash
 * of every sample and the length decoded are compared. Playback is stopped
 * first, as only one codec may be loaded.
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "core_alloc.h"
#include "kernel.h"
#include "audio.h"
#include "dir.h"
#include "file.h"
#include "metadata.h"
#include "string-extra.h"
#include "dsp_core.h"
#include "audio/track_decode.h"
#include "screens/system/codec_cop_check.h"
#include "draw/viewport.h"
#include "draw/screen_access.h"
#include "widgets/list.h"
#include "widgets/splash.h"
#include "input/action.h"
#include "api/misc.h"
#include "system/hash.h"

#define CHECK_MS        30000
#define CHECK_FILES     40
#define CHECK_WINDOW    (256 * 1024)
/* The simple list's own limits (list.h): 48 lines of 48 bytes. A line past
 * its share writes past the list's buffer, so every line here is cut to
 * fit and the lines stop with room for the last. */
#define LINE_NAME       34
#define LINES_MAX       (SIMPLELIST_MAX_LINES - 2)

enum outcome { SAME, DIFFERED, CPU_ONLY, NOT_AUDIO };

static char (*names)[MAX_PATH];  /* After the window, in its allocation */
static int nnames, nfound;
static enum outcome outcomes[CHECK_FILES];
static const char *whys[CHECK_FILES];
static uint32_t hash;
static int check_event;     /* System event check_abort() took, or 0 */
static bool cancelled;

/* Every sample, in order, whatever blocks it came in */
static void hash_sink(const void *ch1, const void *ch2, int count,
                      const struct track_pcm *fmt, unsigned long track_ms)
{
    const int32_t *a = ch1, *b = ch2;
    int n = count;

    if (fmt->stereo_mode == STEREO_INTERLEAVED)
        n = count * 2;
    for (int i = 0; i < n; i++)
    {
        hash = (hash ^ (uint32_t)a[i]) * FNV1A_PRIME;
        if (fmt->stereo_mode == STEREO_NONINTERLEAVED && b != NULL)
            hash = (hash ^ (uint32_t)b[i]) * FNV1A_PRIME;
    }
    (void)track_ms;
}

/* On the decode thread: a USB connect, power-off or reboot is noted and
 * left to the screen, since its handler would run on the codec's stack. */
static bool check_abort(void)
{
    int button = get_action(CONTEXT_STD, TIMEOUT_NOBLOCK);

    switch (button)
    {
    case ACTION_STD_CANCEL:
        cancelled = true;
        return true;
    case SYS_USB_CONNECTED:
    case SYS_POWEROFF:
    case SYS_REBOOT:
        check_event = button;
        return true;
    default:
        return false;
    }
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

/* The first CHECK_FILES files in the folder, sorted; nfound counts them all */
static void list_folder(const char *dirpath)
{
    DIR *dir = opendir(dirpath);
    struct dirent *ent;

    nnames = nfound = 0;
    if (dir == NULL)
        return;
    while ((ent = readdir(dir)) != NULL)
    {
        if (dir_get_info(dir, ent).attribute & ATTR_DIRECTORY)
            continue;
        if (ent->d_name[0] == '.')
            continue;
        nfound++;
        if (nnames < CHECK_FILES)
            strmemccpy(names[nnames++], ent->d_name, MAX_PATH);
    }
    closedir(dir);
    qsort(names, nnames, MAX_PATH, by_name);
}

/* One run; the hash and the length decoded through *h and *ms */
static int decode_once(const char *path, void *window, bool cop,
                       uint32_t *h, unsigned long *ms)
{
    int rc;

    hash = FNV1A_BASIS;
    track_decode_on_cop(cop);
    rc = track_decode_run(path, 0, CHECK_MS, window, CHECK_WINDOW,
                          hash_sink, NULL, check_abort, ms);
    track_decode_on_cop(false);
    *h = hash;
    return rc;
}

bool codec_cop_check_screen(void)
{
    struct simplelist_info info;
    struct mp3entry *id3 = audio_current_track();
    char dir[MAX_PATH], path[MAX_PATH];
    char *slash;
    void *window;
    int handle, same = 0, differed = 0, cpu_only = 0, checked, lines = 0;

    if (id3 == NULL || id3->path[0] == '\0')
    {
        splash(HZ * 2, "Play a track from the folder first");
        return false;
    }
    strmemccpy(dir, id3->path, sizeof(dir));
    slash = strrchr(dir, '/');
    if (slash == NULL)
        return false;
    *slash = '\0';

    audio_stop();

    /* Immovable: track_decode.c holds the window's address for the whole
     * decode. The names follow it. */
    handle = core_alloc_ex(CHECK_WINDOW + CHECK_FILES * MAX_PATH,
                           &buflib_ops_locked);
    if (handle <= 0)
    {
        splash(HZ * 2, "No memory");
        return false;
    }
    window = core_get_data(handle);
    names = (char (*)[MAX_PATH])((char *)window + CHECK_WINDOW);
    list_folder(dir);

    check_event = 0;
    cancelled = false;
    checked = 0;

    /* Off for the decode: the theme's status bar would otherwise be drawn
     * from check_abort(), on the codec's stack. */
    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, false, NULL);

    for (int i = 0; i < nnames; i++)
    {
        uint32_t h_cpu, h_cop;
        unsigned long ms_cpu = 0, ms_cop = 0;
        int rc_cpu, rc_cop;

        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        splashf(0, "%d/%d %s", i + 1, nnames, names[i]);

        rc_cpu = decode_once(path, window, false, &h_cpu, &ms_cpu);
        if (check_event || cancelled)
            break;
        outcomes[i] = NOT_AUDIO;
        if (rc_cpu == TRACK_DECODE_NO_CODEC || rc_cpu == TRACK_DECODE_NO_FILE)
        {
            checked = i + 1;
            continue;
        }

        rc_cop = decode_once(path, window, true, &h_cop, &ms_cop);
        if (check_event || cancelled)
            break;
        checked = i + 1;

        if (!track_decode_ran_on_cop())
        {
            outcomes[i] = CPU_ONLY;
            whys[i] = track_decode_cop_why();
            cpu_only++;
        }
        else if (rc_cpu == rc_cop && h_cpu == h_cop && ms_cpu == ms_cop)
        {
            outcomes[i] = SAME;
            same++;
        }
        else
        {
            outcomes[i] = DIFFERED;
            differed++;
        }
    }

    FOR_NB_SCREENS(i)
        viewportmanager_theme_undo(i, true);

    if (check_event)
    {
        core_free(handle);
        default_event_handler(check_event);
        return true;
    }

    simplelist_info_init(&info, "Codec on COP Check", 0, NULL);
    info.scroll_all = true;
    simplelist_reset_lines();
    simplelist_addline("%d same, %d differed, %d CPU only", same, differed,
                       cpu_only);
    if (cancelled || nfound > nnames)
        simplelist_addline("%d of %d files%s", checked, nfound,
                           cancelled ? ", stopped" : "");
    for (int i = 0; i < checked; i++)
    {
        if (outcomes[i] == SAME || outcomes[i] == NOT_AUDIO)
            continue;
        if (++lines > LINES_MAX)
        {
            simplelist_addline("...");
            break;
        }
        if (outcomes[i] == DIFFERED)
            simplelist_addline("DIFFERED %.*s", LINE_NAME, names[i]);
        else
            simplelist_addline("CPU %.*s", LINE_NAME, names[i]);
    }
    for (int i = 0; i < checked; i++)
    {
        if (outcomes[i] == CPU_ONLY && lines < LINES_MAX)
        {
            simplelist_addline("CPU only: %.36s", whys[i]);
            break;
        }
    }
    core_free(handle);
    return simplelist_show_list(&info);
}
