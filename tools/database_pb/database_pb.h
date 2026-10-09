/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Shared between database_pb.c and its stubs.
 ****************************************************************************/
#ifndef _DATABASE_PB_H
#define _DATABASE_PB_H

/* -v given */
extern int database_pb_verbose;

/* Ends the progress line the stubs keep up to date during a scan */
void database_pb_progress_end(void);

#endif /* _DATABASE_PB_H */
