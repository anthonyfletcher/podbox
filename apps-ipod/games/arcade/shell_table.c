/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A cart's high scores as tables, drawn the way the Arcade draws: its CAPS
 * font at the game's 160x120, doubled, in the theme's colours, the album's
 * vivid colour marking the score just set when dynamic colours are on.
 * One screen for every way to them -- score.show() in a game, the pause
 * menu's High Scores, and the Arcade's -- so a ranked table looks like one
 * wherever it is opened. The Arcade's list of every cart with scores is
 * drawn the same way.
 *
 * A page a board, then the achievements, then how much the cart was played
 * with Clear High Scores; the wheel or Right turns the pages, and only
 * Select clears. Left goes back, Menu out to the root.
 *
 * The CAPS font holds ASCII only: a character beyond it draws as '?'.
 *
 * Parts, in order:
 *   - drawing
 *   - the pages
 *   - the way in
 *   - a list of carts
 ****************************************************************************/
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "lang.h"
#include "input/action.h"
#include "settings/settings.h"
#include "system/shutdown.h"          /* default_event_handler */
#include "draw/viewport.h"
#include "skin/skin_albumart_color.h"
#include "games/arcade/arcade.h"
#include "games/arcade/core_font.h"
#include "games/arcade/shell_table.h"

/* ---- drawing ------------------------------------------------------------- */

#define W 160                   /* the game's screen, each dot 2x2 */
#define DOT 2
#define CHAR_W 4                /* 3 wide and a space */
#define ROW_H 9

static unsigned c_bg, c_fg, c_dim, c_faint, c_accent;

static unsigned mix(unsigned a, unsigned b, int t)  /* t/8 of b */
{
    return LCD_RGBPACK((RGB_UNPACK_RED(a) * (8 - t) + RGB_UNPACK_RED(b) * t) / 8,
                       (RGB_UNPACK_GREEN(a) * (8 - t) +
                        RGB_UNPACK_GREEN(b) * t) / 8,
                       (RGB_UNPACK_BLUE(a) * (8 - t) +
                        RGB_UNPACK_BLUE(b) * t) / 8);
}

/* The theme's colours, through dynamic colours when they are on */
static void colours(void)
{
    unsigned dominant, vivid;
    dynamic_colors_check_extraction(-1);
    c_bg = dynamic_colors_resolve(global_settings.bg_color);
    c_fg = dynamic_colors_resolve(global_settings.fg_color);
    c_accent = dynamic_colors_roles(&dominant, &vivid)
               ? vivid : dynamic_colors_resolve(global_settings.lss_color);
    c_dim = mix(c_fg, c_bg, 3);
    c_faint = mix(c_fg, c_bg, 5);
}

static void box(int x, int y, int w, int h, unsigned c)
{
    lcd_set_foreground(c);
    lcd_fillrect(x * DOT, y * DOT, w * DOT, h * DOT);
}

/* The width of s in dots: a character a code point */
static int text_w(const char *s)
{
    int n = 0;
    for (; *s; s++)
        n += ((unsigned char)*s & 0xc0) != 0x80;
    return n ? n * CHAR_W - 1 : 0;
}

/* s at x, y, cut at the right edge xmax */
static void text(const char *s, int x, int y, int xmax, unsigned c)
{
    lcd_set_foreground(c);
    for (; *s && x + 3 <= xmax; s++) {
        unsigned ch = (unsigned char)*s;
        if ((ch & 0xc0) == 0x80)
            continue;
        if (ch < 32 || ch > 126)
            ch = '?';
        unsigned g = font_small[ch - 32];
        for (int b = 0; b < 15; b++)
            if (g & (1u << (14 - b)))
                lcd_fillrect((x + b % 3) * DOT, (y + b / 3) * DOT, DOT, DOT);
        x += CHAR_W;
    }
}

static void text_right(const char *s, int xr, int y, unsigned c)
{
    text(s, xr - text_w(s), y, W, c);
}

/* ---- the pages ----------------------------------------------------------- */

#define ACH_PER_PAGE 5

struct pages
{
    const struct arcade_scores *s;
    int boards, ach, total;     /* pages of each, and in all */
};

static void count_pages(struct pages *p, const struct arcade_scores *s)
{
    p->s = s;
    p->boards = s->count;
    p->ach = (s->nach + ACH_PER_PAGE - 1) / ACH_PER_PAGE;
    p->total = p->boards + p->ach + 1;
}

/* The header, the page's title and where it is among the pages */
static void head(const struct pages *p, int page, const char *title)
{
    char where[24];
    box(0, 0, W, 11, c_accent);
    text(str(LANG_HIGH_SCORES), 4, 3, 80, c_bg);
    text_right(p->s->cart, 156, 3, c_bg);
    text(title, 4, 15, 100, c_fg);
    if (p->total > 1) {
        snprintf(where, sizeof(where), "< %d OF %d >", page + 1, p->total);
        text_right(where, 156, 15, c_faint);
    }
    box(4, 23, 152, 1, c_faint);
}

static void board_page(const struct pages *p, int page, int board, int rank)
{
    const struct arcade_board *b = &p->s->b[board];
    char buf[32];
    head(p, page, b->title);
    if (!b->count) {
        text(str(LANG_NO_HIGH_SCORES), 4, 30, W, c_dim);
        return;
    }
    for (int k = 0; k < b->count; k++) {
        const struct arcade_entry *e = &b->e[k];
        int y = 27 + k * ROW_H;
        bool marked = k + 1 == rank;
        unsigned ink = marked ? c_bg : k < 3 ? c_fg : c_dim;
        if (marked)
            box(2, y - 2, 156, ROW_H, c_accent);
        snprintf(buf, sizeof(buf), "%d", k + 1);
        text_right(buf, 14, y, marked ? c_bg : k < 3 ? c_accent : c_dim);
        arcade_score_text(p->s, board, e->value, buf, sizeof(buf));
        text_right(buf, 80, y, ink);
        text(e->name, 88, y, 132, marked ? c_bg : c_dim);
        if (e->date) {
            snprintf(buf, sizeof(buf), "%02ld/%02ld",
                     (long)(e->date / 100 % 100), (long)(e->date % 100));
            text_right(buf, 156, y, marked ? c_bg : c_faint);
        }
    }
}

static void ach_page(const struct pages *p, int page, int first)
{
    char buf[48];
    int earned = 0;
    for (int i = 0; i < p->s->nach; i++)
        earned += p->s->ach[i].date != 0;
    snprintf(buf, sizeof(buf), "%s %d/%d", str(LANG_ACHIEVEMENTS), earned,
             p->s->nach);
    head(p, page, buf);
    for (int k = 0; k < ACH_PER_PAGE && first + k < p->s->nach; k++) {
        const struct arcade_achievement *a = &p->s->ach[first + k];
        int y = 27 + k * 2 * ROW_H;
        text(a->title, 4, y, 128, a->date ? c_fg : c_faint);
        if (a->date > 1) {
            snprintf(buf, sizeof(buf), "%02ld/%02ld",
                     (long)(a->date / 100 % 100), (long)(a->date % 100));
            text_right(buf, 156, y, c_faint);
        }
        text(a->text, 4, y + 7, W - 4, a->date ? c_dim : c_faint);
    }
}

static void played_page(const struct pages *p, int page, bool clearable)
{
    char buf[48];
    head(p, page, str(LANG_ARCADE_PLAYED));
    snprintf(buf, sizeof(buf), str(LANG_TIMES_PLAYED), (int)p->s->plays);
    text(buf, 4, 27, W, c_fg);
    snprintf(buf, sizeof(buf), str(LANG_TIME_PLAYED),
             (int)(p->s->seconds / 3600), (int)(p->s->seconds / 60 % 60));
    text(buf, 4, 36, W, c_fg);
    if (clearable) {
        snprintf(buf, sizeof(buf), "SELECT: %s", str(LANG_CLEAR_HIGH_SCORES));
        text(buf, 4, 110, W, c_dim);
    }
}

static void draw_page(const struct pages *p, int page, int rank, bool clearable)
{
    box(0, 0, W, 120, c_bg);
    if (page < p->boards)
        board_page(p, page, page, rank);
    else if (page < p->boards + p->ach)
        ach_page(p, page, (page - p->boards) * ACH_PER_PAGE);
    else
        played_page(p, page, clearable);
    lcd_update();
}

/* Which button the last ACTION_STD_OK came from: Right and the remote's
 * Right turn a page, Select and the remote's Select clear, and the
 * remote's Play, OK too, does neither */
static bool ok_was(unsigned buttons)
{
    int button;
    get_action_statuscode(&button);
    return (button & buttons) != 0;
}

#define RIGHTS (BUTTON_RIGHT | BUTTON_RC_RIGHT)
#define SELECTS (BUTTON_SELECT | BUTTON_RC_SELECT)

/* Clear's question over the page: true for yes */
static bool ask_clear(void)
{
    box(8, 44, 144, 32, c_accent);
    box(9, 45, 142, 30, c_bg);
    text(str(LANG_CLEAR_HIGH_SCORES), 14, 50, 146, c_fg);
    text("SELECT: YES   LEFT: NO", 14, 64, 146, c_dim);
    lcd_update();
    for (;;) {
        int a = get_action(CONTEXT_LIST, TIMEOUT_BLOCK);
        if (a == ACTION_STD_OK && ok_was(SELECTS))
            return true;
        if (a == ACTION_STD_CANCEL || a == ACTION_STD_MENU)
            return false;
        if (default_event_handler(a) == SYS_USB_CONNECTED)
            return false;
    }
}

/* ---- the way in ---------------------------------------------------------- */

bool shell_table(struct arcade_scores *s, int board, int rank,
                 void (*clear)(struct arcade_scores *s, void *ctx), void *ctx)
{
    struct viewport vp;
    struct pages p;
    int page = board, rank_on = board;
    bool to_root = false;
    viewportmanager_theme_enable(SCREEN_MAIN, false, &vp);
    lcd_set_viewport(NULL);
    colours();
    count_pages(&p, s);
    if (page < 0 || page >= p.total)
        page = 0;
    for (;;) {
        bool clearable = clear && s->count > 0;
        int a;
        draw_page(&p, page, page == rank_on ? rank : 0, clearable);
        a = get_action(CONTEXT_LIST, TIMEOUT_BLOCK);
        if (a == ACTION_STD_NEXT || a == ACTION_STD_NEXTREPEAT) {
            if (page < p.total - 1)
                page++;
        } else if (a == ACTION_STD_PREV || a == ACTION_STD_PREVREPEAT) {
            if (page > 0)
                page--;
        } else if (a == ACTION_STD_CANCEL) {
            break;
        } else if (a == ACTION_STD_MENU) {
            to_root = true;
            break;
        } else if (a == ACTION_STD_OK) {
            if (ok_was(RIGHTS)) {
                if (page < p.total - 1)
                    page++;
            } else if (ok_was(SELECTS) && clearable && page == p.total - 1
                       && ask_clear()) {
                clear(s, ctx);
            }
        } else if (default_event_handler(a) == SYS_USB_CONNECTED) {
            to_root = true;
            break;
        }
    }
    viewportmanager_theme_undo(SCREEN_MAIN, true);
    return to_root;
}

/* ---- a list of carts ----------------------------------------------------- */

#define LIST_TOP 15
#define LIST_ROWS 11

static void list_page(const char *title, const char *(*name)(int row),
                      int count, int sel, int top)
{
    char where[24];
    box(0, 0, W, 120, c_bg);
    box(0, 0, W, 11, c_accent);
    text(title, 4, 3, 100, c_bg);
    snprintf(where, sizeof(where), "%d OF %d", sel + 1, count);
    text_right(where, 156, 3, c_bg);
    for (int k = 0; k < LIST_ROWS && top + k < count; k++) {
        int y = LIST_TOP + k * ROW_H;
        bool on = top + k == sel;
        if (on)
            box(2, y - 2, 156, ROW_H, c_accent);
        text(name(top + k), 4, y, W - 4, on ? c_bg : c_fg);
    }
    lcd_update();
}

bool shell_table_list(const char *title, const char *(*name)(int row),
                      int count, bool (*pick)(int row))
{
    struct viewport vp;
    int sel = 0, top = 0;
    bool to_root = false;
    viewportmanager_theme_enable(SCREEN_MAIN, false, &vp);
    lcd_set_viewport(NULL);
    for (;;) {
        int a;
        if (sel < top)
            top = sel;
        else if (sel >= top + LIST_ROWS)
            top = sel - LIST_ROWS + 1;
        colours();
        list_page(title, name, count, sel, top);
        a = get_action(CONTEXT_LIST, TIMEOUT_BLOCK);
        if (a == ACTION_STD_NEXT || a == ACTION_STD_NEXTREPEAT) {
            if (sel < count - 1)
                sel++;
        } else if (a == ACTION_STD_PREV || a == ACTION_STD_PREVREPEAT) {
            if (sel > 0)
                sel--;
        } else if (a == ACTION_STD_OK) {
            if (pick(sel)) {
                to_root = true;
                break;
            }
        } else if (a == ACTION_STD_CANCEL) {
            break;
        } else if (a == ACTION_STD_MENU) {
            to_root = true;
            break;
        } else if (default_event_handler(a) == SYS_USB_CONNECTED) {
            to_root = true;
            break;
        }
    }
    viewportmanager_theme_undo(SCREEN_MAIN, true);
    return to_root;
}
