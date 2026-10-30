/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to ts_io_core.c.
 ****************************************************************************/
#ifndef TS_IO_CORE_H
#define TS_IO_CORE_H

#include "txt_source.h"

struct ts_core_file {
    int      fd;
    bs_off_t pos;                            /* -1 when unknown */
};

/* Opens `path` and fills `io`. `f` must stay valid until ts_close(), which
 * takes ownership of the fd and closes it. Returns BS_OK or BS_ERR_IO. */
int ts_io_core(bs_io *io, struct ts_core_file *f, const char *path);

/* When set, told the file position after every read: how far a long read
 * has got, for a progress bar */
extern void (*ts_io_progress)(bs_off_t pos);

#endif /* TS_IO_CORE_H */
