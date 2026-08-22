/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Renames built-in phrases from a text file, so a menu item can be relabelled
 * without rebuilding the firmware.
 *
 * The runtime's language model is already a patch table: language_strings[] is
 * one pointer per phrase, and loading a .lng overwrites individual entries and
 * leaves the rest built-in. An override is the same operation driven from a
 * config file --
 *
 *     Shuffle: Randomise
 *
 * finds the id whose built-in English text is "Shuffle" and points it at a
 * copy of "Randomise". Every %Sx() in a skin and every ID2P() in a menu picks
 * the new text up, because they all resolve to the same id.
 *
 * The key is matched against the built-in English blob, never against what is
 * on screen, so it stays English under a translation. Eighteen english strings
 * are shared by two phrases each; every one of them is renamed, which is
 * usually what was meant since they read the same on screen. Where it is not,
 * a "Date#2" suffix picks a single occurrence.
 *
 * Text that never passes through str() has no id and so cannot be overridden.
 * The splash() calls with literal strings are the ones users will notice.
 *
 * A handful of phrases are used as printf formats rather than as text, so a
 * replacement has to ask for the same arguments the built-in one does. That
 * is checked here -- see format_signature() -- and a line that fails it is
 * skipped rather than applied.
 ****************************************************************************/

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "file.h"
#include "debug.h"
#include "rbpaths.h"
#include "lang.h"

#include "language.h"
#include "lang_override.h"
#include "system/strutil.h"

#define OVERRIDE_FILE LANG_DIR "/overrides.cfg"

/* A phrase and its replacement share the line, so this bounds both. Longer
 * lines are read to their end and the tail discarded, as elsewhere. */
#define MAX_OVERRIDE_LINE 256

/* Conversion specifiers in one format string, in order: one entry per '%'
 * that consumes an argument, holding its length modifier and conversion
 * character. "%%" is a literal per cent and consumes nothing, so it
 * contributes none.
 *
 * Trap: some phrases are handed to splashf() and snprintf() as the format
 * rather than as text -- str(LANG_BUILDING_DATABASE) takes an int,
 * str(LANG_EQUALIZER_BAND) takes another. A replacement that drops or retypes
 * one of those makes the caller read the wrong thing off the stack, and
 * "%s" against an int is a data abort. This file is hand-edited and not
 * compiled by genlang, so the check has to live here.
 *
 * Sixteen is far more than any phrase in the tree carries; a replacement
 * needing more is refused by the truncation, which is the safe direction. */
#define SIG_MAX 16

static void format_signature(const char *s, char *sig)
{
    int n = 0;

    while (*s)
    {
        if (*s++ != '%')
            continue;
        if (*s == '\0')
            break;                  /* a trailing '%' converts nothing */
        if (*s == '%')
        {
            s++;                    /* a literal per cent, no argument */
            continue;
        }

        /* Flags, width and precision say nothing about the argument's type,
         * so a translation may set them freely. */
        while (*s && strchr("-+ #0123456789.*", *s))
            s++;
        while (*s && strchr("hlLzjt", *s))
        {
            if (n < SIG_MAX - 1)
                sig[n++] = *s;
            s++;
        }
        if (*s == '\0')
            break;
        if (n < SIG_MAX - 1)
            sig[n++] = *s;
        s++;
    }

    sig[n] = '\0';
}

/* Whether 'replacement' asks its caller for the same arguments 'english'
 * does. Word order is free -- rearranging the sentence around the numbers is
 * most of what a rename is for -- but the conversions themselves must match
 * in kind and in order, since Rockbox's printf has no positional form. */
static bool format_compatible(const char *english, const char *replacement)
{
    char a[SIG_MAX], b[SIG_MAX];

    format_signature(english, a);
    format_signature(replacement, b);
    return strcmp(a, b) == 0;
}

void lang_override_load(void)
{
    char line[MAX_OVERRIDE_LINE];
    unsigned char *spare;
    int spare_left;
    int fd = open_utf8(OVERRIDE_FILE, O_RDONLY);

    if (fd < 0)
        return;

    /* The replacements live in whatever is left of the language buffer after
     * the loaded .lng, so an English user has the whole of it and a Bulgarian
     * one has almost none. */
    spare = lang_spare_buffer(&spare_left);

    while (read_line(fd, line, sizeof line) > 0)
    {
        char *english, *replacement, *hash;
        unsigned char *stored = NULL;
        int id, size, wanted = 0, seen = 0;
        bool full = false;

        if (!settings_parseline(line, &english, &replacement))
            continue;

        /* "Date#2" is the second phrase reading "Date", for the pairs where
         * one replacement does not suit both. No phrase contains a '#', so
         * the suffix needs no escape. */
        hash = strrchr(english, '#');
        if (hash && isdigit((unsigned char)hash[1]))
        {
            wanted = atoi(hash + 1);
            *hash = '\0';
        }

        /* The 131 voice-only phrases have no english text, and an empty key
         * would name every one of them. */
        if (!*english)
            continue;

        /* The key is the built-in text, which is what a caller using it as a
         * format was written against. */
        if (!format_compatible(english, replacement))
        {
            DEBUGF("Override changes the arguments of: %s\n", english);
            continue;
        }

        for (id = lang_english_to_id(english); id >= 0;
             id = lang_english_to_id_from(english, id + 1))
        {
            if (wanted && ++seen != wanted)
                continue;

            /* One copy however many phrases point at it. */
            if (!stored)
            {
                size = strlen(replacement) + 1;
                if (size > spare_left)
                {
                    full = true;
                    break;
                }
                memcpy(spare, replacement, size);
                stored = spare;
                spare += size;
                spare_left -= size;
            }

            language_strings[id] = stored;
        }

        if (full)
        {
            DEBUGF("Overrides do not fit the language buffer\n");
            break;
        }

        if (!stored)
            DEBUGF("Override names no phrase: %s\n", english);
    }

    close(fd);
}
