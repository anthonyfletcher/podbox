/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The Theme IDE's way into the simulator compiled to WebAssembly.
 *
 * The page writes a .cfg into the simulator's disk and calls one of these
 * with its path: theme_ide_apply() for a theme, whose skins are applied again
 * even when the .cfg is unchanged (an edit to a skin file changes nothing
 * settings_load_config() can see), and theme_ide_settings() for a few of the
 * player's settings, the IDE's State panel, which leaves the skins alone.
 * theme_ide_play() plays a folder. theme_ide_viewports() reads what the
 * player shows, for the IDE's outlines.
 *
 * The first three arrive while the module sleeps, outside any Rockbox
 * thread, so each only notes its path and posts SYS_THEME_IDE; the UI thread
 * does the work when the screen it is on hands the event to
 * default_event_handler(), as every screen does with an event it does not
 * know.
 ****************************************************************************/
#include "config.h"

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <stdio.h>
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "settings.h"
#include "string-extra.h"
#include "screens.h"
#include "skin/skin_engine.h"
#include "skin/wps_internals.h"
#include "system/activity.h"
#include "playlist/playlist.h"
#include "database/tagcache.h"
#include "audio.h"
#include "metadata.h"

/* what SYS_THEME_IDE asks for, in its data */
enum { IDE_SETTINGS, IDE_THEME, IDE_PLAY, IDE_KINDS };

/* One path per kind, so a request does not overwrite another's that the UI
 * thread has yet to take; and one event per kind in the queue at a time. */
static char theme_ide_path[IDE_KINDS][MAX_PATH];
static unsigned pending;
static bool busy;

static void post(const char *path, int what)
{
    strlcpy(theme_ide_path[what], path, MAX_PATH);
    if (!(pending & (1u << what)))
    {
        pending |= 1u << what;
        button_queue_post(SYS_THEME_IDE, what);
    }
}

EMSCRIPTEN_KEEPALIVE void theme_ide_apply(const char *cfg);
EMSCRIPTEN_KEEPALIVE void theme_ide_apply(const char *cfg)
{
    post(cfg, IDE_THEME);
}

EMSCRIPTEN_KEEPALIVE void theme_ide_settings(const char *cfg);
EMSCRIPTEN_KEEPALIVE void theme_ide_settings(const char *cfg)
{
    post(cfg, IDE_SETTINGS);
}

/* Plays the tracks of a folder from the first, as choosing it in Files
 * would; the State panel's albums */
EMSCRIPTEN_KEEPALIVE void theme_ide_play(const char *dir);
EMSCRIPTEN_KEEPALIVE void theme_ide_play(const char *dir)
{
    post(dir, IDE_PLAY);
}

static void play_folder(const char *dir)
{
    if (playlist_create(dir, NULL) == 0 &&
        playlist_insert_directory(NULL, dir, PLAYLIST_INSERT_LAST,
                                  false, false, NULL) >= 0 &&
        playlist_amount() > 0)
        playlist_start(0, 0, 0);
    else
        audio_stop();       /* nothing to play: not the last list's track */
}

/* A theme change stops playback -- the skins take their buffers from the
 * audio buffer -- which on the player is a rare choice and here is every
 * edit: so the track starts again where it was. */
static void load(const char *cfg, bool theme)
{
    int status = audio_status();
    struct mp3entry *id3 = (status & AUDIO_STATUS_PLAY) ? audio_current_track() : NULL;
    int index = id3 ? playlist_get_current()->index : 0;
    unsigned long elapsed = id3 ? id3->elapsed : 0, offset = id3 ? id3->offset : 0;

    if (!theme)
        settings_load_config(cfg, true);
    else
    {
        /* Applied here rather than by settings_load_config(), which applies
         * the skins only when the .cfg names different ones. Stopped before
         * settings_apply() for its reason: each font it loads would
         * otherwise stop and rebuffer the track. */
        if (cfg[0])
            settings_load_config(cfg, false);
        if (status & AUDIO_STATUS_PLAY)
            audio_stop();
        settings_save();
        settings_apply(true);
        settings_apply_skins();
    }
    if (id3 && !(audio_status() & AUDIO_STATUS_PLAY))
    {
        playlist_start(index, elapsed, offset);
        if (status & AUDIO_STATUS_PAUSE)
            audio_pause();
    }
    /* The screen that handed over the event repaints only when asked, and
     * its first repaint after a theme change leaves the selected row half
     * drawn; the second is whole. */
    button_queue_post(BUTTON_REDRAW, 0);
    button_queue_post(BUTTON_REDRAW, 0);
}

void theme_ide_event(intptr_t what)
{
    char path[MAX_PATH];

    if (what < 0 || what >= IDE_KINDS)
        return;
    /* A long job here -- a folder's tracks inserted -- hands queued events
     * to default_event_handler() as it goes; one of ours waits for it. */
    if (busy)
    {
        button_queue_post(SYS_THEME_IDE, what);
        return;
    }
    strlcpy(path, theme_ide_path[what], sizeof path);
    pending &= ~(1u << what);

    busy = true;
    if (what == IDE_PLAY)
        play_folder(path);
    else
        load(path, what == IDE_THEME);
    busy = false;
}

/* Writes s as a JSON string's contents: at most max bytes of it, cut only
 * between UTF-8 characters. */
static int json_string(char *out, int size, const char *s, int max)
{
    int n = 0, len = strlen(s);

    if (len > max)
    {
        len = max;
        while (len > 0 && ((unsigned char)s[len] & 0xc0) == 0x80)
            len--;
    }
    for (int i = 0; i < len && n < size - 7; i++)
    {
        unsigned char c = s[i];
        if (c == '"' || c == '\\')
        {
            out[n++] = '\\';
            out[n++] = c;
        }
        else if (c < 0x20)
            n += snprintf(out + n, size - n, "\\u%04x", c);
        else
            out[n++] = c;
    }
    out[n] = '\0';
    return n;
}

/* The viewports of one loaded skin as the engine resolved them, as JSON:
 * { "activity": n, "audio": AUDIO_STATUS_*, "library": 0|1, "viewports":
 * [{ label, x, y, w, h, font, line, ui, hideable, hidden, never }] }, line
 * counting from 1. skin is 0 for the statusbar skin (.sbs) and 1 for Now
 * Playing (.wps); activity is what the player is showing, which says whether
 * that skin is on screen. library is 1 once the database can be browsed.
 * Read while the module sleeps, so nothing is half drawn; a skin that is not
 * loaded has no viewports. */
EMSCRIPTEN_KEEPALIVE const char *theme_ide_viewports(int skin);
EMSCRIPTEN_KEEPALIVE const char *theme_ide_viewports(int skin)
{
    static char json[16384];
    char label[200];
    int n = snprintf(json, sizeof json,
                     "{\"activity\":%d,\"audio\":%d,\"library\":%d,"
                     "\"viewports\":[",
                     (int)get_current_activity(), audio_status(),
                     tagcache_is_usable() && tagcache_search_ready());
    struct gui_wps *gwps = skin_get_gwps_loaded(skin ? WPS : CUSTOM_STATUSBAR,
                                                SCREEN_MAIN);
    struct wps_data *data = gwps ? gwps->data : NULL;
    char *buf = data && data->wps_loaded ? get_skin_buffer(data) : NULL;
    struct skin_element *vp = buf ? SKINOFFSETTOPTR(buf, data->tree) : NULL;
    bool first = true;

    for (; vp && n < (int)sizeof json - 512;
         vp = SKINOFFSETTOPTR(buf, vp->next))
    {
        struct skin_viewport *svp = SKINOFFSETTOPTR(buf, vp->data);
        if (!svp)
            break;
        label[0] = '\0';
        if (svp->label != VP_DEFAULT_LABEL && SKINOFFSETTOPTR(buf, svp->label))
            json_string(label, sizeof label, SKINOFFSETTOPTR(buf, svp->label),
                        32);
        n += snprintf(json + n, sizeof json - n,
            "%s{\"label\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
            "\"font\":%d,\"line\":%d,\"ui\":%d,\"hideable\":%d,"
            "\"hidden\":%d,\"never\":%d}",
            first ? "" : ",", label, svp->vp.x, svp->vp.y, svp->vp.width,
            svp->vp.height, svp->parsed_fontid, vp->line, svp->is_infovp,
            (svp->hidden_flags & VP_DRAW_HIDEABLE) != 0,
            (svp->hidden_flags & VP_DRAW_HIDDEN) != 0,
            (svp->hidden_flags & VP_NEVER_VISIBLE) != 0);
        first = false;
    }
    snprintf(json + n, sizeof json - n, "]}");
    return json;
}

#endif /* __EMSCRIPTEN__ */
