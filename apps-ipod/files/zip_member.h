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

/* How a member is stored, as a zip names it -- 0 as it is, 8 deflated --
 * and MEMBER_BASE64 for an FB2 picture, kept as base64 text: a number no
 * zip method takes, which the book index keeps in a byte */
#define MEMBER_STORED   0
#define MEMBER_DEFLATED 8
#define MEMBER_BASE64   255

/* Working memory zip_member_to_file() needs: the inflater's window and
 * tables, and the file stream's buffer */
#define ZIP_MEMBER_WORK (48 * 1024)

/* The member of `book` whose data starts at `at`, `csize` bytes stored and
 * `usize` unpacked (0 for a base64 member, which ends where its text does),
 * unpacked by `method` and written whole to `out`, with `work` as the
 * unpacker's memory. False, and no file, if it could not be. */
bool zip_member_to_file(const char *book, long at, uint32_t csize,
                        uint32_t usize, int method, const char *out,
                        void *work, size_t work_size);

#endif /* _ZIP_MEMBER_H */
