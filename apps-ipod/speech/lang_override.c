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
 * on screen, so it stays English under a translation. A phrase that shares its
 * English text with another (there are 18 such pairs) resolves to the lower id
 * and the other cannot be reached; naming phrases by their LANG_ id instead is
 * a later stage.
 *
 * Text that never passes through str() has no id and so cannot be overridden.
 * The splash() calls with literal strings are the ones users will notice.
 ****************************************************************************/

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
        char *english, *replacement;
        int id, size;

        if (!settings_parseline(line, &english, &replacement))
            continue;

        id = lang_english_to_id(english);
        if (id < 0)
        {
            DEBUGF("Override names no phrase: %s\n", english);
            continue;
        }

        size = strlen(replacement) + 1;
        if (size > spare_left)
        {
            DEBUGF("Overrides do not fit the language buffer\n");
            break;
        }

        memcpy(spare, replacement, size);
        language_strings[id] = spare;
        spare += size;
        spare_left -= size;
    }

    close(fd);
}
