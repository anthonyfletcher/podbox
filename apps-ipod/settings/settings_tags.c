/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The tag table: one row per setting worth finding, saying what it is about
 * and whether it is advanced. See settings_tags.h for what the two kinds of
 * tag mean.
 *
 * The table is keyed by cfg name, so it is a table of contents for
 * settings_list.c rather than an edit to it. A key naming no setting is a
 * fault; settings_tags_validate() is what finds one, since the compiler
 * cannot.
 *
 * Untagged settings are fine. Those with no menu entry at all (remembered
 * state -- the quickscreen slots, the root menu order, the start directory)
 * are deliberately absent: search skips anything with no name to show, so a
 * row for one would never be reached.
 *
 * Parts, in order:
 *   - the topic names
 *   - the table
 *   - resolving a setting to its row
 *   - the interface
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "logf.h"
#include "string-extra.h"
#include "lang.h"
#include "settings.h"
#include "settings_list.h"
#include "settings_tags.h"

/* ---- the topic names ---------------------------------------------------- */

/* What a topic is called when it is searched for. Ordered by tag value so the
 * lookup can be a plain scan. */
static const struct {
    uint16_t tag;
    const char *name;
} topic_names[] = {
    { TAG_SOUND,       "sound"      },
    { TAG_PLAYBACK,    "playback"   },
    { TAG_PLAYLIST,    "playlist"   },
    { TAG_LIBRARY,     "library"    },
    { TAG_DATABASE,    "database"   },
    { TAG_ARTWORK,     "artwork"    },
    { TAG_APPEARANCE,  "appearance" },
    { TAG_SCROLLING,   "scrolling"  },
    { TAG_THEMEAUTHOR, "theme"      },
    { TAG_BATTERY,     "battery"    },
    { TAG_SYSTEM,      "system"     },
    { TAG_USB,         "usb"        },
    { TAG_VOICE,       "voice"      },
};

/* ---- the table ---------------------------------------------------------- */

struct tag_row {
    const char *cfg_name;
    uint16_t    tags;
    const char *words;   /* extra search terms, space separated; NULL if none */
};

/* Words earn their place only where the name and the topics do not already
 * reach them: an abbreviation, the other spelling of a pair, a misspelling
 * that actually happens, or the symptom someone would search for rather than
 * the setting that fixes it. */
static const struct tag_row tag_rows[] = {

/* --- sound ------------------------------------------------------------- */
{ "volume",              TAG_SOUND,                              NULL },
{ "volume limit",        TAG_SOUND,                              "maximum loud" },
{ "bass",                TAG_SOUND,                              "base low end" },
{ "bass cutoff",         TAG_ADVANCED|TAG_SOUND,                 "base corner frequency" },
{ "treble",              TAG_SOUND,                              "high" },
{ "treble cutoff",       TAG_ADVANCED|TAG_SOUND,                 "corner frequency" },
{ "balance",             TAG_SOUND,                              "left right" },
{ "channels",            TAG_SOUND,                              "mono stereo karaoke swap" },
{ "stereo_width",        TAG_SOUND,                              "stereo width" },
{ "volume fade",         TAG_SOUND|TAG_PLAYBACK,                 "fade stop pause" },
{ "dithering enabled",   TAG_ADVANCED|TAG_SOUND,                 "dither noise" },
{ "afr enabled",         TAG_SOUND,                              "fatigue harsh listening" },
{ "volume adjustment mode",     TAG_ADVANCED|TAG_SOUND|TAG_SYSTEM, "perceptual steps" },
{ "perceptual volume step count", TAG_ADVANCED|TAG_SOUND|TAG_SYSTEM, "steps" },

/* crossfeed: the first row turns it on, the rest shape it */
{ "crossfeed",               TAG_SOUND,                          "headphone meier" },
{ "crossfeed direct gain",   TAG_ADVANCED|TAG_SOUND,             "crossfeed" },
{ "crossfeed cross gain",    TAG_ADVANCED|TAG_SOUND,             "crossfeed" },
{ "crossfeed hf attenuation",TAG_ADVANCED|TAG_SOUND,             "crossfeed" },
{ "crossfeed hf cutoff",     TAG_ADVANCED|TAG_SOUND,             "crossfeed" },

/* equalizer. The per-band filters carry no lang_id, so they are not here:
 * search would have no name to show for them. */
{ "eq enabled",          TAG_SOUND,                              "eq equalizer equaliser" },
{ "eq precut",           TAG_ADVANCED|TAG_SOUND,                 "eq equalizer headroom clipping" },

/* haas surround */
{ "surround enabled",    TAG_SOUND,                              "haas surround" },
{ "surround balance",    TAG_ADVANCED|TAG_SOUND,                 "haas surround" },
{ "surround_fx1",        TAG_ADVANCED|TAG_SOUND,                 "haas surround band" },
{ "surround_fx2",        TAG_ADVANCED|TAG_SOUND,                 "haas surround band" },
{ "side only",           TAG_ADVANCED|TAG_SOUND,                 "haas surround" },
{ "surround mix",        TAG_ADVANCED|TAG_SOUND,                 "haas surround dry wet" },

/* perceptual bass enhancement */
{ "pbe",                 TAG_SOUND,                              "perceptual bass base" },
{ "pbe precut",          TAG_ADVANCED|TAG_SOUND,                 "perceptual bass clipping" },

/* compressor: threshold is the on/off, the rest shape it */
{ "compressor threshold",    TAG_ADVANCED|TAG_SOUND,                          "compressor dynamic range" },
{ "compressor makeup gain",  TAG_ADVANCED|TAG_SOUND,             "compressor" },
{ "compressor ratio",        TAG_ADVANCED|TAG_SOUND,             "compressor limit" },
{ "compressor knee",         TAG_ADVANCED|TAG_SOUND,             "compressor" },
{ "compressor attack time",  TAG_ADVANCED|TAG_SOUND,             "compressor" },
{ "compressor release time", TAG_ADVANCED|TAG_SOUND,             "compressor" },

/* replaygain */
{ "replaygain type",     TAG_SOUND|TAG_PLAYBACK,                 "replaygain loudness level" },
{ "replaygain noclip",   TAG_ADVANCED|TAG_SOUND|TAG_PLAYBACK,    "replaygain clipping" },
{ "replaygain preamp",   TAG_ADVANCED|TAG_SOUND|TAG_PLAYBACK,    "replaygain" },

/* --- playback ------------------------------------------------------------ */
{ "shuffle",             TAG_PLAYBACK,                           "random" },
{ "repeat",              TAG_PLAYBACK,                           NULL },
{ "play selected",       TAG_PLAYBACK,                           NULL },
{ "single mode",         TAG_PLAYBACK,                           "stop after" },
{ "party mode",          TAG_PLAYBACK,                           "queue" },
{ "cuesheet support",    TAG_ADVANCED|TAG_PLAYBACK,                           "cue sheet" },
{ "folder navigation",   TAG_PLAYBACK,                           "auto change directory next folder" },
{ "constrain next folder", TAG_ADVANCED|TAG_PLAYBACK,            "auto change directory" },
{ "skip length",         TAG_ADVANCED|TAG_PLAYBACK,                           "podcast jump" },
{ "prevent track skip",  TAG_ADVANCED|TAG_PLAYBACK,              "lock kiosk" },
{ "rewind across tracks",TAG_ADVANCED|TAG_PLAYBACK,              "rewind previous" },
{ "resume rewind",       TAG_ADVANCED|TAG_PLAYBACK,              "rewind before resume" },
{ "rewind duration on pause", TAG_ADVANCED|TAG_PLAYBACK,         "rewind unpause" },
{ "antiskip",            TAG_ADVANCED|TAG_PLAYBACK,              "stutter skipping buffer" },
{ "beep",                TAG_ADVANCED|TAG_PLAYBACK,              "skip beep" },
{ "playback frequency",  TAG_ADVANCED|TAG_PLAYBACK|TAG_SYSTEM,   "sample rate hz" },
{ "play log",            TAG_ADVANCED|TAG_PLAYBACK,              "scrobble last.fm logging" },
{ "scan min step",       TAG_ADVANCED|TAG_PLAYBACK,              "fast forward rewind seek" },
{ "seek acceleration",   TAG_ADVANCED|TAG_PLAYBACK,              "fast forward rewind" },
{ "pause on headphone unplug", TAG_PLAYBACK,                     "headphone unplug" },
{ "disable autoresume if phones not present", TAG_ADVANCED|TAG_PLAYBACK, "headphone resume" },
{ "album art",           TAG_PLAYBACK|TAG_ARTWORK,               "cover art artwork" },

/* crossfade: the first row turns it on, the rest shape it */
{ "crossfade",                     TAG_PLAYBACK,                 "overlap gapless" },
{ "crossfade fade in delay",       TAG_ADVANCED|TAG_PLAYBACK,    "crossfade" },
{ "crossfade fade in duration",    TAG_ADVANCED|TAG_PLAYBACK,    "crossfade" },
{ "crossfade fade out delay",      TAG_ADVANCED|TAG_PLAYBACK,    "crossfade" },
{ "crossfade fade out duration",   TAG_ADVANCED|TAG_PLAYBACK,    "crossfade" },
{ "crossfade fade out mode",       TAG_ADVANCED|TAG_PLAYBACK,    "crossfade mix" },

/* bookmarks and automatic resume */
{ "autocreate bookmarks", TAG_ADVANCED|TAG_PLAYBACK,                          "bookmark on stop" },
{ "autoupdate bookmarks", TAG_ADVANCED|TAG_PLAYBACK,             "bookmark" },
{ "autoload bookmarks",   TAG_ADVANCED|TAG_PLAYBACK,                          "bookmark load" },
{ "use most-recent-bookmarks", TAG_ADVANCED|TAG_PLAYBACK,        "bookmark recent" },
{ "autoresume enable",    TAG_PLAYBACK,                          "resume position" },
{ "autoresume next track",TAG_ADVANCED|TAG_PLAYBACK,             "resume" },

/* --- playlists ----------------------------------------------------------- */
{ "sort playlists",      TAG_PLAYLIST|TAG_LIBRARY,               NULL },
{ "recursive directory insert", TAG_PLAYLIST,                    "subfolder insert" },
{ "warn when erasing dynamic playlist", TAG_PLAYLIST,            "warn erase" },
{ "keep current track when replacing playlist", TAG_ADVANCED|TAG_PLAYLIST, NULL },
{ "show shuffled adding options", TAG_ADVANCED|TAG_PLAYLIST,     "shuffle add" },
{ "show queue options",  TAG_ADVANCED|TAG_PLAYLIST,              "queue" },
{ "max files in playlist", TAG_ADVANCED|TAG_PLAYLIST|TAG_SYSTEM, "limit maximum size" },
{ "playlist viewer icons",   TAG_PLAYLIST|TAG_APPEARANCE,        "playlist viewer" },
{ "playlist viewer indices", TAG_PLAYLIST|TAG_APPEARANCE,        "playlist viewer numbers" },
{ "playlist viewer track display", TAG_PLAYLIST|TAG_APPEARANCE,  "playlist viewer" },

/* --- files and the browser ----------------------------------------------- */
{ "sort case",           TAG_LIBRARY,                            "sorting uppercase" },
{ "sort dirs",           TAG_LIBRARY,                            "sorting directories folders" },
{ "sort files",          TAG_LIBRARY,                            "sorting" },
{ "sort interpret number", TAG_LIBRARY,                          "sorting numbers" },
{ "show files",          TAG_LIBRARY,                            "filter hide" },
{ "show filename exts",  TAG_LIBRARY,                            "extension" },
{ "follow playlist",     TAG_LIBRARY,                            NULL },
{ "show path in browser",TAG_LIBRARY|TAG_APPEARANCE,             "path title" },
{ "hotkey tree",         TAG_ADVANCED|TAG_LIBRARY,               "hotkey button" },
{ "max files in dir",    TAG_ADVANCED|TAG_LIBRARY|TAG_SYSTEM,    "limit maximum entries" },
{ "dircache",            TAG_LIBRARY|TAG_SYSTEM,                 "directory cache speed" },

/* --- the database -------------------------------------------------------- */
{ "tagcache_ram",              TAG_ADVANCED|TAG_LIBRARY|TAG_DATABASE,         "database ram memory load" },
{ "tagcache_scan_on_startup",  TAG_ADVANCED|TAG_LIBRARY|TAG_DATABASE,         "database scan boot" },
{ "tagcache_scan_on_eject",    TAG_ADVANCED|TAG_LIBRARY|TAG_DATABASE,         "database scan usb eject" },
{ "tagcache_autocommit",       TAG_ADVANCED|TAG_LIBRARY|TAG_DATABASE, "database commit" },
{ "gather runtime data",       TAG_ADVANCED|TAG_LIBRARY|TAG_DATABASE,         "play count rating runtime" },
{ "database scan paths",       TAG_ADVANCED|TAG_LIBRARY|TAG_DATABASE, "directories folders" },
{ "database sort albums by",   TAG_LIBRARY,                      "sorting albums year" },
{ "debug log tagcache",        TAG_ADVANCED|TAG_LIBRARY|TAG_DATABASE, "log debug" },
{ "search max rows",     TAG_LIBRARY,                            "search find results" },
{ "search min letters",  TAG_LIBRARY,                            "search find letters" },
{ "search order",        TAG_LIBRARY,                            "search find order" },
{ "featured artists",    TAG_LIBRARY,                            "guest feat credits" },

/* --- artwork ------------------------------------------------------------- */
{ "art cache fast build",TAG_ADVANCED|TAG_ARTWORK,               "thumbnail cache" },
{ "debug log artcache",  TAG_ADVANCED|TAG_ARTWORK,               "log debug thumbnail" },
{ "database album art",  TAG_ARTWORK|TAG_APPEARANCE|TAG_LIBRARY, "art rows cover thumbnail" },
{ "database artist art", TAG_ARTWORK|TAG_APPEARANCE|TAG_LIBRARY, "art rows photo thumbnail" },
{ "database art row height", TAG_ADVANCED|TAG_ARTWORK|TAG_THEMEAUTHOR, "art rows" },
{ "wps art source",      TAG_ARTWORK|TAG_APPEARANCE,             "cover art artist photo" },
{ "dynamic colors",      TAG_APPEARANCE|TAG_ARTWORK,             "dynamic colours album" },

/* --- the carousel -------------------------------------------------------- */
{ "album covers on select",       TAG_LIBRARY|TAG_APPEARANCE,    "carousel cover flow" },
{ "album covers show album name", TAG_LIBRARY|TAG_APPEARANCE,    "carousel caption" },
{ "album covers show year",       TAG_LIBRARY|TAG_APPEARANCE,    "carousel caption" },
{ "album covers background",      TAG_APPEARANCE,                "carousel" },
{ "album covers statusbar",       TAG_APPEARANCE,                "carousel status bar" },
{ "album covers sort albums by",  TAG_LIBRARY,                   "carousel sorting" },
{ "album covers sort artists by", TAG_LIBRARY,                   "carousel sorting" },
{ "album covers year sort order", TAG_ADVANCED|TAG_LIBRARY,      "carousel sorting year" },
{ "album covers view mode",       TAG_APPEARANCE,                "carousel 3d flat" },
{ "album covers scroll speed",    TAG_APPEARANCE,                "carousel speed" },
{ "album covers center margin",   TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "carousel 3d" },
{ "album covers slide tuck",      TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "carousel 3d" },
{ "album covers parallel slides", TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "carousel 3d" },
{ "album covers transition speed",TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "carousel 3d speed" },
{ "album covers pile fade",       TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "carousel flat" },
{ "album covers pile offset",     TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "carousel flat" },

/* --- the theme ----------------------------------------------------------- */
{ "wps",                 TAG_APPEARANCE,                         "theme while playing skin" },
{ "sbs",                 TAG_APPEARANCE,                         "theme base skin statusbar" },
{ "font",                TAG_APPEARANCE,                         "theme typeface" },
{ "font bold",           TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "theme typeface" },
{ "backdrop",            TAG_APPEARANCE|TAG_THEMEAUTHOR,         "theme background image wallpaper" },
{ "iconset",             TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "icons theme" },
{ "viewers iconset",     TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "icons theme" },
{ "filetype colours",    TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "colors file browser" },
{ "ui viewport",         TAG_ADVANCED|TAG_THEMEAUTHOR,           "viewport layout" },
{ "progress bar radius", TAG_ADVANCED|TAG_THEMEAUTHOR,           "progress bar corner" },
{ "show icons",          TAG_APPEARANCE,                         "icons list" },
{ "foreground color",    TAG_APPEARANCE,                         "colour text" },
{ "background color",    TAG_APPEARANCE,                         "colour" },
{ "selector type",       TAG_APPEARANCE,                         "line selector cursor highlight" },
{ "line selector start color", TAG_APPEARANCE,                   "colour selector gradient" },
{ "line selector end color",   TAG_APPEARANCE,                   "colour selector gradient" },
{ "line selector text color",  TAG_APPEARANCE,                   "colour selector" },
{ "list separator height", TAG_ADVANCED|TAG_APPEARANCE,          "separator rule line" },
{ "list separator color",  TAG_ADVANCED|TAG_APPEARANCE,          "colour separator rule" },
{ "statusbar",           TAG_APPEARANCE,                         "status bar" },
{ "scrollbar",           TAG_APPEARANCE,                         "scroll bar" },
{ "scrollbar width",     TAG_ADVANCED|TAG_APPEARANCE,            "scroll bar" },
{ "volume display",      TAG_APPEARANCE,                         "status bar volume" },
{ "battery display",     TAG_APPEARANCE|TAG_BATTERY,             "status bar battery" },

/* dialogs: the first row picks how they are coloured, the rest are geometry
 * and the nine colours behind it */
{ "dialog colours",              TAG_APPEARANCE,                 "dialog colors box" },
{ "dialog box border width",     TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "dialog box" },
{ "dialog box margin",           TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "dialog box" },
{ "dialog box shadow",           TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "dialog box drop shadow" },
{ "dialog box shadow colour",    TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "dialog box shadow color" },
{ "dialog box foreground",       TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "dialog colour text" },
{ "dialog box background",       TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "dialog colour" },
{ "dialog box border colour",    TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "dialog colour color" },
{ "dialog button border width",  TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "dialog button" },
{ "dialog button border radius", TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "dialog button corner rounded" },
{ "dialog button foreground",    TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "dialog colour text" },
{ "dialog button background",    TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "dialog colour" },
{ "dialog button border colour", TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "dialog colour color" },
{ "dialog button foreground selected", TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "dialog colour text" },
{ "dialog button background selected", TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "dialog colour" },
{ "dialog button border colour selected", TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "dialog colour color" },

/* the peak meter some themes draw */
{ "peak meter release",  TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "peak meter" },
{ "peak meter hold",     TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "peak meter" },
{ "peak meter clip hold",TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "peak meter clipping" },
{ "peak meter dbfs",     TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "peak meter scale" },
{ "peak meter min",      TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "peak meter range" },
{ "peak meter max",      TAG_ADVANCED|TAG_APPEARANCE|TAG_THEMEAUTHOR, "peak meter range" },

/* --- scrolling ----------------------------------------------------------- */
{ "scroll speed",        TAG_SCROLLING|TAG_APPEARANCE,           "scrolling text" },
{ "scroll delay",        TAG_SCROLLING|TAG_APPEARANCE,           "scrolling text start" },
{ "scroll step",         TAG_ADVANCED|TAG_SCROLLING|TAG_APPEARANCE, "scrolling text" },
{ "bidir limit",         TAG_ADVANCED|TAG_SCROLLING|TAG_APPEARANCE, "bidirectional scrolling bounce" },
{ "screen scroll step",  TAG_ADVANCED|TAG_SCROLLING|TAG_APPEARANCE, "scrolling screen" },
{ "scroll paginated",    TAG_SCROLLING|TAG_APPEARANCE,           "paged scrolling list" },
{ "list wraparound",     TAG_SCROLLING|TAG_APPEARANCE,           "wrap list" },
{ "list order",          TAG_ADVANCED|TAG_SCROLLING|TAG_APPEARANCE, "list ascending descending" },
/* Both carry a sentence for a cfg name rather than the usual lowercase words.
   They are settings like any other; the odd keys are upstream's. */
{ "Screen Scrolls Out Of View", TAG_ADVANCED|TAG_SCROLLING|TAG_APPEARANCE, "scrolling offscreen edge" },
{ "Disable main menu scrolling", TAG_ADVANCED|TAG_SCROLLING|TAG_APPEARANCE, "scrolling main menu root" },
{ "hold_lr_for_scroll_in_list", TAG_ADVANCED|TAG_SCROLLING,      "list scrolling hold" },

/* --- the viewers --------------------------------------------------------- */
{ "text viewer colour mode",  TAG_APPEARANCE,                    "text viewer colors" },
{ "text viewer margin",       TAG_APPEARANCE,                    "text viewer" },
{ "text viewer line spacing", TAG_ADVANCED|TAG_APPEARANCE,       "text viewer leading" },
{ "text viewer page number",  TAG_APPEARANCE,                    "text viewer" },
{ "text viewer font",         TAG_APPEARANCE,                    "text viewer typeface" },
{ "lyric colour mode",        TAG_APPEARANCE,                    "lyrics colors" },
{ "lyric align",              TAG_APPEARANCE,                    "lyrics alignment" },
{ "lyric line spacing",       TAG_ADVANCED|TAG_APPEARANCE,       "lyrics leading" },
{ "lyric previous opacity",   TAG_ADVANCED|TAG_APPEARANCE,       "lyrics fade" },
{ "lyric next opacity",       TAG_ADVANCED|TAG_APPEARANCE,       "lyrics fade" },
{ "lyric animation",          TAG_APPEARANCE,                    "lyrics scroll" },
{ "lyric highlight",          TAG_APPEARANCE,                    "lyrics words" },
{ "lyric backlight",          TAG_APPEARANCE|TAG_BATTERY,        "lyrics backlight" },
{ "lyric font",               TAG_APPEARANCE,                    "lyrics typeface" },

/* --- battery and power --------------------------------------------------- */
{ "backlight timeout",   TAG_BATTERY|TAG_APPEARANCE,             "backlight light screen timeout" },
{ "backlight timeout plugged", TAG_BATTERY|TAG_APPEARANCE,       "backlight light charging plugged" },
{ "backlight on button hold",  TAG_ADVANCED|TAG_BATTERY|TAG_APPEARANCE, "backlight lock hold" },
{ "caption backlight",   TAG_ADVANCED|TAG_BATTERY|TAG_APPEARANCE,"backlight track change" },
{ "backlight fade in",   TAG_ADVANCED|TAG_BATTERY|TAG_APPEARANCE,"backlight fade" },
{ "backlight fade out",  TAG_ADVANCED|TAG_BATTERY|TAG_APPEARANCE,"backlight fade" },
{ "backlight filters first keypress", TAG_ADVANCED|TAG_BATTERY,  "backlight first button" },
{ "lcd sleep after backlight off", TAG_ADVANCED|TAG_BATTERY,     "screen sleep panel" },
{ "brightness",          TAG_BATTERY|TAG_APPEARANCE,             "screen bright" },
{ "idle poweroff",       TAG_BATTERY|TAG_SYSTEM,                 "idle power off shutdown" },
{ "sleeptimer duration", TAG_BATTERY|TAG_SYSTEM,                 "sleep timer" },
{ "sleeptimer on startup", TAG_ADVANCED|TAG_BATTERY|TAG_SYSTEM,  "sleep timer boot" },
{ "keypress restarts sleeptimer", TAG_ADVANCED|TAG_BATTERY|TAG_SYSTEM, "sleep timer" },
{ "battery capacity",    TAG_ADVANCED|TAG_BATTERY|TAG_SYSTEM,    "battery mah cell" },
{ "usb charging",        TAG_BATTERY|TAG_USB|TAG_SYSTEM,         "charge charging usb" },
{ "disk spindown",       TAG_ADVANCED|TAG_BATTERY|TAG_SYSTEM,    "disk drive spin idle" },
{ "storage mode",        TAG_ADVANCED|TAG_BATTERY|TAG_SYSTEM,    "disk ssd hdd drive" },
{ "car adapter mode",    TAG_ADVANCED|TAG_BATTERY|TAG_SYSTEM,    "car adapter" },
{ "delay before resume", TAG_ADVANCED|TAG_BATTERY|TAG_SYSTEM,    "car adapter resume" },

/* --- system -------------------------------------------------------------- */
{ "start in screen",     TAG_SYSTEM|TAG_APPEARANCE,              "start screen boot" },
{ "show shutdown message", TAG_ADVANCED|TAG_SYSTEM,                           "shutdown splash" },
{ "clear settings on hold",TAG_ADVANCED|TAG_SYSTEM,              "reset recovery" },
{ "show debug menu",     TAG_ADVANCED|TAG_SYSTEM,                "debug" },
{ "settings mode",       TAG_SYSTEM,                             "basic advanced" },
{ "time format",         TAG_SYSTEM,                             "clock 12 24 hour" },
{ "lang",                TAG_SYSTEM,                             "language" },
{ "default codepage",    TAG_ADVANCED|TAG_SYSTEM,                "codepage character set encoding" },
{ "glyphs",              TAG_ADVANCED|TAG_SYSTEM|TAG_APPEARANCE, "glyph cache font limit" },
{ "default browser",     TAG_LIBRARY|TAG_APPEARANCE,             "browser default" },
{ "wps select action",   TAG_ADVANCED|TAG_APPEARANCE,            "select action" },
{ "shortcuts instead of quickscreen", TAG_SYSTEM|TAG_APPEARANCE, "quickscreen shortcuts" },
{ "keyclick",            TAG_ADVANCED|TAG_SYSTEM,                "keyclick click" },
{ "hardware keyclick",   TAG_ADVANCED|TAG_SYSTEM,                "keyclick click speaker" },
{ "keyclick repeats",    TAG_ADVANCED|TAG_SYSTEM,                "keyclick click" },
{ "serial bitrate",      TAG_ADVANCED|TAG_SYSTEM,                "accessory serial dock" },
{ "accessory power supply", TAG_ADVANCED|TAG_SYSTEM,             "accessory dock power" },
{ "lineout",             TAG_ADVANCED|TAG_SYSTEM,                "line out dock" },
{ "usb mode",            TAG_USB|TAG_SYSTEM,                     "usb mass storage" },
{ "usb hid",             TAG_ADVANCED|TAG_USB|TAG_SYSTEM,        "usb hid keyboard remote" },
{ "usb keypad mode",     TAG_ADVANCED|TAG_USB|TAG_SYSTEM,        "usb hid keypad" },
{ "usb-dac",             TAG_ADVANCED|TAG_USB|TAG_SYSTEM|TAG_SOUND, "usb dac audio" },

/* --- voice --------------------------------------------------------------- */
{ "talk menu",           TAG_VOICE,                              "voice speak menus" },
{ "talk dir",            TAG_ADVANCED|TAG_VOICE,                 "voice directories" },
{ "talk file",           TAG_ADVANCED|TAG_VOICE,                 "voice filenames" },
{ "talk dir clip",       TAG_ADVANCED|TAG_VOICE,                 "voice clips" },
{ "talk file clip",      TAG_ADVANCED|TAG_VOICE,                 "voice clips" },
{ "talk filetype",       TAG_ADVANCED|TAG_VOICE,                 "voice file type" },
{ "talk mixer level",    TAG_ADVANCED|TAG_VOICE,                 "voice volume" },
};

/* ---- resolving a setting to its row ------------------------------------- */

/* Each row's setting, resolved once on first use. Resolving costs one pass
 * over settings[] per row, so it is done lazily rather than at boot: a session
 * that never searches and never opens a menu with a hidden row never pays for
 * it.
 *
 * A row whose key names no setting resolves to NULL and then matches nothing,
 * which is why a typo is inert rather than dangerous -- and why
 * settings_tags_validate() has to exist to find one.
 *
 * Resolving to NULL is not always a fault: settings compiled in per target
 * (the backlight fades and the USB DAC are iPod Video only) leave their row
 * unresolved on the other build, and that is the row doing its job. */
static const struct settings_list *row_setting[ARRAYLEN(tag_rows)];
static bool resolved;

static void resolve_rows(void)
{
    if (resolved)
        return;
    resolved = true;

    for (unsigned i = 0; i < ARRAYLEN(tag_rows); i++)
        row_setting[i] = find_setting_by_cfgname(tag_rows[i].cfg_name);
}

/* Compared by pointer, not by name: settings[] is a fixed array, so the
 * pointer identifies the setting and the scan costs no string work. */
static const struct tag_row *row_for(const struct settings_list *setting)
{
    if (!setting)
        return NULL;

    resolve_rows();

    for (unsigned i = 0; i < ARRAYLEN(tag_rows); i++)
        if (row_setting[i] == setting)
            return &tag_rows[i];

    return NULL;
}

/* ---- the interface ------------------------------------------------------ */

uint16_t settings_tags_get(const struct settings_list *setting)
{
    const struct tag_row *row = row_for(setting);
    return row ? row->tags : 0;
}

const char *settings_tag_name(uint16_t tag)
{
    for (unsigned i = 0; i < ARRAYLEN(topic_names); i++)
        if (topic_names[i].tag == tag)
            return topic_names[i].name;
    return NULL;
}

/* What separates one word from the next. Bytes above 0x7f count as separators,
 * so an accented word still starts a word; over setting names that is close
 * enough and costs no locale machinery. */
static bool is_word_char(char c)
{
    return (c >= '0' && c <= '9')
        || (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z');
}

/* True if `query` starts a word in `text`.
 *
 * Deliberately not a plain substring search. "base" occurs inside "database",
 * so a substring match on the misspelling alias for Bass also returns Sort
 * Albums By (cfg name "database sort albums by"), Load to RAM (search word
 * "database"), and -- worst -- every setting tagged TAG_DATABASE, because the
 * topic name contains it too. Anchoring to a word start removes all three at
 * once.
 *
 * The cost is that a query has to start a word: "backlight" and "back" find
 * Backlight, "light" no longer does. That is what the search words column is
 * for -- a mid-word term someone would really type is added there as a word of
 * its own. */
static bool word_prefix_match(const char *text, const char *query)
{
    size_t qlen;
    bool word_start = true;

    if (!text)
        return false;

    qlen = strlen(query);

    for (const char *p = text; *p; p++)
    {
        if (word_start && strncasecmp(p, query, qlen) == 0)
            return true;
        word_start = !is_word_char(*p);
    }

    return false;
}

bool settings_tags_match(const struct settings_list *setting,
                         const char *query)
{
    const struct tag_row *row;

    if (!setting || !query || !*query)
        return false;

    /* The two the setting carries itself. cfg_name is worth matching on its
     * own: it is what a .cfg file and every piece of documentation calls the
     * setting, and it often spells out what the menu name abbreviates. */
    if (setting->lang_id != -1
        && word_prefix_match(str(setting->lang_id), query))
        return true;
    if (word_prefix_match(setting->cfg_name, query))
        return true;

    row = row_for(setting);
    if (!row)
        return false;

    if (word_prefix_match(row->words, query))
        return true;

    /* Topic names last: they are the broadest match, so anything more specific
     * has already been tried. */
    for (unsigned i = 0; i < ARRAYLEN(topic_names); i++)
        if ((row->tags & topic_names[i].tag)
            && word_prefix_match(topic_names[i].name, query))
            return true;

    return false;
}

int settings_tags_validate(void)
{
    int bad = 0;

    resolve_rows();

    for (unsigned i = 0; i < ARRAYLEN(tag_rows); i++)
    {
        if (!row_setting[i])
        {
            logf("settings_tags: no such setting '%s'", tag_rows[i].cfg_name);
            bad++;
        }
    }

    return bad;
}
