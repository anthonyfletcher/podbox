/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to lexicon.c: words to IPA for the speech synthesiser.
 ****************************************************************************/

#ifndef __LEXICON_H
#define __LEXICON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Use the dictionary loaded at data, the contents of cmudict.bin. False if
 * it is not one; words are then left to the letter-to-sound rules. */
bool lexicon_init(const void *data, size_t size);

/* True if the dictionary holds word (lowercase, len bytes). */
bool lexicon_has(const char *word, int len);

/* IPA for as many of words (lowercase, separated by single spaces) as fit
 * in max code points, words separated by spaces. Returns how many bytes of
 * words were used, so the caller can carry on from there; *len is set to
 * the code points written. */
size_t lexicon_ipa(const char *words, uint16_t *ipa, int max, int *len);

#endif /* __LEXICON_H */
