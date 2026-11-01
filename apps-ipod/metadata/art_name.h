/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to art_name.c: the key a person's picture is cached by.
 ****************************************************************************/

#ifndef _ART_NAME_H_
#define _ART_NAME_H_

#include <stdbool.h>
#include <stddef.h>

/* The cache key of the person called 'name': its letters and digits, Latin
 * ones lowercased and unaccented, "&" read as "and", a leading "The " or
 * trailing ", The" dropped, hashed. 0 for NULL or a name with nothing left,
 * which matches nothing. A folder name and a tag for the same person, written
 * either way, give the same key. */
unsigned int art_name_hash(const char *name);

/* The same for the first 'len' bytes of 'name', such as one component of a
 * path or a file name without its extension. */
unsigned int art_name_hash_n(const char *name, size_t len);

/* The name read with the part after its one comma first: "Pratchett, Terry"
 * as "Terry Pratchett". 0 when the name has no comma or more than one. A
 * folder or file matches a person when either key of one equals either key
 * of the other. */
unsigned int art_name_alt_hash(const char *name);
unsigned int art_name_alt_hash_n(const char *name, size_t len);

/* The key of a work, "<artist> - <title>" as a file in /Artwork/Covers
 * would be named; the title's alone when there is no artist. 0 with no
 * title. */
unsigned int art_name_work_hash(const char *artist, const char *title);

#endif /* _ART_NAME_H_ */
