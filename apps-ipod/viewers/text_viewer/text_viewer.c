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
 *
 * A book in parts (an EPUB's chapter files) is read one part at a time:
 * ts_seek_part() starts any chapter as cheaply as the first, each chapter
 * starts a new page, and the window, the offsets and the page stack all
 * belong to the chapter on screen. Opening at a saved place, or paging back
 * into the chapter before, then lays out one chapter at most.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>          /* atoi */
#include <stdarg.h>
#include <stdio.h>           /* snprintf */
#include <string.h>
#include "string-extra.h"    /* strmemccpy */
#include "config.h"
#include "file.h"            /* off_t, open/close/rename/remove, fdprintf */
#include "rbpaths.h"         /* ROCKBOX_DIR */
#include "system/library_files.h"
#include "database/libfile.h"
#include "database/path_key.h"
#include "input/action.h"
#include "draw/screen_access.h"   /* screens[] */
#include "kernel.h"          /* HZ, SYS_USB_CONNECTED */
#include "system.h"          /* cpu_boost */
#include "core_alloc.h"
#include "settings/settings.h"
#include "lang.h"
#include "widgets/splash.h"
#include "widgets/dialog_prose.h"     /* dialog_prose_show */
#include "reading_stats.h"
#include "draw/viewport.h"
#include "font.h"
#include "system/activity.h"
#include "system/shutdown.h"
#include "system/strutil.h"
#include "root_menu.h"       /* GO_TO_*, MENU_ATTACHED_USB */
#include "widgets/menu.h"            /* do_menu */
#include "widgets/list.h"            /* simplelist */
#include "screens/settings/exported_settings.h" /* text_viewer_menu */
#include "txt_source.h"
#include "ts_io_core.h"
#include "text_viewer.h"
#include "speech/talk.h"
#include "speech/speech.h"     /* speech_enabled, speech_reader */
#include "audio/voice_thread.h" /* voice_is_playing */
#include "powermgmt.h"   /* reset_poweroff_timer */
#include "backlight.h"   /* backlight_set_brightness, backlight_set_timeout */
#include "draw/image_load.h"     /* a book's pictures */
#include "draw/resize.h"         /* format_native */
#include "files/zip_member.h"
#include "crc32.h"
#include "audio/sound_feedback.h"   /* keyclick_click */
#include "metadata/book_marks.h"
#include "rbunicode.h"          /* utf8decode, a mark's label */
#include "metadata/book_resume.h"       /* book_resume_key */
#include "widgets/yesno.h"

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
#define TV_RESUME_FILE  LIB_READING_FILE
/* Places kept, the Books shelf's In progress and Finished among them */
#define TV_RESUME_MAX   256

/* Inset on each edge when the margin setting is on. */
#define TV_MARGIN       15

/* The footer's face, which every build ships (bundle-theme.sh). Without it
 * the footer is drawn in the page's font. */
#define TV_FOOTER_FONT  FONT_DIR "/12-noto-sans.fnt"

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

    int    part;             /* the chapter on screen, which offsets count in */
    int    nparts;           /* chapters, or 0 for a document without parts */

    off_t  pos;              /* first byte of the page on screen */
    off_t  next;             /* first byte of the page after it */

    off_t *stack;            /* page starts behind `pos`, oldest first */
    int    sp;

    int font;
    int user_font;           /* loaded font override, or -1 for the UI font */
    int footer_font;         /* TV_FOOTER_FONT, or -1 to use tv.font */
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

    /* Boosted: a PDF's per-glyph CMap search and a re-extract from the
     * start both run in here, and the 6G idles at a quarter speed. */
    cpu_boost(true);
    got = ts_read(tv.src, (char *)tv.win + tv.win_len, room);
    cpu_boost(false);
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

/* A soft hyphen, U+00AD, is two bytes in UTF-8. It is invisible and takes
 * no width, and a line may break at it with a '-' drawn. */
static bool tv_is_shy(const unsigned char *s, size_t n)
{
    return n >= 2 && s[0] == 0xc2 && s[1] == 0xad;
}

/* A heading's marks, TS_HEADING_OPEN and TS_HEADING_CLOSE: the text between
 * them is underlined */
#define TV_HEAD_OPEN  "\xee\x80\x82"

static bool tv_is_mark(const unsigned char *s, size_t n)
{
    return n >= 3 && s[0] == 0xee && s[1] == 0x80
           && (s[2] == 0x82 || s[2] == 0x83);
}

/* A note's mark -- TS_NOTE_OPEN, its href, TS_NOTE_CLOSE -- at `s`: its
 * length, all of `n` when they end inside it, or 0 */
static size_t tv_note_mark(const unsigned char *s, size_t n)
{
    if (n < 3 || s[0] != 0xee || s[1] != 0x80 || s[2] != 0x84)
        return 0;
    for (size_t i = 3; i + 3 <= n; i++)
        if (s[i] == 0xee && s[i + 1] == 0x80 && s[i + 2] == 0x85)
            return i + 3;
    return n;
}

/* The bytes of what is invisible and takes no width at `s`, a soft hyphen,
 * a heading's mark or a note's, or 0 */
static size_t tv_hidden(const unsigned char *s, size_t n)
{
    return tv_is_shy(s, n) ? 2 : tv_is_mark(s, n) ? 3 : tv_note_mark(s, n);
}

/* tv_width() of `n` bytes at `s`, what is hidden left out */
static int tv_text_width(unsigned char *s, size_t n)
{
    size_t i = 0, start = 0;
    int w = 0;

    while (i < n)
    {
        size_t h = tv_hidden(s + i, n - i);

        if (h)
        {
            w += tv_width(s + start, i - start);
            i += h;
            start = i;
        }
        else
            i++;
    }
    return w + tv_width(s + start, n - start);
}

/* Width of a string constant, which tv_width() cannot be given: it writes
 * past the end of what it measures */
static int tv_literal_width(const char *s)
{
    int w;

    font_getstringsize((const unsigned char *)s, &w, NULL, tv.font);
    return w;
}

/* One line of the page, as tv_line_break() found it */
struct tv_line
{
    size_t used;    /* bytes the line consumes, its break included */
    size_t draw;    /* bytes it draws */
    int width;      /* pixels they take */
    bool hyphen;    /* broken at a soft hyphen: a '-' follows */
    bool full;      /* ended because the next word did not fit */
    int indent;     /* pixels before it: a paragraph's first line */
};

/* The bytes of `s` that fit in `width`, breaking at the last space, or at a
 * soft hyphen inside the word that does not fit. Rockbox bitmap fonts have no
 * kerning -- font_getstringnsize() just sums per-glyph widths -- so
 * accumulating word widths is exact and avoids re-measuring the line prefix
 * per word. */
static void tv_line_break(unsigned char *s, size_t avail, int width,
                          struct tv_line *ln)
{
    size_t i = 0;
    int used = 0;

    ln->used = ln->draw = 0;
    ln->width = ln->indent = 0;
    ln->hyphen = ln->full = false;

    if (avail == 0)
        return;

    if (s[0] == '\n')                        /* explicit blank line */
    {
        ln->used = 1;
        return;
    }

    while (i < avail)
    {
        size_t word_end = i;
        int word_w;

        while (word_end < avail && s[word_end] != ' ' && s[word_end] != '\n')
            word_end++;

        word_w = tv_text_width(s + i, word_end - i);

        if (used + word_w > width)
        {
            int dash_w = tv_literal_width("-");
            size_t p;

            ln->full = true;

            /* The last soft hyphen in the word whose part fits, dash and
             * all */
            for (p = word_end; p-- > i; )
            {
                int part_w;

                if (!tv_is_shy(s + p, word_end - p))
                    continue;
                part_w = tv_text_width(s + i, p - i);
                if (used + part_w + dash_w <= width)
                {
                    ln->draw = p;
                    ln->used = p + 2;
                    ln->width = used + part_w + dash_w;
                    ln->hyphen = true;
                    return;
                }
            }

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

                    cw = tv_text_width(s + j, k - j);
                    if (w + cw > width && j > 0)
                        break;
                    w += cw;
                    j = k;
                }
                ln->draw = ln->used = j;
                ln->width = w;
                return;
            }
            break;                           /* line ends before this word */
        }

        used += word_w;
        i = word_end;
        ln->width = used;

        if (i >= avail)
            break;

        if (s[i] == '\n')                    /* hard break ends the line */
        {
            ln->draw = i;
            ln->used = i + 1;
            return;
        }

        /* s[i] is a space: it only stays if a word follows it on this line */
        used += tv_width(s + i, 1);
        i++;
    }

    /* Trim the trailing space from what is drawn, but still consume it. */
    ln->draw = (i > 0 && s[i - 1] == ' ') ? i - 1 : i;
    ln->used = i;
}

/* Draws `n` bytes of one word at x, what is hidden left out; returns the x
 * after it */
static int tv_put_word(unsigned char *s, size_t n, int x, int y)
{
    size_t i = 0, start = 0;

    while (i <= n)
    {
        size_t h = i < n ? tv_hidden(s + i, n - i) : 0;

        if (i == n || h)
        {
            unsigned char save = s[i];

            s[i] = '\0';
            screens[SCREEN_MAIN].putsxy(x, y, s + start);
            s[i] = save;
            x += tv_width(s + start, i - start);
            i += h ? h : 1;
            start = i;
        }
        else
            i++;
    }
    return x;
}

/* Draws a line at y. With Alignment Justified, a full line's leftover pixels
 * are shared out between its spaces; a paragraph's last line stays left.
 * A line with neither that nor anything hidden in it is drawn in one call. */
static void tv_draw_line(unsigned char *s, const struct tv_line *ln, int y)
{
    bool justify = global_settings.text_viewer_align == TV_ALIGN_JUSTIFIED
                   && ln->full;
    bool hidden = false;
    int gaps = 0, gap = 0, extra = 0, space_w, x = ln->indent;
    size_t i;

    for (i = 0; i < ln->draw; i++)
    {
        if (s[i] == ' ')
            gaps++;
        else if (tv_hidden(s + i, ln->draw - i))
            hidden = true;
    }

    if (!hidden && !ln->hyphen && (!justify || gaps == 0))
    {
        unsigned char save = s[ln->draw];

        s[ln->draw] = '\0';
        screens[SCREEN_MAIN].putsxy(x, y, s);
        s[ln->draw] = save;
        return;
    }

    space_w = tv_literal_width(" ");
    if (justify)
        extra = tv.vp.width - ln->indent - ln->width;

    i = 0;
    while (i < ln->draw)
    {
        size_t j = i;

        if (s[i] == ' ')
        {
            x += space_w;
            if (justify)
                x += extra * (gap + 1) / gaps - extra * gap / gaps;
            gap++;
            i++;
            continue;
        }
        while (j < ln->draw && s[j] != ' ')
            j++;
        x = tv_put_word(s + i, j - i, x, y);
        i = j;
    }
    if (ln->hyphen)
        screens[SCREEN_MAIN].putsxy(x, y, (unsigned char *)"-");
}

/* The footer's font: the small one where it is smaller than the page's */
static int tv_footer_fid(void)
{
    if (tv.footer_font >= 0
        && font_get(tv.footer_font)->height < font_get(tv.font)->height)
        return tv.footer_font;
    return tv.font;
}

/* One line of the footer's font is reserved at the foot of the page. */
static int tv_footer_height(void)
{
    if (global_settings.text_viewer_footer == TV_FOOTER_OFF)
        return 0;
    return tv_footer_fid() == tv.font ? tv.line_height
                                      : (int)font_get(tv.footer_font)->height;
}

/* ---- pictures ---------------------------------------------------------- */

/* A picture reaches the text as a line of its own: TS_PICTURE_OPEN, its href,
 * TS_PICTURE_CLOSE, three bytes each in UTF-8. Shown, it is a page by itself,
 * fitted inside the margins with no footer; otherwise the line is passed
 * over. One that fails to decode is passed over too, but still ends the page
 * before it. */
#define TV_PIC_OPEN   "\xee\x80\x80"
#define TV_PIC_CLOSE  "\xee\x80\x81"
#define TV_PIC_TMP    LIB_CACHE_DIR "/picture.tmp"
#define TV_PIC_HREF   256
/* A screenful of bitmap, and the decoder's room after it -- which also holds
 * the inflater, before the decode starts, for a deflated picture */
#define TV_PIC_BYTES  (LCD_WIDTH * LCD_HEIGHT * (int)sizeof(fb_data) \
                       + MAX((int)IMAGE_DECODE_OVERHEAD, ZIP_MEMBER_WORK) \
                       + 32 * 1024)

static bool tv_is_picture(const unsigned char *s, size_t n)
{
    return n >= 3 && !memcmp(s, TV_PIC_OPEN, 3);
}

/* Read Aloud passes over pictures, as Pictures off does, while there is a
 * voice to read with */
static bool tv_pictures_shown(void)
{
    return global_settings.text_viewer_pictures
           && (global_settings.text_viewer_read_aloud == TV_READ_OFF
               || !speech_enabled());
}

/* The white space at `s`, newlines and no-break spaces included */
static size_t tv_blank_run(const unsigned char *s, size_t n)
{
    size_t i = 0;

    while (i < n)
    {
        if (s[i] == '\n' || s[i] == ' ' || s[i] == '\t' || s[i] == '\r')
            i++;
        else if (n - i >= 2 && s[i] == 0xc2 && s[i + 1] == 0xa0)
            i += 2;
        else
            break;
    }
    return i;
}

/* The bytes of the picture line at `s`, its newline included, and its href
 * into `href` */
static size_t tv_picture_line(const unsigned char *s, size_t n, char *href,
                              size_t cap)
{
    size_t i = 3, j = 0;

    while (i < n && s[i] != '\n')
    {
        if (n - i >= 3 && !memcmp(s + i, TV_PIC_CLOSE, 3))
        {
            i += 3;
            continue;
        }
        if (j < cap - 1)
            href[j++] = s[i];
        i++;
    }
    href[j] = '\0';
    return i < n ? i + 1 : i;
}

/* The picture last decoded, kept while its page is redrawn */
static int tv_pic_handle;
static int tv_pic_part = -1;
static char tv_pic_href[TV_PIC_HREF];
static struct bitmap tv_pic_bm;
static int tv_pic_w, tv_pic_h;      /* the page it was fitted to */
static bool tv_pic_ok;

/* Pictures of the open book that failed to decode, by chapter and href; a
 * picture past the last slot is tried again each time its page is drawn */
#define TV_PIC_FAILED 16
static uint32_t tv_pic_failed[TV_PIC_FAILED];
static int tv_pic_nfailed;

/* The page on screen is a picture page, so it has no footer */
static bool tv_pic_page;
/* A press stopped the page's decode: drawn again when the reader is idle */
static bool tv_pic_pending;

static uint32_t tv_picture_key(const char *href)
{
    return crc_32(href, strlen(href), 0xffffffff) ^ (uint32_t)tv.part;
}

static bool tv_picture_failed(const char *href)
{
    uint32_t key = tv_picture_key(href);

    for (int i = 0; i < tv_pic_nfailed; i++)
        if (tv_pic_failed[i] == key)
            return true;
    return false;
}

static void tv_picture_free(void)
{
    if (tv_pic_handle > 0)
    {
        core_unpin(tv_pic_handle);
        core_free(tv_pic_handle);
    }
    tv_pic_handle = 0;
    tv_pic_part = -1;
    tv_pic_nfailed = 0;
}

/* The decode's progress, as a bar across the middle of the page once it has
 * taken a moment; a button newly pressed stops it, and is then acted on.
 * Drawn with tv.vp current. */
#define TV_PIC_BAR_AFTER (HZ / 4)
static bool tv_side;
static void tv_side_push(void);
static long tv_pic_start;
static int tv_pic_held, tv_pic_drawn;
static bool tv_pic_stopped;

static void tv_picture_bar_box(int *x, int *y, int *w)
{
    *w = tv.vp.width / 2;
    *x = (tv.vp.width - *w) / 2;
    *y = tv.vp.height / 2 - 2;
}

static bool tv_picture_row(int done, int rows)
{
    struct screen *d = &screens[SCREEN_MAIN];
    int held, w, x, y, fill;

    held = button_status();
    if (held & ~tv_pic_held)
    {
        tv_pic_stopped = true;
        return false;
    }
    tv_pic_held &= held;                /* let go of, so pressed again stops */

    if (rows <= 0 || TIME_BEFORE(current_tick, tv_pic_start + TV_PIC_BAR_AFTER))
        return true;
    tv_picture_bar_box(&x, &y, &w);
    fill = (int)((long)w * done / rows);
    if (fill == tv_pic_drawn)
        return true;
    tv_pic_drawn = fill;
    d->drawrect(x, y, w, 5);
    d->fillrect(x, y, fill, 5);
    if (tv_side)
        tv_side_push();
    else
        d->update_viewport();
    return true;
}

/* Picture `href` of the chapter on screen, decoded to fit w x h */
static bool tv_picture_decode(const char *href, int w, int h)
{
    struct image_src src = { .pos = 0, .len = -1, .flags = 0 };
    uint32_t csize, usize;
    unsigned char *buf;
    int method, rc;
    long at;

    if (tv_pic_part == tv.part && !strcmp(tv_pic_href, href)
        && tv_pic_w == w && tv_pic_h == h)
        return tv_pic_ok;
    tv_pic_part = tv.part;
    tv_pic_w = w;
    tv_pic_h = h;
    strmemccpy(tv_pic_href, href, sizeof(tv_pic_href));
    tv_pic_ok = false;
    tv_pic_stopped = false;

    if (tv_pic_handle <= 0)
    {
        tv_pic_handle = core_alloc(TV_PIC_BYTES);
        if (tv_pic_handle <= 0)
        {
            tv_pic_handle = 0;
            return false;
        }
        core_pin(tv_pic_handle);
    }
    buf = core_get_data(tv_pic_handle);

    if (ts_picture_extent(tv.src, tv.part, href, &at, &csize, &usize,
                          &method) != BS_OK)
        return false;
    if (method != MEMBER_STORED)
    {
        if (!zip_member_to_file(tv.path, at, csize, usize, method,
                                TV_PIC_TMP, buf, TV_PIC_BYTES))
            return false;
        src.fd = open(TV_PIC_TMP, O_RDONLY);
    }
    else
    {
        src.fd = open(tv.path, O_RDONLY);
        src.pos = at;
        src.len = csize;
    }
    if (src.fd < 0)
        return false;

    memset(&tv_pic_bm, 0, sizeof(tv_pic_bm));
    tv_pic_bm.width = w;
    tv_pic_bm.height = h;
    tv_pic_bm.data = buf;
    tv_pic_start = current_tick;
    tv_pic_held = button_status();
    tv_pic_drawn = -1;
    struct custom_format decode_format = format_native;
    decode_format.progress = tv_picture_row;
    cpu_boost(true);
    rc = image_load(&src, &tv_pic_bm, TV_PIC_BYTES,
                    FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT
                    | FORMAT_DITHER, &decode_format);
    cpu_boost(false);

    if (tv_pic_drawn >= 0)
    {   /* the bar, which a picture smaller than the page leaves showing */
        struct screen *d = &screens[SCREEN_MAIN];
        int x, y, bw;

        tv_picture_bar_box(&x, &y, &bw);
        d->set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
        d->fillrect(x, y, bw, 5);
        d->set_drawmode(DRMODE_SOLID);
    }
    close(src.fd);
    if (method != MEMBER_STORED)
        remove(TV_PIC_TMP);
    if (tv_pic_stopped)
        tv_pic_part = -1;               /* nothing kept: decoded again */
    tv_pic_ok = rc > 0 && tv_pic_bm.width > 0 && tv_pic_bm.height > 0;
    return tv_pic_ok;
}

/* The page's picture, centred inside the margins. False when it will not
 * decode, which is remembered so the page is laid out without it; a decode
 * stopped by a press leaves the page blank until it is drawn again. Drawn
 * with tv.vp current. */
static bool tv_draw_picture(const char *href)
{
    int w = tv.vp.width, h = tv.vp.height;

    if (w > 0 && h > 0 && tv_picture_decode(href, w, h))
    {
        screens[SCREEN_MAIN].bitmap((const fb_data *)tv_pic_bm.data,
                                    (w - tv_pic_bm.width) / 2,
                                    (h - tv_pic_bm.height) / 2,
                                    tv_pic_bm.width, tv_pic_bm.height);
        return true;
    }
    if (tv_pic_stopped)
    {
        tv_pic_pending = true;
        return true;
    }
    if (tv_pic_nfailed < TV_PIC_FAILED)
        tv_pic_failed[tv_pic_nfailed++] = tv_picture_key(href);
    return false;
}

/* Whether the line at `off` starts a paragraph: the first of a part, or one
 * after a line break. Unknown, before the window, counts as not. */
static bool tv_paragraph_start(off_t off)
{
    if (off > tv.win_start)
        return tv.win[off - tv.win_start - 1] == '\n';
    return off == 0;
}

/* Whether a heading is still open at the end of the line's `n` bytes, given
 * whether it was at their start */
static bool tv_heading_after(const unsigned char *s, size_t n, bool open)
{
    for (size_t i = 0; i + 3 <= n; i++)
        if (tv_is_mark(s + i, n - i))
            open = s[i + 2] == 0x82;
    return open;
}

/* A heading has been laid out: a text without chapters has contents to
 * browse */
static bool tv_heads_seen;

/* Lays out one page from `start`, drawing it when `render` is set. Returns the
 * offset of the next page. A heading that runs over from the page before is
 * not underlined on this one. */
static off_t tv_page(off_t start, bool render)
{
    off_t off = start;
    int y = 0;
    bool heading = false;

    while (y + tv.line_height <= tv.vp.height - tv_footer_height())
    {
        unsigned char *s;
        size_t avail;
        struct tv_line ln;

        if (!tv_ensure(off, TV_PAGE_MAX))
            break;

        avail = (size_t)(tv.win_start + (off_t)tv.win_len - off);
        if (avail == 0)
            break;                           /* end of document */

        s = tv.win + (off - tv.win_start);

        /* A page starts with its first line of text, not the blank line
         * that sets a heading apart from the page before */
        if (y == 0 && s[0] == '\n')
        {
            off++;
            continue;
        }

        /* Blank lines before a picture go with it, and so do lines of
         * white space alone: books space pictures with <p>&nbsp;</p> */
        {
            size_t ws = tv_blank_run(s, avail);

            if (ws > 0 && tv_is_picture(s + ws, avail - ws))
            {
                off += ws;
                continue;
            }
        }
        if (tv_is_picture(s, avail))
        {
            char href[TV_PIC_HREF];
            size_t used = tv_picture_line(s, avail, href, sizeof(href));

            if (!tv_pictures_shown())
            {
                off += used;
                continue;
            }
            /* A page of its own. One that will not decode still ends the page
             * before, so the pages are the same whether its failure is known
             * yet or not, and paging back retraces them. */
            if (y > 0)
                break;
            if (tv_picture_failed(href))
            {
                off += used;
                continue;
            }
            if (render && !tv_draw_picture(href))
            {
                off += used;
                continue;
            }
            tv_pic_page = render;
            off += used;
            break;
        }

        {
            bool head = heading
                        || (avail >= 3 && !memcmp(s, TV_HEAD_OPEN, 3));

            tv_heads_seen |= head;
            int indent = global_settings.text_viewer_indent && !head
                         && s[0] != '\n' && tv_paragraph_start(off)
                         ? tv_literal_width("M") : 0;

            /* A heading starts a page that has room for it, the blank line
             * after it and a line of what it heads */
            if (head && !heading && y > 0
                && y + 3 * tv.line_height > tv.vp.height - tv_footer_height())
                break;

            tv_line_break(s, avail, tv.vp.width - indent, &ln);
            if (ln.used == 0)
                break;
            ln.indent = indent;
            if (head)
                ln.full = false;        /* a heading is never justified */

            if (render && ln.draw > 0)
            {
                tv_draw_line(s, &ln, y);
                if (head)
                {
                    int uy = MIN(y + font_get(tv.font)->ascent + 1,
                                 y + tv.line_height - 1);

                    screens[SCREEN_MAIN].hline(0, ln.width - 1, uy);
                }
            }
            heading = tv_heading_after(s, ln.used, heading);
        }

        off += ln.used;
        y += tv.line_height;
    }

    return off;
}

/* ---- the page held sideways ------------------------------------------ */

/* Sideways the page is 240 x 320: drawn into a buffer of that shape and
 * copied a quarter turn into the screen's framebuffer. Held only while the
 * reader is sideways. */
#if LCD_STRIDEFORMAT != HORIZONTAL_STRIDE
#error the sideways copy assumes a framebuffer stored by rows
#endif
static struct frame_buffer_t tv_side_fb;
static int tv_side_handle;
static bool tv_side;

static bool tv_side_alloc(void)
{
    if (tv_side_handle > 0)
        return true;
    tv_side_handle = core_alloc(LCD_WIDTH * LCD_HEIGHT * sizeof(fb_data));
    if (tv_side_handle <= 0)
    {
        tv_side_handle = 0;
        return false;
    }
    core_pin(tv_side_handle);
    tv_side_fb.fb_ptr = core_get_data(tv_side_handle);
    tv_side_fb.get_address_fn = NULL;   /* the default, through this stride */
    tv_side_fb.stride = LCD_HEIGHT;
    tv_side_fb.elems = LCD_WIDTH * LCD_HEIGHT;
    return true;
}

static void tv_side_free(void)
{
    if (tv_side_handle > 0)
    {
        core_unpin(tv_side_handle);
        core_free(tv_side_handle);
    }
    tv_side_handle = 0;
    tv_side = false;
}

/* The page's (u, v) lands at (319 - v, u) with the top of the iPod to the
 * left, the right hand's way, and at (v, 239 - u) the left hand's. */
static void tv_side_push(void)
{
    /* lcd_set_viewport() answers the viewport it replaces: the second call
     * hands back the default one, whose buffer is the screen's. The update
     * reads through the current viewport's buffer, so the default one is
     * current for it. */
    struct viewport *prev = lcd_set_viewport(NULL);
    fb_data *dst = lcd_set_viewport(prev)->buffer->fb_ptr;
    const fb_data *src = tv_side_fb.fb_ptr;
    bool right = global_settings.text_viewer_orientation == TV_ORIENT_RIGHT;
    int u, v;

    for (v = 0; v < LCD_WIDTH; v++)
    {
        fb_data *d = right ? dst + (LCD_WIDTH - 1 - v)
                           : dst + (LCD_HEIGHT - 1) * LCD_WIDTH + v;
        int step = right ? LCD_WIDTH : -LCD_WIDTH;

        for (u = 0; u < LCD_HEIGHT; u++, d += step)
            *d = *src++;
    }
    lcd_set_viewport(NULL);
    lcd_update();
    lcd_set_viewport(prev);
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
    if (tv_side)
    {
        vp->buffer = &tv_side_fb;
        vp->width = LCD_HEIGHT;
        vp->height = LCD_WIDTH;
    }
}

/* A box over the page until the page is drawn again: splashf()'s upright,
 * or drawn into the sideways page and turned with it */
static void tv_splashf(const char *fmt, ...)
{
    struct viewport full;
    char text[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (!tv_side)
    {
        splashf(0, "%s", text);
        return;
    }
    tv_fullscreen_vp(&full);
    splashf_in(&full, "%s", text);
    tv_side_push();
}

/* The text of the chapter, or of a document without chapters: exact once it
 * has all been extracted, which for most chapters is at their first page,
 * and until then no less than the size it is stored at, which bounds it */
static off_t tv_text_len(void)
{
    off_t end = tv.win_start + (off_t)tv.win_len;
    off_t stored = tv.nparts ? ts_part_size(tv.src, tv.part) : tv.file_size;

    return tv.eof || stored < end ? end : stored;
}

/* Pages from the one after this to the end of the chapter, at the average
 * page so far */
static int tv_pages_left(void)
{
    off_t len = tv_text_len();
    off_t per_page = tv.next / (tv.sp + 1);

    if (tv.next >= len || per_page <= 0)
        return 0;
    return (len - tv.next + per_page - 1) / per_page;
}

static bool tv_at_book_end(void);
static bool tv_at_last_page(void);

/* How far through the book the page on screen starts, in percent, and 100 on
 * its last page: by the stored sizes of the chapters before it and the part
 * of this one read */
static int tv_book_percent(void)
{
    long before = 0, total = 0, size;
    off_t len = tv_text_len();

    if (tv_at_book_end())
        return 100;
    if (!tv.nparts)
        return len ? (int)((int64_t)tv.pos * 100 / len) : 100;
    for (int i = 0; i < tv.nparts; i++)
    {
        size = ts_part_size(tv.src, i);
        if (i < tv.part)
            before += size;
        total += size;
    }
    size = ts_part_size(tv.src, tv.part);
    if (len)
        before += (long)((int64_t)size * MIN(tv.pos, len) / len);
    return total ? MIN((int)((int64_t)before * 100 / total), 99) : 0;
}

/* The footer, right-aligned on the reserved bottom line. Drawn with tv.vp
 * current, so it uses the reading colours. */
static void tv_draw_footer(void)
{
    struct screen *d = &screens[SCREEN_MAIN];
    char buf[64];
    int tw, th, n = 0;

    switch (global_settings.text_viewer_footer)
    {
    case TV_FOOTER_PAGE:
        snprintf(buf, sizeof buf, "%d", tv.sp + 1);
        break;
    case TV_FOOTER_CHAPTER:
        if (tv.nparts > 1)
            n = snprintf(buf, sizeof buf, "%d/%d \xc2\xb7 ",
                         tv.part + 1, tv.nparts);
        snprintf(buf + n, sizeof buf - n, str(LANG_TEXT_VIEWER_PAGES_LEFT),
                 tv_pages_left());
        break;
    case TV_FOOTER_BOOK:
        snprintf(buf, sizeof buf, "%d%%", tv_book_percent());
        break;
    default:
        return;
    }
    tv.vp.font = tv_footer_fid();
    d->getstringsize((unsigned char *)buf, &tw, &th);
    d->putsxy(tv.vp.width - tw, tv.vp.height - tv_footer_height(),
              (unsigned char *)buf);
    tv.vp.font = tv.font;
}

static void tv_draw(void)
{
    struct screen *display = &screens[SCREEN_MAIN];
    struct viewport full, *last;

    tv_fullscreen_vp(&full);
    last = display->set_viewport(&full);
    display->clear_viewport();               /* whole screen -> background */

    display->set_viewport(&tv.vp);
    tv_pic_page = false;
    tv_pic_pending = false;
    tv.next = tv_page(tv.pos, true);         /* the page, inset by the margin */
    if (!tv_pic_page)
        tv_draw_footer();

    display->set_viewport(&full);
    if (tv_side)
        tv_side_push();
    else
        display->update_viewport();          /* push the whole screen */
    display->set_viewport(last);
}

/* How far past the page's end a reading may run to finish its sentence */
#define TV_READ_AHEAD 1024
/* Parts held at once: a page's sentences and the next page's first */
#define TV_PARTS      64
/* Characters a second the voice reads at a Speech Rate of 100% */
#define TV_READ_CPS   17

/* A page is read a sentence -- or a line -- at a time, each queued as a part
 * of its own, so Continuously can tell which one the voice is on and turn
 * the page as it crosses the edge, not when the reading runs out. */
struct tv_part
{
    int item;                /* talk_text_part()'s */
    off_t start, end;
};
static struct tv_part tv_parts[TV_PARTS];
static int tv_nparts;
static int tv_heard = -1;    /* the part last heard, and since when */
static long tv_heard_tick;
static off_t tv_read_upto;   /* where the queued reading ends */
static bool tv_reading;      /* the page on screen is being read */

/* Where the part at `from` ends, by `limit`: after a full stop, question or
 * exclamation mark with white space after it, or at a line break -- which
 * *stop asks to be read with a full stop's pause. */
static off_t tv_part_end(off_t from, off_t limit, bool *stop)
{
    *stop = false;
    for (off_t p = from; p < limit; p++)
    {
        unsigned char c = tv.win[p - tv.win_start];
        unsigned char next = p + 1 < limit ? tv.win[p + 1 - tv.win_start]
                                           : ' ';
        if ((c == '.' || c == '!' || c == '?')
            && (next == ' ' || next == '\n' || next == '\r'))
            return p + 1;
        if (c == '\n')
        {
            *stop = true;
            return p + 1;
        }
    }
    return limit;
}

/* Drops what the voice should not read from `n` bytes at `s`: soft hyphens,
 * which would split a word, heading marks and picture lines. The length
 * left. */
static size_t tv_strip_shy(unsigned char *s, size_t n)
{
    size_t r = 0, w = 0;

    while (r < n)
    {
        size_t h = tv_hidden(s + r, n - r);

        if (h)
            r += h;
        else if (tv_is_picture(s + r, n - r))
        {
            while (r < n && s[r] != '\n')
                r++;
        }
        else
            s[w++] = s[r++];
    }
    return w;
}

/* Queue the parts from `from` that start before `page_end`, the last
 * running on to its own end. */
static void tv_queue_parts(off_t from, off_t page_end)
{
    static char text[TV_READ_AHEAD + 2];
    off_t limit;

    tv_ensure(page_end, TV_READ_AHEAD);
    limit = tv.win_start + (off_t)tv.win_len;
    if (limit > page_end + TV_READ_AHEAD)
        limit = page_end + TV_READ_AHEAD;

    while (from < page_end && from < limit)
    {
        bool stop;
        off_t end = tv_part_end(from, limit, &stop);
        size_t n = end - from;
        int item;

        if (n > sizeof(text) - 2)
        {
            n = sizeof(text) - 2;
            while (n > 0 && (tv.win[from - tv.win_start + n] & 0xc0) == 0x80)
                n--;        /* not through a character */
            end = from + n;
            stop = false;
        }
        memcpy(text, tv.win + (from - tv.win_start), n);
        n = tv_strip_shy((unsigned char *)text, n);
        while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\n'
                         || text[n - 1] == '\r' || text[n - 1] == '\t'))
            n--;
        if (stop && n > 0 && !strchr(".,;:!?", text[n - 1]))
            text[n++] = '.';
        text[n] = '\0';

        item = talk_text_part(text);
        if (item >= 0)
        {
            if (tv_nparts == TV_PARTS)
            {
                memmove(tv_parts, tv_parts + 1,
                        (TV_PARTS - 1) * sizeof(*tv_parts));
                tv_nparts--;
            }
            tv_parts[tv_nparts++] = (struct tv_part){ item, from, end };
        }
        from = end;
    }
    tv_read_upto = from;
}

/* Read the page on screen aloud, with Speak Aloud and the synthesiser on,
 * on to the end of the sentence it stops in: a sentence cut at the edge of
 * the screen is said as though it ended there. A page turn cuts off the
 * one before. */
static bool tv_paused;       /* Read Aloud stopped by the reader, to go on */
static off_t tv_pause_from;  /* where it goes on from */

static void tv_speak_page(void)
{
    tv_reading = false;
    tv_nparts = 0;
    tv_heard = -1;
    if (global_settings.text_viewer_read_aloud == TV_READ_OFF
        || !speech_enabled()
        || tv.pos < tv.win_start)
        return;
    talk_force_shutup();
    if (tv_paused)
    {
        tv_pause_from = tv.pos;     /* a page turned while paused */
        return;
    }
    tv_queue_parts(tv.pos, tv.next);
    tv_reading = true;
}

/* Read Aloud is on and has a voice: Select and PLAY pause it */
static bool tv_aloud(void)
{
    return global_settings.text_viewer_read_aloud != TV_READ_OFF
           && speech_enabled();
}

/* Select, or PLAY upright, with Read Aloud on: the voice stops, and goes on
 * again from the start of the sentence it stopped in */
static void tv_pause_toggle(void)
{
    if (!tv_aloud())
        return;
    if (!tv_paused)
    {
        int item = talk_text_playing();

        tv_pause_from = tv.pos;
        for (int i = 0; tv_reading && i < tv_nparts; i++)
            if (tv_parts[i].item == item)
                tv_pause_from = tv_parts[i].start;
        talk_force_shutup();
        tv_reading = false;
        tv_paused = true;
        tv_splashf("%s", str(LANG_PAUSE));
        sleep(HZ / 2);
        tv_draw();
        return;
    }
    tv_paused = false;
    if (tv_pause_from < tv.win_start || tv_pause_from > tv.next)
        tv_pause_from = tv.pos;
    talk_force_shutup();
    tv_nparts = 0;
    tv_heard = -1;
    tv_queue_parts(tv_pause_from, tv.next);
    tv_reading = true;
}

/* Listening with the Hold switch on: the screen goes dark and the voice
 * reads on, turning pages as it goes; off, the screen comes back */
static bool tv_dark;

static void tv_hold_dark(void)
{
    bool hold = button_hold() && (tv_reading || tv_dark);

    if (hold == tv_dark)
        return;
    tv_dark = hold;
    if (hold)
        backlight_off();
    else
    {
        backlight_on();
        tv_draw();
    }
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
    size = ffilesize(fd);
    close(fd);

    return size > TV_WINDOW_SIZE;
}

/* Opening on the contents, with the theme still on: no page yet */
static bool tv_listing;

/* Names the file being opened: over the page, sideways with it if it is,
 * or upright over a list */
static void tv_splash_loading(bool over_page)
{
    const char *name = strrchr(tv.path, '/');

    /* A format, not the name: a '%' in a filename would be read as a
     * conversion. Until overdrawn. */
    if (over_page && !tv_listing)
        tv_splashf("%s\n%s", str(LANG_WAIT), name ? name + 1 : tv.path);
    else
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
    if (tv_side)
        tv_side_push();     /* update_viewport() would reach off the screen */
    else
        d->update_viewport();
    d->set_viewport(last);

    if (tv_slow_open(tv.path))
        tv_splash_loading(true);
}

/* ---- navigation ------------------------------------------------------- */

/* Past any offset a chapter can reach: tv_resume_to() to it lays out to the
 * chapter's last page. */
#define TV_OFF_MAX ((off_t)0x7fffffff)

static void tv_resume_to(off_t target);

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
    int rc;

    if (target < tv.win_start)
    {
        tv_splash_loading(true);
        cpu_boost(true);
        rc = ts_rewind(tv.src);
        cpu_boost(false);
        if (rc != BS_OK)
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

/* Whether picture `href` of the chapter is in the book; one that is not is
 * remembered as failed, so the layout passes over it */
static bool tv_picture_found(const char *href)
{
    long at;
    uint32_t csize, usize;
    int method;

    if (tv_picture_failed(href))
        return false;
    if (ts_picture_extent(tv.src, tv.part, href, &at, &csize, &usize,
                          &method) == BS_OK)
        return true;
    if (tv_pic_nfailed < TV_PIC_FAILED)
        tv_pic_failed[tv_pic_nfailed++] = tv_picture_key(href);
    return false;
}

/* Whether the window from `i` to `end` holds anything to show: a cover page
 * whose picture is passed over, or missing from the book, has nothing */
static bool tv_shows(size_t i, size_t end)
{
    while (i < end)
    {
        unsigned char c = tv.win[i];
        char href[TV_PIC_HREF];

        if (tv_is_picture(tv.win + i, tv.win_len - i))
        {
            i += tv_picture_line(tv.win + i, tv.win_len - i, href,
                                 sizeof(href));
            if (tv_pictures_shown() && tv_picture_found(href))
                return true;
        }
        else if (c == '\n' || c == ' ' || c == '\t' || c == '\r')
            i++;
        else
            return true;
    }
    return false;
}

/* Starts chapter `part` at its first byte, with an empty window and page
 * stack. False if it has no text to show. */
static bool tv_enter_part(int part)
{
    int rc;

    cpu_boost(true);
    rc = ts_seek_part(tv.src, part);
    cpu_boost(false);
    tv.part = part;
    tv.win_start = 0;
    tv.win_len = 0;
    tv.eof = false;
    tv.pos = tv.next = 0;
    tv.sp = 0;
    if (rc != BS_OK)
        return false;
    tv_ensure(0, 1);
    return tv.win_len > 0 && tv_shows(0, tv.win_len);
}

/* The first page of the next chapter with text */
static bool tv_next_part(void)
{
    int part;

    for (part = tv.part + 1; part < tv.nparts; part++)
        if (tv_enter_part(part))
            return true;
    return false;
}

/* The last page of the last chapter with text before `part`, laid out from
 * its start */
static bool tv_end_before(int part)
{
    while (--part >= 0)
        if (tv_enter_part(part))
        {
            tv_resume_to(TV_OFF_MAX);
            return true;
        }
    return false;
}

static bool tv_prev_part(void)
{
    return tv_end_before(tv.part);
}

/* The last chapter with anything to show: the book ends at its last page,
 * whatever empty chapters follow it. Found when the book opens. */
static int tv_last_text;

/* The next page, crossing into the next chapter from the last page of one.
 * False at the end of the document, leaving the last page where it was. */
static bool tv_advance(void)
{
    int part = tv.part;
    off_t pos = tv.pos;

    if (!tv_at_end())
    {
        if (tv.sp < TV_MAX_PAGES)
            tv.stack[tv.sp++] = tv.pos;
        tv.pos = tv.next;
        return true;
    }
    if (!tv.nparts || part == tv.nparts - 1)
        return false;
    if (tv_next_part())
        return true;
    /* Nothing after it has text: back to where the reader was */
    tv_enter_part(part);
    tv_resume_to(pos);
    return false;
}

/* The words on the page on screen, for the reading statistics */
static int tv_page_words(void)
{
    const unsigned char *s = tv.win + (tv.pos - tv.win_start);
    off_t n = tv.next - tv.pos;
    int words = 0;
    bool in = false;

    for (off_t i = 0; i < n; i++)
    {
        bool gap = s[i] == ' ' || s[i] == '\n';

        if (!gap && !in)
            words++;
        in = !gap;
    }
    return words;
}

/* Next on a book's last page: the book is closed, Finished */
static bool tv_book_done;

/* Where the page on screen lies in the book, for the statistics: larger for
 * a later page */
static int64_t tv_book_at(void)
{
    return (int64_t)tv.part << 32 | (uint32_t)tv.pos;
}

/* A page read by eye counts toward the statistics; one Read Aloud says
 * does not. At the end of a chapter, the page is drawn again: looking past
 * it may have put the loading box over it. At the book's end it closes. */
static void tv_next_page(void)
{
    int words = tv_page_words();
    int64_t at = tv_book_at();
    bool moved;

    if (tv_at_last_page())
    {
        /* drawn over the page, not over a loading box from looking past */
        reading_stats_touch();
        tv_splashf("%s", str(LANG_BOOK_SHELF_ROW_FINISHED));
        sleep(HZ);
        tv_book_done = true;
        return;
    }
    moved = tv_advance();
    if (moved && global_settings.text_viewer_read_aloud == TV_READ_OFF)
        reading_stats_page(words, at);
    else
        reading_stats_touch();
    tv_draw();
    if (moved)
        tv_speak_page();
}

/* A page laid out before its picture was tried, as reopening a book lays out
 * the pages to its place, starts at the picture; when that will not decode,
 * the page shows the one after it. Such a page is passed over, so that one
 * press goes back a page. */
static void tv_prev_page(void)
{
    int part = tv.part;
    off_t was_next = tv.next;

    reading_stats_touch();
    if (tv.sp > 0)
    {
        do
        {
            if (!tv_reach(tv.stack[tv.sp - 1]))
                return;
            tv.sp--;
            /* known to fail already: passed over without being drawn */
            if (tv.sp > 0 && tv_page(tv.pos, false) == was_next)
                continue;
            tv_draw();
        } while (tv.sp > 0 && tv.next == was_next);
        /* Unless the chapter opens on such a page, and one comes before */
        if (tv.sp > 0 || tv.next != was_next || !tv.nparts || part == 0)
        {
            tv_speak_page();
            return;
        }
    }

    /* first page of the chapter: the last of the one before */
    if (!tv.nparts || part == 0)
        return;
    if (!tv_prev_part())
    {
        tv_enter_part(part);       /* every chapter before is empty */
        return;
    }
    tv_draw();
    tv_speak_page();
}

/* ---- chapters by their headings ---------------------------------------- */

/* The window's byte at `at`, an offset into the chapter's text */
#define TV_AT(at) (tv.win[(at) - tv.win_start])

/* Past the white space and heading lines at `at`, up to `end`: where the
 * text starts on a page that opens with a chapter's headings */
static off_t tv_past_heads(off_t at, off_t end)
{
    while (at < end)
    {
        unsigned char c = TV_AT(at);

        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            at++;
        else if (end - at >= 3 && !memcmp(&TV_AT(at), TV_HEAD_OPEN, 3))
        {
            for (at += 3; at + 3 <= end && !(TV_AT(at) == 0xee
                     && TV_AT(at + 1) == 0x80 && TV_AT(at + 2) == 0x83); at++)
                ;
            at += 3;
        }
        else
            break;
    }
    return at;
}

/* The first heading from `from` up to `to`, or -1 */
static off_t tv_head_in(off_t from, off_t to)
{
    for (off_t i = from; i + 3 <= to; i++)
        if (!memcmp(&TV_AT(i), TV_HEAD_OPEN, 3))
            return i;
    return -1;
}

/* Back to the first page of the chapter's text, or of a document without
 * chapters */
static void tv_to_start(void)
{
    if (tv.nparts)
        tv_enter_part(tv.part);
    else
    {
        tv_reach(0);
        tv.sp = 0;
    }
}

/* A page that starts at `off`, the text before it on the pages before, laid
 * out from the chapter's start: where a heading begins */
static void tv_start_at(off_t off)
{
    tv_resume_to(off);
    while (off < tv.win_start + (off_t)tv.win_len
           && (TV_AT(off) == '\n' || TV_AT(off) == ' '))
        off++;
    if (tv.pos < off && tv.sp < TV_MAX_PAGES
        && off < tv.win_start + (off_t)tv.win_len)
    {
        tv.stack[tv.sp++] = tv.pos;
        tv.pos = off;
    }
}

/* Laid out from the chapter's start: the start of the last run of headings
 * before `target`, or -1. Headings in a row, a chapter's number and its
 * title, are one. */
static off_t tv_head_before(off_t target)
{
    off_t last = -1, run_end = -1;

    tv_to_start();
    while (tv.pos < target)
    {
        off_t next = tv_page(tv.pos, false), h, at = tv.pos;

        while ((h = tv_head_in(at, MIN(next, target))) >= 0)
        {
            if (h >= run_end)
                last = h;
            run_end = tv_past_heads(h, next);
            at = h + 3;
        }
        if (next <= tv.pos || (tv.eof && next >= tv.win_start
                                             + (off_t)tv.win_len))
            break;
        if (tv.sp < TV_MAX_PAGES)
            tv.stack[tv.sp++] = tv.pos;
        tv.pos = next;
    }
    return last;
}

/* Held back: where this chapter's headings start, or, already there, the
 * chapter before's */
static void tv_chapter_start(void)
{
    int part = tv.part;
    off_t target = tv.pos, h;

    reading_stats_touch();
    tv_splash_loading(true);
    cpu_boost(true);
    h = tv_head_before(target);
    if (h < 0 && tv.nparts)
    {
        /* None before it in this chapter's text: its start, or when the
         * page is that start, the last heading of the text before */
        tv_to_start();
        if (target > tv_past_heads(0, tv.win_start + (off_t)tv.win_len)
            || !tv_end_before(part))
        {
            tv_enter_part(part);
            h = 0;
        }
        else
        {
            h = tv_head_before(TV_OFF_MAX);
            if (h < 0)
                h = 0;
        }
    }
    else if (h < 0)
        h = 0;
    tv_to_start();
    if (h > 0)
        tv_start_at(h);
    cpu_boost(false);
    tv_draw();
    tv_speak_page();
}

/* Held forward: the next chapter's headings, past those the page opens
 * with, or the book's last page */
static void tv_chapter_next(void)
{
    int part = tv.part;
    off_t from;

    reading_stats_touch();
    tv_splash_loading(true);
    cpu_boost(true);
    from = tv_past_heads(tv.pos, tv_page(tv.pos, false));
    while (1)
    {
        off_t next = tv_page(tv.pos, false);
        off_t h = tv_head_in(MAX(from, tv.pos), next);

        if (h >= 0)
        {
            if (h > tv.pos && tv.sp < TV_MAX_PAGES)
            {
                tv.stack[tv.sp++] = tv.pos;
                tv.pos = h;
            }
            break;
        }
        if (next <= tv.pos || (tv.eof && next >= tv.win_start
                                             + (off_t)tv.win_len))
        {
            /* the end of this chapter's text: the next with any, or this
             * last page */
            if (tv.nparts && !tv_next_part())
            {
                tv_enter_part(part);
                tv_resume_to(TV_OFF_MAX);
            }
            break;
        }
        if (tv.sp < TV_MAX_PAGES)
            tv.stack[tv.sp++] = tv.pos;
        tv.pos = next;
    }
    cpu_boost(false);
    tv_draw();
    tv_speak_page();
}

/* Continuously: the voice has reached the next page, so show it and queue
 * the rest of it behind what is still being said. */
static void tv_turn_page(void)
{
    int part = tv.part;

    reading_stats_touch();
    if (!tv_advance())
    {
        tv_reading = false;
        tv_draw();
        return;
    }
    tv_draw();
    reset_poweroff_timer();     /* listening is not idle */
    if (tv.part != part)
    {
        /* A new chapter: what was queued counted in the last one, and has
         * all been said, as the page turns only when the voice is done */
        tv_speak_page();
        return;
    }
    tv_queue_parts(tv_read_upto > tv.pos ? tv_read_upto : tv.pos, tv.next);
}

/* Continuously: turn the page when the voice crosses its edge. In the
 * sentence that runs over it, that is reckoned from the voice's pace and how
 * much of the sentence is on this page. */
static void tv_read_on(void)
{
    const struct tv_part *part = NULL;
    int item;

    if (!tv_reading
        || global_settings.text_viewer_read_aloud != TV_READ_CONTINUOUS)
        return;
    if (!voice_is_playing())
    {
        tv_turn_page();         /* all said: the page had nothing more */
        return;
    }

    item = talk_text_playing();
    for (int i = 0; i < tv_nparts; i++)
        if (tv_parts[i].item == item)
            part = &tv_parts[i];
    if (!part)
        return;
    if (item != tv_heard)
    {
        tv_heard = item;
        tv_heard_tick = current_tick;
    }

    if (part->start >= tv.next)
        tv_turn_page();
    else if (part->end > tv.next)
    {
        long len = part->end - part->start;
        long said = (long)HZ * len * 100
                    / (TV_READ_CPS * global_settings.talk_speech_rate);
        if (current_tick - tv_heard_tick
            >= said * (tv.next - part->start) / len)
            tv_turn_page();
    }
}

/* Pages forward from the start without drawing until `target` falls on the
 * current page. Doubles as the stack rebuild: resuming leaves the history
 * exactly as it would have been had the reader paged there by hand. */
static void tv_resume_to(off_t target)
{
    tv_splash_loading(true);
    cpu_boost(true);

    while (1)
    {
        off_t next = tv_page(tv.pos, false);

        if (next <= tv.pos || next > target
            || (tv.eof && next >= tv.win_start + (off_t)tv.win_len))
            break;                           /* the last page, or target is on
                                              * the page we are on */
        if (tv.sp < TV_MAX_PAGES)
            tv.stack[tv.sp++] = tv.pos;
        tv.pos = next;
    }
    cpu_boost(false);
}

/* ---- resume ----------------------------------------------------------- */

/* One document's place, a record of TV_RESUME_FILE: a chapter, and an
 * offset into its extracted text (the whole document's, without parts).
 * `size` is the document's length when the offset was recorded: offsets
 * index the extracted text, so a document that has changed on disk
 * invalidates them, and the length is the cheap half of noticing. (It
 * cannot catch an edit that preserves the length. A layout change moves the
 * page boundaries, but the offset still points into the same sentence.) */
struct tv_place
{
    int32_t part;            /* TV_PART_UNKNOWN: `off` counts from the start */
    int32_t off;
    int32_t size;
    int32_t flags;           /* TV_PLACE_* */
    char path[MAX_PATH];
};

/* Read to its last page, or marked Finished */
#define TV_PLACE_FINISHED 0x1
/* The book's percent at the page, plus one: 0 in a record from before it */
#define TV_PLACE_PCT_SHIFT 8
#define TV_PLACE_PCT_MASK  (0xff << TV_PLACE_PCT_SHIFT)

/* Version 2 of the record, before the flags */
struct tv_place_v2
{
    int32_t part;
    int32_t off;
    int32_t size;
    char path[MAX_PATH];
};

/* A place recorded before documents had parts, or by the text conversion */
#define TV_PART_UNKNOWN (-1)

/* Version 1 of the record, before parts */
struct tv_place_v1
{
    int32_t off;
    int32_t size;
    char path[MAX_PATH];
};

/* Version 2's places as version 3's, none of them finished */
static void tv_places_upgrade_v2(void)
{
    struct libfile_header h;
    struct libfile_writer w;
    struct tv_place_v2 old;
    struct tv_place p;
    bool ok;
    int fd = libfile_open(TV_RESUME_FILE, LIB_READING_MAGIC, 2,
                          sizeof(old), &h, NULL);

    if (fd < 0)
        return;
    ok = libfile_begin(&w, TV_RESUME_FILE, LIB_READING_MAGIC,
                       LIB_READING_VERSION, sizeof(p), NULL);
    for (uint32_t i = 0; ok && i < h.count; i++)
    {
        if (read(fd, &old, sizeof(old)) != (ssize_t)sizeof(old))
            break;
        old.path[sizeof(old.path) - 1] = '\0';
        memset(&p, 0, sizeof(p));
        p.part = old.part;
        p.off = old.off;
        p.size = old.size;
        strmemccpy(p.path, old.path, sizeof(p.path));
        ok = libfile_write(&w, &p, sizeof(p), 1);
    }
    close(fd);
    if (ok)
        libfile_finish(&w, true);
}

/* Version 1's places as version 3's, their chapters unknown */
static void tv_places_upgrade(void)
{
    struct libfile_header h;
    struct libfile_writer w;
    struct tv_place_v1 old;
    struct tv_place p;
    bool ok;
    int fd = libfile_open(TV_RESUME_FILE, LIB_READING_MAGIC, 1,
                          sizeof(old), &h, NULL);

    if (fd < 0)
    {
        tv_places_upgrade_v2();
        return;
    }
    ok = libfile_begin(&w, TV_RESUME_FILE, LIB_READING_MAGIC,
                       LIB_READING_VERSION, sizeof(p), NULL);
    for (uint32_t i = 0; ok && i < h.count; i++)
    {
        if (read(fd, &old, sizeof(old)) != (ssize_t)sizeof(old))
            break;
        old.path[sizeof(old.path) - 1] = '\0';
        memset(&p, 0, sizeof(p));
        p.part = TV_PART_UNKNOWN;
        p.off = old.off;
        p.size = old.size;
        strmemccpy(p.path, old.path, sizeof(p.path));
        ok = libfile_write(&w, &p, sizeof(p), 1);
    }
    close(fd);
    if (ok)
        libfile_finish(&w, true);
}

static int tv_places_open(struct libfile_header *h)
{
    int fd = libfile_open(TV_RESUME_FILE, LIB_READING_MAGIC,
                          LIB_READING_VERSION, sizeof(struct tv_place), h,
                          NULL);

    if (fd == LIBFILE_BAD)
    {
        tv_places_upgrade();
        fd = libfile_open(TV_RESUME_FILE, LIB_READING_MAGIC,
                          LIB_READING_VERSION, sizeof(struct tv_place), h,
                          NULL);
    }
    return fd;
}

static bool tv_places_begin(struct libfile_writer *w)
{
    return libfile_begin(w, TV_RESUME_FILE, LIB_READING_MAGIC,
                         LIB_READING_VERSION, sizeof(struct tv_place), NULL);
}

/* The next record, its path terminated whatever the file holds and without
 * a volume, as text_viewer() keeps it */
static bool tv_place_read(int fd, struct tv_place *p)
{
    const char *path;

    if (read(fd, p, sizeof(*p)) != (ssize_t)sizeof(*p))
        return false;
    p->path[sizeof(p->path) - 1] = '\0';
    path = path_key_strip(p->path);
    memmove(p->path, path, strlen(path) + 1);
    return true;
}

/* Zeroed past the path, so the same places are the same bytes */
static void tv_place_set(struct tv_place *p, int part, off_t off, off_t size,
                         const char *path)
{
    memset(p, 0, sizeof(*p));
    p->part = part;
    p->off = off;
    p->size = size;
    strmemccpy(p->path, path, sizeof(p->path));
}

/* A finished book opened stays finished until a page is turned: the place
 * it was opened at, kept while it is */
static bool tv_open_finished;
static int tv_open_part;
static off_t tv_open_pos;

/* The page on screen is the last page of the book's last chapter with text */
static bool tv_at_last_page(void)
{
    return (!tv.nparts || tv.part >= tv_last_text) && tv_at_end();
}

/* The book stands at its end: its last page, or still the page a finished
 * book was opened at -- which a change of layout, a picture shown or a font
 * size, can have moved off the last page */
static bool tv_at_book_end(void)
{
    return tv_at_last_page()
           || (tv_open_finished && tv.part == tv_open_part
               && tv.pos == tv_open_pos);
}

/* The saved place in `file`, 0 and 0 if none; *part may be
 * TV_PART_UNKNOWN. Whether it is finished goes in tv_open_finished. */
static off_t tv_resume_load(const char *file, off_t size, int *part)
{
    struct libfile_header h;
    struct tv_place p;
    off_t off = 0;
    int fd = tv_places_open(&h);

    *part = 0;
    tv_open_finished = false;
    if (fd < 0)
        return 0;

    for (uint32_t i = 0; i < h.count && tv_place_read(fd, &p); i++)
    {
        if (strcmp(p.path, file))
            continue;
        tv_open_finished = p.flags & TV_PLACE_FINISHED;
        if ((off_t)p.size == size)
        {
            *part = p.part;
            off = p.off;
        }
        break;                               /* first hit is the newest */
    }

    close(fd);
    return off;
}

/* Rewrites the list with `file` at the front, streaming the old records past
 * a single buffer rather than holding them all on the stack. */
static void tv_resume_save(const char *file, int part, off_t off,
                           off_t size)
{
    struct libfile_writer w;
    struct libfile_header h;
    struct tv_place p;
    int old, kept = 1;
    bool ok;

    /* The player's own documents, such as the licence, are not reading, so
     * they never become "Continue reading". */
    if (!strncasecmp(file, ROCKBOX_DIR "/docs/",
                     sizeof(ROCKBOX_DIR "/docs/") - 1))
        return;

    if (!tv_places_begin(&w))
        return;

    tv_place_set(&p, part, off, size, file);
    if (tv_at_book_end())
        p.flags |= TV_PLACE_FINISHED;
    p.flags |= (tv_book_percent() + 1) << TV_PLACE_PCT_SHIFT;
    ok = libfile_write(&w, &p, sizeof(p), 1);

    old = tv_places_open(&h);
    if (old >= 0)
    {
        for (uint32_t i = 0; ok && kept < TV_RESUME_MAX && i < h.count
                             && tv_place_read(old, &p); i++)
        {
            if (!strcmp(p.path, file))
                continue;                    /* superseded by the new record */
            ok = libfile_write(&w, &p, sizeof(p), 1);
            kept++;
        }
        close(old);
    }
    libfile_finish(&w, ok);
}

void text_viewer_each_place(text_viewer_place_fn fn, void *data)
{
    struct libfile_header h;
    struct tv_place p;
    int fd = tv_places_open(&h);

    if (fd < 0)
        return;
    for (uint32_t i = 0; i < h.count && tv_place_read(fd, &p); i++)
    {
        int pct = ((p.flags & TV_PLACE_PCT_MASK) >> TV_PLACE_PCT_SHIFT) - 1;

        if (!fn(p.path, pct, p.flags & TV_PLACE_FINISHED, data))
            break;
    }
    close(fd);
}

bool text_viewer_finished(const char *path)
{
    struct libfile_header h;
    struct tv_place p;
    bool finished = false;
    int fd = tv_places_open(&h);

    if (fd < 0)
        return false;
    for (uint32_t i = 0; i < h.count && tv_place_read(fd, &p); i++)
        if (!strcmp(p.path, path))
        {
            finished = p.flags & TV_PLACE_FINISHED;
            break;
        }
    close(fd);
    return finished;
}

bool text_viewer_mark_place(const char *path, int state)
{
    struct libfile_writer w;
    struct libfile_header h;
    struct tv_place p;
    bool found = false, ok;
    int old = tv_places_open(&h), kept = 0;

    if (!tv_places_begin(&w))
    {
        if (old >= 0)
            close(old);
        return false;
    }
    ok = true;
    /* A book with no place yet gets one at its start, newest */
    if (state != TV_MARK_NOT_STARTED && old >= 0)
    {
        for (uint32_t i = 0; i < h.count && tv_place_read(old, &p); i++)
            found = found || !strcmp(p.path, path);
        lseek(old, sizeof(h), SEEK_SET);
    }
    if (state != TV_MARK_NOT_STARTED && !found)
    {
        tv_place_set(&p, 0, 0, 0, path);
        p.flags = state == TV_MARK_FINISHED ? TV_PLACE_FINISHED : 0;
        ok = libfile_write(&w, &p, sizeof(p), 1);
        kept++;
    }
    if (old >= 0)
    {
        for (uint32_t i = 0; ok && kept < TV_RESUME_MAX && i < h.count
                             && tv_place_read(old, &p); i++)
        {
            if (!strcmp(p.path, path))
            {
                if (state == TV_MARK_NOT_STARTED)
                    continue;
                p.flags &= ~TV_PLACE_FINISHED;
                if (state == TV_MARK_FINISHED)
                    p.flags |= TV_PLACE_FINISHED;
            }
            ok = libfile_write(&w, &p, sizeof(p), 1);
            kept++;
        }
        close(old);
    }
    return libfile_finish(&w, ok);
}

/* The document on the newest record, for the main menu's "Continue reading".
 *
 * Says nothing about whether the document still exists -- that is the
 * caller's to check, since it is the caller who has to decide what to do
 * about it. */
bool text_viewer_last_document(char *buf, size_t bufsize)
{
    struct libfile_header h;
    struct tv_place p;
    bool found = false;
    int fd = tv_places_open(&h);

    if (fd < 0)
        return false;

    if (h.count >= 1 && tv_place_read(fd, &p) && p.path[0] != '\0')
    {
        strmemccpy(buf, p.path, bufsize);
        found = true;
    }

    close(fd);
    return found;
}

/* The text file held lines of "<offset> <size> <path>", newest first, the
 * path last so it may hold spaces. */
bool text_viewer_convert_places(const char *text_file)
{
    struct libfile_writer w;
    struct tv_place p;
    char line[MAX_PATH + 32];
    int kept = 0;
    bool ok;
    int fd = open(text_file, O_RDONLY);

    if (fd < 0)
        return false;
    ok = tv_places_begin(&w);

    while (ok && kept < TV_RESUME_MAX
           && read_line(fd, line, sizeof(line)) > 0)
    {
        char *end_off = strchr(line, ' ');
        char *end_size = end_off ? strchr(end_off + 1, ' ') : NULL;

        if (!end_size || end_size[1] == '\0')
            continue;
        *end_size = '\0';
        tv_place_set(&p, TV_PART_UNKNOWN, atoi(line), atoi(end_off + 1),
                     end_size + 1);
        ok = libfile_write(&w, &p, sizeof(p), 1);
        kept++;
    }

    close(fd);
    return libfile_finish(&w, ok);
}

/* ---- lifecycle -------------------------------------------------------- */

static bool tv_open(const char *file)
{
    ts_config cfg = ts_config_default();
    bs_io io;
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

    if (ts_io_core(&io, &tv.file, file) != BS_OK)
        return false;

    /* Through the vtable rather than ffilesize(): coreio_size() restores the
     * descriptor's position and keeps its cached copy honest. */
    tv.file_size = io.size(io.ctx);

    /* Boosted for the whole-file scans a PDF or zip opens with. */
    cpu_boost(true);
    rc = ts_open(&tv.src, &io, file, arena, TS_ARENA_RECOMMENDED, &cfg);
    cpu_boost(false);
    if (rc != BS_OK)
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
    tv.part = 0;
    tv.nparts = ts_part_count(tv.src);
    return true;
}

static void tv_close(void)
{
    tv_side_free();
    tv_picture_free();
    backlight_set_brightness(global_settings.brightness);
    backlight_set_timeout(global_settings.backlight_timeout);
    backlight_set_timeout_plugged(global_settings.backlight_timeout_plugged);
    speech_reader(false);

    if (tv.user_font >= 0)
    {
        font_unload(tv.user_font);
        tv.user_font = -1;
    }
    if (tv.footer_font >= 0)
    {
        font_unload(tv.footer_font);
        tv.footer_font = -1;
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

    if (tv.footer_font < 0)
        tv.footer_font = font_load(TV_FOOTER_FONT);
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
    /* Put back in tv_close(), as are the reader's claim on the voice and
     * the backlight timeout */
    speech_reader(global_settings.text_viewer_read_aloud != TV_READ_OFF);
    backlight_set_brightness(global_settings.text_viewer_brightness
                             ? global_settings.text_viewer_brightness
                             : global_settings.brightness);
    if (global_settings.text_viewer_backlight != -1)
    {
        backlight_set_timeout(global_settings.text_viewer_backlight);
        backlight_set_timeout_plugged(global_settings.text_viewer_backlight);
    }
    else
    {
        backlight_set_timeout(global_settings.backlight_timeout);
        backlight_set_timeout_plugged(
            global_settings.backlight_timeout_plugged);
    }

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
        /* The reading schemes. Cream is the British Dyslexia Association's
         * dark-on-cream; Peach and Yellow read fastest in Rello and Bigham
         * (ASSETS 2017); Sepia is the usual e-reader pair; Night is dim
         * amber for a dark room. */
        case TV_COLOUR_CREAM:
            tv.vp.fg_pattern = LCD_RGBPACK(0x33, 0x33, 0x33);
            tv.vp.bg_pattern = LCD_RGBPACK(0xfa, 0xf3, 0xdc);
            break;
        case TV_COLOUR_PEACH:
            tv.vp.fg_pattern = LCD_BLACK;
            tv.vp.bg_pattern = LCD_RGBPACK(0xed, 0xd1, 0xb0);
            break;
        case TV_COLOUR_YELLOW:
            tv.vp.fg_pattern = LCD_BLACK;
            tv.vp.bg_pattern = LCD_RGBPACK(0xf8, 0xfd, 0x89);
            break;
        case TV_COLOUR_SEPIA:
            tv.vp.fg_pattern = LCD_RGBPACK(0x5b, 0x46, 0x36);
            tv.vp.bg_pattern = LCD_RGBPACK(0xf4, 0xec, 0xd8);
            break;
        case TV_COLOUR_NIGHT:
            tv.vp.fg_pattern = LCD_RGBPACK(0xc8, 0xa0, 0x60);
            tv.vp.bg_pattern = LCD_BLACK;
            break;
        case TV_COLOUR_CUSTOM:
            tv.vp.fg_pattern = global_settings.text_viewer_fg;
            tv.vp.bg_pattern = global_settings.text_viewer_bg;
            break;
        case TV_COLOUR_THEME:
        default:
            tv.vp.fg_pattern = global_settings.fg_color;
            tv.vp.bg_pattern = global_settings.bg_color;
            break;
    }

    /* Sideways where memory allows it; upright otherwise */
    tv_side = global_settings.text_viewer_orientation != TV_ORIENT_UPRIGHT
              && tv_side_alloc();
    if (!tv_side)
        tv_side_free();

    margin = global_settings.text_viewer_margin ? TV_MARGIN : 0;
    tv.vp.x = margin;
    tv.vp.y = margin;
    tv.vp.width  = (tv_side ? LCD_HEIGHT : d->lcdwidth)  - 2 * margin;
    tv.vp.height = (tv_side ? LCD_WIDTH : d->lcdheight) - 2 * margin;
    tv.vp.buffer = tv_side ? &tv_side_fb : NULL;

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
     * enabled -- so nothing repaints over the page between our own draws;
     * under a themed (SBS) status bar that repaint makes the reading area
     * flicker while paging. */
    viewportmanager_theme_enable(SCREEN_MAIN, false, &tv.vp);
    tv_apply_settings();
}

/* ---- contents --------------------------------------------------------- */

/* Rows the contents list holds, and the bytes of each title and anchor
 * kept */
#define TV_TOC_MAX    320
#define TV_TOC_TITLE  46
#define TV_TOC_ANCHOR 32

struct tv_toc_entry
{
    int16_t part;
    int32_t off;                    /* a heading in one text, or -1 */
    char title[TV_TOC_TITLE];
    char anchor[TV_TOC_ANCHOR];     /* the id it points at, "" for the start */
};

struct tv_toc
{
    struct tv_toc_entry *e;
    int n;
};

/* An entry at the end, its title cut at a character if it must be */
static int tv_toc_put(struct tv_toc *t, const char *title, int part,
                      const char *anchor, long off)
{
    char *d;
    size_t n;

    if (t->n == TV_TOC_MAX)
        return 1;
    d = t->e[t->n].title;
    n = strlen(title);
    if (n > TV_TOC_TITLE - 1)
    {
        n = TV_TOC_TITLE - 1;
        while (n > 0 && ((unsigned char)title[n] & 0xc0) == 0x80)
            n--;                    /* not through a character */
    }
    memcpy(d, title, n);
    d[n] = '\0';
    strcpy(t->e[t->n].anchor, anchor);
    t->e[t->n].off = off;
    t->e[t->n++].part = part;
    return 0;
}

/* ts_toc()'s callback. An entry pointing where the one before it does, such
 * as a chapter's subtitle, or a second at the start of a file, is left out. */
static int tv_toc_add(void *ud, const char *title, int part,
                      const char *anchor)
{
    struct tv_toc *t = ud;

    if (strlen(anchor) >= TV_TOC_ANCHOR)
        anchor = "";                /* too long to keep: the file's start */
    if (t->n && t->e[t->n - 1].part == part
        && (!anchor[0] || !strcmp(t->e[t->n - 1].anchor, anchor)))
        return 0;
    return tv_toc_put(t, title, part, anchor, -1);
}

/* Back to the first byte of a text without chapters, with an empty window:
 * the engine has been read elsewhere */
static void tv_restart_text(void)
{
    ts_rewind(tv.src);
    tv.win_start = 0;
    tv.win_len = 0;
    tv.eof = false;
    tv.pos = tv.next = 0;
    tv.sp = 0;
}

/* The contents of a text without chapters: its headings, read from its
 * start, where each begins. A run of headings, a number and its title, is
 * one row. */
static void tv_toc_heads(struct tv_toc *t)
{
    char buf[512], title[TV_TOC_TITLE + 4];
    long got, at = 0, start = -1, run_end = -1;
    int in = 0, tl = 0;

    tv_splash_loading(false);
    tv_restart_text();
    while ((got = ts_read(tv.src, buf, sizeof(buf))) > 0)
    {
        for (long i = 0; i < got; i++, at++)
        {
            unsigned char c = buf[i];

            if (c == 0xee && i + 2 < got && (unsigned char)buf[i + 1] == 0x80
                && ((unsigned char)buf[i + 2] == 0x82
                    || (unsigned char)buf[i + 2] == 0x83))
            {
                bool open = (unsigned char)buf[i + 2] == 0x82;

                i += 2;
                at += 2;
                if (open)
                {
                    if (start < 0 || at - 2 > run_end)
                    {
                        if (start >= 0 && tl)
                        {
                            title[tl] = '\0';
                            tv_toc_put(t, title, 0, "", start);
                        }
                        start = at - 2;
                        tl = 0;
                    }
                    else if (tl && tl < TV_TOC_TITLE - 1)
                        title[tl++] = ' ';
                    in = 1;
                }
                else
                {
                    in = 0;
                    run_end = at + 1;
                }
                continue;
            }
            if (in && tl < TV_TOC_TITLE + 2 && c != '\n')
                title[tl++] = c;
            else if (!in && at == run_end && (c == '\n' || c == ' '))
                run_end = at + 1;       /* white space keeps the run going */
        }
    }
    if (start >= 0 && tl)
    {
        title[tl] = '\0';
        tv_toc_put(t, title, 0, "", start);
    }
}

static const char *tv_toc_name(int item, void *data, char *buf, size_t len)
{
    (void)buf;
    (void)len;
    return ((struct tv_toc *)data)->e[item].title;
}

bool text_viewer_has_contents(void)
{
    return tv.src && (tv.nparts > 1 || (!tv.nparts && tv_heads_seen));
}

/* The book's contents, from its own table of contents, or one row per
 * chapter file with text without one; a pick opens that chapter, at the
 * page its id is on when it starts inside a file. Reading the contents ends
 * the chapter being read, so without a pick the page on screen is laid out
 * again. Returns 1 to close the menu after a pick. */
int text_viewer_contents(void)
{
    struct simplelist_info info;
    struct tv_toc toc = { NULL, 0 };
    int handle, part = tv.part, sel = 0, target = -1, i, rc;
    off_t pos = tv.pos;
    long head = -1;
    char anchor[TV_TOC_ANCHOR] = "";

    handle = core_alloc(TV_TOC_MAX * sizeof(struct tv_toc_entry));
    if (handle <= 0)
        return 0;
    toc.e = core_get_data_pinned(handle);

    cpu_boost(true);
    rc = tv.nparts ? ts_toc(tv.src, tv_toc_add, &toc) : BS_ERR_UNSUP;
    if (!tv.nparts)
        tv_toc_heads(&toc);
    else if (rc != BS_OK || !toc.n)
    {
        /* No contents of its own: each chapter file by its first line, and
         * those with no text, such as a cover, left out */
        tv_splash_loading(false);
        toc.n = 0;
        for (i = 0; i < tv.nparts && toc.n < TV_TOC_MAX; i++)
        {
            char line[TV_TOC_TITLE + 8], *s, *nl;
            long got;

            if (ts_seek_part(tv.src, i) != BS_OK)
                break;
            got = ts_read(tv.src, line, sizeof(line) - 1);
            if (got <= 0)
                continue;
            line[got] = '\0';
            for (s = line; *s == '\n' || *s == ' '; s++)
                ;
            nl = strchr(s, '\n');
            if (nl)
                *nl = '\0';
            if (!*s)
                continue;
            tv_toc_add(&toc, s, i, "");
        }
    }
    cpu_boost(false);

    /* The entry the page on screen is in: the first of its file, as where
     * the others start is not known until one is picked */
    for (i = 0; i < toc.n; i++)
        if (tv.nparts ? toc.e[i].part < part
                        || (toc.e[i].part == part
                            && (i == 0 || toc.e[i - 1].part < part))
                      : toc.e[i].off <= pos)
            sel = i;
    simplelist_info_init(&info, str(LANG_BROWSE_CHAPTERS), toc.n, &toc);
    info.get_name = tv_toc_name;
    info.selection = sel;
    /* USB leaves the selection where it was, where MENU and BACK clear it.
     * The book's file is closed under the reader then, so it leaves too. */
    if (!toc.n)
    {
        splash(HZ, ID2P(LANG_NO_CHAPTERS));
        info.selection = -1;
    }
    else if (simplelist_show_list(&info) && info.selection >= 0)
    {
        core_unpin(handle);
        core_free(handle);
        return MENU_ATTACHED_USB;
    }
    if (info.selection >= 0)
    {
        target = toc.e[info.selection].part;
        strcpy(anchor, toc.e[info.selection].anchor);
        head = toc.e[info.selection].off;
    }

    core_unpin(handle);
    core_free(handle);

    /* A heading of a text without chapters: a page that starts at it */
    if (!tv.nparts)
    {
        tv_restart_text();
        if (head > 0)
            tv_start_at((off_t)head);
        else if (head < 0)
            tv_resume_to(pos);
        return head >= 0;
    }

    /* An entry inside a file: a page that starts where its id is, the text
     * before it on the page before */
    if (target >= 0 && anchor[0])
    {
        long off;

        cpu_boost(true);
        rc = ts_anchor(tv.src, target, anchor, &off);
        cpu_boost(false);
        if (rc == BS_OK && tv_enter_part(target))
        {
            tv_start_at((off_t)off);
            return 1;
        }
    }
    if (target >= 0 && (tv_enter_part(target) || tv_next_part()))
        return 1;
    tv_enter_part(part);
    tv_resume_to(pos);
    return target >= 0;
}

/* ---- scrubbing with the wheel ------------------------------------------ */

/* Wheel Scrubs: the wheel moves a target through the book a percent a click,
 * shown over the page; Select jumps there, and any other button or a few
 * idle seconds put the page back. Placed by the chapters' stored sizes, as
 * Book Progress counts them. The first clicks only show where the page is,
 * so a brush of the wheel moves nothing. */
#define TV_SCRUB_IDLE  (HZ * 4)
#define TV_SCRUB_SLACK 4

static int tv_scrub_pct = -1;   /* the target, or -1 when not scrubbing */
static long tv_scrub_until;
static int tv_scrub_slack;      /* clicks still to pass before it moves */

/* The chapter `pct` percent into the book falls in, and how far into it, in
 * thousandths, in *within */
static int tv_scrub_part(int pct, int *within)
{
    long total = 0, before = 0, size, at;
    int i;

    *within = 0;
    for (i = 0; i < tv.nparts; i++)
        total += ts_part_size(tv.src, i);
    at = (long)((int64_t)total * pct / 100);
    for (i = 0; i < tv.nparts - 1; i++)
    {
        size = ts_part_size(tv.src, i);
        if (before + size > at)
            break;
        before += size;
    }
    size = ts_part_size(tv.src, i);
    if (size > 0)
        *within = (int)((int64_t)(at - before) * 1000 / size);
    return i;
}

static void tv_scrub_show(void)
{
    int within;

    if (tv.nparts > 1)
        tv_splashf("%d/%d \xc2\xb7 %d%%",
                tv_scrub_part(tv_scrub_pct, &within) + 1, tv.nparts,
                tv_scrub_pct);
    else
        tv_splashf("%d%%", tv_scrub_pct);
}

static void tv_scrub(int step)
{
    if (tv_scrub_pct < 0)
    {
        tv_scrub_pct = tv_book_percent();
        tv_scrub_slack = TV_SCRUB_SLACK;
    }
    if (tv_scrub_slack > 0)
        tv_scrub_slack--;
    else
        tv_scrub_pct = MIN(MAX(tv_scrub_pct + step, 0), 100);
    tv_scrub_until = current_tick + TV_SCRUB_IDLE;
    tv_scrub_show();
}

/* Ends the scrub, jumping to its target when `go` */
static void tv_scrub_end(bool go)
{
    int pct = tv_scrub_pct, within;

    tv_scrub_pct = -1;
    if (go && !tv.nparts)
    {
        off_t to = (off_t)((int64_t)tv_text_len() * pct / 100);

        if (to < tv.pos)
        {
            tv_reach(0);                /* laid out forward, from the top */
            tv.sp = 0;
        }
        tv_resume_to(to);
    }
    else if (go)
    {
        int part = tv_scrub_part(pct, &within);

        if (tv_enter_part(part) || tv_next_part())
        {
            if (tv.part == part && within > 0)
                tv_resume_to((off_t)((int64_t)tv_text_len() * within / 1000));
        }
        else if (!tv_end_before(part))  /* empty to the end: its last text */
            tv_enter_part(0);
    }
    tv_draw();
    if (go)
        tv_speak_page();
}

/* The wheel: scrubbing with Wheel Scrubs on; otherwise it only clicks */
static void tv_wheel(int step)
{
    if (global_settings.text_viewer_wheel_scrub)
        tv_scrub(step);
}

/* What the loop does when no key came: overlays time out, and Continuously
 * reads on */
static void tv_idle(void)
{
    if (tv_pic_pending)
        tv_draw();
    if (tv_scrub_pct >= 0 && TIME_AFTER(current_tick, tv_scrub_until))
        tv_scrub_end(false);
    tv_hold_dark();
    tv_read_on();
}

/* Turns the page sideways or upright, laid out again from its first
 * character. Remembered, so the next book opens the same way. */
static void tv_orient(int orientation)
{
    global_settings.text_viewer_orientation = orientation;
    settings_save();
    tv_apply_settings();
    tv_draw();
}

/* A second click within this is a double-click */
#define TV_DOUBLE_CLICK (HZ * 4 / 10)

/* Upright, |<< and >>| turn the page when let go, so that holding them can
 * move by chapters instead; >>| is told from Select, which ends a scrub by
 * going there. The rest is the standard keymap. */
static const struct button_mapping tv_keys[] =
{
    { ACTION_NONE,            BUTTON_LEFT,                BUTTON_NONE },
    { ACTION_STD_CANCEL,      BUTTON_LEFT|BUTTON_REL,     BUTTON_LEFT },
    { ACTION_LISTTREE_PGUP,   BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_LEFT },
    { ACTION_NONE,            BUTTON_RIGHT,               BUTTON_NONE },
    { ACTION_TREE_PGRIGHT,    BUTTON_RIGHT|BUTTON_REL,    BUTTON_RIGHT },
    { ACTION_LISTTREE_PGDOWN, BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_RIGHT },
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)
};

static const struct button_mapping *tv_keymap(int context)
{
    return context & CONTEXT_REMOTE
           ? get_context_mapping(CONTEXT_STD | CONTEXT_REMOTE) : tv_keys;
}

/* A box over a sideways page reads the wheel's four buttons as they stand
 * on the turned page, each doing what the one in its place does upright:
 * the top one Menu's, the foot PLAY's, the left |<<'s and the right >>|'s.
 * Right hand turns MENU to the left and >>| to the top. Select and the
 * wheel are the standard keymap's. */
#define TV_SIDE_KEYS(top, foot, left, right)                       \
    { ACTION_NONE,       top,                 BUTTON_NONE },       \
    { ACTION_STD_MENU,   (top)|BUTTON_REL,    top },               \
    { ACTION_NONE,       foot,                BUTTON_NONE },       \
    { ACTION_TREE_WPS,   (foot)|BUTTON_REL,   foot },              \
    { ACTION_NONE,       left,                BUTTON_NONE },       \
    { ACTION_STD_CANCEL, (left)|BUTTON_REL,   left },              \
    { ACTION_NONE,       right,               BUTTON_NONE },       \
    { ACTION_STD_OK,     (right)|BUTTON_REL,  right },             \
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)

static const struct button_mapping tv_side_keys_right[] =
{
    TV_SIDE_KEYS(BUTTON_RIGHT, BUTTON_LEFT, BUTTON_MENU, BUTTON_PLAY)
};

static const struct button_mapping tv_side_keys_left[] =
{
    TV_SIDE_KEYS(BUTTON_LEFT, BUTTON_RIGHT, BUTTON_PLAY, BUTTON_MENU)
};

static const struct button_mapping *tv_side_keymap(int context)
{
    if (context & CONTEXT_REMOTE)
        return get_context_mapping(CONTEXT_STD | CONTEXT_REMOTE);
    return global_settings.text_viewer_orientation == TV_ORIENT_RIGHT
           ? tv_side_keys_right : tv_side_keys_left;
}

enum tv_key { TV_KEY_NONE, TV_KEY_MENU, TV_KEY_NOTES, TV_KEY_LEAVE,
              TV_KEY_USB };

static void tv_event_save(void *param);

/* A raw button while sideways. Forward is the button on the
 * right as the player is held, and Select; back is the one on the left. The
 * button now at the top does what MENU does upright: leaves, and opens the
 * settings when held. The one at the bottom swaps hands on a double-click
 * and goes upright when held. Held Select shows the page's notes; held
 * forward and back move by chapters. */
static enum tv_key tv_side_button(long b)
{
    static long held, last_click;
    static long last_tick;
    bool right = global_settings.text_viewer_orientation == TV_ORIENT_RIGHT;
    long fwd = right ? BUTTON_PLAY : BUTTON_MENU;
    long back = right ? BUTTON_MENU : BUTTON_PLAY;
    long top = right ? BUTTON_RIGHT : BUTTON_LEFT;
    long bottom = right ? BUTTON_LEFT : BUTTON_RIGHT;
    long base = b & ~(BUTTON_REPEAT | BUTTON_REL);
    bool twice;

    if (b == BUTTON_NONE)
    {
        tv_idle();
        return TV_KEY_NONE;
    }
    if (b & SYS_EVENT)
        return default_event_handler_ex(b, tv_event_save, NULL)
               == SYS_USB_CONNECTED ? TV_KEY_USB : TV_KEY_NONE;

    if (base == BUTTON_SCROLL_FWD || base == BUTTON_SCROLL_BACK)
    {
        if (!(b & BUTTON_REL))
            tv_wheel(base == BUTTON_SCROLL_FWD ? 1 : -1);
        return TV_KEY_NONE;
    }

    /* A press ends any hold: the box or menu a hold opened takes its
     * release */
    if (!(b & (BUTTON_REPEAT | BUTTON_REL)))
        held = 0;
    if ((b & BUTTON_REPEAT) && !(b & BUTTON_REL))
    {
        if (held == base)
            return TV_KEY_NONE;
        held = base;
        if (base == BUTTON_SELECT)
            return TV_KEY_NOTES;
        if (base == top)
            return TV_KEY_MENU;
        if (base == bottom)
            tv_orient(TV_ORIENT_UPRIGHT);
        else if (tv_scrub_pct < 0 && base == back)
            tv_chapter_start();
        else if (tv_scrub_pct < 0 && base == fwd)
            tv_chapter_next();
        return TV_KEY_NONE;
    }
    if (!(b & BUTTON_REL))
        return TV_KEY_NONE;             /* a press: acted on at its release */
    if (held == base)
    {
        held = 0;                       /* the end of a hold, not a click */
        return TV_KEY_NONE;
    }

    twice = base == last_click && TIME_BEFORE(current_tick,
                                              last_tick + TV_DOUBLE_CLICK);
    last_click = twice ? 0 : base;
    last_tick = current_tick;

    if (tv_scrub_pct >= 0)
    {
        tv_scrub_end(base == BUTTON_SELECT);
        return TV_KEY_NONE;
    }

    if (base == BUTTON_SELECT && tv_aloud())
        tv_pause_toggle();
    else if (base == BUTTON_SELECT || base == fwd)
        tv_next_page();
    else if (base == back)
        tv_prev_page();
    else if (base == top)
        return TV_KEY_LEAVE;
    else if (base == bottom && twice)
        tv_orient(right ? TV_ORIENT_LEFT : TV_ORIENT_RIGHT);
    return TV_KEY_NONE;
}

/* Hold-Menu settings menu, shared with Library > Viewers > Text Viewer.
 * Re-enables the theme just for the menu chrome, then drops back to our own
 * full-screen drawing and re-applies whatever the reader changed. */
static int tv_menu(void)
{
    int sel = 0, ret;

    /* The menu silences the page, and the page is read again from its top
     * once the menu closes, so the menu also pauses Read Aloud */
    talk_force_shutup();
    tv_reading = false;
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    push_current_activity(ACTIVITY_CONTEXTMENU);
    ret = do_menu(&text_viewer_menu, &sel, NULL, false);
    pop_current_activity();
    viewportmanager_theme_undo(SCREEN_MAIN, false);

    if (ret == MENU_ATTACHED_USB)
        return ret;
    /* A pause ends with Read Aloud: turned on again, it reads */
    if (!tv_aloud())
        tv_paused = false;
    tv_apply_settings();
    /* A picture's page, the picture now passed over, has nothing on it */
    if (!tv_shows(tv.pos - tv.win_start,
                  tv_page(tv.pos, false) - tv.win_start))
        tv_advance();
    tv_draw();
    tv_speak_page();
    reading_stats_touch();
    return ret;
}

/* ---- notes ------------------------------------------------------------ */

#define TV_NOTES_MAX  8         /* notes gathered from one page */
#define TV_NOTE_BYTES 1536      /* the box's text, all of them */

static char tv_note_text[TV_NOTE_BYTES];

/* Lays the page at `pos` of `part` out again after the engine has read
 * elsewhere: a document without parts is read again from its start */
static void tv_reopen(int part, off_t pos)
{
    if (tv.nparts)
        tv_enter_part(part);
    else if (ts_rewind(tv.src) == BS_OK)
    {
        tv.win_start = 0;
        tv.win_len = 0;
        tv.eof = false;
        tv.pos = tv.next = 0;
        tv.sp = 0;
        tv_ensure(0, 1);
    }
    tv_resume_to(pos);
}

/* A note's text for the box: what is hidden left out, the white space at
 * its ends trimmed. Returns its length. */
static size_t tv_note_clean(char *s)
{
    size_t n = tv_strip_shy((unsigned char *)s, strlen(s)), i = 0;

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == ' '))
        n--;
    while (i < n && (s[i] == '\n' || s[i] == ' '))
        i++;
    memmove(s, s + i, n - i);
    s[n - i] = '\0';
    return n - i;
}

/* The progress box while notes are found, by the file position: the first
 * half finding them, the second reading back to the page. Only once it has
 * taken a moment, as the picture bar. */
static long tv_note_start;
static int tv_note_half, tv_note_drawn;
static unsigned int tv_note_thread;

static void tv_note_progress(bs_off_t at)
{
    int64_t size = tv.file_size;
    int pct;

    if (thread_self() != tv_note_thread || size <= 0
        || TIME_BEFORE(current_tick, tv_note_start + TV_PIC_BAR_AFTER))
        return;
    pct = (int)(100 * (tv_note_half * size + MIN((int64_t)at, size))
                / (2 * size));
    if (pct == tv_note_drawn)
        return;
    tv_note_drawn = pct;
    if (tv_side)
    {
        struct viewport full;

        tv_fullscreen_vp(&full);
        splash_progress_in(&full, pct, 100, "%s",
                           str(LANG_TEXT_VIEWER_NOTES));
        tv_side_push();
    }
    else
        splash_progress(pct, 100, "%s", str(LANG_TEXT_VIEWER_NOTES));
}

/* Hold Select: the notes the page on screen refers to, in a box. Reading
 * them ends the chapter being read, so the page is laid out again after.
 * MENU_ATTACHED_USB if USB closed the box, when the reader leaves. */
static int tv_notes(void)
{
    static char hrefs[TV_NOTES_MAX][TS_NOTE_HREF];
    const unsigned char *s = tv.win + (tv.pos - tv.win_start);
    size_t n = (size_t)(tv.next - tv.pos), used = 0;
    int nh = 0, part = tv.part;
    off_t pos = tv.pos;

    for (size_t i = 0; i < n && nh < TV_NOTES_MAX; i++)
    {
        size_t m = tv_note_mark(s + i, n - i);

        if (m > 6 && m - 6 < TS_NOTE_HREF)
        {
            memcpy(hrefs[nh], s + i + 3, m - 6);
            hrefs[nh++][m - 6] = '\0';
            i += m - 1;
        }
    }
    if (!nh)
    {
        tv_splashf("%s", str(LANG_TEXT_VIEWER_NO_NOTES));
        sleep(HZ);
        tv_draw();
        return 0;
    }

    talk_force_shutup();
    tv_reading = false;
    tv_splashf("%s\n%s", str(LANG_WAIT), str(LANG_TEXT_VIEWER_NOTES));
    tv_note_start = current_tick;
    tv_note_thread = thread_self();
    tv_note_half = 0;
    tv_note_drawn = -1;
    ts_io_progress = tv_note_progress;
    cpu_boost(true);
    for (int k = 0; k < nh; k++)
    {
        size_t room = sizeof(tv_note_text) - used;

        if (room < 64)
            break;
        if (used)
        {
            memcpy(tv_note_text + used, "\n\n", 2);
            used += 2;
            room -= 2;
        }
        if (ts_note(tv.src, part, hrefs[k], tv_note_text + used, room)
            == BS_OK)
            used += tv_note_clean(tv_note_text + used);
        else if (used >= 2)
            used -= 2;
        tv_note_text[used] = '\0';
    }
    cpu_boost(false);
    tv_note_half = 1;
    tv_reopen(part, pos);
    ts_io_progress = NULL;

    if (used)
    {
        bool usb;

        /* sideways, the box goes over the page in the sideways buffer */
        if (tv_side)
            tv_draw();
        viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
        push_current_activity(ACTIVITY_CONTEXTMENU);
        if (tv_side)
            usb = dialog_prose_show_in(str(LANG_TEXT_VIEWER_NOTES),
                                       tv_note_text, &tv_side_fb,
                                       LCD_HEIGHT, LCD_WIDTH, tv_side_push,
                                       tv_side_keymap);
        else
            usb = dialog_prose_show(str(LANG_TEXT_VIEWER_NOTES),
                                    tv_note_text);
        pop_current_activity();
        viewportmanager_theme_undo(SCREEN_MAIN, false);
        if (usb)
            return MENU_ATTACHED_USB;
        tv_apply_settings();
    }
    tv_draw();
    reading_stats_touch();
    return 0;
}

/* A power-off never returns to the save below, so the event handler saves the
 * position itself before shutting down. */
static void tv_event_save(void *param)
{
    (void)param;
    reading_stats_end(tv_book_percent(), tv_at_book_end());
    tv_resume_save(tv.path, tv.part, tv.pos, tv.file_size);
}

/* The chapter holding `off`, an offset into the whole book's text, which
 * becomes an offset into that chapter's. Reads the book up to it once. */
static int tv_find_part(off_t *off)
{
    for (int part = 0; part < tv.nparts; part++)
    {
        off_t len;

        if (!tv_enter_part(part))
            continue;
        tv.pos = *off;          /* the window keeps its margin behind this */
        tv_ensure(*off, 1);
        len = tv.win_start + (off_t)tv.win_len;
        if (*off < len)
            return part;
        *off -= len;
    }
    *off = 0;
    return 0;
}

/* Opens at the saved place: in a book with parts, the chapter it is in,
 * laid out from that chapter's start */
static void tv_open_at(int part, off_t off)
{
    if (!tv.nparts)
    {
        if (off > 0)
            tv_resume_to(off);
        return;
    }
    if (part == TV_PART_UNKNOWN)
    {
        tv_splash_loading(true);
        part = tv_find_part(&off);
    }
    for (tv_last_text = tv.nparts - 1;
         tv_last_text > 0 && !tv_enter_part(tv_last_text); tv_last_text--)
        ;
    if (part < 0 || part >= tv.nparts)
        part = off = 0;
    if (!tv_enter_part(part) && !tv_next_part())
        tv_enter_part(0);       /* nothing after it has text */
    else if (off > 0 && tv.part == part)
        tv_resume_to(off);
}

/* ---- bookmarks -------------------------------------------------------- */

/* The first words of the page on screen, which a mark is listed by:
 * pictures' and notes' references left out, headings kept */
static void tv_mark_label(char *buf, size_t size)
{
    const unsigned char *p, *end;
    size_t n = 0, cut = 0;
    bool hidden = false, gap = false;

    buf[0] = '\0';
    if (tv.pos < tv.win_start || size < 8)
        return;
    p = (const unsigned char *)tv.win + (tv.pos - tv.win_start);
    end = (const unsigned char *)tv.win + tv.win_len;
    while (p < end && n < 40)
    {
        const unsigned char *at = p;
        ucschar_t c;

        p = utf8decode(p, &c);
        if (c == TS_PICTURE_OPEN || c == TS_NOTE_OPEN)
            hidden = true;
        else if (c == TS_PICTURE_CLOSE || c == TS_NOTE_CLOSE)
            hidden = false;
        if (hidden || (c >= 0xE000 && c <= 0xF8FF))
            continue;
        if (c <= ' ')
        {
            gap = n > 0;
            continue;
        }
        if (gap)
        {
            cut = n;
            buf[n++] = ' ';
            gap = false;
        }
        if (n + (p - at) + 4 >= size)
            break;
        memcpy(buf + n, at, p - at);
        n += p - at;
    }
    if (p < end && cut > 0)
        n = cut;                        /* at a word's end, then ... */
    if (p < end && n + 4 < size)
    {
        memcpy(buf + n, "\xe2\x80\xa6", 3);
        n += 3;
    }
    buf[n] = '\0';
}

bool text_viewer_reading(void)
{
    return tv.src != NULL && !tv_listing;
}

bool text_viewer_has_marks(void)
{
    return text_viewer_reading() && book_marks_any(tv.path);
}

int text_viewer_add_bookmark(void)
{
    char label[BOOK_MARK_LABEL_MAX];

    tv_mark_label(label, sizeof(label));
    splash(HZ, book_marks_add_labelled(book_resume_key(tv.path), tv.path,
                                       tv.part, tv.pos, label, true)
               ? ID2P(LANG_BOOKMARK_CREATE_SUCCESS)
               : ID2P(LANG_BOOKMARK_CREATE_FAILURE));
    return 0;
}

/* The marks of the book at 'path', listed; one picked into *part and *off */
struct tv_marks
{
    int n;
    long part[TV_MARKS_MAX];
    long off[TV_MARKS_MAX];
    char label[TV_MARKS_MAX][BOOK_MARK_LABEL_MAX];
};

static bool tv_mark_one(const struct book_mark *m, void *data)
{
    struct tv_marks *t = data;

    if (t->n == TV_MARKS_MAX)
        return false;
    t->part[t->n] = m->elapsed;
    t->off[t->n] = m->offset;
    strmemccpy(t->label[t->n], m->label, sizeof(t->label[0]));
    t->n++;
    return true;
}

static const char *tv_mark_name(int item, void *data, char *buf, size_t len)
{
    (void)buf; (void)len;
    return ((struct tv_marks *)data)->label[item];
}

/* Hold Select on a mark deletes it, after asking */
static const char *tv_marks_path;
static int tv_marks_action(int action, struct gui_synclist *lists)
{
    struct tv_marks *t = lists->data;
    int sel = gui_synclist_get_sel_pos(lists);

    if (action != ACTION_STD_CONTEXT || sel < 0 || sel >= t->n)
        return action;
    if (yesno_pop(ID2P(LANG_REALLY_DELETE))
        && book_marks_delete(tv_marks_path, tv_marks_path, t->part[sel],
                             t->off[sel]))
    {
        /* Gone from the file: the list closes rather than show it */
        return ACTION_STD_CANCEL;
    }
    return ACTION_REDRAW;
}

/* 1 a mark picked, 0 none, MENU_ATTACHED_USB */
static int tv_pick_mark(const char *path, long *part, long *off)
{
    static struct tv_marks t;
    struct simplelist_info info;

    t.n = 0;
    tv_marks_path = path;
    book_marks_each(path, tv_mark_one, &t);
    if (!t.n)
    {
        splash(HZ, ID2P(LANG_BOOKMARK_LOAD_EMPTY));
        return 0;
    }
    simplelist_info_init(&info, str(LANG_BOOKMARK_MENU), t.n, &t);
    info.get_name = tv_mark_name;
    info.action_callback = tv_marks_action;
    if (simplelist_show_list(&info))
        return MENU_ATTACHED_USB;
    if (info.selection < 0 || info.selection >= t.n)
        return 0;
    *part = t.part[info.selection];
    *off = t.off[info.selection];
    return 1;
}

int text_viewer_bookmarks(void)
{
    long part, off;
    int rc = tv_pick_mark(tv.path, &part, &off);

    if (rc == 1)
        tv_reopen(part, (off_t)off);
    return rc;
}

/* Set by text_viewer_at_mark() for the next text_viewer(): where to open */
static bool tv_mark_pending;
static long tv_mark_part, tv_mark_off;

int text_viewer_at_mark(const char *file)
{
    int rc = tv_pick_mark(file, &tv_mark_part, &tv_mark_off);

    if (rc != 1)
        return rc == MENU_ATTACHED_USB ? GO_TO_ROOT : GO_TO_PREVIOUS;
    tv_mark_pending = true;
    rc = text_viewer(file);
    tv_mark_pending = false;    /* unused when the book did not open */
    return rc;
}

/* Set by text_viewer_chapters() for the next text_viewer() */
static bool tv_open_contents;

int text_viewer_chapters(const char *file)
{
    tv_open_contents = true;
    return text_viewer(file);
}

int text_viewer(const char *file)
{
    int ret = GO_TO_PREVIOUS;
    off_t resume;
    int part;
    long play_tick = current_tick - TV_DOUBLE_CLICK;  /* PLAY's last tap */
    bool play_wait = false;     /* a tap that pauses, unless a second comes */
    bool contents = tv_open_contents;

    /* A place matches on its path, and the file browser puts the volume on
     * the front ("/<HDD0>/...") where the file index and the menus do not. */
    file = path_key_strip(file);
    tv_open_contents = false;
    push_current_activity(ACTIVITY_TEXTVIEWER);
    tv.path = file;
    tv.user_font = -1;           /* no font override loaded yet */
    tv.footer_font = -1;
    tv_scrub_pct = -1;

    /* Take the screen (theme off) and put up a clean loading frame before the
     * slow open -- the name splash for the container formats, a cleared
     * reading area otherwise -- so the browser never shows through it. Opened
     * on its contents, the theme stays until a chapter is picked: the list
     * and the wait for it are the theme's. */
    tv_listing = contents;
    if (contents)
    {
        tv_apply_settings();
        if (tv_slow_open(file))
            tv_splash_loading(false);
    }
    else
    {
        tv_setup_screen();
        tv_show_loading();
    }

    if (!tv_open(file))
    {
        tv_close();
        if (!contents)
            viewportmanager_theme_undo(SCREEN_MAIN, false);
        pop_current_activity();
        return GO_TO_PREVIOUS;
    }

    resume = tv_resume_load(file, tv.file_size, &part);
    if (tv_mark_pending)
    {
        tv_mark_pending = false;
        part = tv_mark_part;
        resume = tv_mark_off;
    }
    /* Before the pages to the saved place are laid out, which see headings */
    tv_heads_seen = false;
    tv_open_at(part, resume);
    tv_paused = tv_dark = false;

    /* Opened on its contents: without a pick it closes unread, its place
     * as it was */
    if (contents)
    {
        int picked = text_viewer_contents();

        tv_listing = false;
        if (picked != 1)
        {
            tv_close();
            pop_current_activity();
            return picked == MENU_ATTACHED_USB ? GO_TO_ROOT : GO_TO_PREVIOUS;
        }
        tv_setup_screen();
    }
    reading_stats_begin(file, tv_book_at());
    tv_open_part = tv.part;
    tv_open_pos = tv.pos;

    tv_draw();
    tv_speak_page();
    tv_book_done = false;

    while (1)
    {
        bool continuous = tv_reading
            && global_settings.text_viewer_read_aloud == TV_READ_CONTINUOUS;
        int timeout = continuous || tv_scrub_pct >= 0
                      || tv_pic_pending || play_wait ? HZ/10 : TIMEOUT_BLOCK;
        int action;

        if (tv_book_done)
            goto done;
        if (tv_side)
        {
            long b = timeout == TIMEOUT_BLOCK ? button_get(true)
                                              : button_get_w_tmo(timeout);

            /* Read past the action system, which clicks for the rest */
            keyclick_click(true, b);

            switch (tv_side_button(b))
            {
                case TV_KEY_LEAVE:
                    goto done;
                case TV_KEY_USB:
                    ret = GO_TO_ROOT;
                    goto done;
                case TV_KEY_MENU:
                    if (tv_menu() == MENU_ATTACHED_USB)
                    {
                        ret = GO_TO_ROOT;
                        goto done;
                    }
                    break;
                case TV_KEY_NOTES:
                    if (tv_notes() == MENU_ATTACHED_USB)
                    {
                        ret = GO_TO_ROOT;
                        goto done;
                    }
                    break;
                default:
                    break;
            }
            continue;
        }

        action = get_custom_action(CONTEXT_PLUGIN, timeout, tv_keymap);

        /* A scrub takes every button but the wheel, Select to jump */
        if (tv_scrub_pct >= 0 && action != ACTION_NONE
            && !(action & SYS_EVENT)
            && action != ACTION_UNKNOWN && action != ACTION_REDRAW
            && action != ACTION_STD_NEXT && action != ACTION_STD_NEXTREPEAT
            && action != ACTION_STD_PREV && action != ACTION_STD_PREVREPEAT)
        {
            tv_scrub_end(action == ACTION_STD_OK);
            continue;
        }

        switch (action)
        {
            case ACTION_NONE:
                tv_idle();
                if (play_wait && TIME_AFTER(current_tick,
                                            play_tick + TV_DOUBLE_CLICK))
                {
                    play_wait = false;
                    tv_pause_toggle();
                }
                break;

            case ACTION_STD_NEXT:
            case ACTION_STD_NEXTREPEAT:
                tv_wheel(1);
                break;

            case ACTION_STD_PREV:
            case ACTION_STD_PREVREPEAT:
                tv_wheel(-1);
                break;

            /* PLAY twice goes sideways, right hand; once, with Read Aloud
             * on, pauses or goes on, when no second tap has come */
            case ACTION_TREE_WPS:
                if (TIME_BEFORE(current_tick, play_tick + TV_DOUBLE_CLICK))
                {
                    play_tick = current_tick - TV_DOUBLE_CLICK;
                    play_wait = false;
                    tv_orient(TV_ORIENT_RIGHT);
                }
                else
                {
                    play_tick = current_tick;
                    play_wait = tv_aloud();
                }
                break;

            case ACTION_STD_OK:          /* Select: pauses Read Aloud */
                if (tv_aloud())
                    tv_pause_toggle();
                else
                    tv_next_page();
                break;

            case ACTION_TREE_PGRIGHT:    /* forward button (>>|) */
                tv_next_page();
                break;

            case ACTION_STD_CANCEL:      /* back button (|<<) */
                tv_prev_page();
                break;

            case ACTION_LISTTREE_PGUP:   /* |<< held */
                tv_chapter_start();
                break;

            case ACTION_LISTTREE_PGDOWN: /* >>| held */
                tv_chapter_next();
                break;

            case ACTION_STD_MENU:        /* Menu leaves the viewer */
                goto done;

            case ACTION_STD_CONTEXT:     /* hold Select: the page's notes */
                if (tv_notes() == MENU_ATTACHED_USB)
                {
                    ret = GO_TO_ROOT;
                    goto done;
                }
                action_wait_for_release();
                break;

            case ACTION_STD_QUICKSCREEN: /* hold Menu opens the settings */
                if (tv_menu() == MENU_ATTACHED_USB)
                {
                    ret = GO_TO_ROOT;
                    goto done;
                }
                break;

            default:
                /* The framework handles USB and other system events */
                if (default_event_handler_ex(action, tv_event_save, NULL)
                    == SYS_USB_CONNECTED)
                {
                    ret = GO_TO_ROOT;
                    goto done;
                }
                break;
        }
    }

  done:
    talk_shutup();             /* the page stops with the viewer */
    if (tv_dark)
        backlight_on();
    reading_stats_end(tv_book_percent(), tv_at_book_end());
    tv_resume_save(file, tv.part, tv.pos, tv.file_size);
    tv_close();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    pop_current_activity();
    return ret;
}
