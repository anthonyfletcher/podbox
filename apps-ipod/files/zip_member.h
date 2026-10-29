/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to zip_member.c.
 ****************************************************************************/
#ifndef _ZIP_MEMBER_H
#define _ZIP_MEMBER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Working memory zip_member_to_file() needs: the inflater's window and
 * tables, and the file stream's buffer */
#define ZIP_MEMBER_WORK (48 * 1024)

/* The deflated member of `zip` whose data starts at `at`, `csize` bytes
 * stored and `usize` unpacked, written whole to `out`, with `work` as the
 * inflater's memory. False, and no file, if it could not be. */
bool zip_member_to_file(const char *zip, long at, uint32_t csize,
                        uint32_t usize, const char *out, void *work,
                        size_t work_size);

#endif /* _ZIP_MEMBER_H */
