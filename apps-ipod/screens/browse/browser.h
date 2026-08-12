/***************************************************************************
 * Original code from RockBox
 * was: apps/tree.h
 * Copyright (C) 2002 Daniel Stenberg
 * GNU General Public License (version 2+)
 *
 * Interface to browser.c: browser_context, the browse_context entry point
 * (rockbox_browse) and the dirfilter values.
 ****************************************************************************/
#ifndef _BROWSER_H_
#define _BROWSER_H_

#include <stdbool.h>
#include "system/applimits.h"
#include <file.h>
#include "config.h"
#include "draw/icon.h"

/* keep this struct compatible (total size and name member)
 * with struct browser_db_entry (browser_db.h) */
struct entry {
    char *name;
    int attr; /* FAT attributes + file type flags */
    unsigned time_write; /* Last write time */
    int customaction; /* db use */
    char* album_name; /* db use */
    int idx_id; /* db use */
};

#define BROWSE_SELECTONLY       0x0001  /* exit on selecting a file */
#define BROWSE_NO_CONTEXT_MENU  0x0002  /* disable context menu */
#define BROWSE_RUNFILE          0x0004  /* open the file with its viewer instead of browsing */
#define BROWSE_DIRFILTER        0x0080  /* override global_settings.dirfilter with browse_context.dirfilter */
#define BROWSE_SELECTED         0x0100  /* this bit is set if user selected item */


struct browser_context;

struct browser_cache {
    /* A big buffer with plenty of entry structs, contains all files and dirs
     * in the current dir (with filters applied)
     * Note that they're buflib-allocated and can therefore possibly move
     * They need to be locked if used around yielding functions */
    int     entries_handle;         /* handle to the entry cache */
    int     name_buffer_handle;     /* handle to the name cache */
    int     max_entries;            /* Max entries in the cache */
    int     name_buffer_size;       /* in bytes */
};

struct browse_context {
    int dirfilter;
    unsigned flags;             /* ored BROWSE_* */
    bool (*callback_show_item)(char *name, int attr, struct browser_context *tc);
                                /* callback function to determine to show/hide
                                   the item for custom browser */
    char *title;                /* title of the browser. if set to NULL,
                                   directory name is used. */
    enum themable_icons icon;   /* title icon */
    const char *root;           /* full path of start directory */
    const char *selected;       /* name of selected file in the root */
    char *buf;                  /* buffer to store selected file */
    size_t bufsize;             /* size of the buffer */
};

/* browser context for file or db */
struct browser_context {
    /* The directory we are browsing */
    char currdir[MAX_PATH];
    /* the number of directories we have crossed from / */
    int dirlevel;
    /* The currently selected file/id3dbitem index (old dircursor+dirfile) */
    int selected_item;
    /* The selected item in each directory crossed
     * (used when we want to return back to a previouws directory)*/
    int selected_item_history[MAX_DIR_LEVELS];

    int *dirfilter; /* file use */
    int filesindir; /* The number of files in the dircache */
    int dirsindir; /* file use */
    int dirlength; /* total number of entries in dir, incl. those not loaded */
    int currtable; /* db use */
    int currextra; /* db use */
    int special_entry_count; /* db use */
    int sort_dir; /* directory sort order */
    int out_of_tree; /* shortcut from elsewhere */
    struct browser_cache cache;
    bool dirfull;
    bool is_browsing; /* valid browse context? */

    struct browse_context *browse;
};

/*
 * Call one of the two below after yields since the entrys may move inbetween */
struct entry* browser_get_entries(struct browser_context *t);
/* returns NULL on invalid index */
struct entry* browser_get_entry_at(struct browser_context *t, int index);

void browser_mem_init(void) INIT_ATTR;
void browser_init(void) INIT_ATTR;
char* get_current_file(char* buffer, size_t buffer_len);
void set_dirfilter(int l_dirfilter);
void set_current_file(const char *path);
int rockbox_browse(struct browse_context *browse);
int create_playlist(void);
void resume_directory(const char *dir);

void browser_lock_cache(struct browser_context *t);
void browser_unlock_cache(struct browser_context *t);

#ifdef WIN32
/* it takes an int there, and mingw's own declaration must not be contradicted */
#define getcwd_size_t int
#else
#define getcwd_size_t size_t
#endif
char *getcwd(char *buf, getcwd_size_t size);
void reload_directory(void);
bool check_rockboxdir(void);
struct browser_context* browser_get_context(void);
void browser_flush(void);
void browser_restore(void);

/* Throw away the cached row thumbnails. They are filtered as they are read,
 * so a change to the artwork filter leaves the ones already in the cache
 * showing the old treatment. Unlike the browser's, the carousel's slide cache
 * lives in a buffer released when its screen exits, so it has nothing to
 * invalidate -- it reads every slide again on the way in. */
void browser_albumart_invalidate(void);
/* True while the on-screen browse level draws tall album/artist art rows. */
bool browser_showing_art(void);

bool bookmark_play(char* resume_file, int index, unsigned long elapsed,
                   unsigned long offset, int seed, char *filename);

#endif
