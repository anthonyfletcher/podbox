/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Reading the on-device explanations. See settings_help.h for the file format
 * and why the text is not lang strings.
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "rbpaths.h"
#include "string-extra.h"
#include "system/strutil.h"      /* open_utf8, read_line */
#include "settings_help.h"

#define HELP_FILE ROCKBOX_DIR "/docs/settings-help.txt"

/* Longest line the file may hold. Stanzas are prose meant to fit a 320px
 * screen, so this is generous; anything longer is truncated rather than
 * refused. */
#define HELP_LINE 160

bool settings_help_lookup(const char *key, char *buf, size_t bufsz)
{
    char line[HELP_LINE];
    char want[HELP_LINE];
    int fd;
    bool found = false;
    size_t used = 0;

    if (!key || !buf || bufsz == 0)
        return false;

    buf[0] = '\0';

    fd = open_utf8(HELP_FILE, O_RDONLY);
    if (fd < 0)
        return false;

    snprintf(want, sizeof want, "[%s]", key);

    while (read_line(fd, line, sizeof line) > 0)
    {
        if (line[0] == '[')
        {
            /* A second heading ends the stanza we were collecting. */
            if (found)
                break;
            found = (strcasecmp(line, want) == 0);
            continue;
        }

        if (!found)
            continue;

        /* Lines within a paragraph are joined, and a blank line breaks one.
         *
         * The file is wrapped so it can be read and edited, and view_text wraps
         * again for the screen -- so keeping the file's line breaks would put a
         * break in the middle of a sentence wherever the two disagreed. The
         * file has to stay wrapped rather than hold a paragraph per line,
         * because read_line() truncates at HELP_LINE and would drop the tail of
         * a long one without saying so. */
        size_t len = strlen(line);
        bool blank = (len == 0);

        if (used + len + 2 > bufsz)
            break;

        if (blank)
        {
            buf[used++] = '\n';
        }
        else
        {
            memcpy(buf + used, line, len);
            used += len;
            buf[used++] = ' ';
        }
        buf[used] = '\0';
    }

    close(fd);

    /* A heading with nothing under it is the same as no heading at all. */
    return found && used > 0;
}
