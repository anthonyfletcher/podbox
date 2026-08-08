/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * Interface to pv_names.c.
 ****************************************************************************/
#ifndef _PV_NAMES_H
#define _PV_NAMES_H

#include <stdbool.h>
#include <stddef.h>

/* Longest artist, album or title kept. Names are compared and hashed in this
 * truncated form -- a longer name hashed in full would never match its own
 * stored entry and would spawn a fresh aggregate on every play. */
#define PV_NAME_MAX 40

/* Build the path -> metadata map into the bottom of 'buf' and return how many
 * bytes of it were taken. The caller's own allocations start above that.
 *
 * Consults the saved map first and only sweeps the database when the saved
 * one does not match it, so this is usually a single file read. A sweep puts
 * a progress splash up, because on a spinning disk it is not quick.
 *
 * Returns 0 when there is no usable database, which is not an error: every
 * path then resolves by filename guesswork instead. */
size_t pv_names_init(void *buf, size_t bufsz);

/* Where a name came from. Worth knowing beyond curiosity: if nothing on a
 * device with a database ever comes back PV_NAME_DB, the logged paths and
 * the database's disagree in form, and the artwork cache -- which keys off
 * the same strings -- is missing everything too. */
enum pv_name_src
{
    PV_NAME_PATH,
    PV_NAME_DB,
    PV_NAME_LOG     /* the log carried the name itself; nothing resolved it */
};

/* Artist and title for a logged path, and the album when it is known.
 *
 * 'album' comes back empty unless the database named one, since a path does
 * not carry an album; the caller decides what to do about that. The return
 * value describes the artist and title, not the album.
 *
 * All three buffers must hold PV_NAME_MAX bytes. */
enum pv_name_src pv_names_resolve(const char *path, char *artist,
                                      char *title, char *album);

/* What the map is made of: entries the database holds, and how many of them
 * were mapped. Equal numbers mean the whole database is covered.
 *
 * 'swept' (may be NULL) says whether the last init rebuilt the map from the
 * database rather than reading the saved one back -- which is the difference
 * between a first run and every run after it, and worth knowing before
 * drawing conclusions from how long it took. */
void pv_names_info(int *db_entries, int *mapped, bool *swept);

#endif /* _PV_NAMES_H */
