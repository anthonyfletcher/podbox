/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Every document, or every image, in one list.
 *
 * The file browser shows what is in a folder. These show what is on the
 * player, whichever folder it happens to sit in -- which for a handful of
 * documents scattered around a music library is the difference between
 * finding one and remembering where it was put.
 *
 * The list comes from the background walk in files/file_index.c; nothing is
 * scanned here. Opening is left to the core viewers, through the same
 * filetype dispatch the file browser uses, so a document opens exactly as it
 * would from anywhere else -- including resuming where it was left.
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include "config.h"
#include "string-extra.h"        /* strlcpy */
#include "file.h"                /* MAX_PATH */
#include "kernel.h"
#include "lang.h"
#include "settings/settings.h"   /* ID2P */
#include "widgets/list.h"
#include "widgets/splash.h"
#include "input/action.h"
#include "system/activity.h"
#include "files/file_index.h"
#include "files/path_list.h"
#include "files/filetypes.h"
#include "root_menu.h"
#include "browser_flat.h"

static struct path_list list;

/* The filename, not the path it sits at: the path is long, the row is narrow,
 * and the name is what is being looked for. */
static const char *flat_get_name(int selected_item, void *data,
                                 char *buffer, size_t buffer_len)
{
    (void)data; (void)buffer; (void)buffer_len;
    return path_list_leaf(&list, selected_item);
}

int browser_flat(bool images)
{
    struct simplelist_info info;
    int ret = GO_TO_PREVIOUS;
    char title[64];

    if (!path_list_load(&list, file_index_list(images), PATH_LIST_MAX))
    {
        /* No list yet and an empty one look the same from here: the walk runs
         * in the background and may simply not have happened. Say the neutral
         * thing rather than claiming there are none.
         *
         * The id is chosen before ID2P rather than inside it -- the macro does
         * arithmetic on its argument, so a conditional there is not the string
         * id it looks like. */
        int msg = file_index_is_busy() ? LANG_WAIT : LANG_FLAT_LIST_EMPTY;
        splash(HZ * 2, ID2P(msg));
        return GO_TO_PREVIOUS;
    }

    snprintf(title, sizeof(title), "%s (%d%s)",
             str(images ? LANG_IMAGES : LANG_DOCUMENTS),
             list.count, list.truncated ? "+" : "");

    push_current_activity(images ? ACTIVITY_IMAGEBROWSER
                                 : ACTIVITY_DOCUMENTBROWSER);

    while (1)
    {
        simplelist_info_init(&info, title, list.count, NULL);
        info.get_name = flat_get_name;

        simplelist_show_list(&info);

        if (info.selection < 0)
            break;                       /* backed out */

        {
            /* Copied out, not pointed at: the list is handed back below and
             * the path has to outlive it. */
            char path[MAX_PATH];
            int attr, rc;

            strlcpy(path, path_list_get(&list, info.selection), sizeof(path));
            attr = filetype_get_attr(path);

            /* The list is only as fresh as the last walk, so a file named in
             * it may be gone. */
            if (!file_exists(path))
            {
                splash(HZ * 2, ID2P(LANG_FILE_NOT_FOUND));
                continue;
            }

            /* Give the scratch buffer up before the viewer starts. Holding it
             * across the call would panic the moment the viewer wanted it, and
             * letting go also leaves the viewer a whole free buflib pool --
             * the image viewer takes the largest contiguous block it can find,
             * so this is the difference between opening a big picture from
             * here and from the file browser. */
            path_list_free(&list);

            if (filetype_open_core_viewer(attr, path, &rc))
            {
                /* A viewer that returns anywhere other than back here is
                 * going somewhere real (the root on a USB session, say). */
                if (rc != GO_TO_PREVIOUS)
                {
                    ret = rc;
                    break;
                }
            }

            /* Read again rather than remembering: the background walk may have
             * run while the viewer was up, so this is the current list, not
             * the one we let go of. It can also have become empty. */
            if (!path_list_load(&list, file_index_list(images), PATH_LIST_MAX))
                break;

            snprintf(title, sizeof(title), "%s (%d%s)",
                     str(images ? LANG_IMAGES : LANG_DOCUMENTS),
                     list.count, list.truncated ? "+" : "");
        }
    }

    pop_current_activity();
    path_list_free(&list);
    return ret;
}
