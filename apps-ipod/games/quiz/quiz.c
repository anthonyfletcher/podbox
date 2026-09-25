/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The Music Quiz: a track plays from partway in, five titles are listed, and
 * the points for the round drain away until one is picked.
 *
 * The clips play through the ordinary playback engine, from a throwaway
 * playlist of the ten tracks. The user's own playlist is set aside for the
 * length of a game and put back after it (playlist_set_aside()), and nothing
 * a clip does is recorded as a listen (audio_set_unrecorded()): a clip starts
 * mid-song and stops partway, which would otherwise count a play, move the
 * resume point and reach the scrobbler log.
 *
 * The screen draws itself, as Spike does, with the theme off. A theme would
 * give the answer away -- the status bar names the track and the dynamic
 * colours are the cover's.
 *
 * The clock that drains the points starts when the clip is heard, not when
 * the round starts: spinning the disk up and seeking into a track is not the
 * player's time to lose.
 *
 * Parts, in order:
 *   - best scores
 *   - drawing
 *   - one round
 *   - one game
 *   - the way in
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "button.h"                   /* button_hold */
#include "backlight.h"
#include "file.h"
#include "rbpaths.h"
#include "audio.h"
#include "audio/playback.h"           /* audio_set_unrecorded */
#include "lang.h"
#include "input/action.h"
#include "settings/settings.h"
#include "system/activity.h"
#include "system/shutdown.h"          /* default_event_handler */
#include "system/strutil.h"           /* read_line */
#include "draw/viewport.h"
#include "widgets/splash.h"
#include "widgets/yesno.h"
#include "metadata/book_resume.h"
#include "playlist/playlist.h"
#include "root_menu.h"
#include "games/quiz/quiz_pick.h"
#include "games/quiz/quiz.h"

#define QUIZ_SCORE_FILE     ROCKBOX_DIR "/musicquiz.scores"
#define QUIZ_SCORE_MAGIC    "musicquiz 2"

/* What a round is worth, and how long it takes to drain. */
#define ROUND_POINTS        100
#define ROUND_TICKS         (12 * HZ)

/* How long the answer stays on screen. */
#define REVEAL_TICKS        (HZ * 3 / 2)

/* A clip that has not been heard by now starts the clock anyway, so a track
 * that will not play costs the round rather than hanging it. */
#define START_TIMEOUT       (6 * HZ)

/* ------------------------------------------------------------------ *
 * best scores                                                        *
 * ------------------------------------------------------------------ */

static int best;

static void scores_load(void)
{
    char line[64];
    int fd;

    best = 0;

    fd = open(QUIZ_SCORE_FILE, O_RDONLY);
    if (fd < 0)
        return;

    if (read_line(fd, line, sizeof(line)) > 0
        && !strcmp(line, QUIZ_SCORE_MAGIC))
    {
        while (read_line(fd, line, sizeof(line)) > 0)
        {
            if (!strncmp(line, "best ", 5))
                best = strtol(line + 5, NULL, 10);
        }
    }

    close(fd);
}

static void scores_save(void)
{
    int fd = open(QUIZ_SCORE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (fd < 0)
        return;

    fdprintf(fd, "%s\nbest %d\n", QUIZ_SCORE_MAGIC, best);
    close(fd);
}

/* ------------------------------------------------------------------ *
 * drawing                                                            *
 * ------------------------------------------------------------------ */

#define COL_BACK            LCD_RGBPACK(17, 18, 23)
#define COL_TEXT            LCD_RGBPACK(232, 234, 240)
#define COL_ROW_TEXT        LCD_RGBPACK(217, 220, 228)
#define COL_DIM             LCD_RGBPACK(138, 143, 156)
#define COL_FADED           LCD_RGBPACK(107, 112, 128)
#define COL_ROW             LCD_RGBPACK(35, 37, 46)
#define COL_ROW_FADED       LCD_RGBPACK(25, 26, 32)
#define COL_SEL             LCD_RGBPACK(61, 111, 214)
#define COL_SEL_FADED       LCD_RGBPACK(38, 55, 92)
#define COL_SEL_FADED_TEXT  LCD_RGBPACK(154, 166, 196)
#define COL_RIGHT           LCD_RGBPACK(46, 157, 90)
#define COL_RIGHT_TEXT      LCD_RGBPACK(76, 194, 127)
#define COL_WRONG           LCD_RGBPACK(158, 58, 49)
#define COL_WRONG_MARK      LCD_RGBPACK(196, 73, 62)
#define COL_WRONG_TEXT      LCD_RGBPACK(232, 121, 111)
#define COL_MISS            LCD_RGBPACK(74, 37, 34)
#define COL_TROUGH          LCD_RGBPACK(44, 46, 55)
#define COL_BAR             LCD_RGBPACK(240, 170, 40)
#define COL_BAR_FADED       LCD_RGBPACK(110, 90, 51)
#define COL_WHITE           LCD_RGBPACK(255, 255, 255)

/* Glyphs of the icon font, by codepoint as UTF-8. */
#define ICON_NOTE   "a"
#define ICON_PAUSE  "B"
#define ICON_STAR   "\xc2\xb2"
#define ICON_CLOSE  "\xc3\x94"
#define ICON_CHECK  "\xc3\x95"
#define ICON_SIZE   24

#define EDGE        12          /* the text's inset from the screen's sides */
#define ROW_INSET   8           /* the rows' */
#define ROW_GAP     4
#define PIP         8
#define PIP_GAP     4
#define BAR_H       6
#define POINTS_W    36          /* room for "+100" */
#define TILE        24
#define TILE_GAP    5

/* The "New best" badge. The star's ink is 20 of its cell's 24 columns,
 * starting at the third. */
#define BADGE_PAD   16
#define BADGE_GAP   6
#define STAR_INK_X  2
#define STAR_INK_W  20

/* The faces beyond the UI font. Each is optional: one that will not load
 * leaves its part drawn in the UI font, or its icon out. */
static int font_icon = -1, font_big = -1, font_small = -1, font_bold = -1;

static void fonts_load(void)
{
    /* font_load() caps its buffer below what these faces need; load them
     * the way the UI font is loaded. */
    int glyphs = global_settings.glyphs_to_cache;

    font_icon  = font_load_ex(FONT_DIR "/24x24-icons.fnt", 0, glyphs);
    font_big   = font_load_ex(FONT_DIR "/26-noto-sans-medium.fnt", 0, glyphs);
    font_small = font_load_ex(FONT_DIR "/12-noto-sans.fnt", 0, glyphs);
    font_bold  = font_load_ex(FONT_DIR "/18-noto-sans-bold.fnt", 0, glyphs);
}

static void fonts_unload(void)
{
    int *fonts[] = { &font_icon, &font_big, &font_small, &font_bold };

    for (unsigned i = 0; i < ARRAYLEN(fonts); i++)
    {
        if (*fonts[i] >= 0)
            font_unload(*fonts[i]);
        *fonts[i] = -1;
    }
}

static int bold(void)
{
    return font_bold >= 0 ? font_bold : FONT_UI;
}

static int font_h;
static int head_y, timer_y, timer_h, rows_y, row_h;

static void layout(void)
{
    font_h = font_get(FONT_UI)->height;
    head_y = 6;
    timer_y = head_y + font_h + 6;
    timer_h = font_h > ICON_SIZE ? font_h : ICON_SIZE;
    rows_y = timer_y + timer_h + 6;
    row_h = (LCD_HEIGHT - rows_y - ROW_GAP * QUIZ_CHOICES) / QUIZ_CHOICES;
}

static void fill(int x, int y, int w, int h, unsigned colour)
{
    lcd_set_foreground(colour);
    lcd_fillrect(x, y, w, h);
}

static int text_w(int font, const char *s)
{
    int w;

    lcd_setfont(font);
    lcd_getstringsize(s, &w, NULL);
    return w;
}

/* FG, not SOLID: the anti-aliased faces blend against what is under them
 * only in FG mode, and SOLID would paint each glyph's box in the background
 * colour over a row's fill. */
static void text_at(int font, int x, int y, unsigned fg, const char *s)
{
    lcd_setfont(font);
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(fg);
    lcd_putsxy(x, y, s);
    lcd_set_drawmode(DRMODE_SOLID);
}

static void icon_at(int x, int y, unsigned fg, const char *glyph)
{
    if (font_icon >= 0)
        text_at(font_icon, x, y, fg, glyph);
}

/* 's' cut to 'maxw' pixels in the UI font, with "..." where it was cut. A
 * title is one line and there is nowhere for the rest of it to go. */
static const char *fit(char *buf, size_t size, const char *s, int maxw)
{
    size_t len = strlcpy(buf, s, size);

    if (len >= size)
        len = size - 1;
    if (text_w(FONT_UI, buf) <= maxw)
        return buf;

    while (len > 0)
    {
        do
            len--;
        while (len > 0 && ((unsigned char)s[len] & 0xc0) == 0x80);

        snprintf(buf, size, "%.*s...", (int)len, s);
        if (text_w(FONT_UI, buf) <= maxw)
            break;
    }
    return buf;
}

enum view_mode {
    V_LISTENING,        /* the clip is not heard yet, and nothing drains */
    V_PLAYING,
    V_PAUSED,
    V_ANSWER,           /* the round is over and shows its answer */
};

/* Everything one frame of a round shows. */
struct view
{
    const struct quiz_round *r;
    int round;
    int score;          /* before this round */
    int points;         /* what an answer is worth now */
    int sel;
    int picked;         /* V_ANSWER: the row chosen, or -1 for time up */
    enum view_mode mode;
};

/* Each finished round's points, and whether it was answered right. */
static int earned[QUIZ_ROUNDS];
static bool was_right[QUIZ_ROUNDS];

static bool answered_right(const struct view *v)
{
    return v->mode == V_ANSWER && v->picked == v->r->right;
}

/* The ten rounds as squares, and the score. */
static void draw_head(const struct view *v)
{
    char buf[16];
    int score = v->score + (answered_right(v) ? v->points : 0);

    fill(0, 0, LCD_WIDTH, timer_y, COL_BACK);

    for (int i = 0; i < QUIZ_ROUNDS; i++)
    {
        unsigned colour = COL_TROUGH;

        if (i < v->round)
            colour = was_right[i] ? COL_RIGHT : COL_WRONG_MARK;
        else if (i == v->round && v->mode == V_ANSWER)
            colour = answered_right(v) ? COL_RIGHT : COL_WRONG_MARK;
        else if (i == v->round)
            colour = COL_TEXT;

        fill(EDGE + i * (PIP + PIP_GAP), head_y + (font_h - PIP) / 2,
             PIP, PIP, colour);
    }

    snprintf(buf, sizeof(buf), "%d", score);
    text_at(bold(), LCD_WIDTH - EDGE - text_w(bold(), buf), head_y,
            COL_TEXT, buf);
}

/* The line under the squares: listening, the draining bar, or the verdict --
 * and always what an answer is worth. */
static void draw_timer(const struct view *v)
{
    char buf[16];
    int text_y = timer_y + (timer_h - font_h) / 2;
    int bar_w = LCD_WIDTH - 2 * EDGE - POINTS_W - 8;
    unsigned points_colour = COL_BAR;

    fill(0, timer_y, LCD_WIDTH, timer_h, COL_BACK);

    switch (v->mode)
    {
        case V_LISTENING:
        {
            int x = EDGE;

            if (font_icon >= 0)
            {
                icon_at(x, timer_y + (timer_h - ICON_SIZE) / 2, COL_DIM,
                        ICON_NOTE);
                x += ICON_SIZE + 4;
            }
            text_at(FONT_UI, x, text_y, COL_DIM, str(LANG_QUIZ_LISTENING));
            points_colour = COL_DIM;
            break;
        }

        case V_PLAYING:
        case V_PAUSED:
        {
            int bar_y = timer_y + (timer_h - BAR_H) / 2;
            bool paused = v->mode == V_PAUSED;

            fill(EDGE, bar_y, bar_w, BAR_H, COL_TROUGH);
            fill(EDGE, bar_y, bar_w * v->points / ROUND_POINTS, BAR_H,
                 paused ? COL_BAR_FADED : COL_BAR);
            if (paused)
                points_colour = COL_DIM;
            break;
        }

        case V_ANSWER:
        {
            int said = answered_right(v) ? LANG_QUIZ_RIGHT
                     : v->picked < 0     ? LANG_QUIZ_TIMES_UP
                                         : LANG_QUIZ_WRONG;

            text_at(bold(), EDGE, text_y,
                    answered_right(v) ? COL_RIGHT_TEXT : COL_WRONG_TEXT,
                    str(said));
            if (!answered_right(v))
                points_colour = COL_DIM;
            break;
        }
    }

    snprintf(buf, sizeof(buf), "+%d", answered_right(v) || v->mode != V_ANSWER
                                      ? v->points : 0);
    text_at(bold(), LCD_WIDTH - EDGE - text_w(bold(), buf), text_y,
            points_colour, buf);
}

static void draw_rows(const struct view *v)
{
    char buf[QUIZ_TITLE_MAX + 4];
    int w = LCD_WIDTH - 2 * ROW_INSET;

    fill(0, rows_y, LCD_WIDTH, LCD_HEIGHT - rows_y, COL_BACK);

    for (int i = 0; i < QUIZ_CHOICES; i++)
    {
        int y = rows_y + i * (row_h + ROW_GAP);
        unsigned bg = COL_ROW, fg = COL_ROW_TEXT;
        const char *icon = NULL;
        int maxw;

        if (v->mode == V_ANSWER)
        {
            bg = COL_ROW_FADED;
            fg = COL_FADED;
            if (i == v->r->right)
            {
                bg = COL_RIGHT;
                fg = COL_WHITE;
                icon = ICON_CHECK;
            }
            else if (i == v->picked)
            {
                bg = COL_WRONG;
                fg = COL_WHITE;
                icon = ICON_CLOSE;
            }
        }
        else if (v->mode == V_PAUSED)
        {
            bg = i == v->sel ? COL_SEL_FADED : COL_ROW_FADED;
            fg = i == v->sel ? COL_SEL_FADED_TEXT : COL_FADED;
        }
        else if (i == v->sel)
        {
            bg = COL_SEL;
            fg = COL_WHITE;
        }

        fill(ROW_INSET, y, w, row_h, bg);

        maxw = w - 2 * (EDGE - ROW_INSET) - 8;
        if (icon && font_icon >= 0)
        {
            maxw -= ICON_SIZE + 4;
            icon_at(ROW_INSET + w - 4 - ICON_SIZE, y + (row_h - ICON_SIZE) / 2,
                    COL_WHITE, icon);
        }

        text_at(FONT_UI, EDGE + 8, y + (row_h - font_h) / 2, fg,
                fit(buf, sizeof(buf), v->r->title[i], maxw));
    }
}

/* Over the faded rows while the round is paused. */
static void draw_paused(void)
{
    const char *hint = str(LANG_QUIZ_PAUSED_HINT);
    int icon_w = font_icon >= 0 ? ICON_SIZE + 12 : 0;
    int text_max = MAX(text_w(bold(), str(LANG_QUIZ_PAUSED)),
                       text_w(FONT_UI, hint));
    int w = MIN(16 + icon_w + text_max + 16, LCD_WIDTH - 2 * ROW_INSET);
    int h = 72;
    int x = (LCD_WIDTH - w) / 2;
    int y = rows_y + (QUIZ_CHOICES * (row_h + ROW_GAP) - h) / 2;
    int tx = x + 16;

    fill(x, y, w, h, COL_SEL);
    fill(x + 2, y + 2, w - 4, h - 4, COL_ROW);

    if (font_icon >= 0)
    {
        icon_at(tx, y + (h - ICON_SIZE) / 2, COL_TEXT, ICON_PAUSE);
        tx += ICON_SIZE + 12;
    }
    text_at(bold(), tx, y + h / 2 - font_h, COL_TEXT, str(LANG_QUIZ_PAUSED));
    text_at(FONT_UI, tx, y + h / 2, COL_DIM, hint);
}

static void draw_all(const struct view *v)
{
    lcd_set_background(COL_BACK);
    lcd_clear_display();
    draw_head(v);
    draw_timer(v);
    draw_rows(v);
    if (v->mode == V_PAUSED)
        draw_paused();
    lcd_update();
}

static void update_timer(const struct view *v)
{
    draw_timer(v);
    lcd_update_rect(0, timer_y, LCD_WIDTH, timer_h);
}

static void update_rows(const struct view *v)
{
    draw_rows(v);
    lcd_update_rect(0, rows_y, LCD_WIDTH, LCD_HEIGHT - rows_y);
}

/* The screen is the quiz's while a game runs. */
static void screen_take(void)
{
    struct viewport vp;

    viewportmanager_theme_enable(SCREEN_MAIN, false, &vp);
    lcd_set_backdrop(NULL);
    lcd_setfont(FONT_UI);
    layout();

    /* Held on, where a timeout was running: a round is twelve seconds of
     * reading and nothing pressed. */
    if (global_settings.backlight_timeout > 0)
        backlight_set_timeout(0);
    if (global_settings.backlight_timeout_plugged > 0)
        backlight_set_timeout_plugged(0);
}

static void screen_give_back(void)
{
    backlight_set_timeout(global_settings.backlight_timeout);
    backlight_set_timeout_plugged(global_settings.backlight_timeout_plugged);
    lcd_setfont(FONT_UI);
    viewportmanager_theme_undo(SCREEN_MAIN, true);
}

/* A yes/no over the quiz, drawn with the theme's dialog. */
static bool ask(int lang_id)
{
    bool yes;

    viewportmanager_theme_undo(SCREEN_MAIN, true);
    lcd_setfont(FONT_UI);
    yes = yesno_pop(str(lang_id));
    screen_take();
    return yes;
}

/* ------------------------------------------------------------------ *
 * one round                                                          *
 * ------------------------------------------------------------------ */

enum round_end {
    ROUND_ANSWERED,
    ROUND_LEAVE,
    ROUND_USB,
};

/* Where a clip starts: far enough in to be past an introduction, and inside
 * the stretch the Sound Index measures, so a round whose wrong titles were
 * matched by sound sounds like what they were matched against. */
static unsigned long clip_start(unsigned long length)
{
    unsigned long at = length * 15 / 100;

    if (at < 5000)
        at = 5000;
    if (at > 30000)
        at = 30000;
    return at;
}

/* Whether round 'round's clip is being heard yet. */
static bool clip_heard(int round, unsigned long from)
{
    struct mp3entry *id3;

    if ((audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
        != AUDIO_STATUS_PLAY)
        return false;
    if (playlist_get_display_index() - 1 != round)
        return false;

    id3 = audio_current_track();
    return id3 != NULL && id3->elapsed > from + 200;
}

static struct quiz_round rounds[QUIZ_ROUNDS];

/* Plays one round and adds what it earned to *score. */
static enum round_end play_round(int round, int *score)
{
    const struct quiz_round *r = &rounds[round];
    struct view v = {
        .r = r, .round = round, .score = *score, .points = ROUND_POINTS,
        .sel = 0, .picked = -1, .mode = V_LISTENING,
    };
    unsigned long from = clip_start(r->length);
    long round_tick = current_tick;
    long drain_tick = 0, pause_tick = 0;
    bool started = false, paused = false, by_hold = false, picked = false;

    audio_stop();
    playlist_start(round, from, 0);

    draw_all(&v);

    while (1)
    {
        int button = get_action(CONTEXT_MUSIC_QUIZ, HZ / 10);

        if (button == SYS_USB_CONNECTED)
        {
            audio_stop();
            return ROUND_USB;
        }

        /* The hold switch pauses, and letting it go carries on -- but only
         * from a pause the switch made. */
        if (button_hold() ? !paused : (paused && by_hold))
            button = ACTION_QUIZ_PAUSE;

        switch (button)
        {
            case ACTION_QUIZ_UP:
                if (!paused && v.sel > 0)
                {
                    v.sel--;
                    update_rows(&v);
                }
                break;

            case ACTION_QUIZ_DOWN:
                if (!paused && v.sel < QUIZ_CHOICES - 1)
                {
                    v.sel++;
                    update_rows(&v);
                }
                break;

            case ACTION_QUIZ_PICK:
                picked = !paused;
                break;

            case ACTION_QUIZ_PAUSE:
                paused = !paused;
                by_hold = paused && button_hold();
                if (paused)
                {
                    pause_tick = current_tick;
                    audio_pause();
                    v.mode = V_PAUSED;
                }
                else
                {
                    /* Time spent paused is not the player's to lose. */
                    drain_tick += current_tick - pause_tick;
                    round_tick += current_tick - pause_tick;
                    audio_resume();
                    v.mode = started ? V_PLAYING : V_LISTENING;
                }
                draw_all(&v);
                break;

            case ACTION_QUIZ_LEAVE:
                if (!paused)
                {
                    pause_tick = current_tick;
                    audio_pause();
                }
                if (ask(LANG_QUIZ_LEAVE))
                {
                    audio_stop();
                    return ROUND_LEAVE;
                }
                if (!paused)
                {
                    drain_tick += current_tick - pause_tick;
                    round_tick += current_tick - pause_tick;
                    audio_resume();
                }
                draw_all(&v);
                break;

            default:
                default_event_handler(button);
                break;
        }

        if (picked)
            break;
        if (paused)
            continue;

        if (!started && (clip_heard(round, from)
                         || TIME_AFTER(current_tick, round_tick + START_TIMEOUT)))
        {
            started = true;
            drain_tick = current_tick;
            v.mode = V_PLAYING;
            update_timer(&v);
        }

        if (started)
        {
            long spent = current_tick - drain_tick;
            int now = ROUND_POINTS
                      - (int)(spent * ROUND_POINTS / ROUND_TICKS);

            if (now < 0)
                now = 0;
            if (now != v.points)
            {
                v.points = now;
                update_timer(&v);
            }
            if (v.points == 0)
                break;
        }
    }

    audio_stop();

    v.mode = V_ANSWER;
    v.picked = picked ? v.sel : -1;
    was_right[round] = answered_right(&v);
    earned[round] = was_right[round] ? v.points : 0;
    *score += earned[round];

    draw_all(&v);
    sleep(REVEAL_TICKS);
    return ROUND_ANSWERED;
}

/* ------------------------------------------------------------------ *
 * one game                                                           *
 * ------------------------------------------------------------------ */

/* The ten tracks, as a playlist of their own. */
static bool build_playlist(void)
{
    struct playlist_insert_context ctx;
    bool ok = true;

    if (playlist_create(NULL, NULL) < 0)
        return false;

    if (playlist_insert_context_create(NULL, &ctx, PLAYLIST_INSERT_LAST,
                                       false, false) < 0)
    {
        /* create() keeps the playlist lock even when it fails; release() is
         * the only thing that gives it back. */
        playlist_insert_context_release(&ctx);
        return false;
    }

    for (int i = 0; i < QUIZ_ROUNDS && ok; i++)
        ok = playlist_insert_context_add(&ctx, rounds[i].path) >= 0;

    playlist_insert_context_release(&ctx);
    return ok;
}

static void text_centred(int font, int y, unsigned fg, const char *s)
{
    text_at(font, (LCD_WIDTH - text_w(font, s)) / 2, y, fg, s);
}

/* The score, each round's share of it, and the best. True to play again. */
static bool show_result(int score, bool record)
{
    char buf[32], out_of[32];
    int big = font_big >= 0 ? font_big : bold();
    int big_h = font_get(big)->height;
    int tiles_w = QUIZ_ROUNDS * TILE + (QUIZ_ROUNDS - 1) * TILE_GAP;
    int tiles_x = (LCD_WIDTH - tiles_w) / 2;
    int y = 16, w1, w2, x;

    lcd_set_background(COL_BACK);
    lcd_clear_display();

    text_centred(FONT_UI, y, COL_DIM, str(LANG_MUSIC_QUIZ));
    y += font_h + 6;

    /* The score and "of 1000" sit on one baseline. */
    snprintf(buf, sizeof(buf), "%d", score);
    snprintf(out_of, sizeof(out_of), str(LANG_QUIZ_OUT_OF),
             QUIZ_ROUNDS * ROUND_POINTS);
    w1 = text_w(big, buf);
    w2 = text_w(FONT_UI, out_of);
    x = (LCD_WIDTH - w1 - 6 - w2) / 2;
    text_at(big, x, y, COL_TEXT, buf);
    text_at(FONT_UI, x + w1 + 6, y + big_h - font_h - 2, COL_DIM, out_of);
    y += big_h + 14;

    for (int i = 0; i < QUIZ_ROUNDS; i++)
    {
        int tx = tiles_x + i * (TILE + TILE_GAP);

        fill(tx, y, TILE, TILE, was_right[i] ? COL_RIGHT : COL_MISS);
        if (font_small < 0)
            continue;

        if (was_right[i])
            snprintf(buf, sizeof(buf), "%d", earned[i]);
        else
            strlcpy(buf, "-", sizeof(buf));
        text_at(font_small, tx + (TILE - text_w(font_small, buf)) / 2,
                y + (TILE - font_get(font_small)->height) / 2,
                was_right[i] ? COL_WHITE : COL_WRONG_MARK, buf);
    }
    y += TILE + 4;

    /* The rounds get harder left to right, and the strip says so. */
    if (font_small >= 0)
    {
        const char *harder = str(LANG_QUIZ_HARDER);

        text_at(font_small, tiles_x, y, COL_FADED, str(LANG_QUIZ_EASIER));
        text_at(font_small, tiles_x + tiles_w - text_w(font_small, harder), y,
                COL_FADED, harder);
        y += font_get(font_small)->height;
    }
    y += 10;

    if (record)
    {
        const char *label = str(LANG_QUIZ_NEW_BEST);
        int badge_h = ICON_SIZE + 4;
        int icon_w = font_icon >= 0 ? STAR_INK_W + BADGE_GAP : 0;
        int badge_w = BADGE_PAD + icon_w + text_w(bold(), label) + BADGE_PAD;
        int bx = (LCD_WIDTH - badge_w) / 2;

        /* Spaced by the star's ink, not its cell: the glyph's box carries
         * blank columns either side, which would pad one end and not the
         * other. */
        fill(bx, y, badge_w, badge_h, COL_BAR);
        icon_at(bx + BADGE_PAD - STAR_INK_X, y + 2, COL_BACK, ICON_STAR);
        text_at(bold(), bx + BADGE_PAD + icon_w, y + (badge_h - font_h) / 2,
                COL_BACK, label);
    }
    else
    {
        snprintf(buf, sizeof(buf), str(LANG_QUIZ_BEST), best);
        text_centred(FONT_UI, y + 5, COL_DIM, buf);
    }

    /* What the two keys do, along the foot. */
    y = LCD_HEIGHT - 8 - font_h;
    fill(EDGE, y - 8, LCD_WIDTH - 2 * EDGE, 1, COL_TROUGH);

    x = EDGE;
    text_at(bold(), x, y, COL_TEXT, str(LANG_QUIZ_KEY_SELECT));
    x += text_w(bold(), str(LANG_QUIZ_KEY_SELECT)) + 6;
    text_at(FONT_UI, x, y, COL_DIM, str(LANG_QUIZ_PLAY_AGAIN));

    w2 = text_w(FONT_UI, str(LANG_QUIZ_LEAVE_HINT));
    w1 = text_w(bold(), str(LANG_QUIZ_KEY_MENU));
    x = LCD_WIDTH - EDGE - w2;
    text_at(FONT_UI, x, y, COL_DIM, str(LANG_QUIZ_LEAVE_HINT));
    text_at(bold(), x - 6 - w1, y, COL_TEXT, str(LANG_QUIZ_KEY_MENU));

    lcd_update();

    while (1)
    {
        int button = get_action(CONTEXT_MUSIC_QUIZ, TIMEOUT_BLOCK);

        if (button == ACTION_QUIZ_PICK)
            return true;
        if (button == ACTION_QUIZ_LEAVE)
            return false;
        if (default_event_handler(button) == SYS_USB_CONNECTED)
            return false;
    }
}

enum game_end {
    GAME_AGAIN,         /* played through, and another wanted */
    GAME_DONE,          /* played through, or left partway */
    GAME_USB,           /* USB took the player away */
    GAME_FAILED,        /* no game could be made; already said why */
};

static enum game_end play_game(void)
{
    enum round_end end = ROUND_ANSWERED;
    bool again = false;
    int score = 0, res;

    splash(0, ID2P(LANG_WAIT));
    res = quiz_pick(rounds);
    if (res != QUIZ_PICK_OK)
    {
        /* Chosen before ID2P rather than inside it: the macro does arithmetic
         * on its argument. */
        int msg = res == QUIZ_PICK_TOO_FEW ? LANG_QUIZ_TOO_FEW
                : res == QUIZ_PICK_NO_MEM  ? LANG_OUT_OF_MEMORY
                                           : LANG_TAGCACHE_BUSY;

        splash(HZ * 2, ID2P(msg));
        return GAME_FAILED;
    }

    playlist_set_aside();
    audio_set_unrecorded(true);

    if (!build_playlist())
    {
        audio_set_unrecorded(false);
        playlist_bring_back();
        splash(HZ * 2, ID2P(LANG_TAGCACHE_BUSY));
        return GAME_FAILED;
    }

    memset(earned, 0, sizeof(earned));
    memset(was_right, 0, sizeof(was_right));

    /* Loaded with playback stopped: a font's buffer comes out of the audio
     * buffer, and taking it mid-clip would rebuffer the clip. */
    fonts_load();
    push_current_activity(ACTIVITY_MUSICQUIZ);
    screen_take();

    for (int i = 0; i < QUIZ_ROUNDS && end == ROUND_ANSWERED; i++)
        end = play_round(i, &score);

    audio_stop();
    audio_set_unrecorded(false);
    playlist_bring_back();

    if (end == ROUND_ANSWERED)
    {
        bool record = score > best;

        if (record)
        {
            best = score;
            scores_save();
        }
        again = show_result(score, record);
    }

    screen_give_back();
    fonts_unload();
    pop_current_activity();

    if (end == ROUND_USB)
    {
        /* The playlist is back on disk, so it can go to the host as it was. */
        default_event_handler(SYS_USB_CONNECTED);
        return GAME_USB;
    }

    return again ? GAME_AGAIN : GAME_DONE;
}

/* ------------------------------------------------------------------ *
 * the way in                                                         *
 * ------------------------------------------------------------------ */

int music_quiz_screen(void)
{
    if (audio_status())
    {
        if (global_settings.party_mode)
        {
            splash(HZ, ID2P(LANG_PARTY_MODE));
            return GO_TO_PREVIOUS;
        }
        if (!yesno_pop(str(LANG_QUIZ_STOP_PLAYBACK)))
            return GO_TO_PREVIOUS;

        /* Where a book was left, before it stops being the one playing. */
        book_resume_save();
        audio_stop();
    }

    scores_load();

    while (play_game() == GAME_AGAIN)
        ;

    return GO_TO_PREVIOUS;
}
