/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The Arcade: every game cart on the player, by name, each with its
 * logo, then High Scores, every cart's. The File Index (files/file_index.c)
 * finds the carts, wherever they are filed and in /.rockbox/games where the
 * release puts its own, so a cart copied over USB shows once its walk after
 * the session is done; until then the shelf shows the last walk's.
 *
 * Each cart is read only as far as its name, its id and where its logo is:
 * the logo is decoded when its row is drawn, from the cart, in the colours
 * of the cart's first palette, and run through the skin's filter as an
 * album's cover is. A few are kept, so scrolling back does not read them
 * again. A cart with no logo shows its icon instead.
 *
 * A game left with Quit is marked Paused, and starts where it was left.
 * Holding Select on a game offers its High Scores, its Help and Delete
 * Saved Game.
 *
 * Parts, in order:
 *   - the carts
 *   - logos
 *   - a game's menu
 *   - every cart's high scores
 *   - the list, and the way in
 ****************************************************************************/
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "dir.h"
#include "lcd.h"
#include "lang.h"
#include "strnatcmp.h"
#include "string-extra.h"
#include "settings/settings.h"
#include "system/activity.h"
#include "input/action.h"
#include "widgets/list.h"
#include "widgets/splash.h"
#include "widgets/dialog_prose.h"
#include "draw/img_filter.h"
#include "screens/browse/browser.h"      /* browser_set_showing_art */
#include "files/file_index.h"
#include "files/path_list.h"
#include "root_menu.h"
#include "games/arcade/arcade.h"
#include "games/arcade/shell.h"
#include "games/arcade/shell_score.h"
#include "games/arcade/shell_table.h"
#include "games/arcade/shelf.h"

#define CARTS_MAX 64

/* ---- the carts ----------------------------------------------------------- */

struct cart
{
    char path[MAX_PATH];
    char name[32];
    uint8_t id[8];
    uint32_t logo_at, logo_len;     /* the LOGO chunk in the file, 0 none */
    uint32_t pals_at, pals_len;
    uint32_t meta_at, meta_len;
    bool paused;                    /* left with Quit: a .sus to resume */
};

static struct cart carts[CARTS_MAX];
static int cart_ct;

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

/* META's text with this tag, cut to fit and ended; false when it has none */
static bool meta_text(const uint8_t *m, uint32_t n, int tag, char *buf,
                      size_t size)
{
    uint32_t at = 0;
    while (at + 3 <= n) {
        uint32_t t = m[at], len = m[at + 1] | (uint32_t)m[at + 2] << 8;
        at += 3;
        if (len > n - at)
            return false;
        if ((int)t == tag) {
            size_t k = len < size - 1 ? len : size - 1;
            /* cut before a character, not inside one */
            while (k < len && k > 0 && (m[at + k] & 0xc0) == 0x80)
                k--;
            memcpy(buf, m + at, k);
            buf[k] = '\0';
            return true;
        }
        at += len;
    }
    return false;
}

/* The chunks' places in a cart, and its name and id: false for a file that
 * is not one. Chunks are 4 bytes of tag, 4 of length, the data, a CRC. */
static bool read_cart(const char *path, struct cart *c)
{
    uint8_t hd[8], head[20], meta[256];
    int fd = open(path, O_RDONLY);
    bool ok = false;
    if (fd < 0)
        return false;
    memset(c, 0, sizeof(*c));
    while (read(fd, hd, 8) == 8) {
        uint32_t len = le32(hd + 4);
        off_t at = lseek(fd, 0, SEEK_CUR);
        if (!ok && memcmp(hd, "PBG1", 4))
            break;
        if (!memcmp(hd, "PBG1", 4)) {
            if (len < sizeof(head) || read(fd, head, sizeof(head)) !=
                                      (ssize_t)sizeof(head))
                break;
            memcpy(c->id, head + 12, 8);
            ok = true;
        } else if (!memcmp(hd, "META", 4)) {
            c->meta_at = (uint32_t)at;
            c->meta_len = len;
            if (read(fd, meta, MIN(len, sizeof(meta))) > 0)
                meta_text(meta, MIN(len, sizeof(meta)), 1, c->name,
                          sizeof(c->name));
        } else if (!memcmp(hd, "LOGO", 4)) {
            c->logo_at = (uint32_t)at;
            c->logo_len = len;
        } else if (!memcmp(hd, "PALS", 4)) {
            c->pals_at = (uint32_t)at;
            c->pals_len = len;
        }
        if (lseek(fd, at + len + 4, SEEK_SET) < 0)
            break;
    }
    close(fd);
    if (!ok)
        return false;
    strmemccpy(c->path, path, sizeof(c->path));
    if (!c->name[0]) {
        /* a cart that names no game: its file's name */
        const char *base = strrchr(path, '/');
        strmemccpy(c->name, base ? base + 1 : path, sizeof(c->name));
        char *dot = strrchr(c->name, '.');
        if (dot)
            *dot = '\0';
    }
    char sus[MAX_PATH];
    shell_id_path(sus, sizeof(sus), SAVE_DIR, c->id, ".sus");
    c->paused = file_exists(sus);
    return true;
}

static int by_name(const void *a, const void *b)
{
    return strnatcasecmp(((const struct cart *)a)->name,
                         ((const struct cart *)b)->name);
}

/* The File Index's list, each line a time, a tab and a path. The list is
 * handed back before anything else runs: it holds the buffer a game is
 * played in. */
static void scan(void)
{
    static struct path_list list;
    cart_ct = 0;
    if (!path_list_load(&list, file_index_games(), PATH_LIST_MAX))
        return;
    for (int i = 0; i < list.count && cart_ct < CARTS_MAX; i++) {
        const char *line = path_list_get(&list, i);
        const char *tab = strchr(line, '\t');
        if (read_cart(tab ? tab + 1 : line, &carts[cart_ct]))
            cart_ct++;
    }
    path_list_free(&list);
    qsort(carts, cart_ct, sizeof(carts[0]), by_name);
}

/* ---- logos --------------------------------------------------------------- */

#define LOGO_PX (ARCADE_LOGO_SIZE * ARCADE_LOGO_SIZE)
#define LOGO_SLOTS 6

/* The logos decoded lately: whose, through which filter */
static struct
{
    int cart;                       /* -1 empty */
    uint32_t filter_hash;
    int w, h;
    fb_data px[LOGO_PX];
} slots[LOGO_SLOTS];
static int slot_next;
static struct bitmap logo_bm;

static void logos_forget(void)
{
    for (int i = 0; i < LOGO_SLOTS; i++)
        slots[i].cart = -1;
}

/* A chunk's bytes, at most size: how many were read */
static uint32_t read_at(const char *path, uint32_t at, void *buf,
                        uint32_t size)
{
    int fd = open(path, O_RDONLY);
    ssize_t n = -1;
    if (fd < 0)
        return 0;
    if (lseek(fd, at, SEEK_SET) == (off_t)at)
        n = read(fd, buf, size);
    close(fd);
    return n < 0 ? 0 : (uint32_t)n;
}

/* Cart c's logo into slot s; false when it has none, or it is damaged */
static bool logo_load(int c, int s, const struct img_filter *filter)
{
    /* a logo LZ4-packs into less than its pixels and a little over; only
     * the first palette of PALS is read */
    static uint8_t logo[LOGO_PX + 64], pals[3 + 256 * 3];
    static uint32_t rgb[LOGO_PX];
    const struct cart *k = &carts[c];
    uint32_t nl, np = 0;
    int w, h;
    if (!k->logo_len || k->logo_len > sizeof(logo))
        return false;
    nl = read_at(k->path, k->logo_at, logo, k->logo_len);
    if (k->pals_len)
        np = read_at(k->path, k->pals_at, pals, MIN(k->pals_len, sizeof(pals)));
    if (nl != k->logo_len ||
        !arcade_logo_rgb(logo, nl, np ? pals : NULL, np, rgb, LOGO_PX, &w, &h))
        return false;
    for (int i = 0; i < w * h; i++)
        slots[s].px[i] = FB_RGBPACK(rgb[i] >> 16 & 0xff, rgb[i] >> 8 & 0xff,
                                    rgb[i] & 0xff);
    if (filter)
        img_filter_apply(slots[s].px, w, h, filter);
    slots[s].w = w;
    slots[s].h = h;
    return true;
}

static const struct bitmap *get_logo(int n, void *data, struct dim *size,
                                     const struct img_filter *filter,
                                     uint32_t filter_hash)
{
    int s;
    (void)data;
    (void)size;
    if (n >= cart_ct)
        return NULL;
    for (s = 0; s < LOGO_SLOTS; s++)
        if (slots[s].cart == n && slots[s].filter_hash == filter_hash)
            goto hit;
    s = slot_next;
    slot_next = (slot_next + 1) % LOGO_SLOTS;
    slots[s].cart = -1;
    if (!logo_load(n, s, filter))
        return NULL;
    slots[s].cart = n;
    slots[s].filter_hash = filter_hash;
hit:
    logo_bm.width = slots[s].w;
    logo_bm.height = slots[s].h;
    logo_bm.format = FORMAT_NATIVE;
    logo_bm.data = (unsigned char *)slots[s].px;
    return &logo_bm;
}

/* ---- a game's menu ------------------------------------------------------- */

enum { G_SCORES, G_HELP, G_DELETE };

static int game_rows[3], game_row_ct;

static const char *game_row(int row, void *data, char *buf, size_t len)
{
    static const int label[] = { LANG_HIGH_SCORES, LANG_GAME_HELP,
                                 LANG_DELETE_SAVED_GAME };
    (void)data; (void)buf; (void)len;
    return str(label[game_rows[row]]);
}

static void id_path(const struct cart *c, char *path, size_t size,
                    const char *dir, const char *ext)
{
    shell_id_path(path, size, dir, c->id, ext);
}

static bool has_saved(const struct cart *c)
{
    char path[MAX_PATH];
    id_path(c, path, sizeof(path), SAVE_DIR, ".sav");
    return c->paused || file_exists(path);
}

/* The record kept for scores, and the help text, one at a time */
static struct arcade_scores record;
static char help[1024];

static bool game_help(const struct cart *c)
{
    static uint8_t meta[1200];
    uint32_t n = read_at(c->path, c->meta_at, meta,
                         MIN(c->meta_len, sizeof(meta)));
    if (!meta_text(meta, n, 4, help, sizeof(help)))
        return false;
    return dialog_prose_show(c->name, help);
}

/* Cart c's menu until backed out of: true for out to the root */
static bool game_menu(int c)
{
    const struct cart *k = &carts[c];
    struct simplelist_info info;
    char path[MAX_PATH];
    int selection = 0;
    for (;;) {
        game_row_ct = 0;
        game_rows[game_row_ct++] = G_SCORES;
        if (k->meta_len)
            game_rows[game_row_ct++] = G_HELP;
        if (has_saved(k))
            game_rows[game_row_ct++] = G_DELETE;
        simplelist_info_init(&info, (char *)k->name, game_row_ct, NULL);
        info.get_name = game_row;
        info.selection = MIN(selection, game_row_ct - 1);
        if (shell_list(&info))
            return true;
        selection = info.selection;
        if (selection < 0)
            return false;
        switch (game_rows[selection]) {
        case G_SCORES:
            id_path(k, path, sizeof(path), SCORES_DIR, ".sco");
            if (!shell_scores_read(path, &record))
                splash(HZ, ID2P(LANG_NO_HIGH_SCORES));
            else if (shell_scores_show(&record))
                return true;
            break;
        case G_HELP:
            if (game_help(k))
                return true;
            break;
        case G_DELETE:
            if (dialog_prose_confirm(str(LANG_DELETE_SAVED_GAME),
                                     str(LANG_DELETE_SAVED_GAME_ASK),
                                     str(LANG_SET_BOOL_YES),
                                     str(LANG_SET_BOOL_NO), false)) {
                id_path(k, path, sizeof(path), SAVE_DIR, ".sus");
                remove(path);
                id_path(k, path, sizeof(path), SAVE_DIR, ".sav");
                remove(path);
                carts[c].paused = false;
                return false;
            }
            break;
        }
    }
}

/* ---- every cart's high scores -------------------------------------------- */

/* Each scores file of a cart on the player, by the cart's name, which the
 * file keeps */
struct scored { char file[24]; char name[32]; };
static struct scored scored[CARTS_MAX];
static int scored_ct;

static int by_scored_name(const void *a, const void *b)
{
    return strnatcasecmp(((const struct scored *)a)->name,
                         ((const struct scored *)b)->name);
}

static bool on_player(const uint8_t *id)
{
    for (int i = 0; i < cart_ct; i++)
        if (!memcmp(carts[i].id, id, sizeof(carts[i].id)))
            return true;
    return false;
}

static void scan_scores(void)
{
    char path[MAX_PATH];
    DIR *d = opendir(SCORES_DIR);
    struct dirent *e;
    scored_ct = 0;
    if (!d)
        return;
    while (scored_ct < CARTS_MAX && (e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 5 || len >= sizeof(scored[0].file) ||
            strcasecmp(e->d_name + len - 4, ".sco"))
            continue;
        snprintf(path, sizeof(path), "%s/%s", SCORES_DIR, e->d_name);
        if (!shell_scores_read(path, &record) || !on_player(record.id))
            continue;
        strmemccpy(scored[scored_ct].file, e->d_name, sizeof(scored[0].file));
        strmemccpy(scored[scored_ct].name, record.cart,
                   sizeof(scored[0].name));
        scored_ct++;
    }
    closedir(d);
    qsort(scored, scored_ct, sizeof(scored[0]), by_scored_name);
}

static const char *scored_name(int row)
{
    return scored[row].name[0] ? scored[row].name : scored[row].file;
}

static bool scored_open(int row)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", SCORES_DIR, scored[row].file);
    return shell_scores_read(path, &record) && shell_scores_show(&record);
}

/* Open until backed out of; true for out to the root */
static bool all_scores(void)
{
    scan_scores();
    if (!scored_ct) {
        splash(HZ, ID2P(LANG_NO_HIGH_SCORES));
        return false;
    }
    return shell_table_list(str(LANG_HIGH_SCORES), scored_name, scored_ct,
                            scored_open);
}

/* ---- the list, and the way in -------------------------------------------- */

/* The carts, then High Scores; with none, a row saying so first, or that
 * the File Index is still walking */
static int none_rows(void) { return cart_ct ? 0 : 1; }
static int rows(void) { return none_rows() + cart_ct + 1; }

static const char *shelf_row(int row, void *data, char *buf, size_t len)
{
    (void)data;
    if (row < none_rows())
        return str(file_index_is_busy() ? LANG_WAIT : LANG_ARCADE_NONE);
    row -= none_rows();
    if (row == cart_ct)
        return str(LANG_HIGH_SCORES);
    if (!carts[row].paused)
        return carts[row].name;
    snprintf(buf, len, str(LANG_GAME_PAUSED), carts[row].name);
    return buf;
}

/* A cart's icon only where it has no logo: a theme drawing art rows shows
 * the icon in the logo's place, so a cart with one has none */
static enum themable_icons shelf_icon(int row, void *data)
{
    (void)data;
    row -= none_rows();
    return row >= 0 && row < cart_ct && !carts[row].logo_len ? Icon_Plugin
                                                             : Icon_NOICON;
}

/* High Scores is the list's own row, as a library list's [Random] is, so
 * a theme draws it no art */
static enum list_row_kind shelf_kind(int row, void *data)
{
    (void)data;
    row -= none_rows();
    if (row < 0)
        return LIST_ROW_PLAIN;
    return row < cart_ct ? LIST_ROW_FILE : LIST_ROW_COMMAND;
}

/* After a game's menu: the list made again for what it did, or left for
 * the root, at the row it was on */
static bool shelf_redo, shelf_to_root;
static int shelf_at;

static int shelf_action(int action, struct gui_synclist *lists)
{
    int c;
    if (action != ACTION_STD_CONTEXT)
        return action;
    c = gui_synclist_get_sel_pos(lists) - none_rows();
    if (c < 0 || c >= cart_ct)
        return ACTION_REDRAW;
    shelf_at = gui_synclist_get_sel_pos(lists);
    shelf_to_root = game_menu(c);
    shelf_redo = true;
    return ACTION_STD_CANCEL;
}

int arcade_shelf_run(void)
{
    struct simplelist_info info;
    int selection = 0, ret = GO_TO_PREVIOUS;
    push_current_activity(ACTIVITY_ARCADE);
    browser_set_showing_art(true);
    for (;;) {
        scan();
        logos_forget();
        simplelist_info_init(&info, str(LANG_ARCADE), rows(), NULL);
        info.get_name = shelf_row;
        info.get_icon = shelf_icon;
        info.get_kind = shelf_kind;
        info.get_albumart = cart_ct ? get_logo : NULL;
        info.row_height = cart_ct ? global_settings.db_art_row_height : 0;
        info.action_callback = shelf_action;
        info.selection = MIN(selection, rows() - 1);
        info.wps_on_play = true;
        shelf_redo = shelf_to_root = false;
        if (simplelist_show_list(&info)) {
            ret = GO_TO_ROOT;
            break;
        }
        if (shelf_redo) {
            if (shelf_to_root) {
                ret = GO_TO_ROOT;
                break;
            }
            selection = shelf_at;
            continue;
        }
        if (info.selection == -2) {
            ret = GO_TO_WPS;
            break;
        }
        if (info.selection < 0)
            break;
        selection = info.selection;
        int row = selection - none_rows();
        if (row == cart_ct) {
            if (all_scores()) {
                ret = GO_TO_ROOT;
                break;
            }
        } else if (row >= 0 && arcade_play(carts[row].path) == GO_TO_ROOT) {
            ret = GO_TO_ROOT;
            break;
        }
    }
    browser_set_showing_art(false);
    pop_current_activity();
    return ret;
}
