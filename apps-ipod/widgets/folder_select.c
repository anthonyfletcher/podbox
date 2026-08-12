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
 * excluded. Each row carries a checkbox saying which it is, and a chevron if
 * there is anything under it to open.
 * GNU General Public License (version 2+)
 *
 * Tri-state folder picker used by the database "Directories to Scan" setting
 * and the custom autoresume folder list (both in screens/settings/
 * general_settings.c). Walks the filesystem into a collapsible tree and
 * serialises the minimal include/exclude path set back into a setting string.
 *
 * A dialog rather than a themed list, and for the reason the state glyphs are
 * this file's own bitmaps: a skinned list draws an icon only if the theme maps
 * it, and `show icons` off draws none at all -- either of which left the only
 * copy of "is this folder included?" undrawn.
 *
 * Parts, in order:
 *   - the tree: allocation, loading, inheritance, path serialisation
 *   - the dialog: measure, draw, actions
 ****************************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "string-extra.h"    /* strlcpy, strlcat */
#include "kernel.h"   /* HZ */
#include "system.h"          /* ALIGN_UP */
#include "system/app_buffer.h"  /* the tree is built in the scratch buffer */
#include "crc32.h"
#include "dir.h"
#include "file.h"            /* MAX_PATH */
#include "lang.h"
#include "settings/settings.h"
#include "splash.h"
#include "yesno.h"
#include "dialog.h"
#include "input/action.h"
#include "draw/screen_access.h"
#include "system/activity.h"  /* push/pop the folder-picker activity (%cs) */
#include "system/shutdown.h"  /* default_event_handler */
/*#define LOGF_ENABLE*/
#include "logf.h"
#include "folder_select.h"

/* The "bitmaps/" prefix is required, not cosmetic: dependencies are generated
 * with -MG, so a header that does not exist yet is recorded at the path as
 * written. Only this spelling matches the rule bitmaps.make gives it. */
#include "bitmaps/podbox_folder_closed_off.h"
#include "bitmaps/podbox_folder_closed_on.h"
#include "bitmaps/podbox_folder_open_off.h"
#include "bitmaps/podbox_folder_open_on.h"
#include "bitmaps/podbox_folder_leaf_off.h"
#include "bitmaps/podbox_folder_leaf_on.h"

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
    bool probed;             /* has_subfolder() has answered for this one */
    bool has_children;       /* what it answered */
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
        child->probed = false;
        child->has_children = false;
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
    root_child.probed = false;
    root_child.has_children = false;

    static struct folder root = {
        .name = "",
        .children = &root_child,
        .children_count = 1,
        .depth = 0,
        .previous = NULL,
    };

    return &root;
}

/* Whether `this` holds at least one subdirectory. Answered once and cached.
 *
 * A row has to show whether it can be expanded before it ever has been, and
 * child_load() cannot be asked: it allocates the whole level, so calling it for
 * every visible row would build subtrees nobody opened, and -- because it
 * reports a full buffer the same way it reports an unreadable directory --
 * would start marking perfectly good folders (?) once the buffer filled.
 *
 * This allocates nothing and stops at the first subdirectory it sees. Called
 * from the draw path, so it costs one opendir per row the first time that row
 * appears and nothing on any later pass. */
static bool has_subfolder(struct child *this, struct folder *parent)
{
    char fullpath[MAX_PATH];
    DIR *dir;
    struct dirent *entry;
    size_t len;

    if (this->folder)          /* loaded: it counted them, so ask it */
        return this->folder->children_count > 0;
    if (this->probed)
        return this->has_children;

    this->probed = true;
    this->has_children = false;

    if (this->eaccess)         /* could not be opened, so nothing to expand */
        return false;

    len = get_full_path(parent, fullpath, sizeof(fullpath));
    if (len >= sizeof(fullpath))
        return false;
    strlcpy(&fullpath[len], this->name, sizeof(fullpath) - len);

    dir = opendir(fullpath);
    if (dir == NULL)
        return false;

    while ((entry = readdir(dir)))
    {
        char *dn = entry->d_name;
        if ((dir_get_info(dir, entry).attribute & ATTR_DIRECTORY) == 0)
            continue;
        if ((dn[0] == '.') && (dn[1] == '\0' || (dn[1] == '.' && dn[2] == '\0')))
            continue;
        this->has_children = true;
        break;
    }
    closedir(dir);
    return this->has_children;
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

/* ---- the dialog --------------------------------------------------------- */

struct folder_dlg {
    struct folder *root;
    int  selected;          /* index into the flattened tree */
    int  top_row;           /* first visible row */
    int  visible_rows;      /* set by measure(), read by draw() and on_action() */

    /* Marquee for the selected row, driven from the poll tick rather than the
     * shared scroll engine -- see dialog_draw_button_ex(). */
    int  scroll_px;
    int  scroll_max;        /* what the last draw said overflows */
    int  scroll_dir;        /* +1 running left, -1 coming back */
    int  scroll_wait;       /* polls to hold still at each end */
    int  scroll_row;        /* the row the marquee belongs to */
};

/* How often the dialog loop wakes with ACTION_NONE. Fast while the marquee
 * runs, idle otherwise: every wake repaints the whole box. */
#define POLL_ANIM     (HZ/20)
#define POLL_IDLE     (HZ/5)
#define SCROLL_STEP   3                 /* pixels per poll */
#define SCROLL_PAUSE  (HZ / POLL_ANIM)  /* about a second at each end */

#define BOX_W_PCT     92   /* box width, as a percentage of the display */
#define BOX_MARGIN_Y  10   /* clearance from the top and bottom */
#define BOX_PAD        6   /* inside the border, all round */
#define MIN_ROWS       3   /* however large the font, show at least this many */
#define ROW_PAD_Y      3
#define ROW_GAP        2   /* so adjacent rows read as separate */
#define INDENT_PX      8   /* per tree level */

static int row_height(struct screen *display)
{
    return display->getcharheight() + 2 * ROW_PAD_Y;
}

/* The row's state, as a bitmap this file owns rather than an iconset entry.
 *
 * That is the whole point of the dialog: with the state in a themable icon, a
 * theme that does not map it -- or `show icons` turned off, which draws no icon
 * at all -- left the picker a folder tree with nothing saying what was in it.
 * These are drawn by dialog_draw_button_ex() in the label's colour, so they
 * still follow the theme without depending on it.
 *
 * A folder with nothing under it gets no chevron -- only the checkbox, in the
 * same column, so the rows still line up. Without that every leaf advertises an
 * expansion that does nothing when you press Select.
 *
 * Indexed by expanded<<1 | included, with the leaf pair past the end, so the
 * array order is load-bearing. */
static const struct bitmap *row_icon(struct child *this, struct folder *parent)
{
    static const struct bitmap * const icons[6] = {
        &bm_podbox_folder_closed_off, &bm_podbox_folder_closed_on,
        &bm_podbox_folder_open_off,   &bm_podbox_folder_open_on,
        &bm_podbox_folder_leaf_off,   &bm_podbox_folder_leaf_on,
    };
    int state = has_subfolder(this, parent) ? (this->expanded ? 2 : 0) : 4;

    return icons[state | (effective_included(this, parent) ? 1 : 0)];
}

/* The name, plus the marker for a folder that could not be opened. The tree
 * depth is not spelled here: the row is drawn indented instead, which survives
 * a proportional font where leading spaces do not. */
static void row_text(struct child *this, char *buf, size_t buf_len)
{
    strlcpy(buf, this->name, buf_len);
    if (this->eaccess)
        strlcat(buf, " (?)", buf_len);
}

/* Size the box to a whole number of rows rather than a percentage, so the
 * bottom of the box is the bottom of a row at every font size. */
static void folder_measure(struct dialog *d, struct screen *display,
                           struct viewport *box, void *data)
{
    struct folder_dlg *s = data;
    struct dialog_insets in;
    int row_h = row_height(display);
    int chrome, limit, rows, box_h;

    dialog_get_insets(&d->style, &in);
    chrome = in.top + in.bottom;

    /* One row short of what fits, so the box reads as something over the
     * settings menu rather than as a screen of its own. */
    limit = display->lcdheight - 2 * BOX_MARGIN_Y;
    rows = (limit - chrome + ROW_GAP) / (row_h + ROW_GAP);
    if (rows > MIN_ROWS)
        rows--;
    if (rows < MIN_ROWS)
        rows = MIN_ROWS;

    box_h = chrome + rows * (row_h + ROW_GAP) - ROW_GAP;
    if (box_h > display->lcdheight)
        box_h = display->lcdheight;

    /* draw() reads this rather than re-deriving it, so a rounding difference
     * cannot leave half a row visible. */
    s->visible_rows = rows;

    box->width = display->lcdwidth * BOX_W_PCT / 100;
    box->height = box_h;
    box->x = (display->lcdwidth - box->width) / 2;
    box->y = (display->lcdheight - box_h) / 2;
}

static void folder_draw(struct dialog *d, struct screen *display,
                        struct viewport *content, void *data)
{
    struct folder_dlg *s = data;
    int row_h = row_height(display);
    int nb_items = count_items(s->root);
    char buf[MAX_PATH];

    /* No title inside the box: it cost a row and a rule to repeat what the
     * menu row just said. dialog_init() is still given one, so a theme that
     * draws a header has something to draw. */
    if (s->selected < s->top_row)
        s->top_row = s->selected;
    else if (s->selected >= s->top_row + s->visible_rows)
        s->top_row = s->selected - s->visible_rows + 1;
    if (s->top_row > nb_items - s->visible_rows)
        s->top_row = nb_items - s->visible_rows;
    if (s->top_row < 0)
        s->top_row = 0;

    /* The marquee belongs to one row; moving the selection starts it over.
     * Done here rather than at every place the selection changes, so no path
     * can leave the new row mid-scroll. */
    if (s->scroll_row != s->selected)
    {
        s->scroll_row = s->selected;
        s->scroll_px = 0;
        s->scroll_max = 0;
        s->scroll_dir = 1;
        s->scroll_wait = SCROLL_PAUSE;
    }

    for (int r = 0; r < s->visible_rows; r++)
    {
        int item = s->top_row + r;
        struct folder *parent;
        struct child *this;
        bool sel;
        int indent;

        if (item >= nb_items)
            break;
        this = find_index(s->root, item, &parent);
        if (this == NULL)
            break;

        /* Capped so a deeply nested tree cannot indent a row off the right
         * edge and leave it with no width to draw in. */
        indent = parent->depth * INDENT_PX;
        if (indent > content->width / 2)
            indent = content->width / 2;

        sel = (item == s->selected);
        row_text(this, buf, sizeof(buf));

        dialog_draw_button_ex(display, &d->style, indent,
                              r * (row_h + ROW_GAP),
                              content->width - indent, row_h, buf, sel,
                              DIALOG_BTN_LEFT, row_icon(this, parent),
                              sel ? s->scroll_px : 0,
                              sel ? &s->scroll_max : NULL);
    }

    display->set_drawmode(DRMODE_SOLID);
}

static int folder_on_action(struct dialog *d, int action, void *data)
{
    struct folder_dlg *s = data;
    struct folder *parent;
    struct child *this;

    switch (action)
    {
        case ACTION_STD_PREV:
        case ACTION_STD_PREVREPEAT:
            if (s->selected > 0)
                s->selected--;
            break;

        case ACTION_STD_NEXT:
        case ACTION_STD_NEXTREPEAT:
            if (s->selected + 1 < count_items(s->root))
                s->selected++;
            break;

        case ACTION_STD_OK:            /* expand / collapse */
            this = find_index(s->root, s->selected, &parent);
            if (this && !this->eaccess && child_load(this, parent) != NULL
                && this->folder->children_count > 0)
            {
                this->expanded = !this->expanded;
            }
            /* Always consumed. A folder with no subfolders (or one that could
             * not be opened) has nothing to expand, but Select must never fall
             * through -- as a list action that closed the picker, and the trap
             * is the same here. Only Back exits; hold-Select toggles. */
            break;

        case ACTION_STD_CONTEXT:       /* include / exclude */
            this = find_index(s->root, s->selected, &parent);
            if (this)
                this->sel = effective_included(this, parent) ? SEL_EXCLUDE
                                                             : SEL_INCLUDE;
            break;

        case ACTION_STD_CANCEL:
            return DIALOG_CANCEL;

        case ACTION_STD_MENU:
        case ACTION_STD_QUICKSCREEN:
        case ACTION_TREE_WPS:
            /* Swallowed: these all arrive from CONTEXT_LIST, and leaving for
             * the menu or the playing screen mid-edit discards the tree. */
            break;

        case ACTION_NONE:
            /* Only tick fast while the marquee is actually moving. Every pass
             * is a full repaint of the box -- the status bar renders into the
             * framebuffer under it on each get_action -- so at rest that is
             * pure waste. */
            d->poll_ticks = (s->scroll_max > 0) ? POLL_ANIM : POLL_IDLE;

            if (s->scroll_max > 0)
            {
                if (s->scroll_wait > 0)
                    s->scroll_wait--;
                else
                {
                    s->scroll_px += s->scroll_dir * SCROLL_STEP;
                    if (s->scroll_px >= s->scroll_max)
                    {
                        s->scroll_px = s->scroll_max;
                        s->scroll_dir = -1;
                        s->scroll_wait = SCROLL_PAUSE;
                    }
                    else if (s->scroll_px <= 0)
                    {
                        s->scroll_px = 0;
                        s->scroll_dir = 1;
                        s->scroll_wait = SCROLL_PAUSE;
                    }
                }
            }
            break;

        default:
            if (default_event_handler(action) == SYS_USB_CONNECTED)
                return DIALOG_ABORT;
            break;
    }
    return DIALOG_CONTINUE;
}

bool folder_select(char * header_text, char* setting, int setting_len)
{
    static const struct dialog_callbacks cb = {
        .measure   = folder_measure,
        .draw      = folder_draw,
        .on_action = folder_on_action,
    };
    struct folder_dlg s;
    struct dialog d;
    struct dialog_style style;
    size_t buf_size;
    bool changed = false;
    int res;

    /* Upstream took the plugin buffer here; this is the same region under the
     * name it has since the plugin system went (system/app_buffer.h).
     *
     * Not core_alloc_maximum(): the audio buffer holds the whole of core, so a
     * request from a screen shrinks it, and shrinking it stops playback and
     * rebuffers the current track -- and this is the largest request there is.
     * The block would also have to be pinned, since the tree built inside it
     * points at itself by address and the list loop below yields; a pinned
     * block is one buflib cannot compact across.
     *
     * Claimed rather than borrowed because it is held while the picker is on
     * screen. Nothing reachable from here wants it: the action callback only
     * expands, collapses and toggles rows, and opens no screen at all. */
    buffer_front = app_claim_buffer(&buf_size, "folder select");
    buffer_end = buffer_front + buf_size;
    logf("folder_select %d bytes free", (int)(buffer_end - buffer_front));
    s.root = load_root();

    logf("folders in: %s", setting);
    /* Load previous selection(s) */
    select_paths(s.root, setting);
    /* open the root so the top-level folders show right away */
    if (child_load(&s.root->children[0], s.root))
        s.root->children[0].expanded = true;
    /* get current hash to check for changes later */
    uint32_t hash = save_folders(s.root, hashed.buf, setting_len);

    s.selected = 0;
    s.top_row = 0;
    s.visible_rows = 1;
    s.scroll_row = -1;
    s.scroll_px = 0;
    s.scroll_max = 0;
    s.scroll_dir = 1;
    s.scroll_wait = SCROLL_PAUSE;

    /* Rows are buttons, but a tree of outlined boxes reads as clutter. An
     * unselected row is borderless and takes the box's own fill, so it is just
     * the glyph and the name; only the selected one becomes a shape, in the
     * accent. Same treatment as the database search dialog. */
    style = *dialog_get_default_style();
    style.box_margin = BOX_PAD;     /* the default 10 costs a row of height */
    style.button_border_width = 0;
    style.button_bg = DIALOG_COLOR_INHERIT;
    style.button_fg = DIALOG_COLOR_INHERIT;
    style.button_bg_selected = DIALOG_COLOR_ACCENT;
    style.button_fg_selected = DIALOG_COLOR_ON_ACCENT;

    /* Distinct activity so a theme can style the status bar behind the box
     * without colliding with the settings list it was opened from. The enum is
     * a skin ABI -- removing a member renumbers %cs for every theme -- so this
     * stays whether or not a theme uses it. */
    push_current_activity(ACTIVITY_FOLDERSELECT);
    dialog_init(&d, CONTEXT_LIST, header_text, &style, &cb, &s);
    res = dialog_run(&d, POLL_IDLE);
    pop_current_activity();

    logf("folder_select %d bytes free", (int)(buffer_end - buffer_front));
    /* done editing. check for changes -- but not after USB took the screen:
     * the tree is discarded then, and a yes/no put up now would run behind it */
    if (res != DIALOG_ABORT && hash != save_folders(s.root, hashed.buf, setting_len))
    {  /* prompt for saving changes and commit if yes */
        if (yesno_pop(ID2P(LANG_SAVE_CHANGES)))
        {
            save_folders(s.root, setting, setting_len);
            settings_save();
            logf("folders out: %s", setting);
            changed = true;
        }
    }

    app_release_buffer("folder select");
    return changed;
}
