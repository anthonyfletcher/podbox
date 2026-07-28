/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/folder_select.c
 * Copyright (C) 2012 Jonathan Gordon
 * Copyright (C) 2012 Thomas Martitz
 * Copyright (C) 2021 William Wilgus
 *
 * Core folder-tree picker, ported from the db_folder_select plugin and then
 * reworked: expansion and inclusion are independent. Scroll to move, Select to
 * expand/collapse a folder, hold Select to include/exclude it. Including a
 * folder includes all its subdirectories unless a subdirectory is individually
 * excluded. Included folders are shown bracketed, e.g. [Music]. Used by the
 * database "Directories to Scan" setting and the custom autoresume folder list
 * (both in apps/menus/settings_menu.c), which call folder_select() directly.
 * GNU General Public License (version 2+)
 *
 * Tri-state folder picker used by the database scan list and autoresume.
 * Walks the filesystem into a collapsible tree and serialises the minimal
 * include/exclude path set back into a setting string.
 ****************************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "string-extra.h"    /* strlcpy, strlcat */
#include "kernel.h"   /* HZ */
#include "system.h"          /* ALIGN_UP */
#include "core_alloc.h"
#include "crc32.h"
#include "dir.h"
#include "file.h"            /* MAX_PATH */
#include "lang.h"
#include "settings/settings.h"
#include "splash.h"
#include "yesno.h"
#include "list.h"
#include "draw/icon.h"        /* enum themable_icons, Icon_Last_Themeable */
#include "system/activity.h"  /* push/pop the folder-picker activity (%cs) */
/*#define LOGF_ENABLE*/
#include "logf.h"
#include "folder_select.h"

/* Inclusion is a tri-state: a folder inherits from its nearest explicitly
 * included/excluded ancestor (default: excluded). Selecting a folder to include
 * it therefore includes all descendants, until one is explicitly excluded. */
enum sel_state {
    SEL_INHERIT,
    SEL_INCLUDE,
    SEL_EXCLUDE,
};

struct child {
    char* name;
    struct folder *folder;   /* loaded sub-folder, NULL until first expand */
    enum sel_state sel;
    bool expanded;           /* children shown in the list */
    bool eaccess;            /* could not be opened */
};

struct folder {
    char *name;
    struct child *children;
    struct folder* previous;
    uint16_t children_count;
    uint16_t depth;
};

static char *buffer_front, *buffer_end;

static struct
{
    int32_t len; /* keeps count versus maxlen to give buffer full notification */
    uint32_t val; /* hash of all selected items */
    char buf[3];/* address used as identifier -- only \0 written to it */
    char maxlen_exceeded; /*0,1*/
} hashed;

static inline void get_hash(const char *key, uint32_t *hash, int len)
{
    *hash = crc_32(key, len, *hash);
}

static char* folder_alloc(size_t size)
{
    char* retval;
    /* 32-bit aligned */
    size = ALIGN_UP(size, 4);
    if (buffer_front + size > buffer_end)
    {
        return NULL;
    }
    retval = buffer_front;
    buffer_front += size;
    return retval;
}

static char* folder_alloc_from_end(size_t size)
{
    if (buffer_end - size < buffer_front)
    {
        return NULL;
    }
    buffer_end -= size;
    return buffer_end;
}

static size_t get_full_path(struct folder *start, char *dst, size_t dst_sz)
{
    size_t pos = 0;
    struct folder *prev, *cur = NULL, *next = start;
    dst[0] = '\0'; /* for strlcat to do its thing */
    /* First traversal R->L mutate nodes->previous to point at child */
    while (next->previous != NULL) /* stop at the root */
    {
#define PATHMUTATE()              \
        ({                        \
            prev = cur;           \
            cur = next;           \
            next = cur->previous;\
            cur->previous = prev; \
        })
        PATHMUTATE();
    }
    /*swap the next and cur nodes to reverse direction */
    prev = next;
    next = cur;
    cur = prev;
    /* Second traversal L->R mutate nodes->previous to point back at parent
     * copy strings to buf as they go by */
    while (next != NULL)
    {
        PATHMUTATE();
        pos = strlcat(dst, cur->name, dst_sz);
        /* do not append slash to paths starting with slash */
        if (cur->name[0] != '/')
            pos = strlcat(dst, "/", dst_sz);
    }
    logf("get_full_path: (%d)[%s]", (int)pos, dst);
    return pos;
#undef PATHMUTATE
}

/* support function for qsort() */
static int compare(const void* p1, const void* p2)
{
    struct child *left = (struct child*)p1;
    struct child *right = (struct child*)p2;
    return strcasecmp(left->name, right->name);
}

static struct folder* load_folder(struct folder* parent, char *folder)
{
    DIR *dir;
    char fullpath[MAX_PATH];

    struct dirent *entry;
    int child_count = 0;
    char *first_child = NULL;
    size_t len = 0;

    struct folder* this = (struct folder*)folder_alloc(sizeof(struct folder));
    if (this == NULL)
        goto fail;

    if (parent)
    {
        len = get_full_path(parent, fullpath, sizeof(fullpath));
        if (len >= sizeof(fullpath))
            goto fail;
    }
    strlcpy(&fullpath[len], folder, sizeof(fullpath) - len);
    logf("load_folder: [%s]", fullpath);

    dir = opendir(fullpath);
    if (dir == NULL)
        goto fail;
    this->previous = parent;
    this->name = folder;
    this->children = NULL;
    this->children_count = 0;
    if (parent)
        this->depth = parent->depth + 1;

    while ((entry = readdir(dir))) {
        /* skip anything not a directory */
        if ((dir_get_info(dir, entry).attribute & ATTR_DIRECTORY) == 0) {
            continue;
        }
        /* skip . and .. */
        char *dn = entry->d_name;
        if ((dn[0] == '.') && (dn[1] == '\0' || (dn[1] == '.' && dn[2] == '\0')))
            continue;
        /* copy entry name to end of buffer, save pointer */
        int len = strlen((char *)entry->d_name);
        char *name = folder_alloc_from_end(len+1); /*for NULL*/
        if (name == NULL)
        {
            closedir(dir);
            goto fail;
        }
        memcpy(name, (char *)entry->d_name, len+1);
        child_count++;
        first_child = name;
    }
    closedir(dir);
    /* now put the names in the array */
    this->children = (struct child*)folder_alloc(sizeof(struct child) * child_count);

    if (this->children == NULL)
        goto fail;

    while (child_count)
    {
        struct child *child = &this->children[this->children_count++];
        child->name = first_child;
        child->folder = NULL;
        child->sel = SEL_INHERIT;
        child->expanded = false;
        child->eaccess = false;
        while(*first_child++ != '\0'){};/* move to next name entry */
        child_count--;
    }
    qsort(this->children, this->children_count, sizeof(struct child), compare);

    return this;
fail:
    return NULL;
}

static struct folder* load_root(void)
{
    static struct child root_child;
    /* reset the root for each call */
    root_child.name = "/";
    root_child.folder = NULL;
    root_child.sel = SEL_INHERIT;
    root_child.expanded = false;
    root_child.eaccess = false;

    static struct folder root = {
        .name = "",
        .children = &root_child,
        .children_count = 1,
        .depth = 0,
        .previous = NULL,
    };

    return &root;
}

/* Load a child's sub-folder if not already loaded. Returns the folder or NULL
 * (marking eaccess on failure). */
static struct folder* child_load(struct child *this, struct folder *parent)
{
    if (this->folder == NULL && !this->eaccess)
    {
        this->folder = load_folder(parent, this->name);
        if (this->folder == NULL)
            this->eaccess = true;
    }
    return this->folder;
}

/* The child in `f`'s parent folder that owns `f` (its ->folder == f). */
static struct child* folder_owner(struct folder *f)
{
    struct folder *parent = f->previous;
    if (!parent)
        return NULL;
    for (int i = 0; i < parent->children_count; i++)
        if (parent->children[i].folder == f)
            return &parent->children[i];
    return NULL;
}

/* Whether `this` (a child of `parent`) is included, resolving inheritance up
 * the tree to the nearest explicit include/exclude. */
static bool effective_included(struct child *this, struct folder *parent)
{
    struct child *node = this;
    struct folder *f = parent;
    while (node)
    {
        if (node->sel == SEL_INCLUDE)
            return true;
        if (node->sel == SEL_EXCLUDE)
            return false;
        node = folder_owner(f);
        f = f->previous;
    }
    return false; /* nothing explicit up to the root: excluded */
}

static int count_items(struct folder *start)
{
    int count = 0;

    for (int i = 0; i < start->children_count; i++)
    {
        struct child *foo = &start->children[i];
        count++;
        if (foo->expanded && foo->folder)
            count += count_items(foo->folder);
    }
    return count;
}

static struct child* find_index(struct folder *start, int index, struct folder **parent)
{
    int i = 0;
    *parent = NULL;

    while (i < start->children_count)
    {
        struct child *foo = &start->children[i];
        if (i == index)
        {
            *parent = start;
            return foo;
        }
        i++;
        if (foo->expanded && foo->folder)
        {
            struct child *bar = find_index(foo->folder, index - i, parent);
            if (bar)
            {
                return bar;
            }
            index -= count_items(foo->folder);
        }
    }
    return NULL;
}

static const char * folder_get_name(int selected_item, void * data,
                                   char * buffer, size_t buffer_len)
{
    struct folder *root = (struct folder*)data;
    struct folder *parent;
    struct child *this = find_index(root, selected_item , &parent);

    if (this == NULL)
    {
        buffer[0] = '\0';
        return buffer;
    }

    /* two spaces of indent per tree level */
    size_t pos = 0;
    for (int i = 0; i < parent->depth && pos + 2 < buffer_len; i++)
    {
        buffer[pos++] = ' ';
        buffer[pos++] = ' ';
    }
    buffer[pos] = '\0';

    /* Inclusion is shown by the checkbox glyph (folder_get_icon), so the name
     * is drawn plain -- the [brackets] that used to mark an included folder
     * would now just duplicate it. */
    strlcpy(&buffer[pos], this->name, buffer_len - pos);

    if (this->eaccess)
        strlcat(buffer, " (?)", buffer_len);

    return buffer;
}

/* Per-row state handed to the theme through %LI: bit0 = included, bit1 =
 * expanded, so 0..3 cover the four expand/select combinations. Offset past the
 * themable-icon range (and clear of the file-type ids at +1/+2) so a theme
 * without a Folder_Select list config can't mistake it for a real iconset
 * entry. The Themify SBS maps these to its expand-arrow + checkbox glyphs. */
#define FOLDER_ICON_BASE (Icon_Last_Themeable + 8)

static enum themable_icons folder_get_icon(int selected_item, void * data)
{
    struct folder *root = (struct folder*)data;
    struct folder *parent;
    struct child *this = find_index(root, selected_item, &parent);

    if (this == NULL)
        return Icon_NOICON;

    int state = (this->expanded ? 2 : 0)
              | (effective_included(this, parent) ? 1 : 0);
    return (enum themable_icons)(FOLDER_ICON_BASE + state);
}

static int folder_action_callback(int action, struct gui_synclist *list)
{
    struct folder *root = (struct folder*)list->data;
    struct folder *parent;
    struct child *this = find_index(root, list->selected_item, &parent);

    if (this == NULL)
        return action;

    if (action == ACTION_STD_OK) /* expand / collapse */
    {
        if (!this->eaccess && child_load(this, parent) != NULL
            && this->folder->children_count > 0)
        {
            this->expanded = !this->expanded;
        }
        /* Always consume Select: a folder with no subfolders (or one that
         * couldn't be opened) has nothing to expand, but Select must never fall
         * through -- an unhandled ACTION_STD_OK closes the picker. Only Back
         * (ACTION_STD_CANCEL) exits; hold-Select (below) includes/excludes. */
        action = ACTION_REDRAW;
    }
    else if (action == ACTION_STD_CONTEXT) /* include / exclude */
    {
        this->sel = effective_included(this, parent) ? SEL_EXCLUDE : SEL_INCLUDE;
        action = ACTION_REDRAW;
    }

    if (action == ACTION_REDRAW)
        list->nb_items = count_items(root);
    return action;
}

static struct child* find_from_filename(const char* filename, struct folder *root)
{
    if (!root)
        return NULL;
    const char *slash = strchr(filename, '/');
    struct child *this;

    /* filenames beginning with a / are specially treated as the
     * loop below can't handle them. they can only occur on the first,
     * and not recursive, calls to this function.*/
    if (filename[0] == '/') /* in the loop nothing starts with '/' */
    {
        logf("find_from_filename [%s]", filename);
        /* filename begins with /. in this case root must be the
         * top level folder */
        this = &root->children[0];
        if (filename[1] == '\0')
        {   /* filename == "/" */
            return this;
        }
        else /* filename == "/XXX/YYY". cascade down */
            goto cascade;
    }

    for (int i = 0; i < root->children_count; i++)
    {
        this = &root->children[i];
        /* when slash == NULL n will be really large but \0 stops the compare */
        if (strncasecmp(this->name, filename, slash - filename) == 0)
        {
            if (slash == NULL)
            {   /* filename == XXX */
                return this;
            }
            else
                goto cascade;
        }
    }
    return NULL;

cascade:
    /* filename == XXX/YYY. cascade down: load and expand so the saved
     * selection is visible when the picker opens */
    if (child_load(this, root) && this->folder->children_count > 0)
        this->expanded = true;
    while (slash[0] == '/') slash++; /* eat slashes */
    return find_from_filename(slash, this->folder);
}

static int select_paths(struct folder* root, const char* filenames)
{
    /* Takes a list of filenames in a ':' delimited string
       splits filenames at the ':' character loads each into buffer
       selects each file in the folder list

       if last item or only item the rest of the string is copied to the buffer
       *End the last item WITHOUT the ':' character /.rockbox/eqs:/.rockbox/wps\0*
   */
    char buf[MAX_PATH];
    const int buflen = sizeof(buf);

    const char *fnp = filenames;
    const char *lastfnp = fnp;
    const char *sstr;
    off_t len;

    while (fnp)
    {
        fnp = strchr(fnp, ':');
        if (fnp)
        {
            len = fnp - lastfnp;
            fnp++;
        }
        else /* no ':' get the rest of the string */
            len = strlen(lastfnp);

        sstr = lastfnp;
        lastfnp = fnp;
        if (len <= 0 || len > buflen)
            continue;
        strlcpy(buf, sstr, len + 1);
        struct child *item = find_from_filename(buf, root);
        if (item)
            item->sel = SEL_INCLUDE;
    }

    return 0;
}

/* Whether any node in the (loaded part of the) subtree is explicitly excluded,
 * i.e. we can't collapse the whole subtree to its root path. */
static bool has_exclude(struct folder *f)
{
    for (int i = 0; i < f->children_count; i++)
    {
        struct child *this = &f->children[i];
        if (this->sel == SEL_EXCLUDE)
            return true;
        if (this->folder && has_exclude(this->folder))
            return true;
    }
    return false;
}

static void emit_path(struct folder *parent, const char *name,
                      char* dst, size_t maxlen, size_t buflen)
{
    size_t len = get_full_path(parent, buffer_front, buflen);
    if (len + strlen(name) + 2 >= buflen)
        return;
    len += snprintf(&buffer_front[len], buflen - len, "%s:", name);
    logf("emit_path: [%s]", buffer_front);
    if (dst != hashed.buf)
    {
        int dlen = strlen(dst);
        if (dlen + len >= maxlen)
            return;
        strlcpy(&dst[dlen], buffer_front, maxlen - dlen);
    }
    else
    {
        if (hashed.len + len >= maxlen)
        {
            hashed.maxlen_exceeded = 1;
            return;
        }
        get_hash(buffer_front, &hashed.val, len);
        hashed.len += len;
    }
}

/* Walk the tree writing the minimal set of paths whose recursive scan equals
 * the selection: a cleanly-included folder emits just its own path; one with
 * excluded descendants recurses so the included sub-parts are emitted instead. */
static void save_node(struct folder *f, char* dst, size_t maxlen, size_t buflen)
{
    for (int i = 0; i < f->children_count; i++)
    {
        struct child *this = &f->children[i];
        if (effective_included(this, f) &&
            (this->folder == NULL || !has_exclude(this->folder)))
            emit_path(f, this->name, dst, maxlen, buflen);
        else if (this->folder != NULL)
            save_node(this->folder, dst, maxlen, buflen);
    }
}

static uint32_t save_folders(struct folder *root, char* dst, size_t maxlen)
{
    hashed.len = 0;
    hashed.val = 0;
    hashed.maxlen_exceeded = 0;
    size_t len = buffer_end - buffer_front;
    dst[0] = '\0';
    save_node(root, dst, maxlen, len);
    len = strlen(dst);
    /* fix trailing ':' */
    if (len > 1) dst[len-1] = '\0';
    /*Notify - user will probably not see save dialog if nothing new got added*/
    if (hashed.maxlen_exceeded > 0) splash(HZ *2, ID2P(LANG_SHOWDIR_BUFFER_FULL));
    return hashed.val;
}

bool folder_select(char * header_text, char* setting, int setting_len)
{
    struct folder *root;
    struct simplelist_info info;
    size_t buf_size;
    bool changed = false;

    int buf_handle = core_alloc_maximum(&buf_size, NULL);
    if (buf_handle <= 0)
    {
        splash(HZ, "Out of memory");
        return false;
    }
    /* PIN IT -- same reason as image_viewer.c. NULL ops means buflib may move
     * this block freely, and we not only cache buffer_front/buffer_end but
     * build the whole folder tree inside it, with every node pointing at other
     * nodes by address. The list loop below yields, so a compaction while the
     * picker is open would dangle all of them at once. */
    core_pin(buf_handle);
    buffer_front = core_get_data(buf_handle);
    buffer_end = buffer_front + buf_size;
    logf("folder_select %d bytes free", (int)(buffer_end - buffer_front));
    root = load_root();

    logf("folders in: %s", setting);
    /* Load previous selection(s) */
    select_paths(root, setting);
    /* open the root so the top-level folders show right away */
    if (child_load(&root->children[0], root))
        root->children[0].expanded = true;
    /* get current hash to check for changes later */
    uint32_t hash = save_folders(root, hashed.buf, setting_len);
    simplelist_info_init(&info, header_text, count_items(root), root);
    info.get_name = folder_get_name;
    info.get_icon = folder_get_icon;
    info.action_callback = folder_action_callback;
    /* Distinct activity so the theme can style the picker (expand/select icons)
     * without colliding with the settings list it was opened from. */
    push_current_activity(ACTIVITY_FOLDERSELECT);
    simplelist_show_list(&info);
    pop_current_activity();
    logf("folder_select %d bytes free", (int)(buffer_end - buffer_front));
    /* done editing. check for changes */
    if (hash != save_folders(root, hashed.buf, setting_len))
    {  /* prompt for saving changes and commit if yes */
        if (yesno_pop(ID2P(LANG_SAVE_CHANGES)))
        {
            save_folders(root, setting, setting_len);
            settings_save();
            logf("folders out: %s", setting);
            changed = true;
        }
    }

    core_unpin(buf_handle);
    core_free(buf_handle);
    return changed;
}
