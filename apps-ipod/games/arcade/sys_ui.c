/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade services: dialogs, drawn by the engine inside the game's own
 * screen -- in the cart's font and size, in the colours ui.style() sets --
 * so they look part of the game. Each runs to its end inside the call: the
 * host shows each frame and hands back the buttons and the wheel
 * (arcade_host.frame) until it is answered, then the game's pixels and
 * drawing state are put back. The time is not charged to the cart.
 *
 * A dialog is a prompt, wrapped to the box, over its controls: OK; Yes and
 * No; a list; a number; or the player's text line -- the wheel puts a
 * character in at the caret and turns it, Left and Right keep it and move,
 * held they delete, SELECT takes it, PLAY goes to OK and Cancel. MENU
 * backs out of those to the line, and from the line out. Text
 * the cart's font cannot draw -- a translation in FONT.CAPS -- is shown in
 * FONT.WORLD at the same size.
 *
 * The host also asks through it (the resume question) and lays the
 * achievement banner over the game's top rows with it.
 *
 * arcade/sdk/lib/gui.js draws the same pieces for a cart's own screens,
 * and a test checks the two give the same pixels: change the drawing here
 * and change it there.
 *
 * Parts, in order:
 *   text in       a cart's value as ended text, of a bounded length
 *   the line      ui.text's characters, and the wheel's cycle of them
 *   layout        the prompt wrapped, the box sized to what it holds
 *   drawing       the box, its rows and buttons
 *   input         presses, the wheel's steps, Left and Right held
 *   the loop      one for all five kinds
 *   natives       ui.choose, confirm, message, number, text, style
 *   for the host  what a dialog shows, answering it, a question of its
 *                 own, and the banner
 ***************************************************************************/
#include <stddef.h>
#include <string.h>
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)
#define RET_INT(v) (*out = mk(T_INT, (int32_t)(v)), 1)

#define MAX_ITEMS  32
#define ITEM_BYTES 48
#define TEXT_BYTES 1024
#define LINE_MAX   64       /* ui.text: characters, and bytes */
#define CYCLE_MAX  160
#define MAX_LINES  48

/* The box: its sides, and the text's inset from them */
#define BOX_X0     6
#define BOX_X1     153
#define PAD        4
#define INNER_W    (BOX_X1 - BOX_X0 + 1 - 2 * PAD)
#define MARGIN     4

/* Wheel clicks to a step of a number or a character; rows and buttons move
 * as the player's lists do, by arcade_scroll() */
#define CLICKS_CHAR 6

/* The wheel's cycle when the cart's font is FONT.CAPS, and the English of
 * the player's own (its translation is the owner's language's) */
static const char cycle_ascii[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.'&";

struct dialog
{
    int kind;                       /* ARCADE_DIALOG_* */
    const char *prompt;
    const char *const *items;       /* choose */
    int n;
    const char *yes, *no;           /* confirm's buttons, NULL for Yes, No */
    int at;                         /* the row, or the button, chosen */
    int32_t min, max, step, value, first;   /* number */
    uint32_t line[LINE_MAX];        /* text: codepoints */
    int len, caret, editing, focus, most;
    int done, answer;               /* answer: an index, 1 yes, 0 backed out */
    /* layout */
    uint16_t brk[MAX_LINES], brk_len[MAX_LINES];
    int lines, shown, top;          /* prompt lines: all, shown, the first */
    int rows, row_top;              /* choose: rows shown, the first */
    int32_t y0, h, line_h, controls;
    int32_t hscroll;                /* text: the line's scroll */
    /* input */
    unsigned held;
    int acc, hold_prev, hold_next;
    struct arcade_scroll scroll;    /* the wheel, as a list takes it */
};

/* ---- text in ------------------------------------------------------------- */

/* v as text into dst, ended, cut before a character rather than inside */
static void text_of(struct vm *vm, struct value v, char *dst, uint32_t cap)
{
    uint32_t n = v.tag == T_NULL ? 0 : vm_text(vm, v, dst, cap - 1);
    dst[utf8_whole(dst, n)] = 0;
}

/* The prompt: title, then text on the next line, either left out */
static void prompt_of(char *dst, uint32_t cap, const char *title,
                      const char *text)
{
    uint32_t n = 0;
    for (const char *s = title; *s && n + 1 < cap; s++)
        dst[n++] = *s;
    if (*title && *text && n + 1 < cap)
        dst[n++] = '\n';
    for (const char *s = text; *s && n + 1 < cap; s++)
        dst[n++] = *s;
    dst[utf8_whole(dst, n)] = 0;
}

/* ---- the line ----------------------------------------------------------- */

static uint32_t cycle[CYCLE_MAX];
static int cycle_len;

/* The next codepoint of s at *i, which moves past it */
static uint32_t next_cp(const char *s, uint32_t *i)
{
    uint8_t c = (uint8_t)s[(*i)++];
    int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    uint32_t cp = more == 3 ? c & 7 : more == 2 ? c & 15 : more ? c & 31 : c;
    for (; more && ((uint8_t)s[*i] & 0xc0) == 0x80; more--)
        cp = cp << 6 | ((uint8_t)s[(*i)++] & 63);
    return cp;
}

/* The wheel's characters: the plain set in FONT.CAPS, which draws no
 * others; the owner's language's in FONT.WORLD. Space is always in it. */
static void cycle_make(struct vm *vm)
{
    const char *s = cycle_ascii;
    if (vm->d.font == 2 && vm->host->translate) {
        const char *t = vm->host->translate(vm->host->ctx, cycle_ascii);
        if (t && *t)
            s = t;
    }
    int space = 0;
    cycle_len = 0;
    for (uint32_t i = 0; s[i] && cycle_len < CYCLE_MAX - 1;) {
        uint32_t cp = next_cp(s, &i);
        if (cp < 0x20)
            continue;
        space |= cp == ' ';
        cycle[cycle_len++] = cp;
    }
    if (!space)
        cycle[cycle_len++] = ' ';
}

static int line_bytes(const struct dialog *g)
{
    int n = 0;
    for (int i = 0; i < g->len; i++)
        n += utf8_bytes(g->line[i]);
    return n;
}

/* The line as UTF-8 into d, ended; trimmed of spaces at its ends */
static void line_text(const struct dialog *g, char *d, int trim)
{
    int a = 0, b = g->len, n = 0;
    if (trim) {
        while (a < b && g->line[a] == ' ')
            a++;
        while (b > a && g->line[b - 1] == ' ')
            b--;
    }
    for (int i = a; i < b; i++)
        n += utf8_put(d + n, g->line[i]);
    d[n] = 0;
}

/* The wheel turned dir steps at the caret: a character put in first when
 * none is being chosen, then turned through the cycle, past any that
 * would make the line too long */
static void line_turn(struct dialog *g, int dir)
{
    if (!g->editing) {
        if (g->len >= LINE_MAX || line_bytes(g) + 1 > g->most)
            return;
        memmove(g->line + g->caret + 1, g->line + g->caret,
                (size_t)(g->len - g->caret) * sizeof(uint32_t));
        g->line[g->caret++] = ' ';
        g->len++;
        g->editing = 1;
    }
    uint32_t *c = &g->line[g->caret - 1];
    int idx = 0;
    while (idx < cycle_len && cycle[idx] != *c)
        idx++;
    if (idx == cycle_len)
        idx = 0;
    int room = g->most - line_bytes(g) + utf8_bytes(*c);
    for (int tries = 0; tries < cycle_len; tries++) {
        idx = ((idx + dir) % cycle_len + cycle_len) % cycle_len;
        if (utf8_bytes(cycle[idx]) <= room)
            break;
    }
    *c = cycle[idx];
}

static void line_delete(struct dialog *g, int at)
{
    if (at < 0 || at >= g->len)
        return;
    memmove(g->line + at, g->line + at + 1,
            (size_t)(g->len - at - 1) * sizeof(uint32_t));
    g->len--;
    if (g->caret > at)
        g->caret--;
    g->editing = 0;
}

/* ---- layout -------------------------------------------------------------- */

/* The prompt broken into lines inner pixels wide: at each newline, at the
 * last space that fits, or inside a word too long for a line */
static void wrap(struct vm *vm, struct dialog *g)
{
    const char *s = g->prompt;
    uint32_t n = (uint32_t)strlen(s), i = 0;
    g->lines = 0;
    while (i < n && g->lines < MAX_LINES) {
        uint32_t end = i, space = 0;
        while (end < n && s[end] != '\n') {
            uint32_t next = end + 1;
            while (next < n && ((uint8_t)s[next] & 0xc0) == 0x80)
                next++;
            if (end > i && text_width(vm, s + i, next - i) > INNER_W)
                break;
            if (s[end] == ' ')
                space = end;
            end = next;
        }
        uint32_t stop = end, from = end;
        if (end < n && s[end] != '\n' && space > i) {
            stop = space;
            from = space + 1;
        } else if (end < n && s[end] == '\n')
            from = end + 1;
        g->brk[g->lines] = (uint16_t)i;
        g->brk_len[g->lines++] = (uint16_t)(stop - i);
        i = from;
    }
}

/* The box's height, from what it holds, centred on the screen */
static void lay_out(struct vm *vm, struct dialog *g)
{
    int32_t l = g->line_h = text_line(vm);
    int32_t avail = SCREEN_H - 2 * MARGIN - 2 * PAD;
    int32_t gap = g->lines ? l / 2 : 0, row = l + 2;
    switch (g->kind) {
    case ARCADE_DIALOG_CHOOSE: {
        int least = g->n < 3 ? g->n : 3;
        g->shown = (avail - gap - least * row) / l;
        if (g->shown < 1)
            g->shown = 1;
        if (g->shown > g->lines)
            g->shown = g->lines;
        g->rows = (avail - g->shown * l - (g->shown ? gap : 0)) / row;
        if (g->rows > g->n)
            g->rows = g->n;
        if (g->rows < 1)
            g->rows = 1;
        g->controls = g->rows * row;
        break;
    }
    case ARCADE_DIALOG_NUMBER:
        g->controls = l + 2;
        break;
    case ARCADE_DIALOG_TEXT:
        g->controls = l + 4 + l / 2 + row;
        break;
    default:
        g->controls = row;
    }
    if (g->kind != ARCADE_DIALOG_CHOOSE) {
        g->shown = (avail - gap - g->controls) / l;
        if (g->shown < 0)
            g->shown = 0;
        if (g->shown > g->lines)
            g->shown = g->lines;
    }
    g->h = 2 * PAD + g->shown * l + (g->shown ? gap : 0) + g->controls;
    g->y0 = (SCREEN_H - g->h) / 2;
}

/* ---- drawing ------------------------------------------------------------- */

/* The game's pixels under the dialog, and its drawing state */
static uint8_t under[SCREEN_W * SCREEN_H];
static struct draw saved;

static void fill(struct vm *vm, int32_t x0, int32_t y0, int32_t x1,
                 int32_t y1, uint8_t c)
{
    uint8_t *px = MEM(vm, vm->screen);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= SCREEN_W) x1 = SCREEN_W - 1;
    if (y1 >= SCREEN_H) y1 = SCREEN_H - 1;
    for (int32_t y = y0; y <= y1 && x0 <= x1; y++)
        memset(px + y * SCREEN_W + x0, c, (size_t)(x1 - x0 + 1));
}

static void clip(struct vm *vm, int32_t x0, int32_t y0, int32_t x1,
                 int32_t y1)
{
    vm->d.clip_x0 = x0;
    vm->d.clip_y0 = y0;
    vm->d.clip_x1 = x1;
    vm->d.clip_y1 = y1;
}

/* A scroll bar down the box's right edge: first of shown of total */
static void bar(struct vm *vm, int32_t y, int32_t h, int first, int shown,
                int total)
{
    if (total <= shown || h < 4)
        return;
    int32_t len = h * shown / total, at = y + h * first / total;
    if (len < 2)
        len = 2;
    fill(vm, BOX_X1 - 2, at, BOX_X1 - 2, at + len - 1, vm->ui_ink);
}

/* The buttons, centred along a row at y, button at chosen when lit */
static void buttons(struct vm *vm, const char *const *labels, int n,
                    int chosen, int lit, int32_t y)
{
    int32_t w[3], total = 0;
    for (int i = 0; i < n; i++) {
        w[i] = text_width(vm, labels[i], (uint32_t)strlen(labels[i]));
        total += w[i];
    }
    int32_t space = 12, x = (SCREEN_W - total - space * (n - 1)) / 2;
    for (int i = 0; i < n; i++) {
        int on = lit && i == chosen;
        if (on)
            fill(vm, x - 3, y, x + w[i] + 1, y + text_line(vm) + 1,
                 vm->ui_hi);
        draw_text(vm, labels[i], (uint32_t)strlen(labels[i]), x, y + 1,
                  on ? vm->ui_box : vm->ui_ink);
        x += w[i] + space;
    }
}

static const char *word(struct vm *vm, const char *english)
{
    const char *t = vm->host->translate
        ? vm->host->translate(vm->host->ctx, english) : NULL;
    return t && *t ? t : english;
}

static void draw_number(struct vm *vm, struct dialog *g, int32_t y)
{
    char num[16];
    int32_t v = g->value, n = 0;
    uint32_t u = v < 0 ? (uint32_t)-v : (uint32_t)v;
    char rev[12];
    int k = 0;
    do
        rev[k++] = (char)('0' + u % 10);
    while ((u /= 10) && k < 11);
    if (v < 0)
        num[n++] = '-';
    while (k)
        num[n++] = rev[--k];
    int32_t w = text_width(vm, num, (uint32_t)n);
    draw_text(vm, num, (uint32_t)n, (SCREEN_W - w) / 2, y + 1, vm->ui_hi);
}

static void draw_line(struct vm *vm, struct dialog *g, int32_t y)
{
    char text[LINE_MAX * 4 + 1], *t = text;
    int32_t l = g->line_h, x0 = BOX_X0 + PAD, x1 = BOX_X1 - PAD;
    /* the field: an outline, the line inside it scrolled to the caret */
    fill(vm, x0, y, x1, y, vm->ui_ink);
    fill(vm, x0, y + l + 3, x1, y + l + 3, vm->ui_ink);
    fill(vm, x0, y, x0, y + l + 3, vm->ui_ink);
    fill(vm, x1, y, x1, y + l + 3, vm->ui_ink);
    int n = 0, caret_at = 0, mark_at = 0;
    for (int i = 0; i < g->len; i++) {
        if (i == g->caret - 1)
            mark_at = n;
        n += utf8_put(t + n, g->line[i]);
        if (i == g->caret - 1)
            caret_at = n;
    }
    if (g->caret == 0)
        caret_at = 0;
    int32_t cx = text_width(vm, text, (uint32_t)caret_at);
    int32_t room = x1 - x0 - 4;
    if (cx - g->hscroll > room)
        g->hscroll = cx - room;
    if (cx < g->hscroll)
        g->hscroll = cx;
    int32_t tx = x0 + 2 - g->hscroll;
    clip(vm, x0 + 1, y + 1, x1, y + l + 3);
    draw_text(vm, text, (uint32_t)n, tx, y + 2, vm->ui_ink);
    if (!g->focus) {
        if (g->editing) {
            /* the character being chosen, in a block */
            int32_t mx = text_width(vm, text, (uint32_t)mark_at);
            fill(vm, tx + mx - 1, y + 1, tx + cx - 1, y + l + 2, vm->ui_hi);
            draw_text(vm, text + mark_at, (uint32_t)(caret_at - mark_at),
                      tx + mx, y + 2, vm->ui_box);
        } else
            fill(vm, tx + cx - 1, y + 2, tx + cx - 1, y + l + 1, vm->ui_hi);
    }
    clip(vm, 0, 0, SCREEN_W, SCREEN_H);
}

static void draw(struct vm *vm, struct dialog *g)
{
    int32_t l = g->line_h, x0 = BOX_X0 + PAD, y = g->y0 + PAD;
    memcpy(MEM(vm, vm->screen), under, sizeof(under));
    fill(vm, BOX_X0, g->y0, BOX_X1, g->y0 + g->h - 1, vm->ui_box);
    fill(vm, BOX_X0, g->y0, BOX_X1, g->y0, vm->ui_ink);
    fill(vm, BOX_X0, g->y0 + g->h - 1, BOX_X1, g->y0 + g->h - 1, vm->ui_ink);
    fill(vm, BOX_X0, g->y0, BOX_X0, g->y0 + g->h - 1, vm->ui_ink);
    fill(vm, BOX_X1, g->y0, BOX_X1, g->y0 + g->h - 1, vm->ui_ink);

    clip(vm, x0, y, BOX_X1 - PAD + 1, SCREEN_H);
    for (int i = 0; i < g->shown; i++) {
        int k = g->top + i;
        draw_text(vm, g->prompt + g->brk[k], g->brk_len[k], x0, y + i * l,
                  vm->ui_ink);
    }
    clip(vm, 0, 0, SCREEN_W, SCREEN_H);
    bar(vm, y, g->shown * l, g->top, g->shown, g->lines);
    y += g->shown * l + (g->shown ? l / 2 : 0);

    switch (g->kind) {
    case ARCADE_DIALOG_CHOOSE:
        clip(vm, BOX_X0 + 1, y, BOX_X1 - PAD + 1, SCREEN_H);
        for (int r = 0; r < g->rows; r++) {
            int i = g->row_top + r, on = i == g->at;
            const char *s = g->items[i];
            int32_t ry = y + r * (l + 2);
            if (on)
                fill(vm, BOX_X0 + 2, ry, BOX_X1 - PAD, ry + l + 1, vm->ui_hi);
            draw_text(vm, s, (uint32_t)strlen(s), x0, ry + 1,
                      on ? vm->ui_box : vm->ui_ink);
        }
        clip(vm, 0, 0, SCREEN_W, SCREEN_H);
        bar(vm, y, g->rows * (l + 2), g->row_top, g->rows, g->n);
        break;
    case ARCADE_DIALOG_CONFIRM: {
        const char *yn[2] = { g->yes ? g->yes : word(vm, "Yes"),
                              g->no ? g->no : word(vm, "No") };
        buttons(vm, yn, 2, g->at, 1, y);
        break;
    }
    case ARCADE_DIALOG_MESSAGE: {
        const char *ok[1] = { word(vm, "OK") };
        buttons(vm, ok, 1, 0, 1, y);
        break;
    }
    case ARCADE_DIALOG_NUMBER:
        draw_number(vm, g, y);
        break;
    case ARCADE_DIALOG_TEXT: {
        const char *oc[2] = { word(vm, "OK"), word(vm, "Cancel") };
        draw_line(vm, g, y);
        buttons(vm, oc, 2, g->at, g->focus, y + l + 4 + l / 2);
        break;
    }
    }
}

/* ---- input --------------------------------------------------------------- */

/* The wheel's whole steps of per clicks, the rest kept; a fast spin
 * counts double */
static int steps(struct dialog *g, int wheel, int per)
{
    g->acc += wheel;
    int s = g->acc / per;
    g->acc -= s * per;
    if (wheel >= 3 * per || wheel <= -3 * per)
        s *= 2;
    return s;
}

static int wheel_rows(struct vm *vm, struct dialog *g, int wheel)
{
    return arcade_scroll(&g->scroll, wheel, vm->rate);
}

static void say(struct vm *vm, const char *text, int queue)
{
    const struct arcade_speak *sp = vm->host->speak;
    if (sp && sp->on && sp->on(vm->host->ctx) && text && *text)
        sp->say(vm->host->ctx, text, queue);
}

/* What the dialog has chosen, spoken as it changes */
static void say_choice(struct vm *vm, struct dialog *g, int queue)
{
    char buf[16];
    switch (g->kind) {
    case ARCADE_DIALOG_CHOOSE:
        say(vm, g->items[g->at], queue);
        break;
    case ARCADE_DIALOG_CONFIRM:
        say(vm, g->at ? (g->no ? g->no : word(vm, "No"))
                      : (g->yes ? g->yes : word(vm, "Yes")), queue);
        break;
    case ARCADE_DIALOG_NUMBER: {
        int32_t v = g->value, n = 0;
        uint32_t u = v < 0 ? (uint32_t)-v : (uint32_t)v;
        char rev[12];
        int k = 0;
        do
            rev[k++] = (char)('0' + u % 10);
        while ((u /= 10) && k < 11);
        if (v < 0)
            buf[n++] = '-';
        while (k)
            buf[n++] = rev[--k];
        buf[n] = 0;
        say(vm, buf, queue);
        break;
    }
    case ARCADE_DIALOG_TEXT:
        if (g->focus)
            say(vm, word(vm, g->at ? "Cancel" : "OK"), queue);
        else if (g->editing) {
            buf[utf8_put(buf, g->line[g->caret - 1])] = 0;
            say(vm, buf, queue);
        }
        break;
    }
}

/* Left or Right held, in frames: a tap moves the caret as it is let go,
 * and held for half a second it deletes, again each sixth */
static int held_repeat(struct vm *vm, int *count, int down)
{
    int rate = vm->rate ? vm->rate : 30, first = rate / 2, every = rate / 6;
    if (every < 1)
        every = 1;
    if (down) {
        (*count)++;
        return *count >= first && (*count - first) % every == 0 ? 2 : 0;
    }
    int tap = *count > 0 && *count < first;
    *count = 0;
    return tap;
}

static void keep_row_shown(struct dialog *g)
{
    if (g->at < g->row_top)
        g->row_top = g->at;
    if (g->at >= g->row_top + g->rows)
        g->row_top = g->at - g->rows + 1;
}

/* One frame's buttons and wheel */
static void input(struct vm *vm, struct dialog *g, unsigned held, int wheel)
{
    unsigned pressed = held & ~g->held;
    int before = g->at;
    int32_t before_v = g->value;
    int was_editing = g->editing, before_len = g->len;
    uint32_t before_c = g->editing ? g->line[g->caret - 1] : 0;
    g->held = held;
    int scrolls = g->lines > g->shown;

    if (pressed & ARCADE_MENU) {
        if (g->kind == ARCADE_DIALOG_TEXT && g->focus) {
            g->focus = 0;
            return;
        }
        g->answer = g->kind == ARCADE_DIALOG_CHOOSE ? -1 : 0;
        g->value = g->first;
        g->done = 1;
        return;
    }
    switch (g->kind) {
    case ARCADE_DIALOG_MESSAGE:
    case ARCADE_DIALOG_CONFIRM: {
        int s = wheel_rows(vm, g, wheel);
        if (scrolls) {
            g->top += s;
            if (g->top > g->lines - g->shown)
                g->top = g->lines - g->shown;
            if (g->top < 0)
                g->top = 0;
        } else if (g->kind == ARCADE_DIALOG_CONFIRM)
            g->at += s;
        if (pressed & ARCADE_PREV)
            g->at = 0;
        if (pressed & ARCADE_NEXT)
            g->at = 1;
        if (g->kind == ARCADE_DIALOG_MESSAGE)
            g->at = 0;
        g->at = g->at < 0 ? 0 : g->at > 1 ? 1 : g->at;
        if (pressed & ARCADE_SELECT) {
            g->answer = g->at == 0;
            g->done = 1;
        }
        break;
    }
    case ARCADE_DIALOG_CHOOSE:
        g->at += wheel_rows(vm, g, wheel);
        if (pressed & ARCADE_PREV)
            g->at--;
        if (pressed & ARCADE_NEXT)
            g->at++;
        g->at = g->at < 0 ? 0 : g->at >= g->n ? g->n - 1 : g->at;
        keep_row_shown(g);
        if (pressed & ARCADE_SELECT) {
            g->answer = g->at;
            g->done = 1;
        }
        break;
    case ARCADE_DIALOG_NUMBER: {
        int64_t v = (int64_t)g->value +
                    (int64_t)steps(g, wheel, CLICKS_CHAR) * g->step;
        if ((pressed & ARCADE_PREV) ||
            held_repeat(vm, &g->hold_prev, held & ARCADE_PREV) == 2)
            v -= g->step;
        if ((pressed & ARCADE_NEXT) ||
            held_repeat(vm, &g->hold_next, held & ARCADE_NEXT) == 2)
            v += g->step;
        g->value = (int32_t)(v < g->min ? g->min : v > g->max ? g->max : v);
        if (pressed & ARCADE_SELECT) {
            g->answer = 1;
            g->done = 1;
        }
        break;
    }
    case ARCADE_DIALOG_TEXT:
        if (g->focus) {
            g->at += wheel_rows(vm, g, wheel);
            if (pressed & ARCADE_PREV)
                g->at = 0;
            if (pressed & ARCADE_NEXT)
                g->at = 1;
            g->at = g->at < 0 ? 0 : g->at > 1 ? 1 : g->at;
            if (pressed & ARCADE_SELECT) {
                g->answer = g->at == 0;
                g->done = 1;
            }
            break;
        }
        for (int s = steps(g, wheel, CLICKS_CHAR); s; s += s > 0 ? -1 : 1)
            line_turn(g, s > 0 ? 1 : -1);
        int left = held_repeat(vm, &g->hold_prev, held & ARCADE_PREV);
        int right = held_repeat(vm, &g->hold_next, held & ARCADE_NEXT);
        if (left == 1 && g->caret > 0) {
            g->caret--;
            g->editing = 0;
        } else if (left == 2)
            line_delete(g, g->caret - 1);
        if (right == 1 && g->caret < g->len) {
            g->caret++;
            g->editing = 0;
        } else if (right == 1)
            g->editing = 0;
        else if (right == 2)
            line_delete(g, g->caret);
        /* as the player's own text entry: SELECT takes the line, PLAY
         * goes down to OK and Cancel */
        if (pressed & ARCADE_SELECT) {
            g->answer = 1;
            g->done = 1;
        } else if (pressed & ARCADE_PLAY) {
            g->editing = 0;
            g->focus = 1;
            g->at = 0;
            say_choice(vm, g, 0);
        }
        break;
    }
    if (g->at != before || g->value != before_v ||
        (g->editing && (!was_editing || g->len != before_len ||
                        g->line[g->caret - 1] != before_c)))
        say_choice(vm, g, 0);
}

/* ---- the loop ------------------------------------------------------------ */

/* Whether the cart's font lacks a character of s */
static int lacks(struct vm *vm, const char *s)
{
    return s && !text_has(vm, s, (uint32_t)strlen(s));
}

/* Drawing of the engine's own: the game's state kept, the screen's rows
 * from 0 to rows - 1 too, and the drawing made plain -- no camera, clip,
 * fill pattern or palette swaps -- in the cart's font, or FONT.WORLD
 * when the text needs it */
static uint32_t quiet_units, quiet_budget, quiet_slice;

static void quiet_begin(struct vm *vm, int rows, int world)
{
    quiet_units = vm->units;
    quiet_budget = vm->budget;
    quiet_slice = vm->slice;
    if (!(vm->ui_box | vm->ui_ink | vm->ui_hi)) {
        /* until ui.style(): the palette's dark blue, white and yellow */
        vm->ui_box = 1;
        vm->ui_ink = 7;
        vm->ui_hi = 10;
    }
    memcpy(under, MEM(vm, vm->screen), (size_t)rows * SCREEN_W);
    saved = vm->d;
    vm->d.target = mk(T_INT, 0);
    vm->d.cam_x = vm->d.cam_y = 0;
    vm->d.cam_turn = 0;
    vm->d.cam_moved = 0;
    vm->d.cam_zoom = 0x10000;
    clip(vm, 0, 0, SCREEN_W, SCREEN_H);
    vm->d.fill = 0;
    for (int i = 0; i < 256; i++)
        vm->d.dpal[i] = (uint8_t)i;
    vm->d.xlat_stale = 1;
    if (world)
        vm->d.font = 2;
}

static void quiet_end(struct vm *vm, int rows)
{
    memcpy(MEM(vm, vm->screen), under, (size_t)rows * SCREEN_W);
    vm->d = saved;
    vm->units = quiet_units;
    vm->budget = quiet_budget;
    vm->slice = quiet_slice;
}

/* g shown and answered: the game's screen and drawing state kept, and put
 * back after; nothing it costs is charged to the cart */
static void run(struct vm *vm, struct dialog *g)
{
    int world = lacks(vm, g->prompt) || lacks(vm, g->yes) ||
                lacks(vm, g->no);
    for (int i = 0; i < g->n; i++)
        world |= lacks(vm, g->items[i]);
    if (g->kind != ARCADE_DIALOG_CHOOSE && g->kind != ARCADE_DIALOG_NUMBER) {
        const char *w[] = { "Yes", "No", "OK", "Cancel" };
        for (int i = 0; i < 4; i++)
            world |= lacks(vm, word(vm, w[i]));
    }
    if (g->kind == ARCADE_DIALOG_NUMBER)
        world |= lacks(vm, "-0123456789");
    if (g->kind == ARCADE_DIALOG_TEXT)
        world |= lacks(vm, cycle_ascii);
    quiet_begin(vm, SCREEN_H, world);
    if (g->kind == ARCADE_DIALOG_TEXT)
        cycle_make(vm);
    wrap(vm, g);
    lay_out(vm, g);
    keep_row_shown(g);
    vm->dialog = g;

    say(vm, g->prompt, 0);
    say_choice(vm, g, 1);
    int wheel = 0;
    if (!vm->host->frame) {
        g->answer = g->kind == ARCADE_DIALOG_CHOOSE ? -1 : 0;
        g->value = g->first;
        g->done = 1;
    }
    /* buttons down in the tick that opened it count once let go */
    g->held = vm->held;
    while (!g->done) {
        unsigned held = 0;
        draw(vm, g);
        vm->d.dirty_lo = 0;
        vm->d.dirty_hi = SCREEN_H - 1;
        if (!vm->host->frame(vm->host->ctx, &held, &wheel)) {
            g->answer = g->kind == ARCADE_DIALOG_CHOOSE ? -1 : 0;
            g->value = g->first;
            break;
        }
        if (g->done)                /* answered by the host */
            break;
        input(vm, g, held, wheel);
    }

    vm->dialog = NULL;
    quiet_end(vm, SCREEN_H);
    vm->d.dirty_lo = 0;
    vm->d.dirty_hi = SCREEN_H - 1;
    /* the press that answered it is not the game's: what is still down
     * counts as held long since, with no repeats, until let go */
    for (int i = 0; i < 5; i++)
        if (g->held & (1u << i))
            vm->held_ticks[i] = 0xffff;
    vm->pressed = 0;
    if (vm->host->frame)
        vm->host->frame(vm->host->ctx, NULL, NULL);
    vm_charge(vm, 50);
}

static void dialog_open(struct dialog *g, int kind, const char *prompt)
{
    memset(g, 0, offsetof(struct dialog, brk));
    memset(&g->scroll, 0, sizeof(g->scroll));
    g->kind = kind;
    g->prompt = prompt;
}

/* ---- natives ------------------------------------------------------------- */

static char prompt[TEXT_BYTES + ITEM_BYTES];
static struct dialog dlg;

/* ui.choose(prompt, items, [at]): the index chosen, or -1 */
NATIVE(ui_choose)
{
    static char items[MAX_ITEMS][ITEM_BYTES];
    static const char *list[MAX_ITEMS];
    if (a[1].tag != T_ARR)
        return vm_fail(vm, "ui.choose() needs a list of items", "", "");
    struct arr *r = ARR(vm, a[1]);
    if (r->len < 1 || r->len > MAX_ITEMS)
        return vm_fail(vm, "ui.choose() takes 1 to 32 items", "", "");
    text_of(vm, a[0], prompt, TEXT_BYTES);
    for (uint32_t i = 0; i < r->len; i++) {
        text_of(vm, BUF(vm, r->buf)->v[i], items[i], ITEM_BYTES);
        list[i] = items[i];
    }
    int at = argc > 2 && is_num(a[2]) ? floor_int(a[2]) : 0;
    dialog_open(&dlg, ARCADE_DIALOG_CHOOSE, prompt);
    dlg.items = list;
    dlg.n = (int)r->len;
    dlg.at = at < 0 || at >= dlg.n ? 0 : at;
    run(vm, &dlg);
    return RET_INT(dlg.answer);
}

/* ui.confirm(text, [title]) and ui.message(text, [title]): the title,
 * when given, its opening line */
static void prose(struct vm *vm, struct value *a, int argc, int kind)
{
    static char text[TEXT_BYTES], title[ITEM_BYTES];
    text_of(vm, a[0], text, sizeof(text));
    title[0] = 0;
    if (argc > 1 && a[1].tag != T_NULL)
        text_of(vm, a[1], title, sizeof(title));
    prompt_of(prompt, sizeof(prompt), title, text);
    dialog_open(&dlg, kind, prompt);
    dlg.at = 1;                     /* No, until the owner says yes */
    run(vm, &dlg);
}

NATIVE(ui_confirm)
{
    prose(vm, a, argc, ARCADE_DIALOG_CONFIRM);
    return RET_INT(dlg.answer);
}

NATIVE(ui_message)
{
    (void)out;
    prose(vm, a, argc, ARCADE_DIALOG_MESSAGE);
    return 1;
}

/* ui.number(prompt, min, max, value, [step]): the number chosen, value
 * when backed out of */
NATIVE(ui_number)
{
    for (int i = 1; i < 4; i++)
        if (!is_num(a[i]))
            return vm_fail(vm, "ui.number() needs a prompt, then the least, "
                           "the most and the starting number", "", "");
    int32_t min = floor_int(a[1]), max = floor_int(a[2]);
    int32_t value = floor_int(a[3]), step = 1;
    if (argc > 4 && is_num(a[4]))
        step = floor_int(a[4]);
    if (min >= max || step < 1)
        return vm_fail(vm, "ui.number() needs the least below the most, ",
                       "and a step of at least 1", "");
    value = value < min ? min : value > max ? max : value;
    text_of(vm, a[0], prompt, TEXT_BYTES);
    dialog_open(&dlg, ARCADE_DIALOG_NUMBER, prompt);
    dlg.min = min;
    dlg.max = max;
    dlg.step = step;
    dlg.value = dlg.first = value;
    run(vm, &dlg);
    return RET_INT(dlg.value < min ? min : dlg.value > max ? max : dlg.value);
}

/* ui.text(prompt, max, [start]): a line of at most max bytes (1-64),
 * typed with the wheel, or null when backed out of */
NATIVE(ui_text)
{
    static char start[LINE_MAX + 1];
    if (!is_num(a[1]) || floor_int(a[1]) < 1 || floor_int(a[1]) > LINE_MAX)
        return vm_fail(vm, "ui.text() needs the most it may hold, 1 to 64 "
                       "bytes", "", "");
    int most = floor_int(a[1]);
    text_of(vm, a[0], prompt, TEXT_BYTES);
    start[0] = 0;
    if (argc > 2)
        text_of(vm, a[2], start, (uint32_t)most + 1);
    dialog_open(&dlg, ARCADE_DIALOG_TEXT, prompt);
    dlg.most = most;
    for (uint32_t i = 0; start[i] && dlg.len < LINE_MAX;)
        dlg.line[dlg.len++] = next_cp(start, &i);
    dlg.caret = dlg.len;
    run(vm, &dlg);
    if (!dlg.answer) {
        *out = mk(T_NULL, 0);
        return 1;
    }
    line_text(&dlg, start, 1);
    return vm_new_string(vm, (const uint8_t *)start, (uint32_t)strlen(start),
                         out);
}

/* ui.style(box, text, highlight): the dialogs' colours */
NATIVE(ui_style)
{
    (void)argc;
    (void)out;
    for (int i = 0; i < 3; i++)
        if (!is_num(a[i]) || floor_int(a[i]) < 0 || floor_int(a[i]) > 255)
            return vm_fail(vm, "ui.style() needs three colours, 0 to 255: "
                           "the box, its text and the highlight", "", "");
    vm->ui_box = (uint8_t)floor_int(a[0]);
    vm->ui_ink = (uint8_t)floor_int(a[1]);
    vm->ui_hi = (uint8_t)floor_int(a[2]);
    return 1;
}

/* ---- for the host -------------------------------------------------------- */

int arcade_dialog(struct vm *vm, struct arcade_dialog *out)
{
    static char text[LINE_MAX * 4 + 1];
    struct dialog *g = vm->dialog;
    if (!g)
        return 0;
    out->kind = g->kind;
    out->prompt = g->prompt;
    out->items = g->items;
    out->n = g->n;
    out->at = g->at;
    out->min = g->min;
    out->max = g->max;
    out->step = g->step;
    out->value = g->value;
    line_text(g, text, 0);
    out->text = text;
    out->most = g->most;
    return 1;
}

static int32_t number_of(const char *s)
{
    int neg = *s == '-';
    int32_t v = 0;
    for (s += neg; *s >= '0' && *s <= '9'; s++)
        v = v * 10 + (*s - '0');
    return neg ? -v : v;
}

void arcade_dialog_answer(struct vm *vm, const char *answer)
{
    struct dialog *g = vm->dialog;
    if (!g)
        return;
    g->done = 1;
    g->answer = g->kind == ARCADE_DIALOG_CHOOSE ? -1 : 0;
    g->value = g->first;
    if (!answer)
        return;
    switch (g->kind) {
    case ARCADE_DIALOG_CHOOSE:
        g->answer = number_of(answer);
        if (g->answer < -1 || g->answer >= g->n)
            g->answer = -1;
        break;
    case ARCADE_DIALOG_CONFIRM:
        g->answer = number_of(answer) != 0;
        break;
    case ARCADE_DIALOG_NUMBER:
        g->value = number_of(answer);
        break;
    case ARCADE_DIALOG_TEXT:
        if (!strcmp(answer, "~"))
            break;
        g->answer = 1;
        g->len = 0;
        for (uint32_t i = 0; answer[i] && g->len < LINE_MAX;) {
            uint32_t cp = next_cp(answer, &i);
            if (line_bytes(g) + utf8_bytes(cp) > g->most)
                break;
            g->line[g->len++] = cp;
        }
        break;
    }
}

int arcade_confirm(struct vm *vm, const char *prompt, const char *yes,
                   const char *no, int no_first)
{
    static char text[TEXT_BYTES];
    prompt_of(text, sizeof(text), "", prompt);
    dialog_open(&dlg, ARCADE_DIALOG_CONFIRM, text);
    dlg.yes = yes;
    dlg.no = no;
    dlg.at = no_first ? 1 : 0;
    run(vm, &dlg);
    return dlg.answer;
}

/* The banner: head over title, in a box across the top */
static int banner_rows;

int arcade_banner(struct vm *vm, const char *head, const char *title)
{
    if (vm->dialog)
        return banner_rows = 0;
    quiet_begin(vm, SCREEN_H, lacks(vm, head) || lacks(vm, title));
    int32_t l = text_line(vm), h = 2 * PAD + 2 * l;
    int32_t x0 = BOX_X0 + PAD, y0 = MARGIN;
    fill(vm, BOX_X0, y0, BOX_X1, y0 + h - 1, vm->ui_box);
    fill(vm, BOX_X0, y0, BOX_X1, y0, vm->ui_ink);
    fill(vm, BOX_X0, y0 + h - 1, BOX_X1, y0 + h - 1, vm->ui_ink);
    fill(vm, BOX_X0, y0, BOX_X0, y0 + h - 1, vm->ui_ink);
    fill(vm, BOX_X1, y0, BOX_X1, y0 + h - 1, vm->ui_ink);
    clip(vm, x0, y0, BOX_X1 - PAD + 1, y0 + h);
    draw_text(vm, head, (uint32_t)strlen(head), x0, y0 + PAD, vm->ui_hi);
    draw_text(vm, title, (uint32_t)strlen(title), x0, y0 + PAD + l,
              vm->ui_ink);
    /* the game's pixels stay kept until arcade_banner_undo() */
    quiet_end(vm, 0);
    return banner_rows = y0 + h;
}

void arcade_banner_undo(struct vm *vm)
{
    memcpy(MEM(vm, vm->screen), under, (size_t)banner_rows * SCREEN_W);
    banner_rows = 0;
}
