/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The text reader UI: paging, scrolling, bookmarking and settings, drawing
 * text pulled from the ts_* extraction engine.
 *
 * The ts_* engine (txt_source.h) is a one-way stream: ts_read() goes forward
 * and ts_rewind() goes back to the start, with nothing in between. Rather
 * than extract whole documents up front -- which would scale with the file
 * and fight the audio buffer for RAM -- this keeps a fixed sliding window of
 * recently extracted text. Reading is overwhelmingly linear and paging back
 * almost always means "the page I just read", so the window absorbs
 * essentially all backward navigation at a flat cost. Only a jump behind the
 * window falls back to a rewind and re-extract.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>          /* atoi */
#include <stdio.h>           /* snprintf */
#include <string.h>
#include "string-extra.h"    /* strmemccpy */
#include "config.h"
#include "file.h"            /* off_t, open/close/rename/remove, fdprintf */
#include "rbpaths.h"         /* ROCKBOX_DIR */
#include "input/action.h"
#include "draw/screen_access.h"   /* screens[] */
#include "kernel.h"          /* HZ, SYS_USB_CONNECTED */
#include "core_alloc.h"
#include "settings/settings.h"
#include "lang.h"
#include "widgets/splash.h"
#include "draw/viewport.h"
#include "font.h"
#include "system/activity.h"
#include "system/shutdown.h"
#include "system/strutil.h"
#include "root_menu.h"       /* GO_TO_*, MENU_ATTACHED_USB */
#include "widgets/menu.h"            /* do_menu */
#include "screens/settings/exported_settings.h" /* text_viewer_menu */
#include "txt_source.h"
#include "ts_io_core.h"
#include "text_viewer.h"

/* Extracted text kept resident, and how much of it sits behind the current
 * page. The margin is the backward reach: at a typical few KiB per page it
 * covers dozens of pages, which is more than enough for the way people
 * actually read. What is left over is forward headroom, refilled a page at a
 * time, so the window compacts only every ~64 KiB of reading. */
#define TV_WINDOW_SIZE  (256 * 1024)
#define TV_BACK_MARGIN  (192 * 1024)

/* Upper bound on what one page can consume. Used to decide how far ahead the
 * window must reach before laying a page out. */
#define TV_PAGE_MAX     (8 * 1024)

/* Page starts visited going forward, so paging back is a pop rather than a
 * re-layout from the start of the document (the only other known page
 * boundary). At a few KiB per page this covers documents far larger than
 * anything these targets will open; past it, paging back simply stops. */
#define TV_MAX_PAGES    8192

/* Most recently read documents and where the reader got to, newest first. */
#define TV_RESUME_FILE  ROCKBOX_DIR "/textviewer.dat"
#define TV_RESUME_MAX   16

/* Inset on each edge when the margin setting is on. */
#define TV_MARGIN       15

/* text_viewer_colour_mode values. */
enum {
    TV_COLOUR_THEME = 0,
    TV_COLOUR_INVERTED,
    TV_COLOUR_BOW,           /* black on white */
    TV_COLOUR_WOB            /* white on black */
};

struct tv_state
{
    struct ts_core_file file;
    ts_source *src;
    const char *path;        /* caller's file path, for the loading splash */

    int arena_handle;
    int window_handle;
    int stack_handle;

    /* window holds extracted bytes [win_start, win_start + win_len) */
    unsigned char *win;
    off_t  win_start;
    size_t win_len;
    bool   eof;              /* extraction reached the end of the document */

    off_t  file_size;        /* of the document on disk, for resume staleness */

    off_t  pos;              /* first byte of the page on screen */
    off_t  next;             /* first byte of the page after it */

    off_t *stack;            /* page starts behind `pos`, oldest first */
    int    sp;

    int font;
    int user_font;           /* loaded font override, or -1 for the UI font */
    int line_height;
    struct viewport vp;
};

static struct tv_state tv;

/* ---- window ---------------------------------------------------------- */

/* Drops text that has fallen behind the backward reach, sliding what is left
 * to the front. Only called when the window is out of forward headroom. */
static void tv_compact(void)
{
    off_t keep_from = tv.pos - TV_BACK_MARGIN;
    size_t drop;

    if (keep_from <= tv.win_start)
        return;                              /* nothing droppable */

    drop = (size_t)(keep_from - tv.win_start);
    if (drop > tv.win_len)
        drop = tv.win_len;

    memmove(tv.win, tv.win + drop, tv.win_len - drop);
    tv.win_start += drop;
    tv.win_len   -= drop;
}

/* Pulls more text from the engine onto the end of the window. Returns false
 * at end of document or on error. */
static bool tv_extract_more(void)
{
    size_t room;
    long got;

    if (tv.eof)
        return false;

    if (TV_WINDOW_SIZE - tv.win_len < TV_PAGE_MAX)
        tv_compact();

    room = TV_WINDOW_SIZE - tv.win_len;
    if (room < 8)                            /* ts_read() needs at least 8 */
        return false;

    got = ts_read(tv.src, (char *)tv.win + tv.win_len, room);
    if (got <= 0)
    {
        tv.eof = true;                       /* clean EOF and errors alike:
                                              * stop reading either way */
        return false;
    }

    tv.win_len += (size_t)got;
    return true;
}

/* Makes sure [from, from + len) is resident, extracting forward as needed.
 * Returns false if `from` has already fallen out the back of the window --
 * the caller must then rewind and re-extract. */
static bool tv_ensure(off_t from, size_t len)
{
    if (from < tv.win_start)
        return false;

    while (tv.win_start + (off_t)tv.win_len < from + (off_t)len)
    {
        if (!tv_extract_more())
            break;                           /* end of document: all there is */
    }
    return true;
}

/* ---- layout ----------------------------------------------------------- */

/* Width of the `n` bytes at `s`.
 *
 * font_getstringnsize()'s "maxbytes" is really a codepoint count -- it
 * decrements once per utf8decode() and leans on unsigned underflow to stop
 * (firmware/font.c) -- and it halts at a NUL either way, so it cannot bound a
 * byte range. The window is ours and single-threaded, so a temporary NUL is
 * both simpler and cheaper than counting codepoints first. The window is
 * allocated with a spare byte so s[n] is always in bounds. */
static int tv_width(unsigned char *s, size_t n)
{
    unsigned char save = s[n];
    int w;

    s[n] = '\0';
    font_getstringsize(s, &w, NULL, tv.font);
    s[n] = save;
    return w;
}

/* Bytes of `s` that fit in `width`, breaking at the last space. `*draw` gets
 * the length to render, excluding the break character; the return value is
 * what the line consumes, including it. Rockbox bitmap fonts have no kerning
 * -- font_getstringnsize() just sums per-glyph widths -- so accumulating word
 * widths is exact and avoids re-measuring the line prefix per word. */
static size_t tv_line_break(unsigned char *s, size_t avail, int width,
                            size_t *draw)
{
    size_t i = 0;
    int used = 0;

    if (avail == 0)
    {
        *draw = 0;
        return 0;
    }

    if (s[0] == '\n')                        /* explicit blank line */
    {
        *draw = 0;
        return 1;
    }

    while (i < avail)
    {
        size_t word_end = i;
        int word_w;

        while (word_end < avail && s[word_end] != ' ' && s[word_end] != '\n')
            word_end++;

        word_w = tv_width(s + i, word_end - i);

        if (used + word_w > width)
        {
            if (i == 0)
            {
                /* A single word too wide for the line: break inside it at the
                 * last codepoint that fits, never mid-sequence. */
                size_t j = 0;
                int w = 0;

                while (j < word_end)
                {
                    size_t k = j + 1;
                    int cw;

                    while (k < word_end && (s[k] & 0xc0) == 0x80)
                        k++;                 /* to the next codepoint start */

                    cw = tv_width(s + j, k - j);
                    if (w + cw > width && j > 0)
                        break;
                    w += cw;
                    j = k;
                }
                *draw = j;
                return j;
            }
            break;                           /* line ends before this word */
        }

        used += word_w;
        i = word_end;

        if (i >= avail)
            break;

        if (s[i] == '\n')                    /* hard break ends the line */
        {
            *draw = i;
            return i + 1;
        }

        /* s[i] is a space: it only stays if a word follows it on this line */
        used += tv_width(s + i, 1);
        i++;
    }

    /* Trim the trailing space from what is drawn, but still consume it. */
    *draw = (i > 0 && s[i - 1] == ' ') ? i - 1 : i;
    return i;
}

/* Lays out one page from `start`, drawing it when `render` is set. Returns the
 * offset of the next page. */
/* One line is reserved at the foot of the page for the page number. */
static int tv_footer_height(void)
{
    return global_settings.text_viewer_page_number ? tv.line_height : 0;
}

static off_t tv_page(off_t start, bool render)
{
    off_t off = start;
    int y = 0;

    while (y + tv.line_height <= tv.vp.height - tv_footer_height())
    {
        unsigned char *s;
        size_t avail, draw, used;

        if (!tv_ensure(off, TV_PAGE_MAX))
            break;

        avail = (size_t)(tv.win_start + (off_t)tv.win_len - off);
        if (avail == 0)
            break;                           /* end of document */

        s = tv.win + (off - tv.win_start);
        used = tv_line_break(s, avail, tv.vp.width, &draw);
        if (used == 0)
            break;

        if (render && draw > 0)
        {
            unsigned char save = s[draw];

            s[draw] = '\0';
            screens[SCREEN_MAIN].putsxy(0, y, s);
            s[draw] = save;
        }

        off += used;
        y += tv.line_height;
    }

    return off;
}

/* A whole-screen viewport filled with the reading background. Used to wipe the
 * margin border and any loading splash before the page is drawn inset. */
static void tv_fullscreen_vp(struct viewport *vp)
{
    /* Both fields must be set before viewport_set_fullscreen(), which writes
     * neither -- exactly as viewport_set_defaults() does it. Callers pass a
     * bare stack struct, so anything left unset here is stack garbage.
     *
     * buffer, because viewport_set_fullscreen() runs lcd_init_viewport()
     * first and that dereferences it.
     *
     * flags, because a stray VP_FLAG_OWNER_UPDATE (0x2000) makes
     * lcd_update_viewport() return without transferring anything
     * (lcd-bitmap-common.c). Drawing still lands in the framebuffer, so the
     * page is simply never sent to the panel: the reader looks frozen on
     * whatever was last displayed, while paging, input and redraws all carry
     * on underneath. Waking the LCD hides it again -- lcd_awake() pushes the
     * whole framebuffer directly, so the current page appears and the next
     * keypress goes missing in the same way. set_default_align_flags() only
     * rewrites the alignment bits, so it cannot clear this one. */
    vp->buffer = NULL;
    vp->flags = VP_DEFAULT_FLAGS;
    viewport_set_fullscreen(vp, SCREEN_MAIN);
    vp->bg_pattern = tv.vp.bg_pattern;
}

/* The page number, right-aligned on the reserved bottom line. Drawn with
 * tv.vp current, so it uses the reading font and colours. */
static void tv_draw_footer(void)
{
    struct screen *d = &screens[SCREEN_MAIN];
    char buf[16];
    int tw, th;

    if (!global_settings.text_viewer_page_number)
        return;

    snprintf(buf, sizeof buf, "%d", tv.sp + 1);
    d->getstringsize((unsigned char *)buf, &tw, &th);
    d->putsxy(tv.vp.width - tw, tv.vp.height - tv.line_height,
              (unsigned char *)buf);
}

static void tv_draw(void)
{
    struct screen *display = &screens[SCREEN_MAIN];
    struct viewport full, *last;

    tv_fullscreen_vp(&full);
    last = display->set_viewport(&full);
    display->clear_viewport();               /* whole screen -> background */

    display->set_viewport(&tv.vp);
    tv.next = tv_page(tv.pos, true);         /* the page, inset by the margin */
    tv_draw_footer();

    display->set_viewport(&full);
    display->update_viewport();              /* push the whole screen */
    display->set_viewport(last);
}

/* ---- loading splash --------------------------------------------------- */

/* The container formats decompress and index before the first byte of text
 * appears, and reaching a page behind the window re-extracts from zero; both
 * are slow enough to want a "hold on" screen rather than a frozen page. */
static bool tv_heavy_format(const char *file)
{
    const char *ext = strrchr(file, '.');

    if (!ext)
        return false;
    ext++;
    return !strcasecmp(ext, "pdf")  || !strcasecmp(ext, "epub")
        || !strcasecmp(ext, "docx");
}

/* True if opening `file` is slow enough to be worth announcing: a container
 * format, or any document past one window's worth, which no longer opens in a
 * single read. */
static bool tv_slow_open(const char *file)
{
    int fd;
    off_t size;

    if (tv_heavy_format(file))
        return true;

    fd = open(file, O_RDONLY);
    if (fd < 0)
        return false;
    size = filesize(fd);
    close(fd);

    return size > TV_WINDOW_SIZE;
}

/* Names the file being opened. */
static void tv_splash_loading(void)
{
    const char *name = strrchr(tv.path, '/');

    /* splashf, not splash: splash()'s string is a format string, and a '%' in a
     * filename would be read as a conversion. 0 ticks = until overdrawn. */
    splashf(0, "%s\n%s", str(LANG_WAIT), name ? name + 1 : tv.path);
}

/* Clears the reading area before the open, and names the file when the open
 * will be slow -- a fast one would only flash the popup. */
static void tv_show_loading(void)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport full, *last;

    tv_fullscreen_vp(&full);
    last = d->set_viewport(&full);
    d->clear_viewport();
    d->update_viewport();
    d->set_viewport(last);

    if (tv_slow_open(tv.path))
        tv_splash_loading();
}

/* ---- navigation ------------------------------------------------------- */

/* True once the page on screen is the last one: nothing left to extract, and
 * the page after it starts at or past the final byte. */
static bool tv_at_end(void)
{
    return tv.eof && tv.next >= tv.win_start + (off_t)tv.win_len;
}

/* Brings `target` into the window and makes it the current page. The engine
 * only rewinds to the start, so reaching a page that has fallen out the back
 * of the window means re-extracting from byte zero. */
static bool tv_reach(off_t target)
{
    if (target < tv.win_start)
    {
        tv_splash_loading();
        if (ts_rewind(tv.src) != TS_OK)
            return false;
        tv.win_start = 0;
        tv.win_len = 0;
        tv.eof = false;
    }

    /* Set first: compaction keeps its margin behind tv.pos, so this is what
     * stops the text we are heading for being discarded on the way. */
    tv.pos = target;
    tv_ensure(target, TV_PAGE_MAX);
    return true;
}

static void tv_next_page(void)
{
    if (tv_at_end())
        return;

    if (tv.sp < TV_MAX_PAGES)
        tv.stack[tv.sp++] = tv.pos;
    tv.pos = tv.next;
    tv_draw();
}

static void tv_prev_page(void)
{
    if (tv.sp == 0)
        return;                              /* first page */

    if (!tv_reach(tv.stack[tv.sp - 1]))
        return;
    tv.sp--;
    tv_draw();
}

/* Pages forward from the start without drawing until `target` falls on the
 * current page. Doubles as the stack rebuild: resuming leaves the history
 * exactly as it would have been had the reader paged there by hand. */
static void tv_resume_to(off_t target)
{
    tv_splash_loading();

    while (1)
    {
        off_t next = tv_page(tv.pos, false);

        if (next <= tv.pos || next > target)
            break;                           /* end of document, or target is
                                              * on the page we are on */
        if (tv.sp < TV_MAX_PAGES)
            tv.stack[tv.sp++] = tv.pos;
        tv.pos = next;
    }
}

/* ---- resume ----------------------------------------------------------- */

/* Lines are "<offset> <size> <path>", newest first. The path trails so it may
 * hold spaces. `size` is the document's length when the offset was recorded:
 * offsets index the extracted text, so a document that has changed on disk
 * invalidates them, and the length is the cheap half of noticing. (It cannot
 * catch an edit that preserves the length, nor a layout change -- page
 * boundaries also move with the font and spacing.) */
static off_t tv_resume_load(const char *file, off_t size)
{
    int fd = open(TV_RESUME_FILE, O_RDONLY);
    char line[MAX_PATH + 32];
    off_t off = 0;

    if (fd < 0)
        return 0;

    while (read_line(fd, line, sizeof line) > 0)
    {
        char *end_off = strchr(line, ' ');
        char *end_size;

        if (!end_off)
            continue;
        end_size = strchr(end_off + 1, ' ');
        if (!end_size)
            continue;

        if (strcmp(end_size + 1, file))
            continue;

        *end_off = *end_size = '\0';
        if ((off_t)atoi(end_off + 1) == size)
            off = (off_t)atoi(line);
        break;                               /* first hit is the newest */
    }

    close(fd);
    return off;
}

/* Rewrites the list with `file` at the front, streaming the old entries past
 * a single line buffer rather than holding them all on the stack. */
static void tv_resume_save(const char *file, off_t off, off_t size)
{
    char line[MAX_PATH + 32];
    int old, new_fd, kept = 1;

    new_fd = open(TV_RESUME_FILE ".tmp", O_CREAT|O_WRONLY|O_TRUNC, 0666);
    if (new_fd < 0)
        return;

    fdprintf(new_fd, "%ld %ld %s\n", (long)off, (long)size, file);

    old = open(TV_RESUME_FILE, O_RDONLY);
    if (old >= 0)
    {
        while (kept < TV_RESUME_MAX && read_line(old, line, sizeof line) > 0)
        {
            char *end_off = strchr(line, ' ');
            char *end_size;

            if (!end_off)
                continue;
            end_size = strchr(end_off + 1, ' ');
            if (!end_size)
                continue;
            if (!strcmp(end_size + 1, file))
                continue;                    /* superseded by the new entry */

            fdprintf(new_fd, "%s\n", line);
            kept++;
        }
        close(old);
    }
    close(new_fd);

    remove(TV_RESUME_FILE);
    rename(TV_RESUME_FILE ".tmp", TV_RESUME_FILE);
}

/* The document on the newest line, for the main menu's "Continue reading".
 *
 * The parse lives here rather than in the caller because the format is this
 * file's: two space-delimited numbers, then the path as everything left. The
 * path is last precisely so it may contain spaces, so it cannot be picked out
 * by tokenising the line.
 *
 * Says nothing about whether the document still exists -- that is the
 * caller's to check, since it is the caller who has to decide what to do
 * about it. */
bool text_viewer_last_document(char *buf, size_t bufsize)
{
    int fd = open(TV_RESUME_FILE, O_RDONLY);
    char line[MAX_PATH + 32];
    char *path;
    bool found = false;

    if (fd < 0)
        return false;

    if (read_line(fd, line, sizeof line) > 0
        && (path = strchr(line, ' ')) != NULL
        && (path = strchr(path + 1, ' ')) != NULL
        && path[1] != '\0')
    {
        strmemccpy(buf, path + 1, bufsize);
        found = true;
    }

    close(fd);
    return found;
}

/* ---- lifecycle -------------------------------------------------------- */

static bool tv_open(const char *file)
{
    ts_config cfg = ts_config_default();
    ts_io io;
    void *arena;
    int rc;

    tv.arena_handle = tv.window_handle = tv.stack_handle = 0;
    tv.src = NULL;

    /* Every stage of the engine holds pointers into the arena and to its
     * neighbours, so it must not move underneath an open source. */
    tv.arena_handle = core_alloc(TS_ARENA_RECOMMENDED);
    if (tv.arena_handle <= 0)
        return false;
    core_pin(tv.arena_handle);
    arena = core_get_data(tv.arena_handle);

    /* One spare byte: tv_width() and tv_draw() NUL-terminate in place, and
     * that write lands one past the end when the window is full. */
    tv.window_handle = core_alloc(TV_WINDOW_SIZE + 1);
    if (tv.window_handle <= 0)
        return false;
    core_pin(tv.window_handle);
    tv.win = core_get_data(tv.window_handle);

    tv.stack_handle = core_alloc(TV_MAX_PAGES * sizeof(off_t));
    if (tv.stack_handle <= 0)
        return false;
    core_pin(tv.stack_handle);
    tv.stack = core_get_data(tv.stack_handle);
    tv.sp = 0;

    if (ts_io_core(&io, &tv.file, file) != TS_OK)
        return false;

    /* Through the vtable rather than filesize(): coreio_size() restores the
     * descriptor's position and keeps its cached copy honest. */
    tv.file_size = io.size(io.ctx);

    rc = ts_open(&tv.src, &io, file, arena, TS_ARENA_RECOMMENDED, &cfg);
    if (rc != TS_OK)
    {
        /* ts_open() takes the fd on success only, so close it ourselves. */
        io.close(io.ctx);
        splashf(HZ * 2, "%s", ts_strerror(rc));
        return false;
    }

    tv.win_start = 0;
    tv.win_len = 0;
    tv.eof = false;
    tv.pos = 0;
    tv.next = 0;
    return true;
}

static void tv_close(void)
{
    if (tv.user_font >= 0)
    {
        font_unload(tv.user_font);
        tv.user_font = -1;
    }

    if (tv.src)
        ts_close(tv.src);
    tv.src = NULL;

    if (tv.stack_handle > 0)
    {
        core_unpin(tv.stack_handle);
        tv.stack_handle = core_free(tv.stack_handle);
    }
    if (tv.window_handle > 0)
    {
        core_unpin(tv.window_handle);
        tv.window_handle = core_free(tv.window_handle);
    }
    if (tv.arena_handle > 0)
    {
        core_unpin(tv.arena_handle);
        tv.arena_handle = core_free(tv.arena_handle);
    }
    tv.win = NULL;
    tv.stack = NULL;
}

/* Loads the font override if one is set, else falls back to the UI font.
 * Unload-then-load keeps re-applying the same file leak-free (font_load is
 * refcounted). Uses the resolved UI font id (getuifont()), never the FONT_UI
 * sentinel (== MAXFONTS): that only resolves inside a font_get() call, while
 * the LCD text path indexes the viewport font directly, so leaving MAXFONTS in
 * it would draw glyphs from out of bounds. */
static void tv_apply_font(void)
{
    int fid = -1;

    if (tv.user_font >= 0)
    {
        font_unload(tv.user_font);
        tv.user_font = -1;
    }

    if (global_settings.text_viewer_font_file[0])
    {
        char buf[MAX_PATH];
        snprintf(buf, sizeof buf, FONT_DIR "/%s.fnt",
                 global_settings.text_viewer_font_file);
        fid = font_load(buf);
    }

    tv.user_font = fid;
    tv.font = (fid >= 0) ? fid : screens[SCREEN_MAIN].getuifont();
    tv.vp.font = tv.font;
}

/* (Re)reads the viewer settings into the viewport: font, colours, page margin
 * and line height. Page starts are byte offsets into the extracted text, which
 * are stable across all of these, so a page just re-flows from tv.pos -- no
 * resume or page-stack invalidation is needed when a setting changes. */
static void tv_apply_settings(void)
{
    struct screen *d = &screens[SCREEN_MAIN];
    int margin;

    tv_apply_font();

    switch (global_settings.text_viewer_colour_mode)
    {
        case TV_COLOUR_INVERTED:
            tv.vp.fg_pattern = global_settings.bg_color;
            tv.vp.bg_pattern = global_settings.fg_color;
            break;
        case TV_COLOUR_BOW:
            tv.vp.fg_pattern = LCD_BLACK;
            tv.vp.bg_pattern = LCD_WHITE;
            break;
        case TV_COLOUR_WOB:
            tv.vp.fg_pattern = LCD_WHITE;
            tv.vp.bg_pattern = LCD_BLACK;
            break;
        case TV_COLOUR_THEME:
        default:
            tv.vp.fg_pattern = global_settings.fg_color;
            tv.vp.bg_pattern = global_settings.bg_color;
            break;
    }

    margin = global_settings.text_viewer_margin ? TV_MARGIN : 0;
    tv.vp.x = margin;
    tv.vp.y = margin;
    tv.vp.width  = d->lcdwidth  - 2 * margin;
    tv.vp.height = d->lcdheight - 2 * margin;

    tv.line_height = font_get(tv.font)->height
                   + global_settings.text_viewer_line_spacing;
}

static void tv_setup_screen(void)
{
    /* This screen owns the whole display. Disabling the theme drops the
     * status-bar skin and its backdrop and hands back a full-screen viewport
     * (viewport_set_defaults -> viewport_set_fullscreen). With the theme off,
     * the skin engine's status-bar re-render is gated out for this screen --
     * viewportmanager_redraw only pumps sb_skin_update while the theme is
     * enabled -- so nothing repaints over the page between our own draws. That
     * race is what made the reading area flicker with status-bar content while
     * paging under a themed (SBS) status bar. */
    viewportmanager_theme_enable(SCREEN_MAIN, false, &tv.vp);
    tv_apply_settings();
}

/* Hold-Menu settings menu, shared with Library > Viewers > Text Viewer.
 * Re-enables the theme just for the menu chrome, then drops back to our own
 * full-screen drawing and re-applies whatever the reader changed. */
static int tv_menu(void)
{
    int sel = 0, ret;

    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    push_current_activity(ACTIVITY_CONTEXTMENU);
    ret = do_menu(&text_viewer_menu, &sel, NULL, false);
    pop_current_activity();
    viewportmanager_theme_undo(SCREEN_MAIN, false);

    tv_apply_settings();
    tv_draw();
    return ret;
}

int text_viewer(const char *file)
{
    int ret = GO_TO_PREVIOUS;
    off_t resume;

    push_current_activity(ACTIVITY_TEXTVIEWER);
    tv.path = file;
    tv.user_font = -1;           /* no font override loaded yet */

    /* Take the screen (theme off) and put up a clean loading frame before the
     * slow open -- the name splash for the container formats, a cleared
     * reading area otherwise -- so the browser never shows through it. */
    tv_setup_screen();
    tv_show_loading();

    if (!tv_open(file))
    {
        tv_close();
        viewportmanager_theme_undo(SCREEN_MAIN, false);
        pop_current_activity();
        return GO_TO_PREVIOUS;
    }

    resume = tv_resume_load(file, tv.file_size);
    if (resume > 0)
        tv_resume_to(resume);

    tv_draw();

    while (1)
    {
        int action = get_action(CONTEXT_STD, TIMEOUT_BLOCK);

        switch (action)
        {
            case ACTION_STD_OK:          /* forward button (>>|) */
                tv_next_page();
                break;

            case ACTION_STD_CANCEL:      /* back button (|<<) */
                tv_prev_page();
                break;

            case ACTION_STD_MENU:        /* Menu leaves the viewer */
                goto done;

            case ACTION_STD_QUICKSCREEN: /* hold Menu opens the settings */
                if (tv_menu() == MENU_ATTACHED_USB)
                {
                    ret = GO_TO_ROOT;
                    goto done;
                }
                break;

            default:
                /* The scroll wheel (ACTION_STD_NEXT/PREV) is deliberately
                 * ignored -- paging is on the forward/back buttons. Still let
                 * the framework handle USB and other system events. */
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                {
                    ret = GO_TO_ROOT;
                    goto done;
                }
                break;
        }
    }

  done:
    tv_resume_save(file, tv.pos, tv.file_size);
    tv_close();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    pop_current_activity();
    return ret;
}
