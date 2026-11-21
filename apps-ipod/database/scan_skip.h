/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The names every walk of the disk passes by: the database's scan, the file
 * index (Images, Books, Documents, Games) and Sound Scan on a computer. One
 * rule, so a folder is in all of them or none.
 ****************************************************************************/

#ifndef _SCAN_SKIP_H
#define _SCAN_SKIP_H

#include <stdbool.h>
#include <ctype.h>
#include "string-extra.h"

/* "." and "..", macOS's ._ files, names hidden by a dot and a letter
 * (.rockbox, .Trashes, .Trash-1000, .Spotlight-V100), a macOS archive's
 * __MACOSX, and the Windows recycle bins at depth 0, the top of the walk. A
 * dot before anything else is a real name and is kept: "...And Justice for
 * All", ".38 Special". */
static inline bool scan_skips_name(const char *name, int depth)
{
    if (name[0] == '.'
        && (name[1] == '\0' || name[1] == '_'
            || isalpha((unsigned char)name[1])
            || (name[1] == '.' && name[2] == '\0')))
        return true;
    if (!strcasecmp(name, "__MACOSX"))
        return true;
    return depth == 0 && (!strcasecmp(name, "$RECYCLE.BIN")
                          || !strcasecmp(name, "RECYCLED"));
}

#endif /* _SCAN_SKIP_H */
