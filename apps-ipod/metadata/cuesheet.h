/***************************************************************************
 * Original code from RockBox
 * was: apps/cuesheet.h
 * Copyright (C) 2007 Nicolas Pennequin, Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * Interface to cuesheet.c and the cuesheet types.
 ****************************************************************************/

#ifndef _CUESHEET_H_
#define _CUESHEET_H_

#include <stdbool.h>
#include "file.h"
#include "metadata.h"

/* cue_draw_markers() takes a screen by pointer only, so a forward declaration
 * is enough and this header need not pull in screens.h. */
struct screen;

#define MAX_NAME 80    /* Max length of information strings */
#define MAX_TRACKS 99  /* Max number of tracks in a cuesheet */

struct cue_track_info {
    char title[MAX_NAME*3+1];
    char performer[MAX_NAME*3+1];
    char songwriter[MAX_NAME*3+1];
    unsigned long offset; /* ms from start of track */
};

struct cuesheet {
    char path[MAX_PATH];
    char file[MAX_PATH];
    char title[MAX_NAME*3+1];
    char performer[MAX_NAME*3+1];
    char songwriter[MAX_NAME*3+1];

    int track_count;
    struct cue_track_info tracks[MAX_TRACKS];

    int curr_track_idx;
    struct cue_track_info *curr_track;

    /* Set when the entries came from the file's own chapter marks rather
       than a cuesheet. The browser then lists one row per entry, the way a
       track list reads, since a chapter has no performer of its own. */
    bool chapters;

    /* Set to open the list with a Resume row, the way a multi-file book's
       track list does. Only a book reached from the shelf has one: a book
       already playing is where it was left. */
    bool resume_row;
};

struct cuesheet_file {
    char path[MAX_PATH];
    int size;
    off_t pos;
    enum character_encoding encoding;
};

/* looks if there is a cuesheet file with a name matching path of "track_id3" */
bool look_for_cuesheet_file(struct mp3entry *track_id3, struct cuesheet_file *cue_file);

/* parse cuesheet_file "cue_file" and store the information in "cue" */
bool parse_cuesheet(struct cuesheet_file *cue_file, struct cuesheet *cue);

/* reads a cuesheet to find the audio track associated to it */
bool get_trackname_from_cuesheet(char *filename, char *buf);

/* What browse_cuesheet() was left on. */
enum cue_browse_result {
    CUE_BROWSE_NONE = 0,   /* backed out of */
    CUE_BROWSE_PLAYED,     /* a chapter of the playing book was seeked to */
    CUE_BROWSE_RESUME,     /* the Resume row was chosen; the caller acts */
    CUE_BROWSE_START,      /* start the book at curr_track; the caller acts,
                              because how a book is played depends on how it
                              was reached and this screen does not know */
};

/* Display a cuesheet struct. Anything but NONE is the caller's cue to close
   the menu behind it and show the WPS. */
enum cue_browse_result browse_cuesheet(struct cuesheet *cue);

/* display a cuesheet file after parsing and loading it to the plugin buffer */
bool display_cuesheet_content(char* filename);

/* finds the index of the current track played within a cuesheet */
int cue_find_current_track(struct cuesheet *cue, unsigned long curpos);

/* skip to next track in the cuesheet towards "direction" (which is 1 or -1) */
bool curr_cuesheet_skip(struct cuesheet *cue, int direction, unsigned long curr_pos);

/* draw track markers on the progressbar */
void cue_draw_markers(struct screen *screen, struct cuesheet *cue,
                      unsigned long tracklen,
                      int x, int y, int w, int h);

/* check if the subtrack has changed */
bool cuesheet_subtrack_changed(struct mp3entry *id3);

#endif
