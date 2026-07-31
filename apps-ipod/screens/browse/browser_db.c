/***************************************************************************
 * Original code from RockBox
 * was: apps/tagtree.c
 * Copyright (C) 2005 by Miika Pekkarinen
 * GNU General Public License (version 2+)
 *
 * Turns database queries into a browsable tree, driven by tagnavi.config.
 * The database equivalent of the filesystem browser's backend.
 * The largest file in screens/.
 *
 * There is no fixed menu structure here. tagnavi.config describes the tree --
 * which tag each level browses, how entries are formatted, what conditions
 * filter them -- and roughly the first third of this file is the parser for
 * that config. The rest answers the browser's requests by running a tagcache
 * search for the current level and formatting the results.
 *
 * Two things to know before editing:
 *   - Parsed config (menus, formats, search clauses) lives in one buflib
 *     allocation, sub-allocated by browser_db_alloc() bumping a pointer.
 *     Nothing is individually freed; the whole block is dropped and reparsed.
 *     move_callback() fixes up the internal pointers when buflib moves it.
 *   - The parser functions read from a shared cursor over the config text,
 *     not from arguments, so their order of call matters.
 *
 * Parts, in order:
 *   - types: tag entries, the table/variable enums, formats and menu roots
 *   - the buflib block and its bump allocator
 *   - tagnavi.config parser: tokens, tags, clauses, formats, conditions
 *   - buffering and track-finish event handlers
 *   - config import/export, and building the generated first-letter menu
 *   - init, reload and unload
 *   - retrieving and formatting entries for the current level
 *   - "enter by tag" entry points used to jump straight to an album/artist
 *   - the browser_db_* API the browser calls: load, enter, exit, queries
 *   - playlist insertion, including the shuffled all-subentries path
 ****************************************************************************/

/**
 * Basic structure on this file was copied from dbtree.c and modified to
 * support the tag cache interface.
 */

//#define LOGF_ENABLE

#include <stdio.h>
#include <stdlib.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "rbpaths.h"
#include "widgets/splash.h"
#include "draw/icon_bitmaps.h"
#include "browser.h"
#include "root_menu.h"   /* GO_TO_* codes the synthetic rows return */
#include "album_charts.h"
#include "input/action.h"
#include "settings/settings.h"
#include "database/tagcache.h"
#include "browser_db.h"
#include "lang.h"
#include "logf.h"
#include "speech/talk.h"
#include "playlist/playlist.h"
#include "widgets/keyboard.h"
#include "widgets/list.h"
#include "core_alloc.h"
#include "widgets/yesno.h"
#include "system/activity.h"
#include "system/app_util.h"
#include "system/strutil.h"
#include "files/filetypes.h"
#include "audio.h"
#include "system/appevents.h"
#include "storage.h"
#include "dir.h"
#include "audio/playback.h"
#include "strnatcmp.h"
#include "panic.h"
#include "screens/context_menu.h"
#include "system/app_buffer.h"
#include "speech/language.h"
#include "playlist/catalog.h"
#include "crc32.h"
#include <ctype.h>

#define str_or_empty(x) (x ? x : "(NULL)")

#define TAGNAVI_DEFAULT_CONFIG  ROCKBOX_DIR "/tagnavi.config"
#define TAGNAVI_USER_CONFIG     ROCKBOX_DIR "/tagnavi_user.config"

static int browser_db_play_folder(struct browser_context* c);

/* reuse of browser_db data after browser_db_play_folder() */
static uint32_t loaded_entries_crc = 0;


/* this needs to be same size as struct entry (tree.h) and name needs to be
 * the first; so that they're compatible enough to walk arrays of both
 * derefencing the name member*/
struct tagentry {
    char* name;
    int newtable;
    int extraseek;
    int customaction;
    char* album_name;
    int idx_id; /* master index id (tagcache_search.idx_id) -- distinct from
                 * extraseek: for a grouping row (e.g. an album listing),
                 * extraseek holds that tag's own result_seek (used for
                 * filtering a nested search), while idx_id is the value
                 * tagcache_retrieve() actually needs to look up a *different*
                 * tag (filename, albumartist) for this row's representative
                 * track. Only extraseek happens to equal idx_id already for
                 * track-level (tag_title/tag_filename) rows. */
};

static struct tagentry* browser_db_get_entry(struct browser_context *c, int id);

#define SEARCHSTR_SIZE 256

enum table {
    TABLE_ROOT = 1,
    TABLE_NAVIBROWSE,
    TABLE_ALLSUBENTRIES,
    TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS,
    TABLE_PLAYTRACK,
    /* Not tables at all: the markers on the synthetic rows load_root() adds,
     * meaning "leave the browser and do this instead". The chart row carries
     * which chart in its extraseek. Append only -- these values are stored in
     * tagentry.newtable and compared back, so renumbering changes what an
     * already-drawn row means. */
    TABLE_ALBUM_CHARTS,
    TABLE_RANDOM_ALBUM,
};

static const struct id3_to_search_mapping {
    char   *string;
    size_t id3_offset;
} id3_to_search_mapping[] = {
    { "",  0 },              /* offset n/a */
    { "#directory#",  0 },   /* offset n/a */
    { "#title#",  offsetof(struct mp3entry, title) },
    { "#artist#", offsetof(struct mp3entry, artist) },
    { "#album#",  offsetof(struct mp3entry, album) },
    { "#genre#",  offsetof(struct mp3entry, genre_string) },
    { "#composer#",  offsetof(struct mp3entry, composer) },
    { "#albumartist#",   offsetof(struct mp3entry, albumartist) },
};
enum variables {
    var_sorttype = 100,
    var_limit,
    var_strip,
    var_menu_start,
    var_include,
    var_rootmenu,
    var_format,
    menu_byfirstletter,
    menu_next,
    menu_load,
    menu_reload,
    menu_shuffle_songs,
};

/* Capacity 10 000 entries (for example 10k different artists) */
#define UNIQBUF_SIZE (64*1024)
static uint32_t uniqbuf[UNIQBUF_SIZE / sizeof(uint32_t)];

#define MAX_TAGS 5
#define MAX_MENU_ID_SIZE 32

#define RELOAD_BROWSER_DB (-1024)

static int(*qsort_fn)(const char*, const char*, size_t);
/* dummmy functions to allow compatibility strncasecmp */
static int strnatcasecmp_n(const char *a, const char *b, size_t n)
{
    (void)n;
     return strnatcasecmp(a, b);
}
static int strnatcasecmp_n_inv(const char *a, const char *b, size_t n)
{
    (void)n;
     return strnatcasecmp(b, a);
}
static int strncasecmp_inv(const char *a, const char *b, size_t n)
{
    return strncasecmp(b, a, n);
}

/*
 * "%3d. %s" autoscore title %sort = "inverse" %limit = "100"
 *
 * valid = true
 * formatstr = "%-3d. %s"
 * tags[0] = tag_autoscore
 * tags[1] = tag_title
 * tag_count = 2
 *
 * limit = 100
 * sort_inverse = true
 */
struct display_format {
    char name[32];
    struct tagcache_search_clause *clause[TAGCACHE_MAX_CLAUSES];
    int clause_count;
    char *formatstr;
    int group_id;
    int tags[MAX_TAGS];
    int tag_count;

    int limit;
    int strip;
    bool sort_inverse;
};

static struct display_format *formats[TAGMENU_MAX_FMTS];
static int format_count;

#define MENUENTRY_MAX_NAME 64
struct menu_entry {
    char _name[MENUENTRY_MAX_NAME];
    const unsigned char *name;
    int type;
    struct search_instruction {
        char name[MENUENTRY_MAX_NAME];
        int tagorder[MAX_TAGS];
        int tagorder_count;
        struct tagcache_search_clause *clause[MAX_TAGS][TAGCACHE_MAX_CLAUSES];
        int format_id[MAX_TAGS];
        int clause_count[MAX_TAGS];
        int result_seek[MAX_TAGS];
    } si;
    int link;
};

struct menu_root {
    char _title[MENUENTRY_MAX_NAME];
    const unsigned char *title;
    char id[MAX_MENU_ID_SIZE];
    int itemcount;
    struct menu_entry *items[TAGMENU_MAX_ITEMS];
};

/* Statusbar text of the current view. */
static char current_title[MAX_TAGS][128];

static struct menu_root * menus[TAGMENU_MAX_MENUS];
static struct menu_root * menu;
static struct search_instruction *csi;
static const char *strp;
static int menu_count;
static int rootmenu;

static int current_offset;
static int current_entry_count;

static struct browser_context *tc;

static int max_history_level; /* depth of menu levels with applicable history */
static int selected_item_history[MAX_DIR_LEVELS];
static int table_history[MAX_DIR_LEVELS];
static int extra_history[MAX_DIR_LEVELS];

/* Set when a level is entered fresh (no restored position), and resolved at the
 * end of browser_db_load() once the special-row count for that level is known.
 * pending_top_item then carries the wanted top row out to the browser's list
 * sync; -1 means no preference. See browser_db_load(). */
static bool position_past_specials;
static int pending_top_item = -1;

/* a few memory alloc helper */
static int browser_db_handle;
static size_t browser_db_bufsize, browser_db_buf_used;

#define UPDATE(x, y) { x = (typeof(x))((char*)(x) + (y)); }
static int move_callback(int handle, void* current, void* new)
{
    (void)handle; (void)current; (void)new;
    ptrdiff_t diff = new - current;

    if (menu)
    {
        if ((char *) menu->title == (char *) menu->_title)
            UPDATE(menu->title, diff);
        UPDATE(menu, diff);
    }

    if (csi)
        UPDATE(csi, diff);

    /* loop over menus */
    for(int i = 0; i < menu_count; i++)
    {
        struct menu_root* menuroot = menus[i];
        /* then over the menu_entries of a menu */
        for(int j = 0; j < menuroot->itemcount; j++)
        {
            struct menu_entry* mentry = menuroot->items[j];
            /* then over the search_instructions of each menu_entry */
            for(int k = 0; k < mentry->si.tagorder_count; k++)
            {
                for(int l = 0; l < mentry->si.clause_count[k]; l++)
                {
                    if(mentry->si.clause[k][l]->str)
                        UPDATE(mentry->si.clause[k][l]->str, diff);
                    UPDATE(mentry->si.clause[k][l], diff);
                }
            }
            if ((char *) menuroot->items[j]->name == (char *) menuroot->items[j]->_name)
                UPDATE(menuroot->items[j]->name, diff);
            UPDATE(menuroot->items[j], diff);
        }
        if ((char *) menus[i]->title == (char *) menus[i]->_title)
            UPDATE(menus[i]->title, diff);
        UPDATE(menus[i], diff);
    }

    /* now the same game for formats */
    for(int i = 0; i < format_count; i++)
    {
        for(int j = 0; j < formats[i]->clause_count; j++)
        {
            UPDATE(formats[i]->clause[j]->str, diff);
            UPDATE(formats[i]->clause[j], diff);
        }

        if (formats[i]->formatstr)
            UPDATE(formats[i]->formatstr, diff);

        UPDATE(formats[i], diff);
    }
    return BUFLIB_CB_OK;
}
#undef UPDATE

static struct buflib_callbacks ops = {
    .move_callback = move_callback,
    .shrink_callback = NULL,
};

static uint32_t browser_db_data_crc(struct browser_context* c)
{
    char* buf;
    uint32_t crc;
    buf = core_get_data(browser_db_handle); /* data for the search clauses etc */
    crc = crc_32(buf, browser_db_buf_used, c->dirlength);
    buf = core_get_data(c->cache.name_buffer_handle); /* names */
    crc = crc_32(buf, c->cache.name_buffer_size, crc);
    buf = core_get_data(c->cache.entries_handle); /* tagentries */
    crc = crc_32(buf, c->cache.max_entries * sizeof(struct tagentry), crc);
    logf("%s 0x%x", __func__, crc);
    return crc;
}

/* Bump allocator over the single buflib block: hands out the next `size`
 * bytes and never frees. Everything it returns is released at once by
 * browser_db_unload() dropping the whole handle, so the returned pointers
 * must not outlive a reload -- and, being raw pointers into a movable
 * allocation, must not be held across a yield. */
static void* browser_db_alloc(size_t size)
{
    size = ALIGN_UP(size, sizeof(void*));
    if (size > (browser_db_bufsize - browser_db_buf_used))
        return NULL;

    char* buf = core_get_data(browser_db_handle) + browser_db_buf_used;

    browser_db_buf_used += size;
    return buf;
}

static void* browser_db_alloc0(size_t size)
{
    void* ret = browser_db_alloc(size);
    if (ret)
        memset(ret, 0, size);
    return ret;
}

static char* browser_db_strdup(const char* buf)
{
    size_t len = strlen(buf) + 1;
    char* dest = browser_db_alloc(len);
    if (dest)
        strcpy(dest, buf);
    return dest;
}

/* save to call without locking */
static int get_token_str(char *buf, int size)
{
    /* Find the start. */
    while (*strp != '"' && *strp != '\0')
        strp++;

    if (*strp == '\0' || *(++strp) == '\0')
        return -1;

    /* Read the data. */
    while (*strp != '"' && *strp != '\0' && --size > 0)
        *(buf++) = *(strp++);

    *buf = '\0';
    if (*strp != '"')
        return -2;

    strp++;

    return 0;
}

static int get_tag(int *tag)
{
    /* case insensitive matching ahead - these should all be lower case */
#define TAG_TABLE \
        TAG_MATCH("lm", tag_virt_length_min) \
        TAG_MATCH("ls", tag_virt_length_sec) \
        TAG_MATCH("pm", tag_virt_playtime_min) \
        TAG_MATCH("ps", tag_virt_playtime_sec) \
        TAG_MATCH("->", menu_next) \
        TAG_MATCH("~>", menu_shuffle_songs) \
        TAG_MATCH("==>", menu_load) \
        TAG_MATCH("year", tag_year) \
        TAG_MATCH("album", tag_album) \
        TAG_MATCH("genre", tag_genre) \
        TAG_MATCH("title", tag_title) \
        TAG_MATCH("%sort", var_sorttype) \
        TAG_MATCH("artist", tag_artist) \
        TAG_MATCH("length", tag_length) \
        TAG_MATCH("rating", tag_rating) \
        TAG_MATCH("%limit", var_limit) \
        TAG_MATCH("%strip", var_strip) \
        TAG_MATCH("bitrate", tag_bitrate) \
        TAG_MATCH("comment", tag_comment) \
        TAG_MATCH("discnum", tag_discnumber) \
        TAG_MATCH("%format", var_format) \
        TAG_MATCH("%reload", menu_reload) \
        TAG_MATCH("filename", tag_filename) \
        TAG_MATCH("basename", tag_virt_basename) \
        TAG_MATCH("tracknum", tag_tracknumber) \
        TAG_MATCH("composer", tag_composer) \
        TAG_MATCH("ensemble", tag_albumartist) \
        TAG_MATCH("grouping", tag_grouping) \
        TAG_MATCH("entryage", tag_virt_entryage) \
        TAG_MATCH("commitid", tag_commitid) \
        TAG_MATCH("%include", var_include) \
        TAG_MATCH("playcount", tag_playcount) \
        TAG_MATCH("autoscore", tag_virt_autoscore) \
        TAG_MATCH("lastplayed", tag_lastplayed) \
        TAG_MATCH("lastoffset", tag_lastoffset) \
        TAG_MATCH("%root_menu", var_rootmenu) \
        TAG_MATCH("albumartist", tag_albumartist) \
        TAG_MATCH("lastelapsed", tag_lastelapsed) \
        TAG_MATCH("%menu_start", var_menu_start) \
        TAG_MATCH("%byfirstletter", menu_byfirstletter) \
        TAG_MATCH("canonicalartist", tag_virt_canonicalartist) \
    /* END OF TAG_TABLE MACRO */

    /* build two separate arrays tag strings and symbol map*/
    #define TAG_MATCH(str, tag) str,
    static const char * const get_tag_match[] =
    {
        TAG_TABLE
    };
    #undef TAG_MATCH
    #define TAG_MATCH(str, tag) tag,
    static const uint8_t get_tag_symbol[]=
    {
        TAG_TABLE
    };
    #undef TAG_MATCH

    const char *tagstr;
    ptrdiff_t tagstr_len;

    /* Find the start. */
    while (*strp == ' ' || *strp == '>')
        strp++;

    if (*strp == '\0' || *strp == '?')
        return 0;

    tagstr = strp;
    while(*strp != '\0' && *strp != ' ') /* walk to the end of the tag */
    {
        strp++;
    }
    tagstr_len = strp - tagstr;

    char first = tolower(*tagstr++); /* get the first letter elide cmp fn call */

    for (size_t i = 0; i < ARRAYLEN(get_tag_match); i++)
    {
        const char *match = get_tag_match[i];

        if (first == match[0] && strncasecmp(tagstr, match + 1, tagstr_len - 1) == 0)
        {
            /* check for full match */
            if (match[tagstr_len] == '\0')
            {
                *tag = get_tag_symbol[i];
                return 1;
            }
        }
    }
    logf("NO MATCH: %.*s\n", (int)tagstr_len, tagstr);

    return -1;
}

static int get_clause(int *condition)
{
    /* one or two operator conditionals */
    #define OPS2VAL(op1, op2) ((uint16_t)op1 << 8 | (uint16_t)op2)
    #define CLAUSE(op1, op2, symbol) {OPS2VAL(op1, op2), symbol }

    struct clause_symbol {uint16_t value;uint16_t symbol;};
    const struct clause_symbol *match;
    static const struct clause_symbol get_clause_match[] =
    {
        CLAUSE('=', ' ', clause_is),
        CLAUSE('=', '=', clause_is),
        CLAUSE('!', '=', clause_is_not),
        CLAUSE('>', ' ', clause_gt),
        CLAUSE('>', '=', clause_gteq),
        CLAUSE('<', ' ', clause_lt),
        CLAUSE('<', '=', clause_lteq),
        CLAUSE('~', ' ', clause_contains),
        CLAUSE('!', '~', clause_not_contains),
        CLAUSE('^', ' ', clause_begins_with),
        CLAUSE('!', '^', clause_not_begins_with),
        CLAUSE('$', ' ', clause_ends_with),
        CLAUSE('!', '$', clause_not_ends_with),
        CLAUSE('@', '~', clause_contains_oneof),
        CLAUSE('@', '^', clause_begins_oneof),
        CLAUSE('@', '$', clause_ends_oneof),
        CLAUSE('@', ' ', clause_oneof),
        CLAUSE('*', '~', clause_not_contains_oneof),
        CLAUSE('*', '^', clause_not_begins_oneof),
        CLAUSE('*', '$', clause_not_ends_oneof),
        CLAUSE('!', '@', clause_not_oneof),
        CLAUSE(0, 0, 0) /* sentinel */
    };

    /* Find the start. */
    while (*strp == ' ' && *strp != '\0')
        strp++;

    if (*strp == '\0')
        return 0;

    char op1 = strp[0];
    char op2 = strp[1];
    if (op2 == '"') /*allow " to end a single op conditional */
        op2 = ' ';

    uint16_t value = OPS2VAL(op1, op2);

    for (match = get_clause_match; match->value != 0; match++)
    {
        if (value == match->value)
        {
            *condition = match->symbol;
            return 1;
        }
    }

    return 0;
#undef OPS2VAL
#undef CLAUSE
}

static bool read_clause(struct tagcache_search_clause *clause)
{
    char buf[SEARCHSTR_SIZE];
    unsigned int i;

    if (get_tag(&clause->tag) <= 0)
        return false;

    if (get_clause(&clause->type) <= 0)
        return false;

    if (get_token_str(buf, sizeof buf) < 0)
        return false;

    for (i=0; i<ARRAYLEN(id3_to_search_mapping); i++)
    {
        if (!strcasecmp(buf, id3_to_search_mapping[i].string))
            break;
    }

    if (i<ARRAYLEN(id3_to_search_mapping)) /* runtime search operand found */
    {
        clause->source = source_runtime+i;
        clause->str = browser_db_alloc(SEARCHSTR_SIZE);
    }
    else
    {
        clause->source = source_constant;
        clause->str = browser_db_strdup(buf);
    }

    if (!clause->str)
    {
        logf("browser_db failed to allocate %s", "clause string");
        return false;
    }
    else if (TAGCACHE_IS_NUMERIC(clause->tag))
    {
        clause->numeric = true;
        clause->numeric_data = atoi(clause->str);
    }
    else
        clause->numeric = false;

    logf("got clause: %d/%d [%s]", clause->tag, clause->type, clause->str);

    return true;
}

static bool read_variable(char *buf, int size)
{
    int condition;

    if (!get_clause(&condition))
        return false;

    if (condition != clause_is)
        return false;

    if (get_token_str(buf, size) < 0)
        return false;

    return true;
}

/* "%3d. %s" autoscore title %sort = "inverse" %limit = "100" */
static int get_format_str(struct display_format *fmt)
{
    int ret;
    char buf[128];
    int i;

    memset(fmt, 0, sizeof(struct display_format));

    if (get_token_str(fmt->name, sizeof fmt->name) < 0)
        return -10;

    /* Determine the group id */
    fmt->group_id = 0;
    for (i = 0; i < format_count; i++)
    {
        if (!strcasecmp(formats[i]->name, fmt->name))
        {
            fmt->group_id = formats[i]->group_id;
            break;
        }

        if (formats[i]->group_id > fmt->group_id)
            fmt->group_id = formats[i]->group_id;
    }

    if (i == format_count)
        fmt->group_id++;

    logf("format: (%d) %s", fmt->group_id, fmt->name);

    if (get_token_str(buf, sizeof buf) < 0)
        return -10;

    fmt->formatstr = browser_db_strdup(buf);

    while (fmt->tag_count < MAX_TAGS)
    {
        ret = get_tag(&fmt->tags[fmt->tag_count]);
        if (ret < 0)
            return -11;

        if (ret == 0)
            break;

        switch (fmt->tags[fmt->tag_count]) {
        case var_sorttype:
            if (!read_variable(buf, sizeof buf))
                return -12;
            if (!strcasecmp("inverse", buf))
                fmt->sort_inverse = true;
            break;

        case var_limit:
            if (!read_variable(buf, sizeof buf))
                return -13;
            fmt->limit = atoi(buf);
            break;

        case var_strip:
            if (!read_variable(buf, sizeof buf))
                return -14;
            fmt->strip = atoi(buf);
            break;

        default:
            fmt->tag_count++;
        }
    }

    return 1;
}

static int add_format(const char *buf)
{
    if (format_count >= TAGMENU_MAX_FMTS)
    {
        logf("too many formats");
        return -1;
    }

    strp = buf;

    if (formats[format_count] == NULL)
        formats[format_count] = browser_db_alloc0(sizeof(struct display_format));
    if (!formats[format_count])
    {
        logf("browser_db failed to allocate %s", "format string");
        return -2;
    }
    if (get_format_str(formats[format_count]) < 0)
    {
        logf("get_format_str() parser failed!");
        if (formats[format_count])
            memset(formats[format_count], 0, sizeof(struct display_format));
        return -4;
    }

    while (*strp != '\0' && *strp != '?')
        strp++;

    if (*strp == '?')
    {
        int clause_count = 0;
        strp++;

        core_pin(browser_db_handle);
        while (1)
        {
            struct tagcache_search_clause *new_clause;

            if (clause_count >= TAGCACHE_MAX_CLAUSES)
            {
                logf("too many clauses");
                break;
            }

            new_clause = browser_db_alloc(sizeof(struct tagcache_search_clause));
            if (!new_clause)
            {
                logf("browser_db failed to allocate %s", "search clause");
                return -3;
            }
            formats[format_count]->clause[clause_count] = new_clause;
            if (!read_clause(new_clause))
                break;

            clause_count++;
        }
        core_unpin(browser_db_handle);

        formats[format_count]->clause_count = clause_count;
    }

    format_count++;

    return 1;
}

static int get_condition(struct search_instruction *inst)
{
    struct tagcache_search_clause *new_clause;
    int clause_count;
    char buf[128];

    switch (*strp)
    {
        case '=':
        {
            int i;

            if (get_token_str(buf, sizeof buf) < 0)
                return -1;

            for (i = 0; i < format_count; i++)
            {
                if (!strcasecmp(formats[i]->name, buf))
                    break;
            }

            if (i == format_count)
            {
                logf("format not found: %s", buf);
                return -2;
            }

            inst->format_id[inst->tagorder_count] = formats[i]->group_id;
            return 1;
        }
        case '?':
        case ' ':
        case '&':
            strp++;
            return 1;
        case '-':
        case '\0':
            return 0;
    }

    clause_count = inst->clause_count[inst->tagorder_count];
    if (clause_count >= TAGCACHE_MAX_CLAUSES)
    {
        logf("Too many clauses");
        return -2;
    }

    new_clause = browser_db_alloc0(sizeof(struct tagcache_search_clause));
    if (!new_clause)
    {
        logf("browser_db failed to allocate %s", "search clause");
        return -3;
    }

    inst->clause[inst->tagorder_count][clause_count] = new_clause;

    if (*strp == '|')
    {
        strp++;
        new_clause->type = clause_logical_or;
    }
    else
    {
        core_pin(browser_db_handle);
        bool ret = read_clause(new_clause);
        core_unpin(browser_db_handle);
        if (!ret)
            return -1;
    }
    inst->clause_count[inst->tagorder_count]++;

    return 1;
}

/* example search:
 * "Best" artist ? year >= "2000" & title !^ "crap" & genre = "good genre" \
 *      : album  ? year >= "2000" : songs
 * ^  begins with
 * *  contains
 * $  ends with
 */

static bool parse_search(struct menu_entry *entry, const char *str)
{
    int ret;
    int type;
    struct search_instruction *inst = &entry->si;
    char buf[MAX_PATH];
    int i;

    strp = str;

    /* Parse entry name */
    if (get_token_str(entry->_name, sizeof entry->_name) < 0)
    {
        logf("No name found.");
        return false;
    }

    /* Attempt to entry name to lang_id for voicing/translation
    (excepted for single character entries like those in the 'First Letter' menus)
    */
    if (entry->_name[0] != '\0' && entry->_name[1] != '\0')
    {
        int lang_id = lang_english_to_id(entry->_name);
        if (lang_id >= 0)
            entry->name = ID2P(lang_id);
        else
            entry->name = entry->_name;
    }
    else
        entry->name = entry->_name;

    /* Parse entry type */
    if (get_tag(&entry->type) <= 0)
        return false;

    if (entry->type == menu_load)
    {
        if (get_token_str(buf, sizeof buf) < 0)
            return false;

        /* Find the matching root menu or "create" it */
        for (i = 0; i < menu_count; i++)
        {
            if (!strcasecmp(menus[i]->id, buf))
            {
                entry->link = i;
                return true;
            }
        }

        if (menu_count >= TAGMENU_MAX_MENUS)
        {
            logf("max menucount reached");
            return false;
        }

        /* Allocate a new menu unless link is found. */
        menus[menu_count] = browser_db_alloc0(sizeof(struct menu_root));
        if (!menus[menu_count])
        {
            logf("browser_db failed to allocate %s", "menu");
            return false;
        }
        strmemccpy(menus[menu_count]->id, buf, MAX_MENU_ID_SIZE);
        entry->link = menu_count;
        ++menu_count;

        return true;
    }

    if (entry->type != menu_next && entry->type != menu_shuffle_songs)
        return false;

    while (inst->tagorder_count < MAX_TAGS)
    {
        ret = get_tag(&inst->tagorder[inst->tagorder_count]);
        if (ret < 0)
        {
            logf("Parse error #1");
            logf("%s", strp);
            return false;
        }

        if (ret == 0)
            break ;

        logf("tag: %d", inst->tagorder[inst->tagorder_count]);

        core_pin(browser_db_handle);
        while ( (ret = get_condition(inst)) > 0 ) ;
        core_unpin(browser_db_handle);

        if (ret < 0)
            return false;

        inst->tagorder_count++;

        if (get_tag(&type) <= 0 || (type != menu_next && type != menu_shuffle_songs))
            break;
    }

    return true;
}

static int compare(const void *p1, const void *p2)
{
    struct tagentry *e1 = (struct tagentry *)p1;
    struct tagentry *e2 = (struct tagentry *)p2;
    return qsort_fn(e1->name, e2->name, MAX_PATH);
}

static int compare_with_albums(const void *p1, const void *p2)
{
    struct tagentry *e1 = (struct tagentry *)p1;
    struct tagentry *e2 = (struct tagentry *)p2;
    int sort_album_res = qsort_fn(
        e1->album_name == NULL ? "" : e1->album_name,
        e2->album_name == NULL ? "" : e2->album_name, MAX_PATH);
    if (sort_album_res != 0)
    {
        /* If album name is different */
        return sort_album_res;
    }
    return qsort_fn(e1->name, e2->name, MAX_PATH);
}

static void browser_db_buffer_event(unsigned short id, void *ev_data)
{
    (void)id;
    struct tagcache_search tcs;
    struct mp3entry *id3 = ((struct track_event *)ev_data)->id3;

    bool runtimedb = global_settings.runtimedb;
    bool autoresume = global_settings.autoresume_enable;

    /* Do not gather data unless proper setting has been enabled. */
    if (!runtimedb && !autoresume)
        return;

    logf("be:%s", id3->path);

    while (! tagcache_is_fully_initialized())
        yield();

    if (!tagcache_find_index(&tcs, id3->path))
    {
        logf("tc stat: not found: %s", id3->path);
        return;
    }

    if (runtimedb)
    {
        id3->playcount  = tagcache_get_numeric(&tcs, tag_playcount);
        if (!id3->rating)
            id3->rating = tagcache_get_numeric(&tcs, tag_rating);
        id3->lastplayed = tagcache_get_numeric(&tcs, tag_lastplayed);
        id3->score      = tagcache_get_numeric(&tcs, tag_virt_autoscore) / 10;
        id3->playtime   = tagcache_get_numeric(&tcs, tag_playtime);

        logf("-> %ld/%ld", id3->playcount, id3->playtime);
    }

    if (autoresume)
    {
        /* Load current file resume info if not already defined (by
           another resume mechanism) */
        if (id3->elapsed == 0)
        {
            id3->elapsed = tagcache_get_numeric(&tcs, tag_lastelapsed);

            logf("browser_db_buffer_event: Set elapsed for %s to %lX\n",
                 str_or_empty(id3->title), id3->elapsed);
        }

        if (id3->offset == 0)
        {
            id3->offset = tagcache_get_numeric(&tcs, tag_lastoffset);

            logf("browser_db_buffer_event: Set offset for %s to %lX\n",
                 str_or_empty(id3->title), id3->offset);
        }
    }

    /* Store our tagcache index pointer. */
    id3->tagcache_idx = tcs.idx_id+1;

    tagcache_search_finish(&tcs);
}

static void browser_db_track_finish_event(unsigned short id, void *ev_data)
{
    (void)id;
    struct track_event *te = (struct track_event *)ev_data;
    struct mp3entry *id3 = te->id3;

    long tagcache_idx = id3->tagcache_idx;
    if (!tagcache_idx)
    {
        logf("No tagcache index pointer found");
        return;
    }
    tagcache_idx--;

    bool auto_skip = te->flags & TEF_AUTO_SKIP;
    bool runtimedb = global_settings.runtimedb;
    bool autoresume = global_settings.autoresume_enable;

    /* Don't process unplayed tracks, or tracks interrupted within the
       first 15 seconds but always process autoresume point */
    if (runtimedb && (id3->elapsed == 0
        || (id3->elapsed < 15 * 1000 && !auto_skip)
        ))
    {
        logf("not db logging unplayed or skipped track");
        runtimedb = false;
    }

    /* 3s because that is the threshold the WPS uses to rewind instead
       of skip backwards */
    if (autoresume && (id3->elapsed == 0
        || (id3->elapsed < 3 * 1000 && !auto_skip)))
    {
        logf("not logging autoresume");
        autoresume = false;
    }

    /* Do not gather data unless proper setting has been enabled and at least
       one is still slated to be recorded */
    if (!(runtimedb || autoresume))
    {
        logf("runtimedb gathering and autoresume not enabled/ignored");
        return;
    }

    long lastplayed = tagcache_increase_serial();
    if (lastplayed < 0)
    {
        logf("incorrect tc serial:%ld", lastplayed);
        return;
    }

    if (runtimedb)
    {
        long playcount;
        long playtime;

        playcount = id3->playcount + 1;

        /* Ignore the last 15s (crossfade etc.) */
        playtime = id3->playtime + MIN(id3->length, id3->elapsed + 15 * 1000);

        logf("ube:%s", id3->path);
        logf("-> %ld/%ld", playcount, playtime);
        logf("-> %ld/%ld/%ld", id3->elapsed, id3->length,
             MIN(id3->length, id3->elapsed + 15 * 1000));

        /* Queue the updates to the tagcache system. */
        tagcache_update_numeric(tagcache_idx, tag_playcount, playcount);
        tagcache_update_numeric(tagcache_idx, tag_playtime, playtime);
        tagcache_update_numeric(tagcache_idx, tag_lastplayed, lastplayed);
    }

    if (autoresume)
    {
        unsigned long elapsed = auto_skip ? 0 : id3->elapsed;
        unsigned long offset = auto_skip ? 0 : id3->offset;
        tagcache_update_numeric(tagcache_idx, tag_lastelapsed, elapsed);
        tagcache_update_numeric(tagcache_idx, tag_lastoffset, offset);

        logf("browser_db_track_finish_event: Save resume for %s: %lX %lX",
             str_or_empty(id3->title), elapsed, offset);
    }
}

int browser_db_export(void)
{
    struct tagcache_search tcs;

    ui_set_working(true);
    if (!tagcache_create_changelog(&tcs))
    {
        ui_set_working(false);
        splash(HZ*2, ID2P(LANG_FAILED));
        return 0;
    }
    ui_set_working(false);

    return 0;
}

int browser_db_import(void)
{
    ui_set_working(true);
    bool ok = tagcache_import_changelog();
    ui_set_working(false);

    if (!ok)
        splash(HZ*2, ID2P(LANG_FAILED));

    return 0;
}

static bool alloc_menu_parse_buf(char *buf, int type)
{
    /* allocate a new menu item (if needed) initialize it with data parsed
       from buf Note: allows setting menu type, type ignored when < 0
    */
    /* Allocate */
    if (menu->items[menu->itemcount] == NULL)
        menu->items[menu->itemcount] = browser_db_alloc0(sizeof(struct menu_entry));
    if (!menu->items[menu->itemcount])
    {
        logf("browser_db failed to allocate %s", "menu items");
        return false;
    }

    /* Initialize */
    core_pin(browser_db_handle);
    if (parse_search(menu->items[menu->itemcount], buf))
    {
        if (type >= 0)
            menu->items[menu->itemcount]->type = type;
        menu->itemcount++;
    }
    core_unpin(browser_db_handle);
    return true;
}

static void build_firstletter_menu(char *buf, size_t bufsz)
{
    /* Copied out rather than pointed at: the format string built below is
     * written back into buf, so subitem must not alias it. */
    char subitem[32]; /* canonicalartist longest subitem we expect add a bit extra..*/
    strmemccpy(subitem, buf, sizeof(subitem));

    const char * const fmt ="\"%s\"-> %s ? %s %c\"%c\"-> %s =\"fmt_title\"";
    const char * const showsub = /* album subitem for canonicalartist */
        ((strcasestr(subitem, "artist") == NULL) ? "title" : "album -> title");

    /* Using >= "0" & <= "9" does not work as intended in all cases
    we need to use the previous & the next character in the ASCII table
    which are "/" and ":" to get the intended effect */
    const char * fmt_numeric ="\"%s\"-> %s ? %s > \"/\" & %s < \":\" -> %s =\"fmt_title\"";
    snprintf(buf, bufsz, fmt_numeric,
            str(LANG_DISPLAY_NUMERIC), subitem, subitem, subitem, showsub);

    if (!alloc_menu_parse_buf(buf, menu_byfirstletter))
    {
        return;
    }

    for (int i = 0; i < 26; i++)
    {
        snprintf(buf, bufsz, fmt, "#", subitem, subitem,'^', 'A' + i, showsub);
        buf[1] = 'A' + i; /* overwrite the placeholder # with the current letter */
        /* ex: "A" -> title ? title ^ "A" -> title = "fmt_title" */
        if (!alloc_menu_parse_buf(buf, menu_byfirstletter))
        {
            return;
        }
    }

    /* use lower character there because strncasecmp works will convert them anyway */
    const char * fmt_special ="\"%s\"-> %s ? %s *^ "\
    "\"a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q|r|s|t|u|v|w|x|y|z|0|1|2|3|4|5|6|7|8|9\" -> %s =\"fmt_title\"";
    snprintf(buf, bufsz, fmt_special,
        str(LANG_DISPLAY_SPECIAL_CHARACTER), subitem, subitem, showsub);

    if (!alloc_menu_parse_buf(buf, menu_byfirstletter))
    {
        return;
    }
}

static bool parse_menu(const char *filename);
static int parse_line(int n, char *buf, void *parameters)
{
    char data[256];
    int variable;
    static bool read_menu;
    int i;
    char *p;

    (void)parameters;

    /* Strip possible <CR> at end of line. */
    p = strchr(buf, '\r');
    if (p != NULL)
        *p = '\0';

    logf("parse:%d/%s", n, buf);

    /* First line, do initialisation. */
    if (n == 0)
    {
        if (strcasecmp(TAGNAVI_VERSION, buf))
        {
            logf("Version mismatch");
            return -1;
        }

        read_menu = false;
    }

    if (buf[0] == '#')
        return 0;

    if (buf[0] == '\0')
    {
        if (read_menu)
        {
            /* End the menu */
            read_menu = false;
        }
        return 0;
    }

    if (!read_menu)
    {
        strp = buf;
        if (get_tag(&variable) <= 0)
            return 0;

        switch (variable)
        {
            case var_format:
                if (add_format(strp) < 0)
                {
                    logf("Format add fail: %s", data);
                }
                break;

            case var_include:
                if (get_token_str(data, sizeof(data)) < 0)
                {
                    logf("%%include empty");
                    return 0;
                }

                if (!parse_menu(data))
                {
                    logf("Load menu fail: %s", data);
                }
                break;
            case menu_byfirstletter: /* Fallthrough */
            case var_menu_start:
                if (menu_count >= TAGMENU_MAX_MENUS)
                {
                    logf("max menucount reached");
                    return 0;
                }

                if (get_token_str(data, sizeof data) < 0)
                {
                    logf("%%menu_start id empty");
                    return 0;
                }

                menu = NULL;
                for (i = 0; i < menu_count; i++)
                {
                    if (!strcasecmp(menus[i]->id, data))
                    {
                        menu = menus[i];
                    }
                }

                if (menu == NULL)
                {
                    menus[menu_count] = browser_db_alloc0(sizeof(struct menu_root));
                    if (!menus[menu_count])
                    {
                        logf("browser_db failed to allocate %s", "menu");
                        return -2;
                    }
                    menu = menus[menu_count];
                    ++menu_count;
                    strmemccpy(menu->id, data, MAX_MENU_ID_SIZE);
                }

                if (get_token_str(menu->_title, sizeof(menu->_title)) < 0)
                {
                    logf("%%menu_start title empty");
                    return 0;
                }

                /* Attempt to match title to lang_id for voicing/translation */
                int lang_id = lang_english_to_id(menu->_title);
                if (lang_id >= 0)
                    menu->title = ID2P(lang_id);
                else
                    menu->title = menu->_title;

                logf("menu: %s id: %ld", P2STR(menu->title), P2ID(menu->title));

                if (variable == menu_byfirstletter)
                {
                    if (get_token_str(data, sizeof(data)) < 0)
                    {
                        logf("%%firstletter_menu has no subitem"); /*artist,album*/
                        return 0;
                    }
                    logf("A-Z Menu subitem: %s", data);
                    read_menu = false;
                    build_firstletter_menu(data, sizeof(data));
                    break;
                }
                read_menu = true;
                break;

            case var_rootmenu:
                /* Only set root menu once. */
                if (rootmenu >= 0)
                    break;

                if (get_token_str(data, sizeof(data)) < 0)
                {
                    logf("%%rootmenu empty");
                    return 0;
                }

                for (i = 0; i < menu_count; i++)
                {
                    if (!strcasecmp(menus[i]->id, data))
                    {
                        rootmenu = i;
                    }
                }
                break;
        }

        return 0;
    }

    if (menu->itemcount >= TAGMENU_MAX_ITEMS)
    {
        logf("max itemcount reached");
        return 0;
    }

    if (!alloc_menu_parse_buf(buf, -1))
    {
        return -2;
    }

    return 0;
}

static bool parse_menu(const char *filename)
{
    int fd;
    char buf[1024];
    int rc;

    if (menu_count >= TAGMENU_MAX_MENUS)
    {
        logf("max menucount reached");
        return false;
    }

    fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        logf("Search instruction file not found.");
        return false;
    }

    /* Now read file for real, parsing into si */
    rc = fast_readline(fd, buf, sizeof buf, NULL, parse_line);
    close(fd);

    return (rc >= 0);
}

static void browser_db_unload(struct browser_context *c)
{
    /* may be spurious... */
    core_pin(browser_db_handle);

    remove_event(PLAYBACK_EVENT_TRACK_BUFFER, browser_db_buffer_event);
    remove_event(PLAYBACK_EVENT_TRACK_FINISH, browser_db_track_finish_event);

    if (c)
    {
        browser_lock_cache(c);
        struct tagentry *dptr = core_get_data(c->cache.entries_handle);
        menu = menus[c->currextra];
        if (!menu)
        {
            logf("browser_db menu doesn't exist");
            return;
        }

        for (int i = 0; i < menu->itemcount; i++)
        {
            dptr->name = NULL;
            dptr->newtable = 0;
            dptr->extraseek = 0;
            dptr->customaction = ONPLAY_NO_CUSTOMACTION;
            dptr++;
        }
    }

    for (int i = 0; i < menu_count; i++)
        menus[i] = NULL;
    menu_count = 0;

    for (int i = 0; i < format_count; i++)
        formats[i] = NULL;
    format_count = 0;

    core_free(browser_db_handle);
    browser_db_handle   = 0;
    browser_db_buf_used = 0;
    browser_db_bufsize  = 0;

    if (c)
        browser_unlock_cache(c);
}

static bool initialize_browser_db(void) /* also used when user selects 'Reload' in 'custom menu'*/
{
    max_history_level = 0;
    format_count = 0;
    menu_count = 0;
    menu = NULL;
    rootmenu = -1;
    browser_db_handle = core_alloc_maximum(&browser_db_bufsize, &ops);
    if (browser_db_handle < 0)
        panicf("browser_db OOM");

    /* Use the user tagnavi config if present, otherwise use the default. */
    const char* tagnavi_file;
    if(file_exists(TAGNAVI_USER_CONFIG))
        tagnavi_file = TAGNAVI_USER_CONFIG;
    else
        tagnavi_file = TAGNAVI_DEFAULT_CONFIG;

    if (!parse_menu(tagnavi_file))
    {
        browser_db_unload(NULL);
        return false;
    }

    /* safety check since tree.c needs to cast tagentry to entry */
    if (sizeof(struct tagentry) != sizeof(struct entry))
        panicf("tagentry(%zu) and entry mismatch(%zu)",
                sizeof(struct tagentry), sizeof(struct entry));

    /* If no root menu is set, assume it's the first single menu
     * we have. That shouldn't normally happen. */
    if (rootmenu < 0)
        rootmenu = 0;

    add_event(PLAYBACK_EVENT_TRACK_BUFFER, browser_db_buffer_event);
    add_event(PLAYBACK_EVENT_TRACK_FINISH, browser_db_track_finish_event);

    core_shrink(browser_db_handle, NULL, browser_db_buf_used);
    return true;
}

void browser_db_init(void)
{
    initialize_browser_db();
}

static int format_str(struct tagcache_search *tcs, struct display_format *fmt,
                      char *buf, int buf_size)
{
    static char fmtbuf[20];
    bool read_format = false;
    unsigned fmtbuf_pos = 0;
    int parpos = 0;
    int buf_pos = 0;
    int i;

    /* memset(buf, 0, buf_size); probably uneeded */
    for (i = 0; fmt->formatstr[i] != '\0'; i++)
    {
        if (fmt->formatstr[i] == '%')
        {
            read_format = true;
            fmtbuf_pos = 0;
            if (parpos >= fmt->tag_count)
            {
                logf("too many format tags");
                return -1;
            }
        }

        char formatchar = fmt->formatstr[i];

        if (read_format)
        {
            fmtbuf[fmtbuf_pos++] = formatchar;
            if (fmtbuf_pos >= sizeof fmtbuf)
            {
                logf("format parse error");
                return -2;
            }

            if (formatchar == 's' || formatchar == 'd')
            {
                unsigned space_left = buf_size - buf_pos;
                char tmpbuf[MAX_PATH];
                char *result;

                fmtbuf[fmtbuf_pos] = '\0';
                read_format = false;

                switch (formatchar)
                {
                case 's':
                    if (fmt->tags[parpos] == tcs->type)
                    {
                        result = tcs->result;
                    }
                    else
                    {
                        /* Need to fetch the tag data. */
                        int tag = fmt->tags[parpos];

                        if (!tagcache_retrieve(tcs, tcs->idx_id,
                                               tag, tmpbuf, sizeof tmpbuf))
                        {
                            logf("retrieve failed");
                            return -3;
                        }

                        result = tmpbuf;
                    }
                    buf_pos +=
                        snprintf(&buf[buf_pos], space_left, fmtbuf, result);
                    break;

                case 'd':
                    buf_pos +=
                        snprintf(&buf[buf_pos], space_left, fmtbuf,
                                 tagcache_get_numeric(tcs, fmt->tags[parpos]));
                }

                parpos++;
            }
        }
        else
            buf[buf_pos++] = formatchar;

        if (buf_pos >= buf_size - 1)    /* need at least one more byte for \0 */
        {
            logf("buffer overflow");
            return -4;
        }
    }

    buf[buf_pos++] = '\0';

    return 0;
}

static struct tagentry* get_entries(struct browser_context *tc)
{
    return core_get_data(tc->cache.entries_handle);
}

static void tcs_get_basename(struct tagcache_search *tcs, bool is_basename)
{
    if (is_basename)
    {
        char* basename = strrchr(tcs->result, '/');
        if (basename != NULL)
        {
            tcs->result = basename + 1;
            tcs->result_len = strlen(tcs->result) + 1;
        }
    }
}

static int retrieve_entries(struct browser_context *c, int offset, bool init)
{
    logf( "%s", __func__);
    char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    struct tagcache_search tcs;
    struct display_format *fmt;
    int i;
    int namebufused = 0;
    int total_count = 0;
    c->special_entry_count = 0;
    int level = c->currextra;
    int tag;
    bool sort = false;
    bool sort_inverse;
    bool is_basename = false;
    int sort_limit;
    int strip;

    if (c->currtable == TABLE_ALLSUBENTRIES || c->currtable == TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS)
    {
        tag = tag_title;
        level--;
    }
    else
        tag = csi->tagorder[level];

    if (tag == menu_reload)
        return RELOAD_BROWSER_DB;

    if (tag == tag_virt_basename) /* basename shortcut */
    {
        is_basename = true;
        tag = tag_filename;
    }

    if (!tagcache_search(&tcs, tag))
        return -1;

    /* Prevent duplicate entries in the search list. */
    tagcache_search_set_uniqbuf(&tcs, uniqbuf, UNIQBUF_SIZE);

    if (level || is_basename|| csi->clause_count[0] || TAGCACHE_IS_NUMERIC(tag))
        sort = true;

    for (i = 0; i < level; i++)
    {
        if (TAGCACHE_IS_NUMERIC(csi->tagorder[i]))
        {
            static struct tagcache_search_clause cc;

            memset(&cc, 0, sizeof(struct tagcache_search_clause));
            cc.tag = csi->tagorder[i];
            cc.type = clause_is;
            cc.numeric = true;
            cc.numeric_data = csi->result_seek[i];
            tagcache_search_add_clause(&tcs, &cc);
        }
        else
        {
            tagcache_search_add_filter(&tcs, csi->tagorder[i],
                                       csi->result_seek[i]);
        }
    }

    /* because tagcache saves the clauses, we need to lock the buffer
     * for the entire duration of the search */
    core_pin(browser_db_handle);
    for (i = 0; i <= level; i++)
    {
        int j;

        for (j = 0; j < csi->clause_count[i]; j++)
            tagcache_search_add_clause(&tcs, csi->clause[i][j]);
    }

    current_offset = offset;
    current_entry_count = 0;
    c->dirfull = false;

    fmt = NULL;
    for (i = 0; i < format_count; i++)
    {
        if (c->currtable == TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS)
        {
            /* If it is sorted by albums, we need to use the proper view
            that includes the disc number etc so sorting will be correct for each albums.
            Otherwise, tracks will be sorted by title names */
            if (formats[i]->group_id != csi->format_id[level + 1])
                continue;
        }
        else if (formats[i]->group_id != csi->format_id[level])
            continue;
        fmt = formats[i];
    }

    if (fmt)
    {
        sort_inverse = fmt->sort_inverse;
        sort_limit = fmt->limit;
        strip = fmt->strip;
        sort = true;
    }
    else
    {
        sort_inverse = false;
        sort_limit = 0;
        strip = 0;
    }

    /* lock buflib out due to possible yields */
    browser_lock_cache(c);
    struct tagentry *dptr = core_get_data(c->cache.entries_handle);

    /* Special entries, in display order at the head of the list. Which ones
     * apply depends on the level:
     *
     * <All tracks> flattens everything below this node, so it only means
     * something above the track level -- at a track list there is nothing
     * below, and selecting the top track already plays on through the rest.
     *
     * <Random> works at every level, including the track list: the handler in
     * browser_db_enter_entry() picks a random NON-special row and adopts that
     * row's own newtable/extraseek, so on a track list it lands on
     * TABLE_PLAYTRACK and plays a random track from the album.
     *
     * `sidx` is the logical index of the next special entry, which is what the
     * paging `offset` is compared against -- so <Random> sits at index 1 when
     * <All tracks> precedes it and at index 0 when it does not. */
    int sidx = 0;

    if (tag != tag_title && tag != tag_filename)
    {
        if (offset <= sidx)
        {
            dptr->newtable = TABLE_ALLSUBENTRIES;
            dptr->name = ID2P(LANG_TAGNAVI_ALL_TRACKS);
            dptr->extraseek = 0;
            dptr->customaction = ONPLAY_NO_CUSTOMACTION;
            dptr->idx_id = 0;
            dptr++;
            current_entry_count++;
            c->special_entry_count++;
        }
        sidx++;
    }
    if (tag != tag_filename)
    {
        if (offset <= sidx)
        {
            dptr->newtable = TABLE_NAVIBROWSE;
            dptr->name = ID2P(LANG_TAGNAVI_RANDOM);
            dptr->extraseek = -1;
            dptr->customaction = ONPLAY_NO_CUSTOMACTION;
            dptr->idx_id = 0;
            dptr++;
            current_entry_count++;
            c->special_entry_count++;
        }
        sidx++;
    }

    total_count += sidx;

    while (tagcache_get_next(&tcs, tcs_buf, tcs_bufsz))
    {
        if (total_count++ < offset)
            continue;

        dptr->newtable = TABLE_NAVIBROWSE;
        if (tag == tag_title || tag == tag_filename)
        {
            dptr->newtable = TABLE_PLAYTRACK;
            dptr->extraseek = tcs.idx_id;
        }
        else
            dptr->extraseek = tcs.result_seek;
        dptr->customaction = ONPLAY_NO_CUSTOMACTION;
        dptr->idx_id = tcs.idx_id;

        fmt = NULL;
        /* Check the format */
        for (i = 0; i < format_count; i++)
        {
            if (c->currtable == TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS)
            {
                /* If it is sorted by albums, we need to use the proper view
                that includes the disc number etc so sorting will be correct for each albums.
                Otherwise, tracks will be sorted by title names */
                if (formats[i]->group_id != csi->format_id[level + 1])
                    continue;
            }
            else if (formats[i]->group_id != csi->format_id[level])
                continue;

            if (tagcache_check_clauses(&tcs, formats[i]->clause,
                                       formats[i]->clause_count))
            {
                fmt = formats[i];
                break;
            }
        }

        if (strcmp(tcs.result, UNTAGGED) == 0)
        {
            if (tag == tag_title && tcs.type == tag_title && tcs.filter_count <= 1)
            { /* Fallback to basename */
                char *lastname = dptr->name;
                dptr->name = core_get_data(c->cache.name_buffer_handle)+namebufused;
                if ((c->cache.name_buffer_size - namebufused) > 0 && 
                    tagcache_retrieve(&tcs, tcs.idx_id, tag_virt_basename, dptr->name,
                                      c->cache.name_buffer_size - namebufused))
                {
                    namebufused += strlen(dptr->name)+1;
                    dptr->album_name = core_get_data(c->cache.name_buffer_handle)+namebufused;
                    if ((c->cache.name_buffer_size - namebufused) > 0 &&
                        tagcache_retrieve(&tcs, tcs.idx_id, tag_album, dptr->album_name,
                                    c->cache.name_buffer_size - namebufused))
                        namebufused += strlen(dptr->album_name)+1;
                    else
                        dptr->album_name = NULL;
                    goto entry_skip_formatter;
                }
                dptr->name = lastname; /* restore last entry if filename failed */
                dptr->album_name = NULL;
            }

            tcs.result = str(LANG_TAGNAVI_UNTAGGED);
            tcs.result_len = strlen(tcs.result);
            tcs.ramresult = true;
        }

        if (!tcs.ramresult || fmt)
        {

            dptr->name = core_get_data(c->cache.name_buffer_handle)+namebufused;

            if (fmt)
            {
                int ret = format_str(&tcs, fmt, dptr->name,
                                     c->cache.name_buffer_size - namebufused);
                bool error_on_str_format = ret < 0;
                if (!error_on_str_format)
                {
                    namebufused += strlen(dptr->name)+1; /* include NULL */
                    dptr->album_name = core_get_data(c->cache.name_buffer_handle)+namebufused;
                    if ((c->cache.name_buffer_size - namebufused) > 0 &&
                            tagcache_retrieve(&tcs, tcs.idx_id, tag_album, dptr->album_name,
                                      c->cache.name_buffer_size - namebufused))
                        namebufused += strlen(dptr->album_name)+1;
                    else
                        dptr->album_name = NULL;
                }
                else
                {
                    if (ret == -4)          /* buffer full */
                    {
                        logf("chunk mode #2: %d", current_entry_count);
                        c->dirfull = true;
                        sort = false;
                        break;
                    }

                    logf("format_str() failed");
                    tagcache_search_finish(&tcs);
                    browser_unlock_cache(c);
                    core_unpin(browser_db_handle);
                    return 0;
                }
            }
            else
            {
                tcs_get_basename(&tcs, is_basename);
                namebufused += tcs.result_len;
                bool buffer_full = (namebufused >= c->cache.name_buffer_size);
                if (!buffer_full)
                {
                    dptr->album_name = core_get_data(c->cache.name_buffer_handle)+namebufused;
                    if (tagcache_retrieve(&tcs, tcs.idx_id, tag_album, dptr->album_name,
                                      c->cache.name_buffer_size - namebufused))
                        namebufused += strlen(dptr->album_name)+1;
                    else
                        dptr->album_name = NULL;
                    strcpy(dptr->name, tcs.result);
                }
                else
                {
                    logf("chunk mode #2a: %d", current_entry_count);
                    c->dirfull = true;
                    sort = false;
                    break ;
                }
            }
        }
        else
        {
            tcs_get_basename(&tcs, is_basename);
            dptr->name = tcs.result;
            dptr->album_name = NULL;
        }
entry_skip_formatter:
        dptr++;
        current_entry_count++;

        if (current_entry_count >= c->cache.max_entries)
        {
            logf("chunk mode #3: %d", current_entry_count);
            c->dirfull = true;
            sort = false;
            break ;
        }
    }

    if (sort)
    {
        if (global_settings.interpret_numbers)
            qsort_fn = sort_inverse ? strnatcasecmp_n_inv : strnatcasecmp_n;
        else
            qsort_fn = sort_inverse ? strncasecmp_inv : strncasecmp;

        struct tagentry *entries = get_entries(c);
        qsort(&entries[c->special_entry_count],
              current_entry_count - c->special_entry_count,
              sizeof(struct tagentry),
              c->currtable == TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS ? compare_with_albums : compare
        );
    }

    if (!init)
    {
        tagcache_search_finish(&tcs);
        browser_unlock_cache(c);
        core_unpin(browser_db_handle);
        return current_entry_count;
    }

    while (tagcache_get_next(&tcs, tcs_buf, tcs_bufsz))
        total_count++;

    tagcache_search_finish(&tcs);
    browser_unlock_cache(c);
    core_unpin(browser_db_handle);

    if (!sort && (sort_inverse || sort_limit))
    {
        if (global_settings.talk_menu) {
            talk_id(LANG_SHOWDIR_BUFFER_FULL, true);
            talk_value(total_count, UNIT_INT, true);
        }

        /* (voiced above) */
        splashf(HZ*4, str(LANG_SHOWDIR_BUFFER_FULL), total_count);
        logf("Too small dir buffer");
        return 0;
    }

    if (sort_limit)
        total_count = MIN(total_count, sort_limit);

    if (strip)
    {
        dptr = get_entries(c);
        for (i = c->special_entry_count; i < current_entry_count; i++, dptr++)
        {
            int len = strlen(dptr->name);

            if (len < strip)
                continue;

            dptr->name = &dptr->name[strip];
        }
    }

    return total_count;

}

/* Does this "==>" row lead anywhere?
 *
 * An "==>" naming a menu that was never defined does not fail to parse: the
 * parser allocates an empty menu and links to it (see the menu_load case in
 * parse_search()). The row then exists and opens a blank list. That is exactly
 * what a "Custom menu" row does on a player with no tagnavi_custom.config --
 * which is every player, until someone writes one.
 *
 * So a submenu row is shown only once its menu has something in it. Nothing
 * here is specific to the custom menu; any "==>" pointing at an empty or
 * missing menu is hidden, and appears by itself the moment a config gives it
 * contents. The generated %byfirstletter menus are unaffected -- those are
 * filled with 28 real rows at parse time. */
static bool row_submenu_has_items(const struct menu_entry *item)
{
    return item->link >= 0 && item->link < menu_count
        && menus[item->link] && menus[item->link]->itemcount > 0;
}

/* The tagnavi menu these rows attach to, by its config id rather than its
 * position, so reordering tagnavi.config cannot move them somewhere else.
 * -1 if the user's config has no such menu, in which case the rows simply do
 * not appear. */
static int menu_index_by_id(const char *id)
{
    int i;
    for (i = 0; i < menu_count; i++)
    {
        if (!strcasecmp(menus[i]->id, id))
            return i;
    }
    return -1;
}

/* Is this a tag-browse row on the given tag? Used to find the Music menu's
 * "Album" row by what it does rather than by its name or position.
 *
 * Not unique: "Recently Added" also browses tag_album first, so the caller
 * must take the first match rather than every one. */
static bool row_browses_tag(const struct menu_entry *item, int tag)
{
    return item->type == menu_next && item->si.tagorder_count > 0
        && item->si.tagorder[0] == tag;
}

/* Does this row carry the given label? The Playback History rows are anchored
 * this way because, unlike the Album row, they are all `-> title` searches
 * distinguished only by their clauses -- there is nothing structural to match
 * on. Matching the shipped tagnavi.config's own English strings, so a user who
 * replaces that menu simply loses the anchor and the row is appended instead
 * (see load_root()). */
static bool row_named(const struct menu_entry *item, const char *name)
{
    return strcmp((const char *)P2STR((unsigned char *)item->name), name) == 0;
}

/* One row that came from here rather than from tagnavi.config. 'param' is
 * carried in extraseek for the marker to interpret. */
static void add_synthetic_row(struct tagentry **dptr, int lang_id,
                              int marker, int param)
{
    (*dptr)->name = (char*)ID2P(lang_id);
    (*dptr)->newtable = marker;
    (*dptr)->extraseek = param;
    (*dptr)->customaction = ONPLAY_NO_CUSTOMACTION;
    (*dptr)++;
}

/* The album charts, and where each one slots into Playback History. Kept as a
 * table so the order shown is the order written here, next to the anchors that
 * put it there: each chart follows the track-level row it is the album
 * counterpart of. */
static const struct {
    int lang_id;
    enum album_chart kind;
    const char *after;      /* row to follow, or NULL to append */
} album_chart_rows[] = {
    /* Album then artist under each track-level row, so the menu reads
     * tracks / albums / artists for each kind of history. Order within one
     * anchor is the order written here. */
    { LANG_MOST_PLAYED_ALBUMS,      ALBUM_CHART_MOST_PLAYED,
      "Most played tracks" },
    { LANG_MOST_PLAYED_ARTISTS,     ARTIST_CHART_MOST_PLAYED,
      "Most played tracks" },
    { LANG_RECENTLY_PLAYED_ALBUMS,  ALBUM_CHART_RECENTLY_PLAYED,
      "Recently played tracks" },
    { LANG_RECENTLY_PLAYED_ARTISTS, ARTIST_CHART_RECENTLY_PLAYED,
      "Recently played tracks" },
    { LANG_FORGOTTEN_ALBUMS,        ALBUM_CHART_FORGOTTEN,
      "Never played tracks" },
    { LANG_FORGOTTEN_ARTISTS,       ARTIST_CHART_FORGOTTEN,
      "Never played tracks" },
};
#define ALBUM_CHART_ROWS ((int)ARRAYLEN(album_chart_rows))

static int load_root(struct browser_context *c)
{
    struct tagentry *dptr = core_get_data(c->cache.entries_handle);
    bool chart_placed[ALBUM_CHART_ROWS] = { 0 };
    bool random_placed = false;
    bool is_runtime_menu;
    int i, n, rows = 0;

    tc = c;
    c->currtable = TABLE_ROOT;
    /* The root menu has no <All tracks>/<Random> rows. Only retrieve_entries()
     * cleared this, so without it the count from the last browse level would
     * still be standing when a root submenu is loaded -- and anything keyed on
     * it (row positioning, the art callback) would skip that many real rows. */
    c->special_entry_count = 0;
    if (c->dirlevel == 0)
        c->currextra = rootmenu;

    menu = menus[c->currextra];
    if (menu == NULL)
        return 0;

    is_runtime_menu = (c->currextra == menu_index_by_id("runtime"));

    /* Room for the synthetic rows added below -- three on Playback History,
     * one on the Music root, never both. Counting them here is not optional:
     * this panics rather than truncating. */
    if (menu->itemcount + SYNTHETIC_ROWS_MAX > c->cache.max_entries)
            panicf("%s browser_cache too small", __func__);

    for (i = 0; i < menu->itemcount; i++)
    {
        /* A submenu that leads nowhere is not drawn -- see
         * row_submenu_has_items(). This is the row itself being skipped;
         * root_row_is_shortcutable() keeps the same row out of the main-menu
         * offer list, so the two stay consistent. */
        if (menu->items[i]->type == menu_load
            && !row_submenu_has_items(menu->items[i]))
            continue;

        dptr->name = (char*)menu->items[i]->name;

        logf( "%s loading menu %d name: %s, lang_id %ld", __func__, i,
             P2STR((unsigned char*)dptr->name),P2ID((unsigned char*)dptr->name));

        switch (menu->items[i]->type)
        {
            case menu_next:
                dptr->newtable = TABLE_NAVIBROWSE;
                dptr->extraseek = i;
                dptr->customaction = ONPLAY_NO_CUSTOMACTION;
                break;

            case menu_load:
                dptr->newtable = TABLE_ROOT;
                dptr->extraseek = menu->items[i]->link;
                dptr->customaction = ONPLAY_NO_CUSTOMACTION;
                break;

            case menu_shuffle_songs:
                dptr->newtable = TABLE_NAVIBROWSE;
                dptr->extraseek = i;
                dptr->customaction = ONPLAY_CUSTOMACTION_SHUFFLE_SONGS;
                break;

            case menu_byfirstletter:
                dptr->newtable = TABLE_NAVIBROWSE;
                dptr->extraseek = i;
                dptr->customaction = ONPLAY_CUSTOMACTION_FIRSTLETTER;
                break;
        }

        dptr++;
        rows++;

        /* Random album, immediately beneath the Music menu's Album row --
         * requested there rather than at the foot, since it is an album
         * action. Anchored to the row that browses tag_album so it follows
         * that row wherever tagnavi.config puts it, but only the first such
         * row: "Recently Added" browses tag_album too, and matching every one
         * put a second copy of this row halfway down the menu. */
        if (c->currextra == rootmenu && !random_placed
            && row_browses_tag(menu->items[i], tag_album))
        {
            add_synthetic_row(&dptr, LANG_RANDOM_ALBUM, TABLE_RANDOM_ALBUM, 0);
            random_placed = true;
            rows++;
        }

        /* Each album chart follows its track-level counterpart, so Playback
         * History reads tracks-then-albums for each kind of history rather
         * than listing all the album ones at the end. They cannot be config
         * rows: a %format groups by the tag it displays and sorts on the
         * formatted string, so "sum playcount across an album" is not
         * expressible (see screens/browse/album_charts.c). Their labels come
         * from the language file, so they are translated. */
        if (is_runtime_menu)
        {
            for (n = 0; n < ALBUM_CHART_ROWS; n++)
            {
                if (chart_placed[n] || !album_chart_rows[n].after
                    || !row_named(menu->items[i], album_chart_rows[n].after))
                    continue;

                /* No break: two rows share each anchor (the album chart and
                 * the artist one), and both belong under it. */
                add_synthetic_row(&dptr, album_chart_rows[n].lang_id,
                                  TABLE_ALBUM_CHARTS, album_chart_rows[n].kind);
                chart_placed[n] = true;
                rows++;
            }
        }
    }

    /* Anything whose anchor was not found -- a replaced Playback History menu,
     * or renamed rows -- goes at the end rather than disappearing. */
    if (is_runtime_menu)
    {
        for (n = 0; n < ALBUM_CHART_ROWS; n++)
        {
            if (chart_placed[n])
                continue;
            add_synthetic_row(&dptr, album_chart_rows[n].lang_id,
                              TABLE_ALBUM_CHARTS, album_chart_rows[n].kind);
            rows++;
        }
    }

    current_offset = 0;
    current_entry_count = rows;

    return rows;
}

/* Set by browser_db_enter_by_tag_on_next_load(); consumed the next time
 * browser_db_load() sees a fresh root load. -1 means none armed. */
static int pending_root_shortcut_tag = -1;
/* Its submenu counterpart; empty when none is armed. */
static char pending_root_shortcut_menu[MAX_MENU_ID_SIZE];

void browser_db_enter_by_tag_on_next_load(int tag)
{
    pending_root_shortcut_tag = tag;
    pending_root_shortcut_menu[0] = '\0';
}

/* The submenu counterpart, armed by id ("runtime" and so on) rather than by
 * position for the same reason the tag version uses tag identity: a row that
 * moves in tagnavi.config must still be found. */
void browser_db_enter_menu_on_next_load(const char *menu_id)
{
    pending_root_shortcut_tag = -1;
    strmemccpy(pending_root_shortcut_menu, menu_id,
               sizeof(pending_root_shortcut_menu));
}

/* Finding a row to select, among the rows actually on screen.
 *
 * c->selected_item is a *screen* position, and a row's position in
 * tagnavi.config is not one: load_root() skips submenus that lead nowhere and
 * inserts synthetic rows of its own (Random album, the album charts), so the
 * two diverge from the first insertion onwards. Selecting by config position
 * therefore picks a neighbour -- with Random album sitting under Album, every
 * shortcut below it opened the row above the one asked for.
 *
 * These match against what load_root() just produced rather than repeating its
 * skip and insert rules, so the two cannot drift apart when either changes.
 * load_root() leaves the raw config index in a tag row's extraseek and the
 * target menu in a submenu row's, which is what makes the identification
 * exact. Synthetic rows carry neither table and are skipped.
 *
 * Note browser_db_find_root_entry_by_tag() below is a different question and
 * still correct: it answers "which entry of menus[rootmenu]->items[]", which
 * is a config position, and its callers index that array with it. */
/* The level a main-menu shortcut landed on, or -1. BACK here leaves the
 * browser instead of popping, so the shortcut takes one press to undo rather
 * than depositing the user in the Music menu it existed to skip. */
static int shortcut_base_level = -1;

/* Note the level a shortcut just entered, and open it the way a descent would.
 *
 * Faking dirlevel = 0 was the obvious way to make BACK leave, and it is wrong
 * twice over. load_root() takes dirlevel 0 to mean the root menu and resets
 * currextra to it, so a submenu shortcut lost which submenu it had chosen and
 * showed the Music root. And exiting from a level the browser thinks is the
 * top skips browser_db_exit(), which is what used to put currtable back --
 * leaving it pointing at the shortcut's listing for the next Music entry to
 * inherit. Recording the level instead keeps dirlevel, currtable and currextra
 * agreeing with each other, which everything else here assumes.
 *
 * It also asks for the <All tracks>/<Random> rows to be scrolled past.
 * browser_db_enter() arms that itself, but only on a visible descent, and a
 * shortcut enters with is_visible false -- so this was the one way into a list
 * that opened sitting on a special row. */
static void enter_as_root_shortcut(struct browser_context *c)
{
    shortcut_base_level = c->dirlevel;
    c->selected_item = 0;
    position_past_specials = true;
}

bool browser_db_back_exits(const struct browser_context *c)
{
    return shortcut_base_level >= 0 && c->dirlevel == shortcut_base_level;
}

static int loaded_row_for_menu_id(struct browser_context *c, int rows,
                                  const char *menu_id)
{
    int i;

    for (i = 0; i < rows; i++)
    {
        struct tagentry *e = browser_db_get_entry(c, i);
        int link = e->extraseek;

        if (e->newtable != TABLE_ROOT)
            continue;
        if (link >= 0 && link < menu_count && menus[link]
            && !strcasecmp(menus[link]->id, menu_id))
            return i;
    }
    return -1;
}

static int loaded_row_for_tag(struct browser_context *c, int rows, int tag)
{
    struct menu_root *root = (rootmenu >= 0 && rootmenu < menu_count)
                           ? menus[rootmenu] : NULL;
    int i;

    if (!root)
        return -1;

    for (i = 0; i < rows; i++)
    {
        struct tagentry *e = browser_db_get_entry(c, i);
        int raw = e->extraseek;

        if (e->newtable != TABLE_NAVIBROWSE)
            continue;
        if (raw < 0 || raw >= root->itemcount)
            continue;
        /* Shuffle and by-first-letter rows share TABLE_NAVIBROWSE. */
        if (root->items[raw]->type == menu_next
            && root->items[raw]->si.tagorder_count > 0
            && root->items[raw]->si.tagorder[0] == tag)
            return i;
    }
    return -1;
}

/* Set by browser_db_enter_album_tracks_on_next_load(); consumed on the next
 * fresh root load. -1 means none armed. */
static long pending_album_seek = -1;
static char pending_album_title[MENUENTRY_MAX_NAME];

/* Arms a direct jump straight from the root into a specific album's own
 * track list -- skipping the intermediate "Album" grouping listing (all
 * albums) entirely, unlike a normal Album-browse session. Used by Album
 * covers (apps/gui/album_covers.c): it already knows exactly which album
 * by tagcache seek (pf_idx.album_index[].seek), the same identifier
 * tagcache_search_add_filter() takes elsewhere in this file, so there's no
 * need to search for the album by name/position at all -- that was the
 * root cause of an earlier, much more fragile version of this landing on
 * the wrong row (or none): it tried to replicate retrieve_entries()' qsort
 * + special-entry-count row numbering with a separate, unsorted search.
 * Filtering directly by seek sidesteps that whole class of bug.
 *
 * This also means dirlevel only ever advances by one from the root (see
 * enter_album_tracks_directly()), not two -- so a single BACK press exits
 * the browse entirely, matching Album covers' own requirement that this
 * "specific entry point" not need to show or remember the intermediate
 * Album listing at all. */
void browser_db_enter_album_tracks_on_next_load(long album_seek,
                                             const char *album_title)
{
    pending_album_seek = album_seek;
    strlcpy(pending_album_title, album_title ? album_title : "",
            sizeof(pending_album_title));
    browser_db_enter_by_tag_on_next_load(tag_album);
}

/* Set by browser_db_enter_artist_albums_on_next_load(); consumed on the next fresh
 * root load. -1 means none armed. */
static long pending_artist_seek = -1;
static char pending_artist_title[MENUENTRY_MAX_NAME];

/* Arms a direct jump from the root into a specific album-artist's own album
 * list -- the visual counterpart, for Artist portraits, of
 * browser_db_enter_album_tracks_on_next_load(). Same seek-based approach: the
 * carousel already knows the album-artist by tagcache seek, so no name/position
 * matching is needed. Lands one level deep (the album listing) with dirlevel
 * still 0, so a single BACK returns to the carousel while selecting an album
 * still descends normally into its tracks. */
void browser_db_enter_artist_albums_on_next_load(long albumartist_seek,
                                              const char *artist_title)
{
    pending_artist_seek = albumartist_seek;
    strlcpy(pending_artist_title, artist_title ? artist_title : "",
            sizeof(pending_artist_title));
    browser_db_enter_by_tag_on_next_load(tag_albumartist);
}

/* Finds the row in the currently-loaded root ("main") menu whose first tag
 * matches 'tag' (e.g. tag_album), independent of tagnavi.config's row order.
 * Returns the row index, or -1 if not found. Must be called after load_root()
 * has populated 'menu'. */
static int browser_db_find_root_entry_by_tag(int tag)
{
    int i;
    struct menu_root *root;
    /* Look at menus[rootmenu] (populated by parse_menu() at boot) rather than
     * the file-scope 'menu', which only holds the root while a fresh root is
     * loaded. The direct-entry callers below reach this at dirlevel 0, where the
     * two coincide -- but keying off the boot-time root can never go stale from
     * a mid-tree browse state, matching browser_db_get_main_menu_tag_row(). */
    if (rootmenu < 0 || rootmenu >= menu_count)
        return -1;
    root = menus[rootmenu];
    if (!root)
        return -1;
    for (i = 0; i < root->itemcount; i++)
    {
        if (root->items[i]->type == menu_next &&
            root->items[i]->si.tagorder_count > 0 &&
            root->items[i]->si.tagorder[0] == tag)
            return i;
    }
    return -1;
}

/* Consumes pending_album_seek: points csi straight at the root's "Album"
 * row's own search_instruction (same one a normal two-level Album browse
 * uses) but jumps directly to its title level, filtered by album_seek,
 * without ever entering/displaying the grouping level itself.
 *
 * Deliberately does NOT touch dirlevel/table_history/extra_history the way
 * browser_db_enter() would (leaves dirlevel at 0, wherever rockbox_browse()'s
 * own reset left it) -- apps/tree.c's dirbrowse() only treats BACK as "exit
 * the browse" when dirlevel == 0; at any deeper level it instead pops one
 * level via browser_db_exit() and keeps browsing. Bumping dirlevel to 1 (tried
 * first) meant BACK from an album's tracks landed on the root Music menu
 * first, needing a second BACK to actually leave -- not the single-press
 * exit this entry point is supposed to give. Leaving dirlevel at 0 makes
 * this look, to tree.c, exactly like any other one-level-deep tag browse
 * (e.g. root_menu.c's GO_TO_DBBROWSER), so BACK exits immediately, which
 * root_menu.c then redirects straight back to Album covers.
 *
 * Returns false if the root has no tag_album row or it isn't a plain
 * "album -> title" chain (e.g. tagnavi_user.config was customized away
 * from the shipped shape) -- caller falls back to just loading the root. */
static bool enter_album_tracks_directly(struct browser_context *c, long album_seek,
                                        const char *album_title)
{
    int idx = browser_db_find_root_entry_by_tag(tag_album);
    struct search_instruction *si;

    if (idx < 0)
        return false;

    si = &menus[rootmenu]->items[idx]->si;
    if (si->tagorder_count < 2 || si->tagorder[1] != tag_title)
        return false;

    csi = si;
    csi->result_seek[0] = album_seek;
    c->currtable = TABLE_NAVIBROWSE;
    c->currextra = 1;
    c->selected_item = 0;
    /* Open on the first real track, not <All tracks>/<Random> -- same as a
     * normal descent (see position_past_specials in browser_db_load()). */
    position_past_specials = true;
    strmemccpy(current_title[c->currextra], album_title,
               sizeof(current_title[0]));
    return true;
}

/* Consumes pending_artist_seek: points csi at the root's "Album Artist" row's
 * search_instruction and jumps straight to its album level, filtered by the
 * album-artist's seek, without displaying the album-artist grouping itself. The
 * album-artist row's chain is albumartist -> album -> title, so currextra 1 is
 * the album listing. Leaves dirlevel at 0 (see enter_album_tracks_directly for
 * why). Returns false if the root has no such albumartist -> album chain. */
static bool enter_artist_albums_directly(struct browser_context *c,
                                         long artist_seek, const char *artist_title)
{
    int idx = browser_db_find_root_entry_by_tag(tag_albumartist);
    struct search_instruction *si;

    if (idx < 0)
        return false;

    si = &menus[rootmenu]->items[idx]->si;
    if (si->tagorder_count < 3 || si->tagorder[1] != tag_album)
        return false;

    csi = si;
    csi->result_seek[0] = artist_seek;
    c->currtable = TABLE_NAVIBROWSE;
    c->currextra = 1;
    c->selected_item = 0;
    /* Open on the first real album, not <All tracks> -- same as a normal
     * descent (see position_past_specials in browser_db_load()). */
    position_past_specials = true;
    strmemccpy(current_title[c->currextra], artist_title,
               sizeof(current_title[0]));
    return true;
}

/* root_menu.c's tagnavi-derived main-menu shortcuts (GO_TO_TAGNAVI_FIRST..LAST)
 * only cover direct tag-browse rows ("->" / menu_next) of the root ("main")
 * menu -- rows that load a nested sub-menu ("==>") or trigger an action
 * (e.g. "~>" shuffle) aren't simple browse shortcuts and are excluded. These
 * two look directly at menus[rootmenu] (populated by parse_menu() at boot)
 * rather than the file-scope 'menu' pointer load_root() sets, since
 * root_menu.c may query this before any browsing has actually happened. */
/* Can this root-menu row be offered as a main-menu shortcut, on this pass?
 *
 * Both kinds qualify: a "->" row that browses a tag (pass 0), and a "==>" row
 * that opens a submenu (pass 1). Only the first kind used to, which is why
 * Playback History could not be put on the main menu however much it looked
 * like it should be. What rules a row out is having no stable identity to arm
 * a jump with: a tag browse with no tags, or a submenu that leads nowhere.
 * Everything else (shuffle songs) is an action rather than a place.
 *
 * The two passes are the point, not a tidiness. A row's position in this
 * enumeration is its *slot number*, and the slot number is what a saved
 * main-menu configuration records -- root_menu.c writes "tagnavi6" and reads
 * it back. Admitting submenu rows in menu order therefore renumbered every
 * slot from the first submenu onwards, silently repointing saved
 * configurations at different rows: a "tagnavi6" that meant User Rating came
 * back as First Letter. Enumerating all the tag rows first and the submenus
 * after leaves every previously saved slot meaning exactly what it did, and
 * gives the submenus numbers that were never in use.
 *
 * So: only ever append a new kind of row as a later pass. */
static bool root_row_eligible(const struct menu_entry *item, int pass)
{
    if (pass == 0)
        return item->type == menu_next && item->si.tagorder_count > 0;
    return item->type == menu_load && row_submenu_has_items(item);
}

#define ROOT_ROW_PASSES 2

bool browser_db_get_main_menu_row(int index, int *out_tag,
                                  const char **out_menu_id,
                                  const unsigned char **out_name)
{
    struct menu_root *root;
    int pass;
    int i, count = 0;

    if (out_tag)
        *out_tag = -1;
    if (out_menu_id)
        *out_menu_id = NULL;

    if (rootmenu < 0 || rootmenu >= menu_count)
        return false;

    root = menus[rootmenu];
    if (!root)
        return false;

    for (pass = 0; pass < ROOT_ROW_PASSES; pass++)
    {
        for (i = 0; i < root->itemcount; i++)
        {
            if (!root_row_eligible(root->items[i], pass))
                continue;

            if (count == index)
            {
                if (root->items[i]->type == menu_next)
                {
                    if (out_tag)
                        *out_tag = root->items[i]->si.tagorder[0];
                }
                else if (out_menu_id)
                    *out_menu_id = menus[root->items[i]->link]->id;

                if (out_name)
                    *out_name = root->items[i]->name;
                return true;
            }
            count++;
        }
    }
    return false;
}

int browser_db_get_main_menu_tag_row_count(void)
{
    int i, count = 0;
    struct menu_root *root;
    int pass;

    if (rootmenu < 0 || rootmenu >= menu_count)
        return 0;

    root = menus[rootmenu];
    if (!root)
        return 0;

    for (pass = 0; pass < ROOT_ROW_PASSES; pass++)
    {
        for (i = 0; i < root->itemcount; i++)
        {
            if (root_row_eligible(root->items[i], pass))
                count++;
        }
    }
    return count;
}

int browser_db_load(struct browser_context* c)
{
    logf( "%s", __func__);

    int count;
    int table = c->currtable;

    c->dirsindir = 0;

    if (!table)
    {
        c->dirfull = false;
        table = TABLE_ROOT;
        c->currtable = table;
        c->currextra = rootmenu;
        /* A session starting at the root is not one a shortcut opened. */
        shortcut_base_level = -1;
    }

    /* A shortcut (e.g. root_menu.c's Artists/Albums/Genres entries) armed a
     * jump straight into a specific tag's browse table. This must happen
     * here, on the first load of a fresh root, rather than before
     * rockbox_browse() is called -- rockbox_browse() unconditionally resets
     * dirlevel/selected_item to 0 for any ID3-DB entry (tree.c), which would
     * silently discard a dirlevel bump made any earlier. */
    /* The submenu form of the same thing, for a root row that opens a nested
     * menu ("Playback History ==> runtime") rather than browsing a tag. One
     * plain hop: select the row and enter it, exactly as pressing it would. */
    if (pending_root_shortcut_menu[0] && table == TABLE_ROOT
        && c->dirlevel == 0)
    {
        char target[MAX_MENU_ID_SIZE];
        int idx, rows;

        strmemccpy(target, pending_root_shortcut_menu, sizeof(target));
        pending_root_shortcut_menu[0] = '\0';

        rows = load_root(c);

        idx = loaded_row_for_menu_id(c, rows, target);
        if (idx >= 0)
        {
            c->selected_item = idx;
            browser_db_enter(c, false);
            enter_as_root_shortcut(c);
            return browser_db_load(c);
        }
        /* Submenu gone (a customised tagnavi_user.config) -- fall through and
         * show the root menu rather than a dead end, as the tag case does. */
    }

    if (pending_root_shortcut_tag != -1 && table == TABLE_ROOT && c->dirlevel == 0)
    {
        int target_tag = pending_root_shortcut_tag;
        int rows;
        pending_root_shortcut_tag = -1;

        rows = load_root(c);

        /* Album covers' jump (only ever paired with target_tag == tag_album)
         * bypasses the normal single-hop browser_db_enter() below entirely --
         * it goes straight to the album's track level itself, never
         * entering/displaying the "Album" grouping listing at all. */
        if (target_tag == tag_album && pending_album_seek != -1)
        {
            long album_seek = pending_album_seek;
            char album_title[MENUENTRY_MAX_NAME];
            strlcpy(album_title, pending_album_title, sizeof(album_title));
            pending_album_seek = -1;

            if (enter_album_tracks_directly(c, album_seek, album_title))
                return browser_db_load(c);
            /* Root has no plain tag_album row to anchor on (e.g. tagnavi
             * customized away from the shipped shape) -- fall through to
             * the root menu instead of a dead end. */
        }
        else if (target_tag == tag_albumartist && pending_artist_seek != -1)
        {
            /* Artist portraits' jump: straight to a specific album-artist's
             * album listing, mirroring the tag_album case above. */
            long artist_seek = pending_artist_seek;
            char artist_title[MENUENTRY_MAX_NAME];
            strlcpy(artist_title, pending_artist_title, sizeof(artist_title));
            pending_artist_seek = -1;

            if (enter_artist_albums_directly(c, artist_seek, artist_title))
                return browser_db_load(c);
            /* No albumartist -> album row to anchor on -- fall through. */
        }
        else
        {
            int idx = loaded_row_for_tag(c, rows, target_tag);
            if (idx >= 0)
            {
                c->selected_item = idx;
                browser_db_enter(c, false);
                enter_as_root_shortcut(c);
                return browser_db_load(c);
            }
            /* Tag not found (e.g. removed from tagnavi_user.config) -- fall
             * through and show the root menu instead of a blank screen. */
        }
    }

    switch (table)
    {
        case TABLE_ROOT:
            logf( "root...");
            count = load_root(c);
            break;

        case TABLE_ALLSUBENTRIES:
        case TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS:
        case TABLE_NAVIBROWSE:
            logf("navibrowse...");

            if (loaded_entries_crc != 0)
            {
                if (loaded_entries_crc == browser_db_data_crc(c))
                {
                    count = c->dirlength;
                    logf("Reusing %d entries", count);
                    break;
                }
            }

            cpu_boost(true);
            ui_set_working(true);
            count = retrieve_entries(c, 0, true);
            ui_set_working(false);
            cpu_boost(false);
            break;

        default:
            logf("Unsupported table %d\n", table);
            return -1;
    }

    loaded_entries_crc = 0;

    if (count < 0)
    {
        /* No modal "Loading..."/"Database busy" splash here -- background
         * database activity is surfaced by the status-bar indicator (%ld)
         * instead. A busy load just recovers to the root level. */
        if (count == RELOAD_BROWSER_DB) /* unload and re-init the database browser */
        {
            browser_db_unload(c);
            if (!initialize_browser_db())
                return 0;
        }
        c->dirlevel = 0;
        count = load_root(c);
    }

    /* The _total_ numer of entries available. */
    c->dirlength = c->filesindir = count;

    /* A freshly entered level opens on its first real row rather than on
     * <All tracks>/<Random>: those are shortcuts, not what the list is *for*,
     * and landing on one meant an extra press on every descent. Deferred to
     * here because special_entry_count is only known once the entries have been
     * built. Restored positions (coming back up a level) are left alone. */
    if (position_past_specials)
    {
        position_past_specials = false;
        /* selected_item == 0 confirms this really is a list opening at the top
         * rather than one restoring a stored position -- the request is armed
         * one navigation earlier, so it must not act on a load that has since
         * been given a real position to return to. */
        if (c->selected_item == 0 && c->special_entry_count > 0 &&
            count > c->special_entry_count)
        {
            c->selected_item = c->special_entry_count;
            /* Also scroll them off the top, so the list reads as starting at
             * the first real row. Consumed once by the browser's list sync. */
            pending_top_item = c->special_entry_count;
        }
    }
    else if (c->special_entry_count > 0
             && c->selected_item >= c->special_entry_count)
    {
        /* Coming back to a level, on a row past the special ones. Ask for the
         * same top row: the list keeps one row above the cursor of its own
         * accord, so returning to the first album or two put <All tracks> back
         * on screen even though the descent had scrolled it away -- the list
         * looked different depending on how it was arrived at. The browser
         * drops this request if the selection is too far down for it, in which
         * case the special rows are off the top anyway. */
        pending_top_item = c->special_entry_count;
    }

    return count;
}

/* Returns the top row a freshly loaded database list wants shown, or -1 if it
 * has no preference. Clears the request, so a later redraw (or a list the user
 * has since scrolled) is not forced back to it. */
int browser_db_take_pending_top_item(void)
{
    int item = pending_top_item;
    pending_top_item = -1;
    return item;
}

/* Enters menu or table for selected item in the database.
 *
 * Call this with the is_visible parameter set to false to
 * prevent selected_item_history from being updated or applied, in
 * case the menus aren't displayed to the user.
 * Before calling browser_db_enter again with the parameter set to
 * true, make sure that you are back at the previous dirlevel, by
 * calling browser_db_exit as needed, with is_visible set to false.
 */
int browser_db_enter(struct browser_context* c, bool is_visible)
{
    logf( "%s", __func__);

    int rc = 0;
    struct tagentry *dptr;
    struct mp3entry *id3;
    int newextra;
    int seek;
    int source;
    bool is_random_item = false;
    bool adjust_selection = true;

    dptr = browser_db_get_entry(c, c->selected_item);

    c->dirfull = false;
    seek = dptr->extraseek;
    if (seek == -1) /* <Random> menu item was selected */
    {
        is_random_item = true;
        if(c->filesindir<=c->special_entry_count) /* Menu contains only special entries */
            return 0;
        srand(current_tick);
        int random_item = c->special_entry_count +
                          (rand() % (c->filesindir - c->special_entry_count));
        dptr = browser_db_get_entry(c, random_item);
        seek = dptr->extraseek;
        /* On a track list the chosen row plays rather than descending, and
         * browser_db_play_folder() reads its start position from
         * c->selected_item -- still the <Random> row. Point it at the row
         * actually drawn, or every "random" would start the album at track one.
         * Only track rows carry TABLE_PLAYTRACK, so this cannot fire on the
         * levels where <Random> descends instead. */
        if (dptr->newtable == TABLE_PLAYTRACK)
            c->selected_item = random_item;
    }
    newextra = dptr->newtable;

    /* The synthetic rows are actions, not browse levels. Leaving here --
     * before any of the history/dirlevel bookkeeping below, and before the
     * cache lock is taken -- is what makes returning from one land back on
     * this menu exactly as it was. Descending first and unwinding afterwards
     * would have to undo four separate pieces of state.
     *
     * Which chart is in the row's extraseek, already read into 'seek'; the
     * armed value is picked up by root_menu.c's dispatch. */
    if (newextra == TABLE_ALBUM_CHARTS)
    {
        album_charts_arm(seek);
        return GO_TO_ALBUM_CHARTS;
    }
    if (newextra == TABLE_RANDOM_ALBUM)
        return GO_TO_RANDOM_ALBUM;

    if (c->dirlevel >= MAX_DIR_LEVELS)
        return 0;

    if (is_visible) /* update selection history only for user-selected items */
    {
        /* We need to discard selected item history for levels
        descending from current one if selection has changed */
        if (max_history_level < c->dirlevel + 1
            || (max_history_level > c->dirlevel
                && selected_item_history[c->dirlevel] != c->selected_item)
            || is_random_item)
        {
            max_history_level = c->dirlevel + 1;
            if (max_history_level < MAX_DIR_LEVELS)
                selected_item_history[max_history_level] = 0;
            /* The level being entered has no stored position -- the line above
             * just zeroed it. That, not the "no history" branch further down,
             * is what a fresh descent looks like: max_history_level now equals
             * the new dirlevel, so the selection is restored from this
             * zeroed entry rather than defaulted. Ask the load to put the
             * cursor past that level's special rows once their count is known
             * (see browser_db_load()). */
            position_past_specials = true;
        }

        selected_item_history[c->dirlevel]=c->selected_item;
    }
    table_history[c->dirlevel] = c->currtable;
    extra_history[c->dirlevel] = c->currextra;

    if (c->dirlevel + 1 < MAX_DIR_LEVELS)
    {
        c->dirlevel++;
        /*DEBUGF("Tagtree depth %d\n", c->dirlevel);*/
    }
    else
    {
        DEBUGF("Tagtree depth exceeded\n");
    }

    /* lock buflib for possible I/O to protect dptr */
    browser_lock_cache(c);
    core_pin(browser_db_handle);

    switch (c->currtable) {
        case TABLE_ROOT:
            c->currextra = newextra;

            if (newextra == TABLE_ROOT)
            {
                menu = menus[seek];
                c->currextra = seek;
            }

            else if (newextra == TABLE_NAVIBROWSE)
            {
                int i, j;

                csi = &menu->items[seek]->si;
                c->currextra = 0;

                unsigned char *name = dptr->name;

                strmemccpy(current_title[c->currextra], P2STR(name),
                           sizeof(current_title[0]));

                logf("%s (ROOT) current title %s", __func__, P2STR(name));

                /* Read input as necessary. */
                for (i = 0; i < csi->tagorder_count; i++)
                {
                    for (j = 0; j < csi->clause_count[i]; j++)
                    {
                        char* searchstring;

                        if (csi->clause[i][j]->type == clause_logical_or)
                            continue;

                        source = csi->clause[i][j]->source;

                        if (source == source_constant)
                            continue;

                        /* discard history for lower levels when doing runtime searches */
                        if (is_visible)
                            max_history_level = c->dirlevel - 1;

                        searchstring=csi->clause[i][j]->str;
                        *searchstring = '\0';

                        id3 = audio_current_track();

                        if (source == source_current_path && id3)
                        {
                            char *e;
                            strmemccpy(searchstring, id3->path, SEARCHSTR_SIZE);
                            e = strrchr(searchstring, '/');
                            if (e)
                                *e = '\0';
                        }
                        else if (source > source_runtime && id3)
                        {

                            int k = source-source_runtime;
                            int offset = id3_to_search_mapping[k].id3_offset;
                            char **src = (char**)((char*)id3 + offset);
                            if (*src)
                            {
                                strmemccpy(searchstring, *src, SEARCHSTR_SIZE);
                            }
                        }
                        else
                        {
                            /* No live substitution available: prompt the user
                             * to type the search string (the "Search by..."
                             * menu and any other runtime-input clause). */
                            rc = kbd_input(searchstring, SEARCHSTR_SIZE, NULL);
                            if (rc < 0 || !searchstring[0])
                            {
                                browser_db_exit(c, is_visible);
                                browser_unlock_cache(c);
                                core_unpin(browser_db_handle);
                                return 0;
                            }
                            if (csi->clause[i][j]->numeric)
                                csi->clause[i][j]->numeric_data = atoi(searchstring);
                        }


                    }
                }
            }
            c->currtable = newextra;

            break;

        case TABLE_NAVIBROWSE:
        case TABLE_ALLSUBENTRIES:
        case TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS:
            if (newextra == TABLE_PLAYTRACK)
            {
                adjust_selection = false;

                if (global_settings.party_mode && audio_status()) {
                    splash(HZ, ID2P(LANG_PARTY_MODE));
                    break;
                }
                c->dirlevel--;
                /* about to create a new current playlist...
                 allow user to cancel the operation */
                if (!warn_on_pl_erase())
                    break;
                if (browser_db_play_folder(c) >= 0)
                    rc = 2;
                break;
            }

            c->currtable = newextra;
            csi->result_seek[c->currextra] = seek;
            if (c->currextra < csi->tagorder_count-1)
                c->currextra++;
            else
                c->dirlevel--;

            unsigned char *name = dptr->name;
            name = P2STR(name);
            logf("%s (NAVI/ALLSUB) current title %s", __func__, name);
            /* Update the statusbar title */
            strmemccpy(current_title[c->currextra], name, sizeof(current_title[0]));
            break;

        default:
            c->dirlevel--;
            break;
    }

    if (adjust_selection)
    {
        if (is_visible && c->dirlevel <= max_history_level)
            c->selected_item = selected_item_history[c->dirlevel];
        else
        {
            c->selected_item = 0;
        }
    }

    browser_unlock_cache(c);
    core_unpin(browser_db_handle);

    return rc;
}

/* Exits current database menu or table */
void browser_db_exit(struct browser_context* c, bool is_visible)
{
    logf( "%s", __func__);
    if (is_visible) /* update selection history only for user-selected items */
    {
        if (c->selected_item != selected_item_history[c->dirlevel])
            max_history_level = c->dirlevel; /* discard descending item history */
        selected_item_history[c->dirlevel] = c->selected_item;
    }
    c->dirfull = false;
    if (c->dirlevel > 0)
    {
        c->dirlevel--;
        /*DEBUGF("Tagtree depth %d\n", c->dirlevel);*/
    }
    else
    {
        DEBUGF("Tagtree nothing to exit\n");
    }

    if (is_visible)
        c->selected_item = selected_item_history[c->dirlevel];
    c->currtable = table_history[c->dirlevel];
    c->currextra = extra_history[c->dirlevel];
}

int browser_db_get_filename(struct browser_context* c, char *buf, int buflen)
{
    struct tagcache_search tcs;
    int extraseek = browser_db_get_entry(c, c->selected_item)->extraseek;


    if (!tagcache_search(&tcs, tag_filename))
        return -1;

    if (!tagcache_retrieve(&tcs, extraseek, tcs.type, buf, buflen))
    {
        tagcache_search_finish(&tcs);
        return -2;
    }

    tagcache_search_finish(&tcs);

    return 0;
}

int browser_db_get_custom_action(struct browser_context* c)
{
    if (c->dirlength == 0)
        return 0;
    return browser_db_get_entry(c, c->selected_item)->customaction;
}

static void swap_array_bool(bool *a, bool *b)
{
    bool temp = *a;
    *a = *b;
    *b = temp;
}

/**
 * Randomly shuffle an array using the Fisher-Yates algorithm :
 *  https://en.wikipedia.org/wiki/Random_permutation
 *  This algorithm has a linear complexity.
 *  Don't forget to srand before call to use it with a relevant seed.
 */
static bool* fill_random_playlist_indexes(bool *bool_array, size_t arr_sz,
                                         size_t track_count, size_t max_slots)
{
    size_t i;
    if (track_count * sizeof(bool) > arr_sz || max_slots > track_count)
        return NULL;

    for (i = 0; i < arr_sz; i++) /* fill max_slots with TRUE */
        bool_array[i] = i < max_slots;

    /* shuffle bool array */
    for (i = track_count - 1; i > 0; i--)
    {
        int j = rand() % (i + 1);
        swap_array_bool(&bool_array[i], &bool_array[j]);
    }
    return bool_array;
}

static bool insert_all_playlist(struct browser_context *c,
                                const char* playlist, bool new_playlist,
                                int position, bool queue)
{
    struct tagcache_search tcs;
    int n;
    int fd = -1;
    unsigned long last_tick;
    int slots_remaining = 0;
    bool fill_randomly = false;
    bool *rand_bool_array = NULL;
    char buf[MAX_PATH];
    struct playlist_insert_context context;

    cpu_boost(true);

    if (!tagcache_search(&tcs, tag_filename))
    {
        splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
        cpu_boost(false);
        return false;
    } /* NOTE: you need to close this search before returning */

    if (playlist == NULL)
    {
        if (playlist_insert_context_create(NULL, &context, position, queue, false) < 0)
        {
            tagcache_search_finish(&tcs);
            cpu_boost(false);
            return false;
        }
    }
    else
    {
        if (new_playlist)
            fd = open_utf8(playlist, O_CREAT|O_WRONLY|O_TRUNC);
        else
            fd = open(playlist, O_CREAT|O_WRONLY|O_APPEND, 0666);
        if(fd < 0)
        {
            tagcache_search_finish(&tcs);
            cpu_boost(false);
            return false;
        }
    }

    last_tick = current_tick + HZ/2; /* Show splash after 0.5 seconds have passed */
    splash_progress_set_delay(HZ / 2); /* wait 1/2 sec before progress */
    n = c->filesindir;

    if (playlist == NULL)
    {
        int max_playlist_size = playlist_get_current()->max_playlist_size;
        slots_remaining = max_playlist_size - playlist_get_current()->amount;
        if (slots_remaining <= 0)
        {
            logf("Playlist has no space remaining");
            tagcache_search_finish(&tcs);
            cpu_boost(false);
            return false;
        }

        fill_randomly = n > slots_remaining;

        if (fill_randomly)
        {
            srand(current_tick);
            size_t bufsize = 0;
            bool *buffer = (bool *) app_get_buffer(&bufsize, "playlist insert");
            rand_bool_array = fill_random_playlist_indexes(buffer, bufsize,
                                                           n, slots_remaining);

            splashf(HZ * 2, ID2P(LANG_RANDOM_SHUFFLE_RANDOM_SELECTIVE_SONGS_SUMMARY),
                    slots_remaining); /* voiced above */
        }
    }

    bool exit_loop_now = false;
    for (int i = 0; i < n; i++)
    {
        if (TIME_AFTER(current_tick, last_tick + HZ/4))
        {
            /* (voiced) */
            splash_progress(i, n, "%s (%s)", str(LANG_WAIT), str(LANG_OFF_ABORT));
            if (action_userabort(TIMEOUT_NOBLOCK))
            {
                exit_loop_now = true;
                break;
            }
            last_tick = current_tick;
        }

        if (playlist == NULL)
        {
            if (fill_randomly)
            {
                int remaining_tracks = n - i;
                if (remaining_tracks > slots_remaining)
                {
                    if (rand_bool_array)
                    {
                        /* Skip the track if rand_bool_array[i] is FALSE */
                        if (!rand_bool_array[i])
                            continue;
                    }
                    else
                    {
                        /* Generate random value between 0 and remaining_tracks - 1 */
                        int selrange = RAND_MAX / remaining_tracks; /* Improve distribution */
                        int random;

                        for (int r = 0; r < 0x0FFF; r++) /* limit loops */
                        {
                            random = rand() / selrange;
                            if (random < remaining_tracks)
                                break;
                            else
                                random = 0;
                        }
                        /* Skip the track if random >= slots_remaining */
                        if (random >= slots_remaining)
                            continue;
                    }
                }
            }
        }

        if (!tagcache_retrieve(&tcs, browser_db_get_entry(c, i)->extraseek, tcs.type, buf, sizeof buf))
            continue;

        if (playlist == NULL)
        {
            if (fill_randomly)
            {
                if (--slots_remaining <= 0)
                {
                    exit_loop_now = true;
                    break;
                }
            }

            if (playlist_insert_context_add(&context, buf) < 0) {
                logf("playlist_insert_track failed");
                exit_loop_now = true;
                break;
            }
        }
        else if (fdprintf(fd, "%s\n", buf) <= 0)
        {
            exit_loop_now = true;
            break;
        }
        yield();

        if (exit_loop_now)
            break;
    }

    if (playlist == NULL)
        playlist_insert_context_release(&context);
    else
        close(fd);

    tagcache_search_finish(&tcs);
    cpu_boost(false);

    return true;
}

static bool goto_allsubentries(int newtable)
{
    int i = 0;
    while (i < 2 && (newtable == TABLE_NAVIBROWSE || newtable == TABLE_ALLSUBENTRIES
        || newtable == TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS))
    {
        browser_db_enter(tc, false);
        browser_db_load(tc);
        newtable = browser_db_get_entry(tc, tc->selected_item)->newtable;
        i++;
    }
    return (newtable == TABLE_PLAYTRACK);
}

static void reset_tc_to_prev(int dirlevel, int selected_item)
{
    while (tc->dirlevel > dirlevel)
        browser_db_exit(tc, false);
    tc->selected_item = selected_item;
    browser_db_load(tc);
}

static bool browser_db_insert_selection(int position, bool queue,
                                     const char* playlist, bool new_playlist)
{
    char buf[MAX_PATH];
    int dirlevel = tc->dirlevel;
    int selected_item = tc->selected_item;
    int newtable;
    int ret;

    show_search_progress(
        storage_disk_is_active()
        , 0, 0, 0);

    newtable = browser_db_get_entry(tc, tc->selected_item)->newtable;

    if (newtable == TABLE_PLAYTRACK) /* Insert a single track? */
    {
        if (browser_db_get_filename(tc, buf, sizeof buf) < 0)
            return false;

        if (!playlist)
            playlist_insert_track(NULL, buf, position, queue, true);
        else
            catalog_insert_into(playlist, new_playlist, buf, FILE_ATTR_AUDIO);

        return true;
    }

    ret = goto_allsubentries(newtable);
    if (ret)
    {
        if (tc->filesindir <= 0)
            splash(HZ, ID2P(LANG_END_PLAYLIST));
        else if (!insert_all_playlist(tc, playlist, new_playlist, position, queue))
            splash(HZ*2, ID2P(LANG_FAILED));
    }

    reset_tc_to_prev(dirlevel, selected_item);
    return ret;
}

/* Execute action_cb for all subentries of the current table's
 * selected item, handing over each entry's filename in the
 * callback function parameter. Parameter will be NULL for
 * entries whose filename couldn't be retrieved.
 *
 * The caller supplies the filename buffer. A MAX_PATH array here would sit on
 * the stack underneath splash_progress() and the callback, which is deep
 * enough already to overflow.
 */
bool browser_db_subentries_do_action(struct tagcache_search *tcs,
                                     bool (*action_cb)(const char *file_name),
                                     char *buf, size_t buf_sz)
{
    int i, n;
    unsigned long last_tick;
    int ret = true;
    int dirlevel = tc->dirlevel;
    int selected_item = tc->selected_item;
    int newtable = browser_db_get_entry(tc, tc->selected_item)->newtable;

    cpu_boost(true);
    if (!goto_allsubentries(newtable))
        ret = false;
    else if (tagcache_search(tcs, tag_filename))
    {
        last_tick = current_tick + HZ/2;
        splash_progress_set_delay(HZ / 2); /* wait 1/2 sec before progress */
        n = tc->filesindir;
        for (i = 0; i < n; i++)
        {
            /* (voiced) */
            splash_progress(i, n, "%s (%s)", str(LANG_WAIT), str(LANG_OFF_ABORT));
            if (TIME_AFTER(current_tick, last_tick + HZ/4))
            {
                if (action_userabort(TIMEOUT_NOBLOCK))
                    break;
                last_tick = current_tick;
            }

            if (!action_cb(tagcache_retrieve(tcs, browser_db_get_entry(tc, i)->extraseek,
                                             tcs->type, buf, buf_sz) ? buf : NULL))
            {
                ret = false;
                break;
            }
            yield();
        }

        tagcache_search_finish(tcs);
    }
    else
    {
        splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
        ret = false;
    }
    reset_tc_to_prev(dirlevel, selected_item);
    cpu_boost(false);
    return ret;
}

/* Try to return first subentry's filename for current selection
 */
bool browser_db_get_subentry_filename(char *buf, size_t bufsize)
{
    int ret = true;
    int dirlevel = tc->dirlevel;
    int selected_item = tc->selected_item;
    int newtable = browser_db_get_entry(tc, tc->selected_item)->newtable;

    if (!goto_allsubentries(newtable) || browser_db_get_filename(tc, buf, bufsize) < 0)
        ret = false;

    reset_tc_to_prev(dirlevel, selected_item);
    return ret;
}

bool browser_db_current_playlist_insert(int position, bool queue)
{
    return browser_db_insert_selection(position, queue, NULL, false);
}


int browser_db_add_to_playlist(const char* playlist, bool new_playlist)
{
    if (!new_playlist)
        browser_db_load(tc); /* because display_playlists was called */
    return browser_db_insert_selection(0, false, playlist, new_playlist) ? 0 : -1;
}

static int browser_db_play_folder(struct browser_context* c)
{
    logf( "%s", __func__);
    /* c->selected_item indexes the displayed list, whose head is the special
     * rows (<Random> on a track list). insert_all_playlist() drops those --
     * their extraseek doesn't resolve to a track -- so playlist position is the
     * display index less the special rows, and the same correction applies to
     * the track count used for the rescale below. */
    int start_index = c->selected_item - c->special_entry_count;
    if (start_index < 0)
        start_index = 0;

    if (playlist_create(NULL, NULL) < 0)
    {
        logf("Failed creating playlist\n");
        return -1;
    }

    if (!insert_all_playlist(c, NULL, false, PLAYLIST_INSERT_LAST, false))
        return -2;

    int n = c->filesindir - c->special_entry_count;
    bool has_playlist_been_randomized = n > playlist_get_current()->max_playlist_size;
    if (has_playlist_been_randomized)
    {
        /* We need to recalculate the start index based on a percentage to put the user
        around its desired start position and avoid out of bounds */

        int percentage_start_index = 100 * start_index / n;
        start_index = percentage_start_index * playlist_get_current()->amount / 100;
    }

    if (global_settings.playlist_shuffle)
    {
        start_index = playlist_shuffle(current_tick, start_index);
        if (!global_settings.play_selected)
            start_index = 0;
    }

    playlist_start(start_index, 0, 0);
    loaded_entries_crc = browser_db_data_crc(c); /* save crc in case we return */
    return 0;
}

static struct tagentry* browser_db_get_entry(struct browser_context *c, int id)
{
    struct tagentry *entry;
    int realid = id - current_offset;

    /* Load the next chunk if necessary. */
    if (realid >= current_entry_count || realid < 0)
    {
        cpu_boost(true);
        if (retrieve_entries(c, MAX(0, id - (current_entry_count / 2)),
                             false) < 0)
        {
            logf("retrieve failed");
            cpu_boost(false);
            return NULL;
        }
        realid = id - current_offset;
        cpu_boost(false);
    }

    entry = get_entries(c);
    return &entry[realid];
}

char* browser_db_get_entry_name(struct browser_context *c, int id,
                                    char* buf, size_t bufsize)
{
    struct tagentry *entry = browser_db_get_entry(c, id);
    if (!entry)
        return NULL;

    unsigned char *name = entry->name;

    int lang_id = P2ID(name);
    logf("%s: '%s' id: %d\n", __func__,
         P2STR(name), lang_id);
    if (lang_id >= 0)
    {
        strmemccpy(buf,P2STR(name), bufsize);
        return entry->name;
    }

    strmemccpy(buf, entry->name, bufsize);
    return buf;
}


char *browser_db_get_title(struct browser_context* c)
{
    switch (c->currtable)
    {
        case TABLE_ROOT:
            logf("%s (ROOT) %s", __func__,  P2STR(menu->title));
            return P2STR(menu->title);

        case TABLE_NAVIBROWSE:
        case TABLE_ALLSUBENTRIES:
        case TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS:
            logf("%s (NAVI/ALLSUB) idx: %d %s", __func__,
                 c->currextra, current_title[c->currextra]);
            return current_title[c->currextra];
    }

    return "?";
}

/* True when this browse level is listing albums -- i.e. the rows that can carry
 * album art, and so the ones worth giving a taller row to. Kept here rather than
 * exposing csi/tagorder to callers. */
bool browser_db_is_album_list(struct browser_context* c)
{
    if (c->currtable != TABLE_NAVIBROWSE || !csi)
        return false;
    if (c->currextra < 0 || c->currextra >= csi->tagorder_count)
        return false;
    return csi->tagorder[c->currextra] == tag_album;
}

/* True when this browse level is listing artists -- the rows that can carry
 * artist art, and so the ones worth a taller row. Covers every tag the default
 * menus group artists by: canonicalartist (the "Artist" menu and the artist
 * sublevels under Genre/Year), plus album-artist and plain artist. */
bool browser_db_is_artist_list(struct browser_context* c)
{
    int tag;
    if (c->currtable != TABLE_NAVIBROWSE || !csi)
        return false;
    if (c->currextra < 0 || c->currextra >= csi->tagorder_count)
        return false;
    tag = csi->tagorder[c->currextra];
    return tag == tag_virt_canonicalartist ||
           tag == tag_albumartist ||
           tag == tag_artist;
}

/* Resolve browse row `item` to a folder on disk: ask the tagcache for the first
 * track filed under that row's selection (ancestor levels -- genre, artist, ...
 * -- still filtered, so the right entry wins when two share a name), then take
 * its filename and climb `extra_parents` directory levels above the track's own
 * folder. extra_parents == 0 yields the track's folder (the album); == 1 yields
 * its parent (the artist), for <artist>/<album>/<track> layouts. Those folder
 * paths are the keys the art thumbnail cache hashes on. False for a row with no
 * retrievable track, or one too shallow for the requested level.
 *
 * This is a tagcache search, i.e. far too expensive to call per row per redraw;
 * callers are expected to cache the result. */
static bool browser_db_get_track_dir(struct browser_context* c, int item,
                                  char *buf, int buflen, int extra_parents)
{
    struct tagcache_search tcs;
    char tcs_buf[TAGCACHE_BUFSZ];
    struct tagentry *entry;
    int level = c->currextra;
    bool ok = false;
    int i;

    entry = browser_db_get_entry(c, item);
    if (!entry)
        return false;

    if (!tagcache_search(&tcs, tag_filename))
        return false;

    for (i = 0; i < level; i++)
    {
        if (!TAGCACHE_IS_NUMERIC(csi->tagorder[i]))
            tagcache_search_add_filter(&tcs, csi->tagorder[i],
                                       csi->result_seek[i]);
    }
    tagcache_search_add_filter(&tcs, csi->tagorder[level], entry->extraseek);

    if (tagcache_get_next(&tcs, tcs_buf, sizeof(tcs_buf)) && tcs.result)
    {
        /* Copy first, then truncate the COPY. tcs.result can point into shared
         * tagcache memory, so writing a NUL into it would corrupt the cache. */
        char *sep = NULL;
        strlcpy(buf, tcs.result, buflen);
        for (i = 0; i <= extra_parents; i++)
        {
            sep = strrchr(buf, '/');
            if (!sep || sep == buf)
            {
                sep = NULL;     /* too shallow to climb to this folder level */
                break;
            }
            *sep = '\0';        /* strip one component: no trailing slash */
        }
        ok = (sep != NULL);
    }

    tagcache_search_finish(&tcs);
    return ok;
}

/* The album folder of browse row `item` -- the album-art cache key. */
bool browser_db_get_album_dir(struct browser_context* c, int item,
                           char *buf, int buflen)
{
    if (!browser_db_is_album_list(c) || item < c->special_entry_count)
        return false;
    return browser_db_get_track_dir(c, item, buf, buflen, 0);
}

/* The artist folder of browse row `item` -- the parent of the album folder for
 * <artist>/<album>/<track> layouts, i.e. the artist-art cache key. */
bool browser_db_get_artist_dir(struct browser_context* c, int item,
                            char *buf, int buflen)
{
    if (!browser_db_is_artist_list(c) || item < c->special_entry_count)
        return false;
    return browser_db_get_track_dir(c, item, buf, buflen, 1);
}

int browser_db_get_attr(struct browser_context* c)
{
    int attr = -1;
    switch (c->currtable)
    {
        case TABLE_NAVIBROWSE:
            if (csi->tagorder[c->currextra] == tag_title
                || csi->tagorder[c->currextra] == tag_virt_basename)
                attr = FILE_ATTR_AUDIO;
            else
                attr = ATTR_DIRECTORY;
            break;

        case TABLE_ALLSUBENTRIES:
        case TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS:
            attr = FILE_ATTR_AUDIO;
            break;

        default:
            attr = ATTR_DIRECTORY;
            break;
    }

    return attr;
}

int browser_db_get_icon(struct browser_context* c)
{
    int icon = Icon_Folder;

    if (browser_db_get_attr(c) == FILE_ATTR_AUDIO)
        icon = Icon_Audio;

    return icon;
}

