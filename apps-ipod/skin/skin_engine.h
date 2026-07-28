/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/skin_engine/skin_engine.h
 * Copyright (C) 2007 Nicolas Pennequin
 * Copyright (C) 2009 Jonathan Gordon
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * The skin engine's public interface: skin_update(), the skinnable_screens
 * enum, and the render-inhibit control.
 ****************************************************************************/

#ifndef _SKIN_ENGINE_H
#define _SKIN_ENGINE_H


#include "tag_table.h"
#include "draw/screen_access.h"

enum skinnable_screens {
    CUSTOM_STATUSBAR,
    WPS,

    SKINNABLE_SCREENS_COUNT
};

struct skin_stats;
struct skin_viewport;
struct gui_wps;


/* Do a update_type update of the skinned screen */
void skin_update(enum skinnable_screens skin, enum screen_type screen,
                 unsigned int update_type);

/* Note that a skin was drawn but not flushed to the screen */
void skin_mark_dirty(enum screen_type screen);
/* Take the pending-flush flag for a screen, clearing it */
bool skin_is_dirty(enum screen_type screen);
/* Hold off the flush while something else owns the screen */
void skin_inhibit_flush(bool inhibit);
bool skin_flush_inhibited(void);
/* Flush now -- for drawing done outside the action loop */
void skin_flush_dirty(void);
/* Redraw cost since boot; difference two readings for a rate. Renderers and
 * flushers outside skin_display.c report their own time through the note_
 * calls so the totals cover every path that repaints the screen. */
unsigned int skin_flush_count(void);
unsigned int skin_render_usec(void);
unsigned int skin_flush_usec(void);
void skin_note_render(unsigned int usec);
void skin_note_flush(unsigned int usec);

bool skin_has_sbs(struct gui_wps *gwps);


/* load a backdrop into the skin buffer.
 * reuse buffers if the file is already loaded */
char* skin_backdrop_load(char* backdrop, char *bmpdir, enum screen_type screen);
bool skin_backdrop_init(void);
int skin_backdrop_assign(char* backdrop, char *bmpdir,
                          enum screen_type screen);
bool skin_backdrops_preload(void);
void skin_backdrop_show(int backdrop_id);
void skin_backdrop_load_setting(void);
void skin_backdrop_unload(int backdrop_id);
#define BACKDROP_BUFFERNAME "#backdrop_buffer#"
void skin_backdrop_set_buffer(int backdrop_id, struct skin_viewport *svp);

/* do the button loop as often as required for the peak meters to update
 * with a good refresh rate.
 * gwps is really gwps[NB_SCREENS]! don't wrap this in FOR_NB_SCREENS()
 */
int skin_wait_for_action(enum skinnable_screens skin, int context, int timeout);

void skin_load(enum skinnable_screens skin, enum screen_type screen,
               const char *buf, bool isfile);
struct gui_wps *skin_get_gwps(enum skinnable_screens skin, enum screen_type screen);
void gui_sync_skin_init(void);


bool skin_do_full_update(enum skinnable_screens skin, enum screen_type screen);
void skin_request_full_update(enum skinnable_screens skin);
void skin_request_update_locked(bool locked);

bool dbg_skin_engine(void);

#endif
