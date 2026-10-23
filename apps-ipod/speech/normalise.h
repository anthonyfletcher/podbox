/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to normalise.c: text to clauses of words for the synthesiser.
 ****************************************************************************/

#ifndef __NORMALISE_H
#define __NORMALISE_H

#include <stdbool.h>

/* Start on a UTF-8 string, which must outlive the clauses read from it. */
void normalise_begin(const char *utf8);

/* The next clause's words, lowercase and separated by single spaces, into
 * buf. *clause is its closing punctuation -- '.', ',', '?' or '!' -- or 0.
 * A clause too long for buf is ended early and carried on by the next
 * call. False once the text is used up. */
bool normalise_next(char *buf, int size, int *clause);

/* Scripts with no reading here, which are said by their names */
enum normalise_script
{
    SCRIPT_CHINESE = 0,
    SCRIPT_JAPANESE,
    SCRIPT_KOREAN,
    SCRIPT_ARABIC,
    SCRIPT_HEBREW,
    SCRIPT_THAI,
};

/* The first run in utf8 that normalise_next() would say as a script's name,
 * as [*start, *end), and which script; false if there is none. For a caller
 * that says the name itself -- from the language, which normalise_next()'s
 * English words are not. */
bool normalise_find_script(const char *utf8, const char **start,
                           const char **end, enum normalise_script *script);

#endif /* __NORMALISE_H */
