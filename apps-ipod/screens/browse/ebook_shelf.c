/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The Books screen: every EPUB on the player by its title, with its cover,
 * and above them the three lists the Audiobooks shelf has -- In progress,
 * Not started and Finished.
 *
 * Two sources. The book index (files/book_index.c) names every book, its
 * title and its chapter count; the reader's places (reading.dat) say which
 * books have been opened, where each was left and which were read to the
 * end. A book with a place is in progress unless it is finished; a book with
 * none is not started.
 *
 * Choosing a book opens it in the reader, at its place. Holding Select on a
 * book offers Mark as, which moves it to any of the three lists.
 *
 * Parts, in order:
 *   - the arena and what it holds
 *   - collecting
 *   - the lists
 *   - the way in
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "lang.h"
#include "strnatcmp.h"
#include "widgets/list.h"
#include "widgets/menu.h"
#include "widgets/splash.h"
#include "settings/settings.h"
#include "system/activity.h"
#include "system/app_buffer.h"
#include "files/book_index.h"
#include "metadata/art_cache.h"
#include "root_menu.h"
#include "screens/browse/browser.h"      /* browser_art_row */
#include "screens/browse/book_shelf.h"   /* enum book_shelf */
#include "viewers/text_viewer/text_viewer.h"
#include "ebook_shelf.h"

/* ------------------------------------------------------------------ *
 * the arena                                                          *
 * ------------------------------------------------------------------ */

#define EBOOKS_MAX 1024

struct ebook
{
    uint32_t path;              /* into the names */
    uint32_t title;             /* the book's own, or its file name */
    int16_t  parts;
    int16_t  pct;               /* how far through, or -1 */
    uint8_t  state;             /* enum book_shelf */
};

/* The app buffer, claimed while the screen is up: the books, the rows, and
 * the strings above them */
static struct ebook *books;
static int *rows;
static char *names;
static size_t names_sz, names_used;
static int book_ct, row_ct;

/* -1 for the top level: the three lists' rows, then every book */
static int showing;

/* The top level's list rows, as the Audiobooks list has them: those with a
 * book in, in this order */
static const struct
{
    int lang_id;
    enum book_shelf kind;
} top_row[] = {
    { LANG_BOOK_SHELF_ROW_FINISHED, BOOK_SHELF_FINISHED },
    { LANG_BOOK_SHELF_ROW_NEW,      BOOK_SHELF_NOT_STARTED },
    { LANG_BOOK_SHELF_ROW_PROGRESS, BOOK_SHELF_IN_PROGRESS },
};
static int top_kinds[3];
static int top_ct;

static bool claim(void)
{
    size_t sz, fixed = EBOOKS_MAX * (sizeof(*books) + sizeof(*rows));
    char *p = app_claim_buffer(&sz, "books");

    if (p == NULL || sz < fixed + MAX_PATH)
    {
        if (p != NULL)
            app_release_buffer("books");
        return false;
    }
    books = (struct ebook *)p;  p += EBOOKS_MAX * sizeof(*books);
    rows = (int *)p;            p += EBOOKS_MAX * sizeof(*rows);
    names = p;
    names_sz = sz - fixed;
    return true;
}

static void release(void)
{
    app_release_buffer("books");
}

static bool add_name(const char *s, uint32_t *at)
{
    size_t len = strlen(s) + 1;

    if (len > names_sz - names_used)
        return false;
    memcpy(names + names_used, s, len);
    *at = (uint32_t)names_used;
    names_used += len;
    return true;
}

/* ------------------------------------------------------------------ *
 * collecting                                                         *
 * ------------------------------------------------------------------ */

static bool collect_book(const struct book_entry *e, void *data)
{
    struct ebook *b = &books[book_ct];
    const char *title = e->title;
    (void)data;

    if (!title[0])
    {
        title = strrchr(e->path, '/');
        title = title ? title + 1 : e->path;
    }
    if (!add_name(e->path, &b->path) || !add_name(title, &b->title))
        return false;
    b->parts = e->parts;
    b->pct = -1;
    b->state = BOOK_SHELF_NOT_STARTED;
    return ++book_ct < EBOOKS_MAX;
}

static bool collect_place(const char *path, int part, bool finished,
                          void *data)
{
    (void)data;
    for (int i = 0; i < book_ct; i++)
    {
        struct ebook *b = &books[i];

        if (strcmp(names + b->path, path))
            continue;
        b->state = finished ? BOOK_SHELF_FINISHED : BOOK_SHELF_IN_PROGRESS;
        if (!finished && part >= 0 && b->parts > 0)
            b->pct = MIN(part * 100 / b->parts, 99);
        break;
    }
    return true;
}

static int compare_rows(const void *a, const void *b)
{
    return strnatcasecmp(names + books[*(const int *)a].title,
                         names + books[*(const int *)b].title);
}

/* Everything read afresh, and the rows of the list `showing` */
static void load(void)
{
    book_ct = row_ct = 0;
    names_used = 0;
    book_index_each(collect_book, NULL);
    text_viewer_each_place(collect_place, NULL);
    int counts[3] = { 0, 0, 0 };

    for (int i = 0; i < book_ct; i++)
    {
        counts[books[i].state]++;
        if (showing < 0 || books[i].state == showing)
            rows[row_ct++] = i;
    }
    qsort(rows, row_ct, sizeof(*rows), compare_rows);

    top_ct = 0;
    for (size_t k = 0; showing < 0 && k < ARRAYLEN(top_row); k++)
        if (counts[top_row[k].kind] > 0)
            top_kinds[top_ct++] = k;
}

/* ------------------------------------------------------------------ *
 * the lists                                                          *
 * ------------------------------------------------------------------ */

static int top_rows(void)
{
    return showing < 0 ? top_ct : 0;
}

static int list_lang(int which)
{
    switch (which)
    {
        case BOOK_SHELF_IN_PROGRESS: return LANG_BOOKS_IN_PROGRESS;
        case BOOK_SHELF_NOT_STARTED: return LANG_BOOKS_NOT_STARTED;
        default:                     return LANG_BOOKS_FINISHED;
    }
}

/* The book on list row n, or -1 for one of the three lists' rows */
static int book_at(int n)
{
    n -= top_rows();
    return n >= 0 && n < row_ct ? rows[n] : -1;
}

static const char *get_name(int n, void *data, char *buffer, size_t len)
{
    int b = book_at(n);
    (void)data;

    if (b < 0)
        return str(top_row[top_kinds[n]].lang_id);
    if (books[b].state != BOOK_SHELF_IN_PROGRESS || books[b].pct < 0)
        return names + books[b].title;
    snprintf(buffer, len, str(LANG_BOOK_SHELF_ROW), names + books[b].title,
             books[b].pct);
    return buffer;
}

static unsigned int art_hash(int n, void *data)
{
    int b = book_at(n);
    (void)data;

    return b < 0 ? 0 : art_cache_dir_hash(names + books[b].path);
}

static const struct bitmap *get_art(int n, void *data, struct dim *size,
                                    const struct img_filter *filter,
                                    uint32_t filter_hash)
{
    return browser_art_row(n, data, art_hash, size, filter, filter_hash);
}

/* %Lr: how far through an In progress book, for a theme's gauge */
static int get_range(int n, void *data)
{
    int b = book_at(n);
    (void)data;

    return b < 0 || books[b].state != BOOK_SHELF_IN_PROGRESS
           ? -1 : books[b].pct;
}

static enum list_row_kind get_kind(int n, void *data)
{
    (void)data;
    return book_at(n) < 0 ? LIST_ROW_COMMAND : LIST_ROW_FILE;
}

/* Mark as..., on the book held; true if it moved */
static bool mark_menu(int b)
{
    static const int marks[] = {
        [BOOK_SHELF_IN_PROGRESS] = TV_MARK_IN_PROGRESS,
        [BOOK_SHELF_NOT_STARTED] = TV_MARK_NOT_STARTED,
        [BOOK_SHELF_FINISHED]    = TV_MARK_FINISHED,
    };
    char path[MAX_PATH];
    int choice = books[b].state;

    MENUITEM_STRINGLIST(menu, ID2P(LANG_BOOK_MARK_AS), NULL,
                        ID2P(LANG_BOOKS_IN_PROGRESS),
                        ID2P(LANG_BOOKS_NOT_STARTED),
                        ID2P(LANG_BOOKS_FINISHED));
    push_current_activity(ACTIVITY_CONTEXTMENU);
    choice = do_menu(&menu, &choice, NULL, false);
    if (get_current_activity() == ACTIVITY_CONTEXTMENU)
        pop_current_activity();
    if (choice < 0 || choice >= (int)ARRAYLEN(marks)
        || choice == books[b].state)
        return false;
    strmemccpy(path, names + books[b].path, sizeof(path));
    return text_viewer_mark_place(path, marks[choice]);
}

/* The row a mark was made on, which ends the list for a reload */
static int marked_row;

static int action_cb(int action, struct gui_synclist *lists)
{
    int n, b;

    if (action != ACTION_STD_CONTEXT)
        return action;
    n = gui_synclist_get_sel_pos(lists);
    b = book_at(n);
    if (b < 0 || !mark_menu(b))
        return ACTION_REDRAW;
    marked_row = n;
    return ACTION_STD_CANCEL;
}

/* ------------------------------------------------------------------ *
 * the way in                                                         *
 * ------------------------------------------------------------------ */

/* One list, until it is left or something is chosen: a book, its path in
 * `path`; one of the three lists, its enum book_shelf in *list. A GO_TO_*
 * code, GO_TO_PREVIOUS unless the list went to the root. */
static int show(char *path, size_t size, int *selection, int *list)
{
    struct simplelist_info info;
    int ret = GO_TO_PREVIOUS;

    path[0] = '\0';
    *list = -1;
    do
    {
        marked_row = -1;
        load();
        if (top_rows() + row_ct == 0)
        {
            splash(HZ * 2, ID2P(LANG_BOOK_SHELF_EMPTY));
            return GO_TO_PREVIOUS;
        }
        simplelist_info_init(&info, str(showing < 0 ? LANG_EBOOKS
                                                    : list_lang(showing)),
                             top_rows() + row_ct, NULL);
        info.get_name = get_name;
        info.get_kind = get_kind;
        info.get_range = get_range;
        browser_albumart_invalidate();
        browser_set_showing_art(global_settings.db_bookart);
        if (global_settings.db_bookart)
        {
            info.get_albumart = get_art;
            info.row_height = global_settings.db_art_row_height;
        }
        info.action_callback = action_cb;
        info.selection = MIN(*selection, top_rows() + row_ct - 1);

        if (simplelist_show_list(&info))
            ret = GO_TO_ROOT;
        else if (info.selection >= 0 && marked_row < 0)
        {
            int b = book_at(info.selection);

            *selection = info.selection;
            if (b >= 0)
                strmemccpy(path, names + books[b].path, size);
            else
                *list = top_row[top_kinds[info.selection]].kind;
        }
        if (marked_row >= 0)
            *selection = marked_row;
    } while (marked_row >= 0);

    browser_albumart_invalidate();
    browser_set_showing_art(false);
    return ret;
}

int ebook_shelf_run(void)
{
    static char path[MAX_PATH];
    int top_sel = 0, list_sel = 0;
    int ret, list;

    push_current_activity(ACTIVITY_BOOKSHELF);
    showing = -1;
    while (1)
    {
        if (!claim())
        {
            splash(HZ * 2, ID2P(LANG_OUT_OF_MEMORY));
            ret = GO_TO_PREVIOUS;
            break;
        }
        ret = show(path, sizeof(path), showing < 0 ? &top_sel : &list_sel,
                   &list);
        release();

        if (path[0])
        {
            /* The reader takes the whole screen and the buffer back */
            ret = text_viewer(path);
            if (ret == GO_TO_ROOT)
                break;
            continue;
        }
        if (ret == GO_TO_ROOT)
            break;
        if (list >= 0)
        {
            showing = list;
            list_sel = 0;
            continue;
        }
        if (showing >= 0)
        {
            showing = -1;               /* back to the top level */
            continue;
        }
        break;
    }
    pop_current_activity();
    return ret == GO_TO_ROOT ? GO_TO_ROOT : GO_TO_PREVIOUS;
}
