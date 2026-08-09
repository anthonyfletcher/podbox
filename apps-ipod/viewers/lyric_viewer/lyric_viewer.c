/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The synchronised-lyrics screen. Draws three lines of the playing track --
 * the one being sung, full strength, between the one before and the one after
 * at reduced opacity -- and follows the track's elapsed time.
 *
 * How it is built:
 *   - A plain core screen, entered as lyric_viewer() and returning a GO_TO_*
 *     code. Reached from the WPS hotkey and the WPS context menu.
 *   - It owns the whole screen with the theme disabled. That is not only about
 *     chrome: opacity here is colour interpolation toward the background
 *     (color_blend), which is exact over a solid fill and wrong over a
 *     backdrop image.
 *   - The document model is lyrics.c. This file asks it what line N says and
 *     which line belongs at time T; it never parses anything.
 *
 * Waking: the loop sleeps until the next line is due rather than polling, so
 * a screen with nothing happening on it costs nothing. The cap on that sleep
 * is what notices a track change or the music stopping.
 *
 * The screen opens whatever is going on and says what it is waiting for --
 * nothing playing, or a track with no lyrics -- rather than refusing to open.
 * Both of those turn into lyrics on their own, so there is nothing to report
 * as a failure and nowhere useful to send the user back to.
 *
 * Input is read in CONTEXT_WPS, so the transport keys are the ones already in
 * the fingers from the WPS: left/right skip, Play pauses, the wheel is volume.
 *
 * The screen, wrapping, the slide, word highlighting and the settings; no
 * track-info or elapsed header, which the WPS behind it already shows.
 * Settings are read live through the LV_* macros below rather than
 * cached, so holding Menu and changing one takes effect on the next draw;
 * only the few that move the layout need lv_apply_settings().
 *
 * Parts, in order:
 *   - which lines are worth showing (gaps are not), and how much is sung
 *   - screen setup, colours, backlight and the fade
 *   - word wrap, and drawing
 *   - the slide
 *   - following playback: waking, reloading, skipping
 *   - the lyric_viewer() entry point
 ****************************************************************************/

#include <stdbool.h>
#include <string.h>
#include "string-extra.h"    /* strlcpy */
#include "config.h"
#include "file.h"            /* MAX_PATH */
#include "kernel.h"          /* HZ, sleep, SYS_USB_CONNECTED */
#include "lang.h"            /* str(), LANG_* */
#include <stdio.h>           /* snprintf */
#include "font.h"            /* font_get/load/unload, font_getstringsize */
#include "rbpaths.h"         /* FONT_DIR */
#include "rbunicode.h"       /* utf8decode */
#include "diacritic.h"       /* IS_DIACRITIC */
#include "backlight.h"       /* backlight_set_timeout* */
#include "audio.h"           /* audio_current_track/next/prev/pause */
#include "metadata.h"        /* struct mp3entry */
#include "settings/settings.h"    /* global_settings */
#include "draw/screen_access.h"   /* screens[] */
#include "draw/viewport.h"        /* viewportmanager_theme_enable/undo */
#include "draw/color.h"           /* color_blend */
#include "skin/skin_albumart_color.h"  /* dynamic_colors_resolve */
#include "widgets/splash.h"
#include "input/action.h"
#include "system/activity.h"
#include "system/volume.h"       /* adjust_volume */
#include "system/shutdown.h"      /* default_event_handler */
#include "root_menu.h"       /* GO_TO_*, MENU_ATTACHED_USB */
#include "widgets/menu.h"         /* do_menu */
#include "screens/settings/exported_settings.h"  /* lyric_viewer_menu */
#include "screens/playback/wps.h"  /* wps_do_playpause, DEFAULT_SKIP_THRESH */
#include "lyrics.h"
#include "lyric_viewer.h"

/* Inset on each edge. Not a setting: it exists so text does not touch the
 * bezel, which is not a matter of taste. */
#define LV_MARGIN           8

/* lyric_colour_mode values, matching the text viewer's. */
enum {
    LV_COLOUR_THEME = 0,
    LV_COLOUR_INVERTED,
    LV_COLOUR_BOW,           /* black on white */
    LV_COLOUR_WOB            /* white on black */
};

/* lyric_align values. */
enum { LV_ALIGN_LEFT = 0, LV_ALIGN_CENTRE, LV_ALIGN_RIGHT };

/* lyric_anim is an index into this, not a duration -- see settings_list.c. */
static const short lv_anim_ms[] = { 0, 150, 300, 500 };

/* Longest line drawn. Nothing 320 pixels wide comes near this; it only has to
 * be enough that ellipsising has something to cut. */
#define LV_ROW_MAX          512

/* Most rows the current line may wrap to. At a ~18px row on a 240px screen
 * eight rows plus both neighbours still fit, so the clipping path below is
 * close to unreachable in practice -- it exists so a pathological line cannot
 * push the next one off the screen. */
#define LV_MAX_ROWS         8

/* Most rows a neighbour may wrap to. They fade with distance, so beyond a few
 * rows there is nothing left to see. */
#define LV_SIDE_ROWS        4

/* Cap on how long the loop sleeps, so a track change or the music stopping is
 * noticed even in the middle of a long instrumental. */
#define LV_POLL             (HZ / 2)

/* How long the slide takes. It runs over [T - lv.anim_ms, T], so it finishes
 * exactly as the incoming line is due rather than starting then -- the
 * difference between the screen feeling ahead of the music or behind it. */
#define LV_ANIM_MS          (lv.anim_ms)

/* Light the words of the current line as they are sung, for the files that
 * carry <mm:ss.xx> word timing. Files without it are unaffected -- they parse
 * as one untimed word, which is sung from the line's own first instant. */
#define LV_HIGHLIGHT        (global_settings.lyric_highlight)

/* Percentages of the way from the background to the foreground colour. */
#define LV_OPACITY_PREV     (global_settings.lyric_prev_opacity)
#define LV_OPACITY_NEXT     (global_settings.lyric_next_opacity)

/* lv.shown before anything has been drawn. Distinct from LYRICS_NONE, which
 * is the real state "the song has not reached the first line yet". */
#define LV_INDEX_UNSET      (-2)

/* The track's name, standing in as the current line until the first lyric is
 * due: the song has started but nobody is singing yet, and the title says
 * more than an empty screen does.
 *
 * It gets an index of its own so that wrapping, the fade and the slide treat
 * it as just another line -- it slides up to become "previous" when the first
 * lyric arrives, with no special case anywhere. The model knows nothing about
 * it, because it is not in the file; lv_line_text() is the only place the two
 * meet. */
#define LV_TITLE_INDEX      (-3)

/* What the screen is doing. Only LV_SHOWING draws lyrics; the rest say what
 * they are waiting for and keep looking, because every one of them can turn
 * into LV_SHOWING without the user doing anything. */
enum lv_state
{
    LV_WAITING = 0,     /* nothing playing, or its metadata not filled in */
    LV_NO_LYRICS,       /* this track has none */
    LV_NO_MEMORY,       /* the working buffer could not be allocated */
    LV_SHOWING
};

static struct
{
    struct viewport vp;
    int font;
    int row;                 /* one line of text, including spacing */
    int gap;                 /* vertical space between the three lines */
    int width;               /* vp width less the margins */
    unsigned fg, bg;
    int user_font;           /* font_load handle, -1 when using the UI font */
    int anim_ms;             /* the slide, resolved from lyric_anim */
    int shown;               /* the line currently drawn */
    int sung;                /* bytes of it already sung -- see lv_sung() */
    char track[MAX_PATH];    /* the track the loaded lyrics belong to */
    enum lv_state state;
    char name[MAX_PATH];     /* what to call the track in a message */
    char font_error[MAX_PATH]; /* the font that would not load, if any */
} lv;

/* True if a line has anything worth showing. A .lrc marks an instrumental gap
 * with a timestamp and no words; such a line exists to say when the line
 * before it stops, so drawing one would blank the middle of the screen and
 * push the upcoming words out of view. They are skipped everywhere below --
 * the model keeps them, because their timing is what bounds the line before. */
/* What line `index` says: a lyric, or the track's name for the title slot. */
static const char *lv_line_text(int index)
{
    if (index == LV_TITLE_INDEX)
        return lv.name[0]? lv.name: NULL;
    return lyrics_text(index);
}

static bool lv_has_words(int index)
{
    const char *t = lv_line_text(index);

    if (!t)
        return false;
    while (*t && (unsigned char)*t <= ' ')
        t++;
    return *t != '\0';
}

/* The line to draw for raw index `index`: itself, or the most recent one with
 * words if it is a gap. Before the first lyric the title stands in. */
static int lv_shown_index(int index)
{
    while (index >= 0 && !lv_has_words(index))
        index--;
    if (index >= 0)
        return index;
    return lv_has_words(LV_TITLE_INDEX)? LV_TITLE_INDEX: LYRICS_NONE;
}

/* Nearest line with words strictly after (dir 1) or before (dir -1) `index`,
 * with the title sitting immediately before the first lyric. */
static int lv_step(int index, int dir)
{
    int n = lyrics_count();
    int i;

    if (dir > 0)
    {
        for (i = (index == LV_TITLE_INDEX)? 0: index + 1; i < n; i++)
            if (i >= 0 && lv_has_words(i))
                return i;
        return LYRICS_NONE;
    }

    if (index == LV_TITLE_INDEX)
        return LYRICS_NONE;         /* nothing before the title */

    for (i = index - 1; i >= 0; i--)
        if (lv_has_words(i))
            return i;
    return lv_has_words(LV_TITLE_INDEX)? LV_TITLE_INDEX: LYRICS_NONE;
}

/* How many bytes of line `index` have been sung by `elapsed`: the offset of
 * the first word that has not started yet.
 *
 * lyrics_word_text() hands back a *tail* of the line, so a word's offset is
 * what the line has left over once that tail is taken off. A word time of -1
 * means "starts with the line", which is what a file with no word timing
 * gives for its single word -- so such a line reads as wholly sung from its
 * first instant and needs no special case here. */
static int lv_sung(int index, long elapsed)
{
    const char *text = lv_line_text(index);
    int n = lyrics_word_count(index);
    int w;

    if (!text || !LV_HIGHLIGHT)
        return text? (int)strlen(text): 0;

    for (w = 0; w < n; w++)
    {
        long t = lyrics_word_time(index, w);
        const char *tail;

        if (t < 0)
            t = lyrics_time(index);
        if (t <= elapsed)
            continue;

        tail = lyrics_word_text(index, w);
        return tail? (int)(strlen(text) - strlen(tail)): 0;
    }
    return (int)strlen(text);
}

/* When the next word of line `index` starts, or 0 if there is none to wait
 * for. Drives the wait, so the highlight moves on the word rather than
 * whenever something else happens to wake the screen. */
static long lv_next_word(int index, long elapsed)
{
    int n = lyrics_word_count(index);
    int w;

    if (index == LYRICS_NONE || !LV_HIGHLIGHT)
        return 0;

    for (w = 0; w < n; w++)
    {
        long t = lyrics_word_time(index, w);
        if (t >= 0 && t > elapsed)
            return t;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Screen setup and the fade
 * ------------------------------------------------------------------------ */

/* Re-read the theme colours through the dynamic-colour resolver, which swaps
 * in the palette taken from the album art when that feature is on and the
 * colour is one of the theme's own.
 *
 * Called when the lyrics are loaded and again on a track change -- i.e. when
 * the album art, and so the palette, can actually differ. Deliberately not on
 * every redraw: the resolver interpolates while a fade is running, and
 * re-resolving per frame would put that work inside the animation loop for a
 * change nobody asked to see mid-song. */
static void lv_refresh_colours(void)
{
    unsigned fg = global_settings.fg_color;
    unsigned bg = global_settings.bg_color;

    switch (global_settings.lyric_colour_mode)
    {
        case LV_COLOUR_INVERTED:
            fg = global_settings.bg_color;
            bg = global_settings.fg_color;
            break;
        case LV_COLOUR_BOW:
            fg = LCD_BLACK; bg = LCD_WHITE;
            break;
        case LV_COLOUR_WOB:
            fg = LCD_WHITE; bg = LCD_BLACK;
            break;
        case LV_COLOUR_THEME:
        default:
            break;
    }

    /* Only the theme's own colours follow the album art -- an explicitly
     * chosen black-on-white is a choice, not a default to be overridden.
     * dynamic_colors_resolve() already only acts on colours that match the
     * theme, so passing the fixed pairs through it would be a no-op; it is
     * skipped to say so. */
    if (global_settings.lyric_colour_mode == LV_COLOUR_THEME)
    {
        fg = dynamic_colors_resolve(fg);
        bg = dynamic_colors_resolve(bg);
    }

    lv.fg = fg;
    lv.bg = bg;
    lv.vp.fg_pattern = fg;
    lv.vp.bg_pattern = bg;
}

/* Load the font override if one is set, else fall back to the UI font.
 * Unload-then-load keeps re-applying the same file leak-free (font_load is
 * refcounted). Uses the resolved UI font id, never the FONT_UI sentinel --
 * see the long note on tv_apply_font() in the text viewer for why. */
static void lv_apply_font(void)
{
    int fid = -1;

    if (lv.user_font >= 0)
    {
        font_unload(lv.user_font);
        lv.user_font = -1;
    }

    lv.font_error[0] = '\0';

    if (global_settings.lyric_font_file[0])
    {
        char buf[MAX_PATH];
        snprintf(buf, sizeof buf, FONT_DIR "/%s.fnt",
                 global_settings.lyric_font_file);
        fid = font_load(buf);
        /* A font that will not load otherwise falls back to the UI font in
         * silence, which reads as "the setting does nothing". font_load()
         * fails for a missing file, a full font-slot table, or no buflib room
         * -- and says which by not saying anything, so report the attempt. */
        if (fid < 0)
            strlcpy(lv.font_error, buf, sizeof lv.font_error);
    }

    lv.user_font = fid;
    lv.font = (fid >= 0)? fid: screens[SCREEN_MAIN].getuifont();
    lv.vp.font = lv.font;
}

/* (Re)read everything the settings control. Called on entry and again after
 * the menu, so a change takes effect without leaving the screen. */
static void lv_apply_settings(void)
{
    unsigned int idx = global_settings.lyric_anim;

    lv_apply_font();
    lv_refresh_colours();

    lv.anim_ms = (idx < ARRAYLEN(lv_anim_ms))? lv_anim_ms[idx]: 0;
    lv.row = font_get(lv.font)->height + global_settings.lyric_line_spacing;
    lv.gap = lv.row / 2;
    if (lv.gap < 6)
        lv.gap = 6;
    lv.width = lv.vp.width - 2 * LV_MARGIN;
}

static void lv_setup_screen(void)
{
    /* Theme off hands back a full-screen viewport and stops the skin engine
     * repainting the status bar over us between draws -- see the long comment
     * on tv_setup_screen() in the text viewer, which hit exactly that. */
    viewportmanager_theme_enable(SCREEN_MAIN, false, &lv.vp);
    lv_apply_settings();
}

/* Keep the screen lit, and put the user's timeouts back afterwards. Matches
 * cr_backlight_ignore_timeout() in credits.c -- a timeout of 0 means "never",
 * and only a timeout that was actually running is overridden. */
static void lv_backlight_hold(bool hold)
{
    if (!global_settings.lyric_backlight)
        return;

    if (hold)
    {
        if (global_settings.backlight_timeout > 0)
            backlight_set_timeout(0);
        if (global_settings.backlight_timeout_plugged > 0)
            backlight_set_timeout_plugged(0);
    }
    else
    {
        backlight_set_timeout(global_settings.backlight_timeout);
        backlight_set_timeout_plugged(
            global_settings.backlight_timeout_plugged);
    }
}

/* Wipe the screen to our own background. The WPS leaves without restoring the
 * theme (HOTKEY_FLAG_NOSBS), so until this runs the screen still holds its
 * pixels. */
static void lv_clear(void)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport *last = d->set_viewport(&lv.vp);

    d->clear_viewport();
    d->update_viewport();
    d->set_viewport(last);
}

/* `opacity` per cent of the way from the background to the foreground. There
 * is no alpha channel here: text is drawn in an interpolated colour, which is
 * exact against a solid background and stays consistent across the blended
 * edge of an antialiased glyph. */
static unsigned lv_fade(int opacity)
{
    return color_blend(lv.bg, lv.fg, opacity * 256 / 100);
}

/* ---------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------ */

/* Cut `line` down so that it plus "..." fits maxwidth, then append the "...".
 * Modelled on wt_ellipsize() in skin/skin_render.c; when stage 2 adds word
 * wrapping the pair is worth sharing rather than copying again. */
static void lv_ellipsize(char *line, int size, struct font *pf, int maxwidth)
{
    const unsigned char *start = (const unsigned char *)line;
    const unsigned char *p = start;
    int ellw = 3 * font_get_width(pf, '.');
    int width = 0;
    int cut = 0;                            /* bytes that fit, plus the "..." */
    ucschar_t ch;

    while (*p)
    {
        const unsigned char *prev = p;
        p = utf8decode(p, &ch);
        if (IS_DIACRITIC(ch))
            continue;
        if (width + font_get_width(pf, ch) + ellw > maxwidth)
        {
            p = prev;
            break;
        }
        width += font_get_width(pf, ch);
        cut = (int)(p - start);
    }
    if (!*p)
        return;                             /* the whole line fitted */

    if (cut + 4 <= size)
        strcpy(line + cut, "...");
    else if (size >= 4)
        strcpy(line + size - 4, "...");
}

/* ---- word wrap -------------------------------------------------------- */

/* One wrapped row: a span of the model's string, not a copy of it. */
struct lv_row
{
    const char *start;
    int len;
};

/* May a line break after the character of `len` bytes at `s`? Spaces, plus
 * the punctuation lifted from isbrchr() in the lrcplayer plugin: CJK is
 * written without spaces, so with spaces alone a Japanese or Chinese lyric
 * would only ever break at the hard edge of the screen. */
static bool lv_is_break(const unsigned char *s, int len)
{
    static const unsigned char brk[] = "!,-.:;?　、。！，．：；？―";
    const unsigned char *p = brk;

    if (*s == ' ' || *s == '\t')
        return true;

    while (*p)
    {
        int n = utf8seek(p, 1);
        if (len == n && !memcmp(p, s, len))
            return true;
        p += n;
    }
    return false;
}

/* Break `text` into rows no wider than `maxwidth`, filling `rows`. Returns how
 * many were used, and sets *cut if the text ran past `max` rows. One forward
 * pass, remembering the last place a break was allowed and going back to it on
 * overflow -- the same shape as %wt in the skin engine. */
static int lv_wrap(const char *text, int maxwidth, struct lv_row *rows,
                   int max, bool *cut_off)
{
    struct font *pf = font_get(lv.font);
    const unsigned char *start = (const unsigned char *)text;
    const unsigned char *p = start;
    const unsigned char *brk = NULL;    /* just after the last break point */
    int width = 0;
    int n = 0;
    ucschar_t ch;

    *cut_off = false;

    while (n < max)
    {
        const unsigned char *chstart = p;
        int w;

        if (*p == '\0')                 /* last row, however short */
        {
            rows[n].start = (const char *)start;
            rows[n].len = (int)(p - start);
            return n + 1;
        }

        p = utf8decode(p, &ch);
        w = IS_DIACRITIC(ch)? 0: font_get_width(pf, ch);

        /* chstart != start keeps a single character wider than the screen
         * from breaking before itself, which would never terminate */
        if (width + w > maxwidth && chstart != start)
        {
            const unsigned char *cut = brk? brk: chstart;

            rows[n].start = (const char *)start;
            rows[n].len = (int)(cut - start);
            n++;
            while (*cut == ' ')         /* the break's space starts no row */
                cut++;
            start = p = cut;
            width = 0;
            brk = NULL;
            continue;
        }

        width += w;
        if (lv_is_break(chstart, (int)(p - chstart)))
            brk = p;
    }

    *cut_off = true;
    return n;
}

/* Copy a row into the scratch buffer, trimming the space a break left on its
 * end. `to_end` takes everything from the row's start to the end of the line
 * instead, which is what a truncated last row wants: ellipsising that gives
 * "...", where ellipsising the row alone would have nothing to cut. */
static void lv_row_text(char *buf, size_t size, const struct lv_row *row,
                        bool to_end)
{
    const char *s = row->start;
    size_t len = to_end? strlen(s): (size_t)row->len;

    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t'))
        len--;
    if (len > size - 1)
        len = size - 1;
    memcpy(buf, s, len);
    buf[len] = '\0';
}

/* Draw one lyric line centred at `y`. Out-of-range indices draw nothing, so
 * the caller can ask for index-1 and index+1 without checking the ends -- and
 * LYRICS_NONE, meaning the song has not reached the first line, falls out as
 * "no current line, first line as next". */
/* Draw a prepared string, horizontally centred, with its top at `y`. */
/* Where a row `w` wide starts, for the chosen alignment. */
static int lv_x(int w)
{
    switch (global_settings.lyric_align)
    {
        case LV_ALIGN_LEFT:  return LV_MARGIN;
        case LV_ALIGN_RIGHT: return LV_MARGIN + lv.width - w;
        case LV_ALIGN_CENTRE:
        default:             return LV_MARGIN + (lv.width - w) / 2;
    }
}

static void lv_draw_text(struct screen *d, const char *text, int y, int opacity)
{
    int w, h;

    font_getstringsize((const unsigned char *)text, &w, &h, lv.font);
    d->set_foreground(lv_fade(opacity));
    d->putsxy(lv_x(w), y, (const unsigned char *)text);
}

/* The same, but with the first `cut` bytes drawn at `op_sung` and the rest at
 * `op_rest`. The row is still centred on its whole width, and the two pieces
 * are laid end to end -- the fonts here do not kern, so the halves measure the
 * same as the whole. */
static void lv_draw_split(struct screen *d, const char *text, int cut,
                          int y, int op_sung, int op_rest)
{
    static char head[LV_ROW_MAX];
    int w, h, wsung = 0, x;

    font_getstringsize((const unsigned char *)text, &w, &h, lv.font);
    x = lv_x(w);

    if (cut > 0)
    {
        if (cut > (int)sizeof head - 1)
            cut = (int)sizeof head - 1;
        memcpy(head, text, (size_t)cut);
        head[cut] = '\0';
        font_getstringsize((const unsigned char *)head, &wsung, &h, lv.font);
        d->set_foreground(lv_fade(op_sung));
        d->putsxy(x, y, (const unsigned char *)head);
    }

    if (text[cut])
    {
        d->set_foreground(lv_fade(op_rest));
        d->putsxy(x + wsung, y, (const unsigned char *)text + cut);
    }
}

/* The screen when there are no lyrics to draw: what it is waiting for, and
 * which track it is waiting about. Named rather than left blank, because
 * every one of these states can become lyrics without the user doing
 * anything, and a bare screen would not say that. */
static void lv_draw_status(struct screen *d)
{
    static char buf[LV_ROW_MAX];
    int y = (lv.vp.height - lv.row) / 2;
    int msg = (lv.state == LV_WAITING)?   LANG_LYRICS_WAITING:
              (lv.state == LV_NO_MEMORY)? LANG_OUT_OF_MEMORY:
                                          LANG_LYRICS_NONE;

    if (!lv.name[0])
    {
        lv_draw_text(d, (const char *)str(msg), y, 100);
        return;
    }

    y -= (lv.row + lv.gap) / 2;
    lv_draw_text(d, (const char *)str(msg), y, 100);

    strlcpy(buf, lv.name, sizeof buf);
    lv_ellipsize(buf, sizeof buf, font_get(lv.font), lv.width);
    lv_draw_text(d, buf, y + lv.row + lv.gap, LV_OPACITY_NEXT);
}

/* Opacity for a neighbour row `d` rows away from the current block: the
 * nearest row gets the line's full share, and each row beyond it drops to two
 * fifths, so a wrapped neighbour fades away from the middle of the screen
 * rather than ending on a hard edge. */
static int lv_falloff(int base, int d)
{
    while (--d > 0)
        base = base * 2 / 5;
    return base;
}

/* Opacity of row `r` of an `n`-row line sitting `rel` places from the middle:
 * 0 is the current line, -1 the one above it, +1 the one below. Further out
 * than that is off the layout and invisible -- which is what gives the
 * outgoing line something to fade to and the incoming line something to fade
 * from. For the line above, its *last* row is the one nearest the middle. */
static int lv_row_opacity(int rel, int r, int n)
{
    if (rel == 0)
        return 100;
    if (rel == -1)
        return lv_falloff(LV_OPACITY_PREV, n - r);
    if (rel == 1)
        return lv_falloff(LV_OPACITY_NEXT, r + 1);
    return 0;
}

/* The same, split: what a row's sung and not-yet-sung halves are worth.
 *
 * Only the current line has halves -- a neighbour is not being sung, so both
 * of its values are the one opacity. This is what stops a word-timed line
 * arriving at full strength and being dimmed a moment later: unsung text is
 * the lighter colour *while the line moves in*, not after it lands. A file
 * with no word timing reports itself wholly sung, so nothing changes for it. */
static void lv_row_ops(int rel, int r, int n, int *sung, int *rest)
{
    *sung = *rest = lv_row_opacity(rel, r, n);
    if (rel == 0)
        *rest = LV_OPACITY_NEXT;
}

/* Draw a neighbour line: wrapped like the current one, its rows fading with
 * distance and stopping at the screen edge. `dir` is -1 for the line above
 * (drawn upwards from `edge`) and +1 for the one below. */
static void lv_draw_neighbour(struct screen *d, int index, int edge, int dir,
                              int base)
{
    static struct lv_row rows[LV_SIDE_ROWS];
    static char buf[LV_ROW_MAX];
    bool cut_off;
    int n, i;

    if (index == LYRICS_NONE)
        return;

    n = lv_wrap(lv_line_text(index), lv.width, rows, LV_SIDE_ROWS, &cut_off);

    for (i = 0; i < n; i++)
    {
        /* row nearest the current block is distance 1; for the line above,
         * that is its *last* row */
        int row = (dir < 0)? n - 1 - i: i;
        int dist = i + 1;
        int y = (dir < 0)? edge - dist * lv.row: edge + i * lv.row;
        bool last = cut_off && row == n - 1;

        if (y < LV_MARGIN || y + lv.row > lv.vp.height - LV_MARGIN)
            break;                      /* off the screen; the rest would be */

        lv_row_text(buf, sizeof buf, &rows[row], last);
        if (last)
            lv_ellipsize(buf, sizeof buf, font_get(lv.font), lv.width);
        lv_draw_text(d, buf, y, lv_falloff(base, dist));
    }
}

/* Where the top of the current block sits: centred, then shifted up only far
 * enough to keep the start of the next line on screen, then clamped. */
static int lv_block_top(int nrows, bool next_present)
{
    int top = (lv.vp.height - nrows * lv.row) / 2;

    if (next_present)
    {
        int over = (top + nrows * lv.row + lv.gap + lv.row)
                 - (lv.vp.height - LV_MARGIN);
        if (over > 0)
            top -= over;
    }
    return (top < LV_MARGIN)? LV_MARGIN: top;
}

/* ---- the slide ---------------------------------------------------------
 *
 * A transition is the same four lines seen in two layouts: the one centred
 * before, and the one centred after. Every line's height is the same in both
 * -- that is what stage 2's wrapping of the neighbours bought -- so each line
 * simply moves from where it was to where it is going, and its rows fade
 * between the opacities the two layouts give them.
 *
 * Falling out of that: the line leaving the top has no place in the target
 * layout, so it keeps travelling upwards and fades to nothing; the line
 * arriving at the bottom has no place in the source, so it does the reverse.
 * Neither needs a special case.
 */

enum { LV_SLOT_OUT = 0, LV_SLOT_FROM, LV_SLOT_TO, LV_SLOT_IN, LV_SLOTS };

struct lv_slot
{
    int index;
    struct lv_row rows[LV_MAX_ROWS];
    int nrows;
    bool cut_off;
    int sung;                   /* bytes of the line already sung */
    int y_src, y_dst;
    int rel_src, rel_dst;       /* place in each layout; see lv_row_opacity */
};

static struct
{
    bool active;
    long start, end;            /* ticks */
    int band_top, band_bot;     /* what the last frame painted */
    struct lv_slot slot[LV_SLOTS];
} anim;

static int lv_lerp(int a, int b, int p)
{
    return a + (b - a) * p / 256;
}

/* Wall clock, never a frame count: a slow frame then shortens the animation
 * instead of stretching it, which is what keeps it landing on the beat when
 * the codec is taking the CPU. Eased out, so it moves off smartly and
 * settles. */
static int lv_anim_progress(void)
{
    long span = anim.end - anim.start;
    long done;
    int p;

    if (span <= 0 || TIME_AFTER(current_tick, anim.end))
        return 256;

    done = current_tick - anim.start;
    p = (done <= 0)? 0: (int)(done * 256 / span);
    if (p > 256)
        p = 256;
    return 256 - ((256 - p) * (256 - p)) / 256;
}

/* Wrap the four lines and work out where each sits in both layouts. Done once
 * per transition: the text cannot change while it runs, and re-wrapping four
 * lines every frame is work the frame budget has not got. */
static void lv_anim_build(int from, int to)
{
    struct lv_slot *s = anim.slot;
    int idx[LV_SLOTS];
    int top_src, top_dst;
    int i;

    idx[LV_SLOT_OUT]  = lv_step(from, -1);
    idx[LV_SLOT_FROM] = from;
    idx[LV_SLOT_TO]   = to;
    idx[LV_SLOT_IN]   = lv_step(to, 1);

    for (i = 0; i < LV_SLOTS; i++)
    {
        s[i].index = idx[i];
        s[i].nrows = 0;
        s[i].cut_off = false;
        s[i].sung = 0;
        if (idx[i] != LYRICS_NONE)
        {
            s[i].nrows = lv_wrap(lv_line_text(idx[i]), lv.width,
                                 s[i].rows, LV_MAX_ROWS, &s[i].cut_off);
            /* The slide finishes exactly as `to` falls due, so for its whole
             * length the outgoing line is sung and the incoming one is not.
             * Fixed here rather than re-read per frame. */
            s[i].sung = (i <= LV_SLOT_FROM)?
                        (int)strlen(lv_line_text(idx[i])): 0;
        }
    }

    /* source: `from` is the current line */
    top_src = lv_block_top(s[LV_SLOT_FROM].nrows,
                           s[LV_SLOT_TO].index != LYRICS_NONE);
    s[LV_SLOT_FROM].y_src = top_src;
    s[LV_SLOT_OUT].y_src  = top_src - lv.gap - s[LV_SLOT_OUT].nrows * lv.row;
    s[LV_SLOT_TO].y_src   = top_src + s[LV_SLOT_FROM].nrows * lv.row + lv.gap;
    s[LV_SLOT_IN].y_src   = s[LV_SLOT_TO].y_src
                          + s[LV_SLOT_TO].nrows * lv.row + lv.gap;

    /* target: `to` is */
    top_dst = lv_block_top(s[LV_SLOT_TO].nrows,
                           s[LV_SLOT_IN].index != LYRICS_NONE);
    s[LV_SLOT_TO].y_dst   = top_dst;
    s[LV_SLOT_FROM].y_dst = top_dst - lv.gap - s[LV_SLOT_FROM].nrows * lv.row;
    s[LV_SLOT_OUT].y_dst  = s[LV_SLOT_FROM].y_dst
                          - lv.gap - s[LV_SLOT_OUT].nrows * lv.row;
    s[LV_SLOT_IN].y_dst   = top_dst + s[LV_SLOT_TO].nrows * lv.row + lv.gap;

    s[LV_SLOT_OUT].rel_src  = -1;  s[LV_SLOT_OUT].rel_dst  = -2;
    s[LV_SLOT_FROM].rel_src =  0;  s[LV_SLOT_FROM].rel_dst = -1;
    s[LV_SLOT_TO].rel_src   =  1;  s[LV_SLOT_TO].rel_dst   =  0;
    s[LV_SLOT_IN].rel_src   =  2;  s[LV_SLOT_IN].rel_dst   =  1;
}

/* One frame. Repaints and flushes only the band the lines occupy, full width:
 * lcd_update_rect writes one address to the panel when the width is the whole
 * screen and one per row otherwise, so a full-width strip is much the cheapest
 * partial update -- and a whole-screen flush is ~9.7ms of a ~11ms frame. */
static void lv_anim_draw(int p)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport *last = d->set_viewport(&lv.vp);
    static char buf[LV_ROW_MAX];
    int y[LV_SLOTS];
    int top = lv.vp.height, bot = 0;
    int i, r;

    for (i = 0; i < LV_SLOTS; i++)
    {
        struct lv_slot *s = &anim.slot[i];

        if (s->index == LYRICS_NONE || s->nrows == 0)
            continue;
        y[i] = lv_lerp(s->y_src, s->y_dst, p);
        if (y[i] < top)
            top = y[i];
        if (y[i] + s->nrows * lv.row > bot)
            bot = y[i] + s->nrows * lv.row;
    }

    /* repaint the union of this frame and the last, so what moved away is
     * cleared as well as what moved in */
    if (anim.band_top < top)
        top = anim.band_top;
    if (anim.band_bot > bot)
        bot = anim.band_bot;
    if (top < 0)
        top = 0;
    if (bot > lv.vp.height)
        bot = lv.vp.height;
    if (bot <= top)
        goto out;

    d->set_foreground(lv.bg);
    d->fillrect(0, top, lv.vp.width, bot - top);

    for (i = 0; i < LV_SLOTS; i++)
    {
        struct lv_slot *s = &anim.slot[i];

        if (s->index == LYRICS_NONE)
            continue;

        for (r = 0; r < s->nrows; r++)
        {
            int ry = y[i] + r * lv.row;
            int s0, r0, s1, r1, op_sung, op_rest, cut;
            bool clipped = s->cut_off && r == s->nrows - 1;

            lv_row_ops(s->rel_src, r, s->nrows, &s0, &r0);
            lv_row_ops(s->rel_dst, r, s->nrows, &s1, &r1);
            op_sung = lv_lerp(s0, s1, p);
            op_rest = lv_lerp(r0, r1, p);

            if (op_sung <= 0 && op_rest <= 0)
                continue;
            if (ry < LV_MARGIN || ry + lv.row > lv.vp.height - LV_MARGIN)
                continue;

            lv_row_text(buf, sizeof buf, &s->rows[r], clipped);
            if (clipped)
                lv_ellipsize(buf, sizeof buf, font_get(lv.font), lv.width);

            cut = s->sung - (int)(s->rows[r].start - lv_line_text(s->index));
            if (op_sung == op_rest || cut >= (int)strlen(buf))
                lv_draw_text(d, buf, ry, op_sung);
            else
                lv_draw_split(d, buf, cut < 0? 0: cut, ry, op_sung, op_rest);
        }
    }

    d->update_viewport_rect(0, top, lv.vp.width, bot - top);
    anim.band_top = top;
    anim.band_bot = bot;

  out:
    d->set_viewport(last);
}

/* Lay the three lines out and draw them.
 *
 * Every line wraps, including the neighbours, and neighbour rows fade with
 * their distance from the middle. That is not only how it looks: a line keeps
 * the same height whether it is previous, current or next, so the animation in
 * stage 3 can slide it between those roles as a plain translation instead of
 * re-flowing it mid-move.
 *
 * The current block is centred, and shifts up only far enough to keep the
 * start of the next line on screen -- next is the one the reader is about to
 * want. Anything that still does not fit simply runs off the edge, by which
 * point the falloff has made it nearly invisible anyway. */
static void lv_draw(void)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport *last = d->set_viewport(&lv.vp);
    static struct lv_row cur[LV_MAX_ROWS];
    static char buf[LV_ROW_MAX];
    int prev, next, nrows = 0, top, i;
    bool cut_off = false;

    d->clear_viewport();

    if (lv.state != LV_SHOWING || !lyrics_loaded())
    {
        lv_draw_status(d);
        goto done;
    }

    prev = lv_step(lv.shown, -1);
    next = lv_step(lv.shown, 1);

    /* Ask for the text rather than testing the index: lv.shown is
     * LV_INDEX_UNSET until the first line is picked, and LV_TITLE_INDEX until
     * the first lyric is due -- one of which draws and one of which does
     * not. */
    if (lv_line_text(lv.shown))
        nrows = lv_wrap(lv_line_text(lv.shown), lv.width, cur, LV_MAX_ROWS,
                        &cut_off);

    top = lv_block_top(nrows, next != LYRICS_NONE);

    /* a slide starting from here repaints only what it moves, so it has to
     * know what this frame put on the screen */
    anim.band_top = 0;
    anim.band_bot = lv.vp.height;

    lv_draw_neighbour(d, prev, top - lv.gap, -1, LV_OPACITY_PREV);

    for (i = 0; i < nrows; i++)
    {
        int y = top + i * lv.row;
        bool clipped = cut_off && i == nrows - 1;
        int cut;

        if (y + lv.row > lv.vp.height - LV_MARGIN)
            break;
        /* a truncated last row runs to the end of the text, so ellipsising it
         * shows that something was cut */
        lv_row_text(buf, sizeof buf, &cur[i], clipped);
        if (clipped)
            lv_ellipsize(buf, sizeof buf, font_get(lv.font), lv.width);

        /* where the sung/unsung boundary falls within this row -- it can land
         * anywhere, including part way along a wrapped one */
        cut = lv.sung - (int)(cur[i].start - lv_line_text(lv.shown));
        if (cut >= (int)strlen(buf))
            lv_draw_text(d, buf, y, 100);
        else
            lv_draw_split(d, buf, cut < 0? 0: cut, y, 100, LV_OPACITY_NEXT);
    }

    lv_draw_neighbour(d, next, top + nrows * lv.row + lv.gap, 1,
                      LV_OPACITY_NEXT);

  done:

    /* A whole-screen flush, which is right while a redraw happens once per
     * lyric line. The animation in stage 3 redraws per frame and will want the
     * full-width dirty band described in the spec instead. */
    d->update_viewport();
    d->set_viewport(last);
}

/* ---------------------------------------------------------------------------
 * Following playback
 * ------------------------------------------------------------------------ */

/* How long to sleep before the display could next need to change: until the
 * next line with words is due -- a gap starting changes nothing on screen --
 * less the slide, which has to be finished by then rather than starting then.
 * Capped so the track is still checked regularly. */
static int lv_ticks_to_next(long elapsed)
{
    int next = lv_step(lv.shown, 1);
    long due = (next != LYRICS_NONE)? lyrics_time(next) - LV_ANIM_MS: 0;
    long word = lv_next_word(lv.shown, elapsed);
    int ticks = LV_POLL;

    /* whichever comes first: the slide starting, or the highlight moving on
     * to the next word */
    if (word > 0 && (next == LYRICS_NONE || word < due))
    {
        due = word;
        next = lv.shown;            /* something to wait for either way */
    }

    if (next != LYRICS_NONE && due > elapsed)
    {
        /* round up, so waking lands on or after the boundary rather than a
         * few milliseconds short of it and having to sleep again */
        long ms = due - elapsed;
        ticks = (int)((ms * HZ + 999) / 1000);
    }

    if (ticks < 1)
        return 1;
    return (ticks > LV_POLL)? LV_POLL: ticks;
}

/* Point the screen at a different track. Failing to find lyrics is not an
 * error here -- unlike on entry, the screen stays up saying so, because the
 * track after this one may well have some. */
/* Move to a state, remembering what to call the track, and redraw if either
 * actually changed -- the loop calls this every time round. */
static void lv_set_state(enum lv_state state, const struct mp3entry *id3)
{
    char name[MAX_PATH];

    name[0] = '\0';
    if (id3)
    {
        const char *slash;

        if (id3->title && id3->title[0])
            strlcpy(name, id3->title, sizeof name);
        else if ((slash = strrchr(id3->path, '/')) != NULL)
            strlcpy(name, slash + 1, sizeof name);
    }

    if (state == lv.state && !strcmp(name, lv.name))
        return;

    lv.state = state;
    strlcpy(lv.name, name, sizeof lv.name);
    lv_draw();
}

static enum lyrics_result lv_load_track(const struct mp3entry *id3)
{
    strlcpy(lv.track, id3->path, sizeof lv.track);
    lv.shown = LV_INDEX_UNSET;
    lv.sung = 0;
    /* A slide holds lv_row pointers *into the lyrics buffer*, and loading
     * frees that buffer. Anything still running must stop before it does, or
     * the next frame reads freed memory. */
    anim.active = false;
    /* new track, so possibly new album art and a new palette */
    lv_refresh_colours();
    return lyrics_load(id3);
}

/* Skip tracks the way the WPS does: forward goes to the next track, and back
 * restarts this one unless it has only just started, in which case it goes to
 * the previous. play_hop() in wps.c is the real thing but is static and reads
 * the WPS's own id3 pointer, which is not ours to trust once the WPS has been
 * left, so the rule is reproduced rather than borrowed. */
static void lv_skip(int dir)
{
    struct mp3entry *id3 = audio_current_track();

    if (global_settings.prevent_skip)
        return;

    if (dir > 0)
        audio_next();
    else if (id3 && id3->elapsed > DEFAULT_SKIP_THRESH)
        audio_ff_rewind(0);
    else
        audio_prev();

    sleep(HZ / 10);     /* let it land before elapsed is read again */
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------ */

/* Hold-Menu settings, the same menu as Settings > Lyrics Viewer. The theme
 * goes back on just for the menu's own chrome, then we take the screen again
 * and re-read whatever was changed -- font, colours and spacing all move the
 * layout, so the whole screen is redrawn rather than patched. */
static int lv_menu(void)
{
    int sel = 0, ret;

    /* Let go of the working buffer before the menu runs. It is 32 KiB and
     * immovable, and the menu allocates -- a font picked in it calls
     * font_load(), which wants a buflib block of its own. Nothing in the menu
     * needs the lyrics, and clearing lv.track makes the loop reload them. */
    lyrics_close();
    lv.track[0] = '\0';
    anim.active = false;        /* its rows pointed into that buffer */

    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    push_current_activity(ACTIVITY_CONTEXTMENU);
    ret = do_menu(&lyric_viewer_menu, &sel, NULL, false);
    pop_current_activity();
    viewportmanager_theme_undo(SCREEN_MAIN, false);

    lv_setup_screen();          /* re-reads font, colours, spacing */
    lv_backlight_hold(true);    /* the setting may have just been turned on */
    lv.state = LV_SHOWING;      /* so the reload's lv_set_state() redraws */
    lv_clear();
    if (lv.font_error[0])
        splashf(HZ * 3, "Font failed: %s", lv.font_error);
    return ret;
}

int lyric_viewer(void)
{
    struct mp3entry *id3;
    int ret = GO_TO_PREVIOUS;

    /* `anim` is static and outlives the screen. Leaving mid-slide would
     * otherwise leave it armed, and the next visit would draw a frame from
     * row pointers into the previous visit's freed lyrics buffer. */
    anim.active = false;
    lv.track[0] = '\0';
    lv.user_font = -1;
    lv.state = LV_SHOWING;   /* forces the first lv_set_state() to draw */

    push_current_activity(ACTIVITY_LYRICS);
    lv_backlight_hold(true);
    lv_setup_screen();
    lv_clear();
    if (lv.font_error[0])
        splashf(HZ * 3, "Font failed: %s", lv.font_error);

    /* The screen opens whatever the state of things and says what it is
     * waiting for, rather than refusing to open. Nothing here is an error
     * worth throwing the user back to where they came from: a track with no
     * lyrics is ordinary, and one whose metadata has not arrived yet becomes
     * a track with lyrics a moment later. */
    while (1)
    {
        int action, index, timeout;

        id3 = audio_current_track();

        if (!id3 || !(audio_status() & AUDIO_STATUS_PLAY) || !id3->path[0])
        {
            /* Nothing playing, or its metadata has not filled in yet. Note
             * that no allocation happens on this path: the load only runs
             * once there is a path to load for. */
            lv_set_state(LV_WAITING, NULL);
            lv.track[0] = '\0';
        }
        else if (strcmp(id3->path, lv.track) != 0)
        {
            enum lyrics_result rc = lv_load_track(id3);

            lv_set_state(rc == LYRICS_OK? LV_SHOWING:
                         rc == LYRICS_NO_MEMORY? LV_NO_MEMORY:
                                                 LV_NO_LYRICS, id3);
        }

        if (lv.state != LV_SHOWING)
        {
            /* gently: nothing to animate, so just look again shortly */
            action = get_action(CONTEXT_WPS, LV_POLL);
            goto handle;
        }

        /* Redraw on a change of the line *shown*, not of the raw index: a gap
         * starting is a change of index that leaves the screen alone. */
        index = lv_shown_index(lyrics_index_at(id3->elapsed));

        if (anim.active)
        {
            int p = lv_anim_progress();

            /* the song moving somewhere the slide was not heading -- a seek,
             * or simply a slow frame overrunning it -- ends it early */
            if (index != lv.shown && index != anim.slot[LV_SLOT_TO].index)
                p = 256;

            lv_anim_draw(p);
            if (p >= 256)
            {
                anim.active = false;
                lv.shown = anim.slot[LV_SLOT_TO].index;
                lv.sung = lv_sung(lv.shown, id3->elapsed);
                /* settle on the real layout, whole screen: the frames above
                 * only repainted the band they moved within */
                lv_draw();
                /* if the song has already gone further, the next turn round
                 * the loop slides or snaps to wherever it actually is */
            }
        }
        else if (index == lv.shown)
        {
            /* same line: the highlight may still have moved on a word */
            int sung = (index == LYRICS_NONE)? 0: lv_sung(index, id3->elapsed);

            if (sung != lv.sung)
            {
                lv.sung = sung;
                lv_draw();
            }
        }
        else
        {
            int upcoming = lv_step(lv.shown, 1);

            /* Slide only for the ordinary case: one line forward, to the line
             * the wait above was timed against. A seek, a track change or a
             * jump of several lines snaps -- animating those would be a lie
             * about where the song went. */
            if (LV_ANIM_MS > 0 && lv.shown != LV_INDEX_UNSET
                && index == upcoming && upcoming != LYRICS_NONE)
            {
                lv_anim_build(lv.shown, index);
                anim.start = current_tick;
                anim.end = current_tick + LV_ANIM_MS * HZ / 1000;
                anim.active = true;
                lv_anim_draw(lv_anim_progress());
            }
            else
            {
                lv.shown = index;
                lv.sung = (index == LYRICS_NONE)?
                          0: lv_sung(index, id3->elapsed);
                lv_draw();
            }
        }

        timeout = anim.active? 1: lv_ticks_to_next(id3->elapsed);
        action = get_action(CONTEXT_WPS, timeout);

      handle:
        /* CONTEXT_WPS, so the transport keys are the ones muscle memory
         * already has from the WPS: left/right skip, Play pauses, the wheel
         * is volume. */
        switch (action)
        {
            case ACTION_WPS_MENU:           /* Menu leaves the screen */
            case ACTION_WPS_BROWSE:
                goto done;

            case ACTION_WPS_PLAY:
                /* the real thing, so pause_rewind and the fade behave as they
                 * do in the WPS */
                wps_do_playpause(false);
                break;

            case ACTION_WPS_SKIPNEXT:
                lv_skip(1);
                break;

            case ACTION_WPS_SKIPPREV:
                lv_skip(-1);
                break;

            case ACTION_WPS_VOLUP:
                adjust_volume(1);
                break;

            case ACTION_WPS_VOLDOWN:
                adjust_volume(-1);
                break;

            case ACTION_WPS_QUICKSCREEN:    /* hold Menu: the settings */
                if (lv_menu() == MENU_ATTACHED_USB)
                {
                    ret = GO_TO_ROOT;
                    goto done;
                }
                break;

            case ACTION_NONE:               /* the sleep expired */
                break;

            default:
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                {
                    ret = GO_TO_ROOT;
                    goto done;
                }
                break;
        }
    }

  done:
    /* The model's buffer is immovable, so it should not outlive the screen --
     * least of all into the USB path, where an immovable block in the middle
     * of the arena is exactly what compaction cannot work around. */
    lyrics_close();
    if (lv.user_font >= 0)
    {
        font_unload(lv.user_font);
        lv.user_font = -1;
    }
    lv_backlight_hold(false);
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    pop_current_activity();
    return ret;
}
