/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The Books screen: every EPUB, FB2 and MOBI on the player, reached through
 * Search, the three lists the Audiobooks shelf has -- Finished, In progress
 * and Not started, the first two the last read first -- and By Title, By
 * Author and By Subject. A book that
 * names no author is filed under its folder; one may name several subjects,
 * and is listed under each.
 *
 * Two sources. The book index (files/book_index.c) names every book, its
 * title, author, subjects and chapter count; the reader's places
 * (reading.dat) say which books have been opened, where each was left and
 * which were read to the end. A book with a place is in progress unless it
 * is finished; a book with none is not started.
 *
 * Choosing a book opens it in the reader, at its place. Holding Select on a
 * book offers Mark As, which moves it to any of the three lists, Book Info
 * and Show in Files.
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
#include "widgets/search_dialog.h"
#include "widgets/splash.h"
#include "settings/settings.h"
#include "screens/system/db_search.h"  /* the search limits */
#include "system/activity.h"
#include "system/app_buffer.h"
#include "files/book_index.h"
#include "metadata/art_cache.h"
#include "metadata/art_name.h"
#include "root_menu.h"
#include "screens/context_menu.h"    /* the Book Menu's hidden rows */
#include "screens/browse/browser.h"      /* browser_art_row */
#include "screens/browse/book_shelf.h"   /* enum book_shelf */
#include "viewers/text_viewer/text_viewer.h"
#include "viewers/text_viewer/txt_source.h"     /* TS_BOOK_AUTHOR */
#include "viewers/text_viewer/reading_stats.h"
#include "bitmaps/podbox_icon_artist.h"
#include "bitmaps/podbox_icon_book.h"
#include "ebook_shelf.h"

/* ------------------------------------------------------------------ *
 * the arena                                                          *
 * ------------------------------------------------------------------ */

#define EBOOKS_MAX 1024
/* A row is a book, or in By Subject one of a book's subjects */
#define ROWS_MAX   (EBOOKS_MAX * TS_BOOK_SUBJECT_MAX)

struct ebook
{
    uint32_t path;              /* into the names */
    uint32_t title;             /* the book's own, or its file name */
    uint32_t author;            /* the book's own, or its folder's name */
    uint32_t subjects;          /* the first of nsubjects, one after another */
    int16_t  pct;               /* how far through, or -1 */
    uint8_t  state;             /* enum book_shelf */
    uint8_t  nsubjects;
    uint16_t recent;            /* 0 the last read; UINT16_MAX never read */
};

/* The app buffer, claimed while the screen is up: the books, the rows, and
 * the strings above them */
static struct ebook *books;
static int *rows;
static char *names;
static size_t names_sz, names_used;
static int book_ct, row_ct;

/* What the list shows: the top level's rows alone; one of the three lists
 * by its enum book_shelf; every book by title; every author, or the books of
 * author_shown; or every subject, or the books of subject_shown. Search is
 * the search box, over every book. */
#define SHOW_TOP      (-1)
#define SHOW_TITLES   3
#define SHOW_AUTHORS  4
#define SHOW_AUTHOR   5
#define SHOW_SUBJECTS 6
#define SHOW_SUBJECT  7
#define SHOW_SEARCH   8
#define SHOW_STATS    9
static int showing;
static char author_shown[TS_BOOK_AUTHOR];
static char subject_shown[TS_BOOK_SUBJECTS];
static char search_text[SEARCH_MAX_QUERY + 1];
static char select_path[MAX_PATH];  /* a book to put the cursor on, or "" */

/* The top level's rows: those with a book to show, in this order */
static const struct
{
    int lang_id;
    int kind;                   /* enum book_shelf, or a SHOW_ */
} top_row[] = {
    { LANG_DB_SEARCH,               SHOW_SEARCH },
    { LANG_BOOK_SHELF_ROW_FINISHED, BOOK_SHELF_FINISHED },
    { LANG_BOOK_SHELF_ROW_PROGRESS, BOOK_SHELF_IN_PROGRESS },
    { LANG_BOOK_SHELF_ROW_NEW,      BOOK_SHELF_NOT_STARTED },
    { LANG_BY_TITLE,                SHOW_TITLES },
    { LANG_BY_AUTHOR,               SHOW_AUTHORS },
    { LANG_BY_SUBJECT,              SHOW_SUBJECTS },
    { LANG_READING_STATS,           SHOW_STATS },
};
static int top_kinds[ARRAYLEN(top_row)];
static int top_ct;

static bool claim(void)
{
    size_t sz, fixed = EBOOKS_MAX * sizeof(*books) + ROWS_MAX * sizeof(*rows);
    char *p = app_claim_buffer(&sz, "books");

    if (p == NULL || sz < fixed + MAX_PATH)
    {
        if (p != NULL)
            app_release_buffer("books");
        return false;
    }
    books = (struct ebook *)p;  p += EBOOKS_MAX * sizeof(*books);
    rows = (int *)p;            p += ROWS_MAX * sizeof(*rows);
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

/* The name of the folder `path` is in, or <Untagged> at the top */
static void folder_name(const char *path, char *out, size_t size)
{
    const char *end = strrchr(path, '/'), *start = end;

    while (start > path && start[-1] != '/')
        start--;
    if (start == NULL || start == end)
    {
        strmemccpy(out, str(LANG_TAGNAVI_UNTAGGED), size);
        return;
    }
    strmemccpy(out, start, MIN(size, (size_t)(end - start) + 1));
}

/* The book's subjects, one line each, into the names one after another */
static bool add_subjects(struct ebook *b, const char *s)
{
    char one[TS_BOOK_SUBJECTS];
    uint32_t at;

    b->nsubjects = 0;
    while (*s && b->nsubjects < TS_BOOK_SUBJECT_MAX)
    {
        const char *nl = strchr(s, '\n');
        size_t n = nl ? (size_t)(nl - s) : strlen(s);

        strmemccpy(one, s, MIN(n + 1, sizeof(one)));
        if (one[0] >= 'a' && one[0] <= 'z')
            one[0] -= 'a' - 'A';    /* "prose" is listed as "Prose" */
        if (!add_name(one, &at))
            return false;
        if (b->nsubjects++ == 0)
            b->subjects = at;
        s += n + (nl != NULL);
    }
    return true;
}

static bool collect_book(const struct book_entry *e, void *data)
{
    struct ebook *b = &books[book_ct];
    const char *title = e->title;
    char folder[TS_BOOK_AUTHOR], file[MAX_PATH];
    (void)data;

    /* No title, or a tool's placeholder for one: the file's name */
    if (!title[0] || !strcasecmp(title, "unknown")
        || !strcasecmp(title, "unknown title"))
    {
        const char *name = strrchr(e->path, '/');
        char *dot;

        strmemccpy(file, name ? name + 1 : e->path, sizeof(file));
        dot = strrchr(file, '.');
        if (dot && dot != file)
            *dot = '\0';
        title = file;
    }
    if (!e->author[0])
        folder_name(e->path, folder, sizeof(folder));
    if (!add_name(e->path, &b->path) || !add_name(title, &b->title)
        || !add_name(e->author[0] ? e->author : folder, &b->author)
        || !add_subjects(b, e->subjects))
        return false;
    b->pct = -1;
    b->state = BOOK_SHELF_NOT_STARTED;
    b->recent = UINT16_MAX;
    return ++book_ct < EBOOKS_MAX;
}

/* The places come newest first, so 'data', counting them, is how recently
 * each was read */
static bool collect_place(const char *path, int pct, bool finished,
                          void *data)
{
    int *order = data;

    (*order)++;
    for (int i = 0; i < book_ct; i++)
    {
        struct ebook *b = &books[i];

        if (strcmp(names + b->path, path))
            continue;
        b->state = finished ? BOOK_SHELF_FINISHED : BOOK_SHELF_IN_PROGRESS;
        b->recent = MIN(*order - 1, UINT16_MAX - 1);
        if (!finished)
            b->pct = MIN(pct, 99);
        break;
    }
    return true;
}

static int compare_rows(const void *a, const void *b)
{
    return strnatcasecmp(names + books[*(const int *)a].title,
                         names + books[*(const int *)b].title);
}

static int compare_recent(const void *a, const void *b)
{
    int c = books[*(const int *)a].recent - books[*(const int *)b].recent;

    return c ? c : compare_rows(a, b);
}

static int compare_authors(const void *a, const void *b)
{
    int c = strnatcasecmp(names + books[*(const int *)a].author,
                          names + books[*(const int *)b].author);

    return c ? c : compare_rows(a, b);
}

/* By Subject's rows are the subjects themselves, as offsets into the names */
static int compare_subjects(const void *a, const void *b)
{
    return strnatcasecmp(names + *(const int *)a, names + *(const int *)b);
}

static bool has_subject(const struct ebook *b, const char *subject)
{
    const char *s = names + b->subjects;

    for (int k = 0; k < b->nsubjects; k++, s += strlen(s) + 1)
        if (!strcasecmp(s, subject))
            return true;
    return false;
}

static bool shown(const struct ebook *b)
{
    switch (showing)
    {
        case SHOW_TOP:     return false;
        case SHOW_TITLES:
        case SHOW_AUTHORS: return true;
        case SHOW_AUTHOR:  return !strcmp(names + b->author, author_shown);
        case SHOW_SUBJECT: return has_subject(b, subject_shown);
        default:           return b->state == showing;
    }
}

/* Every subject once, sorted */
static void subject_rows(void)
{
    int n = 0;

    for (int i = 0; i < book_ct; i++)
    {
        const char *s = names + books[i].subjects;

        for (int k = 0; k < books[i].nsubjects && row_ct < ROWS_MAX;
             k++, s += strlen(s) + 1)
            rows[row_ct++] = (int)(s - names);
    }
    qsort(rows, row_ct, sizeof(*rows), compare_subjects);
    for (int i = 0; i < row_ct; i++)
        if (n == 0 || strcasecmp(names + rows[i], names + rows[n - 1]))
            rows[n++] = rows[i];
    row_ct = n;
}

/* Everything read afresh, and the rows of the list `showing`: for Authors,
 * one book of each author */
static void load(void)
{
    static struct book_index_text text;
    int counts[3] = { 0, 0, 0 };
    int order = 0;
    bool subjects = false;

    book_ct = row_ct = 0;
    names_used = 0;
    book_index_each(collect_book, NULL, &text);
    text_viewer_each_place(collect_place, &order);

    for (int i = 0; i < book_ct; i++)
    {
        counts[books[i].state]++;
        subjects |= books[i].nsubjects > 0;
        if (showing != SHOW_SUBJECTS && shown(&books[i]))
            rows[row_ct++] = i;
    }
    if (showing == SHOW_SUBJECTS)
        subject_rows();
    else if (showing == SHOW_AUTHORS)
    {
        int n = 0;

        qsort(rows, row_ct, sizeof(*rows), compare_authors);
        for (int i = 0; i < row_ct; i++)
            if (n == 0 || strcmp(names + books[rows[i]].author,
                                 names + books[rows[n - 1]].author))
                rows[n++] = rows[i];
        row_ct = n;
    }
    else if (showing == BOOK_SHELF_IN_PROGRESS
             || showing == BOOK_SHELF_FINISHED)
        /* The last read first, as the Audiobooks shelves have them */
        qsort(rows, row_ct, sizeof(*rows), compare_recent);
    else
        /* One author's books are in name order whichever is set */
        qsort(rows, row_ct, sizeof(*rows),
              global_settings.book_sort_by == BOOKS_SORT_AUTHOR_NAME
              && showing != SHOW_AUTHOR ? compare_authors : compare_rows);

    top_ct = 0;
    for (size_t k = 0; showing == SHOW_TOP && k < ARRAYLEN(top_row); k++)
    {
        int kind = top_row[k].kind;

        if (kind == SHOW_SUBJECTS ? subjects
            : kind >= SHOW_TITLES ? book_ct > 0 : counts[kind] > 0)
            top_kinds[top_ct++] = k;
    }
}

/* ------------------------------------------------------------------ *
 * the lists                                                          *
 * ------------------------------------------------------------------ */

static int top_rows(void)
{
    return showing == SHOW_TOP ? top_ct : 0;
}

/* Whether the rows are names that open a list, not books */
static bool names_listed(void)
{
    return showing == SHOW_AUTHORS || showing == SHOW_SUBJECTS;
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

/* The book on list row n, or -1 for one of the top level's rows or a name */
static int book_at(int n)
{
    n -= top_rows();
    return !names_listed() && n >= 0 && n < row_ct ? rows[n] : -1;
}

static const char *get_name(int n, void *data, char *buffer, size_t len)
{
    int b = book_at(n);
    (void)data;
    (void)buffer;
    (void)len;

    if (showing == SHOW_AUTHORS)
        return names + books[rows[n]].author;
    if (showing == SHOW_SUBJECTS)
        return names + rows[n];
    if (b < 0)
        return str(top_row[top_kinds[n]].lang_id);
    return names + books[b].title;  /* how far through is %Lr's, for a gauge */
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

/* An author's photograph, keyed by their name */
static unsigned int author_art_hash(int n, void *data)
{
    (void)data;

    if (showing != SHOW_AUTHORS || n < 0 || n >= row_ct)
        return 0;
    return art_name_hash(names + books[rows[n]].author);
}

static const struct bitmap *get_author_art(int n, void *data,
                                           struct dim *size,
                                           const struct img_filter *filter,
                                           uint32_t filter_hash)
{
    return browser_art_row(n, data, author_art_hash, size, filter,
                           filter_hash);
}

/* %Lr: how far through a book, for a theme's gauge: a finished one is full,
 * one not started has none */
static int get_range(int n, void *data)
{
    int b = book_at(n);
    (void)data;

    if (b < 0 || books[b].state == BOOK_SHELF_NOT_STARTED)
        return -1;
    return books[b].state == BOOK_SHELF_FINISHED ? 100 : books[b].pct;
}

static enum list_row_kind get_kind(int n, void *data)
{
    (void)data;
    if (names_listed())
        return LIST_ROW_CONTAINER;
    return book_at(n) < 0 ? LIST_ROW_COMMAND : LIST_ROW_FILE;
}

static const char *list_title(void)
{
    switch (showing)
    {
        case SHOW_TOP:      return str(LANG_EBOOKS);
        case SHOW_TITLES:   return str(LANG_BY_TITLE);
        case SHOW_AUTHORS:  return str(LANG_BOOKS_AUTHORS);
        case SHOW_AUTHOR:   return author_shown;
        case SHOW_SUBJECTS: return str(LANG_BY_SUBJECT);
        case SHOW_SUBJECT:  return subject_shown;
        default:            return str(list_lang(showing));
    }
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

/* Book Info's lines: what the shelf knows of the book */
#define INFO_LINES (4 + TS_BOOK_SUBJECT_MAX)
static char info_line[INFO_LINES][MAX_PATH];

static const char *info_name(int item, void *data, char *buf, size_t len)
{
    (void)data;
    (void)buf;
    (void)len;
    return info_line[item];
}

/* False when the screen went to the root */
static bool book_info(int b)
{
    struct simplelist_info info;
    const struct ebook *e = &books[b];
    const char *s = names + e->subjects;
    int n = 0;

    snprintf(info_line[n++], MAX_PATH, "%s: %s", str(LANG_ID3_TITLE),
             names + e->title);
    snprintf(info_line[n++], MAX_PATH, "%s: %s", str(LANG_AUTHOR),
             names + e->author);
    for (int k = 0; k < e->nsubjects; k++, s += strlen(s) + 1)
        snprintf(info_line[n++], MAX_PATH, "%s: %s", str(LANG_ID3_GENRE), s);
    if (e->state == BOOK_SHELF_IN_PROGRESS && e->pct >= 0)
        snprintf(info_line[n++], MAX_PATH, str(LANG_BOOK_SHELF_ROW),
                 str(LANG_BOOKS_IN_PROGRESS), e->pct);
    else
        strmemccpy(info_line[n++], str(list_lang(e->state)), MAX_PATH);
    snprintf(info_line[n++], MAX_PATH, "%s: %s", str(LANG_PROPERTIES_PATH),
             names + e->path);

    simplelist_info_init(&info, str(LANG_BOOK_INFO), n, NULL);
    info.get_name = info_name;
    return !simplelist_show_list(&info);
}

/* The row a mark was made on, which ends the list for a reload */
static int marked_row;
/* A GO_TO_* a menu row leaves the shelf with, or GO_TO_PREVIOUS */
static int menu_exit;

/* The book Browse Chapters opens, and the row it was on; -1 for none */
static int chapters_book = -1;
static int chapters_row;

/* The rows of a book's menu, in order */
static const int book_menu_rows[] = {
    LANG_BOOK_MARK_AS, LANG_BROWSE_CHAPTERS, LANG_BOOK_INFO,
    LANG_READING_STATS, LANG_SHOW_IN_FILES,
};

/* Rows hidden in Appearance > Menus > Book Menu are left out */
static int book_menu_callback(int action, const struct menu_item_ex *this_item,
                              struct gui_synclist *this_list)
{
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM
        && context_menu_row_hidden(CONTEXT_KIND_BOOK,
                                   book_menu_rows[(intptr_t)this_item]))
        return ACTION_EXIT_MENUITEM;
    return action;
}

/* A book's menu: Mark As, Browse Chapters, Book Info, Reading Stats, Show in
 * Files. True when the list should end: a mark changed it, or a row leaves
 * the shelf or opens the book. */
static bool book_menu(int b)
{
    int choice = 0;

    MENUITEM_STRINGLIST(menu, ID2P(LANG_ONPLAY_MENU_TITLE), book_menu_callback,
                        ID2P(LANG_BOOK_MARK_AS), ID2P(LANG_BROWSE_CHAPTERS),
                        ID2P(LANG_BOOK_INFO), ID2P(LANG_READING_STATS),
                        ID2P(LANG_SHOW_IN_FILES));
    push_current_activity(ACTIVITY_CONTEXTMENU);
    choice = do_menu(&menu, &choice, NULL, false);
    if (get_current_activity() == ACTIVITY_CONTEXTMENU)
        pop_current_activity();
    switch (choice)
    {
        case 0:
            return mark_menu(b);
        case 1:
            chapters_book = b;
            return true;
        case 2:
            if (!book_info(b))
                menu_exit = GO_TO_ROOT;
            break;
        case 3:
            if (!reading_stats_show(names + books[b].path,
                                    names + books[b].title))
                menu_exit = GO_TO_ROOT;
            break;
        case 4:
            browser_reveal_on_next_load(names + books[b].path);
            menu_exit = GO_TO_FILEBROWSER;
            break;
        case MENU_ATTACHED_USB:
            menu_exit = GO_TO_ROOT;
            break;
    }
    return menu_exit != GO_TO_PREVIOUS;
}

static int action_cb(int action, struct gui_synclist *lists)
{
    int n, b;

    if (action != ACTION_STD_CONTEXT)
        return action;
    n = gui_synclist_get_sel_pos(lists);
    b = book_at(n);
    if (b < 0 || !book_menu(b))
        return ACTION_REDRAW;
    if (chapters_book >= 0)
        chapters_row = n;
    else if (menu_exit == GO_TO_PREVIOUS)
        marked_row = n;
    return ACTION_STD_CANCEL;
}

/* ------------------------------------------------------------------ *
 * the way in                                                         *
 * ------------------------------------------------------------------ */

/* One list, until it is left or something is chosen: a book, its path in
 * `path`; another list, what it shows in *list (an author's or a subject's,
 * with it in author_shown or subject_shown). A GO_TO_* code, GO_TO_PREVIOUS
 * unless the list went to the root. */
static int show(char *path, size_t size, int *selection, int *list)
{
    struct simplelist_info info;
    int ret = GO_TO_PREVIOUS;

    path[0] = '\0';
    *list = -1;
    do
    {
        marked_row = chapters_book = -1;
        menu_exit = GO_TO_PREVIOUS;
        load();
        if (top_rows() + row_ct == 0)
        {
            /* A list emptied by the book just read goes back up quietly */
            if (showing == SHOW_TOP)
                splash(HZ * 2, ID2P(LANG_BOOK_SHELF_EMPTY));
            return GO_TO_PREVIOUS;
        }
        simplelist_info_init(&info, (char *)list_title(),
                             top_rows() + row_ct, NULL);
        info.get_name = get_name;
        info.get_kind = get_kind;
        info.get_range = get_range;
        browser_albumart_invalidate();
        if (global_settings.db_ebookart && showing != SHOW_TOP
            && !names_listed())
            info.get_albumart = get_art;
        else if (global_settings.db_ebookauthorart
                 && showing == SHOW_AUTHORS)
            info.get_albumart = get_author_art;
        browser_set_showing_art(global_settings.db_ebookart
                                || info.get_albumart != NULL);
        if (info.get_albumart)
            info.row_height = global_settings.db_art_row_height;
        info.action_callback = action_cb;
        info.selection = MIN(*selection, top_rows() + row_ct - 1);
        /* Opening on the top level, Search and Finished are scrolled off
         * above the cursor, as the Music menu's Search is */
        if (showing == SHOW_TOP && info.selection == 0)
        {
            int n = 0;

            while (n < top_ct - 1
                   && (top_row[top_kinds[n]].kind == SHOW_SEARCH
                       || top_row[top_kinds[n]].kind == BOOK_SHELF_FINISHED))
                n++;
            info.selection = n;
            info.top_item = n;
        }
        /* The book just finished, in Finished */
        for (int n = 0; select_path[0] && n < top_rows() + row_ct; n++)
            if (book_at(n) >= 0
                && !strcmp(names + books[book_at(n)].path, select_path))
                *selection = info.selection = n;
        select_path[0] = '\0';

        if (simplelist_show_list(&info))
            ret = GO_TO_ROOT;
        else if (menu_exit != GO_TO_PREVIOUS)
            ret = menu_exit;
        else if (chapters_book >= 0)
        {
            *selection = chapters_row;
            strmemccpy(path, names + books[chapters_book].path, size);
        }
        else if (info.selection >= 0 && marked_row < 0)
        {
            int b = book_at(info.selection);

            *selection = info.selection;
            if (showing == SHOW_AUTHORS)
            {
                strmemccpy(author_shown,
                           names + books[rows[info.selection]].author,
                           sizeof(author_shown));
                *list = SHOW_AUTHOR;
            }
            else if (showing == SHOW_SUBJECTS)
            {
                strmemccpy(subject_shown, names + rows[info.selection],
                           sizeof(subject_shown));
                *list = SHOW_SUBJECT;
            }
            else if (b >= 0)
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

/* The search box over every book: the authors whose name matches, each once,
 * then the books whose title does. A row is a book's index, or an author's
 * name in the names with SEARCH_AUTHOR set. */
#define SEARCH_AUTHOR 0x40000000

static int compare_search_authors(const void *a, const void *b)
{
    return strnatcasecmp(names + (*(const int *)a & ~SEARCH_AUTHOR),
                         names + (*(const int *)b & ~SEARCH_AUTHOR));
}

static int search_scan(const char *query, void *ctx)
{
    int authors, k, n;
    int most = db_search_max_results();
    (void)ctx;

    row_ct = 0;
    if ((int)strlen(query) < db_search_min_letters())
        return 0;
    for (int i = 0; query[0] && i < book_ct; i++)
    {
        const char *a = names + books[i].author;

        if (!strcasestr(a, query))
            continue;
        for (k = 0; k < row_ct; k++)
            if (!strcmp(names + (rows[k] & ~SEARCH_AUTHOR), a))
                break;
        if (k == row_ct)
            rows[row_ct++] = SEARCH_AUTHOR | (int)books[i].author;
    }
    qsort(rows, row_ct, sizeof(*rows), compare_search_authors);
    authors = row_ct;
    for (int i = 0; query[0] && i < book_ct; i++)
        if (strcasestr(names + books[i].title, query))
            rows[row_ct++] = i;
    qsort(rows + authors, row_ct - authors, sizeof(*rows), compare_rows);

    /* Book Order: the titles first, by moving the authors after them */
    if (global_settings.book_search_order == BOOK_SEARCH_TITLES_FIRST
        && authors > 0)
    {
        for (k = 0; k < authors; k++)
        {
            int a = rows[0];

            memmove(rows, rows + 1, (row_ct - 1) * sizeof(*rows));
            rows[row_ct - 1] = a;
        }
    }
    n = MIN(row_ct, most);
    return n;
}

static const char *search_row(int i, void *ctx)
{
    (void)ctx;
    return rows[i] & SEARCH_AUTHOR ? names + (rows[i] & ~SEARCH_AUTHOR)
                                   : names + books[rows[i]].title;
}

static const struct bitmap *search_icon(int i, void *ctx)
{
    (void)ctx;
    return rows[i] & SEARCH_AUTHOR ? &bm_podbox_icon_artist
                                   : &bm_podbox_icon_book;
}

/* Search until a book or an author is chosen, its path or name into `out`
 * and *author saying which, or the box is left, `out` empty. */
static int search_books(char *out, size_t size, bool *author)
{
    static const struct search_provider provider = {
        .activity = ACTIVITY_DB_SEARCH,
        .scan     = search_scan,
        .row_text = search_row,
        .row_icon = search_icon,
    };
    struct search_provider p = provider;
    int chosen;

    p.title = str(LANG_DB_SEARCH);
    out[0] = '\0';
    *author = false;
    if (!claim())
    {
        splash(HZ * 2, ID2P(LANG_OUT_OF_MEMORY));
        return GO_TO_PREVIOUS;
    }
    showing = SHOW_TITLES;
    load();
    chosen = search_dialog_run(&p, NULL, search_text, sizeof(search_text));
    if (chosen >= 0)
    {
        int v = rows[chosen];

        *author = v & SEARCH_AUTHOR;
        strmemccpy(out, names + (*author ? v & ~SEARCH_AUTHOR
                                         : (int)books[v].path), size);
    }
    release();
    showing = SHOW_TOP;
    return chosen == SEARCH_USB ? GO_TO_ROOT : GO_TO_PREVIOUS;
}

int ebook_shelf_run(void)
{
    static char path[MAX_PATH];
    int top_sel = 0, list_sel = 0, names_sel = 0, book_sel = 0;
    int ret, list;

    push_current_activity(ACTIVITY_BOOKSHELF);
    showing = SHOW_TOP;
    while (1)
    {
        int *sel = showing == SHOW_TOP ? &top_sel
                 : showing == SHOW_AUTHORS || showing == SHOW_SUBJECTS
                   ? &names_sel
                 : showing == SHOW_AUTHOR || showing == SHOW_SUBJECT
                   ? &book_sel : &list_sel;

        if (!claim())
        {
            splash(HZ * 2, ID2P(LANG_OUT_OF_MEMORY));
            ret = GO_TO_PREVIOUS;
            break;
        }
        ret = show(path, sizeof(path), sel, &list);
        release();

        if (path[0])
        {
            /* The reader takes the whole screen and the buffer back. A book
             * read to its end is shown in Finished. */
            bool finished = text_viewer_finished(path);

            ret = chapters_book >= 0 ? text_viewer_chapters(path)
                                     : text_viewer(path);
            chapters_book = -1;
            if (ret == GO_TO_ROOT)
                break;
            if (!finished && text_viewer_finished(path))
            {
                showing = BOOK_SHELF_FINISHED;
                strmemccpy(select_path, path, sizeof(select_path));
            }
            continue;
        }
        if (ret == GO_TO_ROOT || ret == GO_TO_FILEBROWSER)
            break;
        if (list >= 0)
        {
            if (list == SHOW_STATS)
            {
                if (!reading_stats_show_all())
                {
                    ret = GO_TO_ROOT;
                    break;
                }
                continue;
            }
            if (list == SHOW_SEARCH)
            {
                bool author;
                int r;

                while ((r = search_books(path, sizeof(path), &author))
                           != GO_TO_ROOT
                       && path[0] && !author
                       && (r = text_viewer(path)) != GO_TO_ROOT)
                    ;
                if (r == GO_TO_ROOT)
                {
                    ret = GO_TO_ROOT;
                    break;
                }
                if (path[0] && author)
                {
                    /* an author chosen: their books, as By Author has them */
                    strmemccpy(author_shown, path, sizeof(author_shown));
                    showing = SHOW_AUTHOR;
                    book_sel = 0;
                }
                path[0] = '\0';
                continue;
            }
            showing = list;
            *(list == SHOW_AUTHORS || list == SHOW_SUBJECTS ? &names_sel
              : list == SHOW_AUTHOR || list == SHOW_SUBJECT ? &book_sel
              : &list_sel) = 0;
            continue;
        }
        if (showing != SHOW_TOP)
        {
            /* an author's or a subject's books back to their list, anything
             * else to the top */
            showing = showing == SHOW_AUTHOR ? SHOW_AUTHORS
                    : showing == SHOW_SUBJECT ? SHOW_SUBJECTS : SHOW_TOP;
            continue;
        }
        break;
    }
    pop_current_activity();
    return ret == GO_TO_ROOT || ret == GO_TO_FILEBROWSER ? ret
                                                         : GO_TO_PREVIOUS;
}
