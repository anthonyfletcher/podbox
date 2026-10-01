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

/* Load the moved-folder table into the bottom of 'buf', matching folders
 * again first when the database has changed since it was saved, and return
 * how many bytes it took. The caller's own allocations start above that.
 *
 * Names themselves take no buffer: they come from the database's path index.
 * Returns 0 when there is no usable database, which is not an error: every
 * path then resolves by filename guesswork instead. */
size_t pv_names_init(void *buf, size_t bufsz);

/* Delete the moved-folder table, so the next pv_names_init() matches folders
 * again. */
void pv_names_discard(void);

/* A value that changes whenever the names a path resolves to could: the
 * database's entry count and commit id, the moved-folder table, and whether
 * the database is in RAM. 0 when there is no usable database. Cheap, so a
 * cache of resolved names can be checked against it before deciding to build
 * one. */
unsigned long pv_names_identity(void);

/* Whether names come from the database whenever it has them, now and for the
 * rest of the session: it is in RAM, or there is none, or its RAM copy is
 * switched off. False while the RAM copy is still to load, when a path the
 * database holds is named from its filename. */
bool pv_names_complete(void);

/* Where a logged file is now. 'path' itself unless the database does not know
 * it and its folder is one the moved-folder table has matched; then the path in
 * the new folder, in a static buffer valid until the next call. Resolve names
 * and artwork from what this returns, not from the logged path. */
const char *pv_names_locate(const char *path);

/* Where a name came from. Worth knowing beyond curiosity: if nothing on a
 * device with a database in RAM ever comes back PV_NAME_DB, the logged paths
 * and the database's disagree in form. */
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

/* Entries the database holds, and how many of them the path index can name:
 * equal numbers when it is in RAM, 0 mapped when it is not. */
void pv_names_info(int *db_entries, int *mapped);

#endif /* _PV_NAMES_H */
