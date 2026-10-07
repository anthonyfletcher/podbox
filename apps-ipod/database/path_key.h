/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to path_key.c: the one key that names a file.
 *
 * The sound index keys its records by it, the path index in tagcache finds a
 * database entry by it, and a car is given it as a track's ID. All three
 * outlive a build -- the sound analysis on disk, the car in its own cache --
 * so the value a path hashes to must never change.
 ****************************************************************************/

#ifndef _PATH_KEY_H
#define _PATH_KEY_H

#include <stdint.h>

/* FNV-1a as system/hash.h defines it, but 64-bit and written in path_key.c
 * rather than taken from there: that header says nothing may write its values
 * to disk and expect a later build to reproduce them. These go to disk, so the
 * arithmetic is pinned there with the standard constants. Do not "tidy" it
 * into hash.h.
 *
 * Sixty-four bits, not thirty-two, because thirty-two collide about once in
 * every three hundred libraries of five thousand tracks, and the symptom is
 * one track wearing another's identity for good.
 *
 * Folded to lower case on the way in. FAT does not distinguish case, so the
 * same file can come back differently cased and would otherwise key twice. */
uint64_t path_key_fold_hash(const char *s);

/* The portion of a path the key is taken from -- the same path with any
 * volume specifier removed, pointing into the caller's own string. Callers
 * that group tracks by where they sit need this too, or a path from one
 * tagcache call will not group with the same track's path from another. */
const char *path_key_strip(const char *path);

/* path_key_fold_hash() of path_key_strip(), never 0: zero is the "no key"
 * value, so a path that hashes to it is nudged to 1. */
uint64_t path_key(const char *path);

#endif /* _PATH_KEY_H */
