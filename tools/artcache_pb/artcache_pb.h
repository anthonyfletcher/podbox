/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Shared between artcache_pb.c and its stubs.
 ****************************************************************************/
#ifndef _ARTCACHE_PB_H
#define _ARTCACHE_PB_H

#include <stdbool.h>

/* -v given */
extern int artcache_pb_verbose;

/* --rebuild given */
extern bool artcache_pb_rebuild;

/* Ends the progress line the stubs keep up to date during a pass */
void artcache_pb_progress_end(void);

#endif /* _ARTCACHE_PB_H */
