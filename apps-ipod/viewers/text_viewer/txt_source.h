/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Public API of the ts_* text-extraction engine: open a document, read
 * UTF-8 out of it, close it. The only header a caller needs.
 ****************************************************************************/
#ifndef TXT_SOURCE_H
#define TXT_SOURCE_H

#include <stddef.h>
#include "files/byte_stream.h"   /* bs_off_t, BS_OK and the errors, bs_io */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- errors ---------------------------------------------------------- */

const char *ts_strerror(int err);

/* Beside the BS_ERR_* codes: a book locked by DRM */
#define TS_ERR_LOCKED (-16)

/* ---- formats --------------------------------------------------------- */

typedef enum {
    TS_FMT_UNKNOWN = 0,
    TS_FMT_PLAIN,
    TS_FMT_MARKDOWN,
    TS_FMT_HTML,
    TS_FMT_RTF,
    TS_FMT_FB2,
    TS_FMT_EPUB,
    TS_FMT_DOCX,
    TS_FMT_PDF,
    TS_FMT_LYRICS,       /* .lrc / .lrc8 / .snc, read without the timing */
    TS_FMT_MOBI          /* .mobi / .prc / .azw / .azw3, and PalmDOC .pdb */
} ts_format;

const char *ts_format_name(ts_format f);

/* ---- character sets -------------------------------------------------- */

typedef enum {
    TS_CS_AUTO = 0,      /* BOM sniff, then UTF-8 validation, then fallback */
    TS_CS_UTF8,
    TS_CS_UTF16LE,
    TS_CS_UTF16BE,
    TS_CS_CP1252,        /* superset of ISO-8859-1 */
    TS_CS_ISO8859_1,
    TS_CS_ISO8859_2,
    TS_CS_ISO8859_5,
    TS_CS_ISO8859_7,
    TS_CS_ISO8859_9,
    TS_CS_CP1251
} ts_charset;

/* ---- configuration --------------------------------------------------- */

typedef struct {
    ts_format  force_format;   /* TS_FMT_UNKNOWN = probe                    */
    ts_charset charset;        /* TS_CS_AUTO = sniff                        */
    ts_charset fallback;       /* used when sniffing fails (dflt CP1252)    */
    int  collapse_whitespace;  /* markup: fold runs of space/newline (1)    */
    int  keep_soft_breaks;     /* plain text: keep single \n as-is (1)      */
    int  max_blank_lines;      /* cap consecutive newlines emitted (dflt 2) */
} ts_config;

ts_config ts_config_default(void);

/* ---- arena sizing ----------------------------------------------------
 *
 * Measured floors, not guesses (tests/run_tests.sh binary-searches them).
 * Every format pays for a 4 KiB probe buffer; anything compressed also pays
 * for deflate's mandatory 32 KiB history window and its 4 KiB of input.
 *
 *   plain / markdown / html / rtf / fb2      8.5 KiB
 *   pdf                                       61 KiB
 *   docx                                      54 KiB
 *   epub, 3 chapters                          54 KiB
 *   epub, 200 chapters                        69 KiB
 *   epub, 500 chapters                       100 KiB
 *
 * Only the zip formats scale with the document, at roughly 100 bytes per
 * archive member (the central directory index and the manifest ids). If the
 * arena is too small ts_open returns BS_ERR_NOMEM before reading anything,
 * so a caller with memory to spare can simply retry with a larger one. */
#define TS_ARENA_PLAIN        (12 * 1024)   /* also markdown, html, rtf, fb2 */
#define TS_ARENA_MARKUP       (12 * 1024)
#define TS_ARENA_PDF          (64 * 1024)
#define TS_ARENA_ZIP          (100 * 1024)  /* EPUB / DOCX, up to ~500 parts */
#define TS_ARENA_RECOMMENDED  (100 * 1024)  /* safe for every format         */

/* ---- lifecycle ------------------------------------------------------- */

typedef struct ts_source ts_source;

/* `io` is copied; the engine takes ownership of io->ctx and will call
 * io->close on ts_close(). `name` is used only for extension-based probing
 * and may be NULL. `arena` must stay valid and untouched until ts_close(). */
int  ts_open(ts_source **out, const bs_io *io, const char *name,
             void *arena, size_t arena_size, const ts_config *cfg);

/* Returns bytes written (>0), 0 at end of document, or a negative BS_ERR_*.
 * Never splits a UTF-8 sequence across calls. `n` must be >= 8. */
long ts_read(ts_source *s, char *buf, size_t n);

/* Restart from the beginning, or from the start of the part being read after
 * ts_seek_part(). Cheap for every format. */
int  ts_rewind(ts_source *s);

/* Parts: an EPUB's spine items, each its own chapter file, or a MOBI's
 * stretches between page breaks. 0 for a format
 * without them. ts_seek_part() makes ts_read() return part `part` alone,
 * from its first byte to its end; BS_ERR_UNSUP without parts. */
int  ts_part_count(const ts_source *s);
int  ts_seek_part(ts_source *s, int part);
/* A part's size as stored, its markup and all: known from the start, so a
 * guide to how much of the book it is, not to its text. 0 without parts. */
long ts_part_size(const ts_source *s, int part);

/* The book's own table of contents: cb() gets each entry's title, the part
 * it starts in and the id it points at inside that part ("" for the part's
 * start), in book order, and stops the walk by returning non-zero.
 * BS_ERR_UNSUP when the book has none. Reading the contents ends the part
 * being read, so seek a part before reading on. */
typedef int (*ts_toc_fn)(void *ud, const char *title, int part,
                         const char *anchor);
int  ts_toc(ts_source *s, ts_toc_fn cb, void *ud);

/* Where in `part`'s text the element with id `anchor` starts, in *off,
 * read up to it. BS_ERR_FORMAT when the part has no such id. Like the
 * contents, it ends the part being read. */
int  ts_anchor(ts_source *s, int part, const char *anchor, long *off);

void ts_close(ts_source *s);

/* ---- pictures -------------------------------------------------------- */

/* An EPUB's pictures reach the text as a line of their own: TS_PICTURE_OPEN,
 * the picture's href as the chapter wrote it, TS_PICTURE_CLOSE. Both are
 * private-use codepoints, three bytes each in UTF-8. */
#define TS_PICTURE_OPEN  0xE000
#define TS_PICTURE_CLOSE 0xE001
/* A heading's text -- HTML's h1-h6, FB2's title and subtitle -- lies between
 * TS_HEADING_OPEN, just before its first character, and TS_HEADING_CLOSE,
 * just after its last */
#define TS_HEADING_OPEN  0xE002
#define TS_HEADING_CLOSE 0xE003
/* Just after a note reference's text, which is bracketed when it was not
 * already: TS_NOTE_OPEN, the note's href as the text wrote it, at most
 * TS_NOTE_HREF - 1 bytes, and TS_NOTE_CLOSE. EPUB, FB2 and HTML. */
#define TS_NOTE_OPEN     0xE004
#define TS_NOTE_CLOSE    0xE005
#define TS_NOTE_HREF     96

/* The text of the note `href` refers to from chapter `part`, into buf as a
 * string: the element that has the id, or the paragraph an inline one is
 * in. A note left out of the flow is read here all the same. Like the
 * contents, it ends the part being read; a document without parts is
 * rewound. BS_ERR_FORMAT when there is no such note. */
int ts_note(ts_source *s, int part, const char *href, char *buf, size_t size);

/* Where the picture `href`, written in chapter `part`, lies in the file:
 * the offset of its data, its stored and unpacked sizes, and its method
 * (0 stored, 8 deflated). BS_OK, or an error when there is no such member. */
int ts_picture_extent(ts_source *s, int part, const char *href, long *at,
                      uint32_t *csize, uint32_t *usize, int *method);

/* ---- a book's details ------------------------------------------------ */

#define TS_BOOK_TITLE    128
#define TS_BOOK_AUTHOR   96
#define TS_BOOK_SUBJECTS 192
#define TS_BOOK_SUBJECT_MAX 4

/* What the book index keeps of an EPUB, FB2 or MOBI: its title and first
 * author, empty where it names none, its parts (1 for an FB2), and where its
 * cover image lies in the file -- cover_at is -1 when it has none. An EPUB's
 * cover is found as reading.md 6 orders it, the first three ways (the cover
 * page's first image is not looked for); an FB2's is its coverpage's; a
 * MOBI's is the image EXTH 201 names. Its subjects, up to
 * TS_BOOK_SUBJECT_MAX of them, are one to a line: an EPUB's dc:subject, an
 * FB2's genre, a MOBI's EXTH 105. */
typedef struct {
    char     title[TS_BOOK_TITLE];
    char     author[TS_BOOK_AUTHOR];
    char     subjects[TS_BOOK_SUBJECTS];
    int      parts;
    long     cover_at;
    uint32_t cover_csize, cover_usize;
    int      cover_method;          /* MEMBER_*, files/zip_member.h */
} ts_book;

/* Reads `io`'s details into `out`, with `arena` for working memory
 * (TS_ARENA_ZIP). BS_OK, or an error for a file that is not an EPUB. Does
 * not close `io`. */
int ts_book_read(const bs_io *io, void *arena, size_t arena_size,
                 ts_book *out);
/* The same for an FB2, which needs no arena. BS_OK, or an error for one that
 * cannot be read (a UTF-16 FB2 among them). */
int ts_fb2_book(const bs_io *io, ts_book *out);
/* The same for a MOBI, which reads all its text to count the parts; the
 * arena as for ts_book_read(). TS_ERR_LOCKED for a locked one. */
int ts_mobi_book(const bs_io *io, void *arena, size_t arena_size,
                 ts_book *out);

ts_format ts_source_format(const ts_source *s);
ts_charset ts_source_charset(const ts_source *s);   /* as detected */
size_t     ts_arena_used(const ts_source *s);       /* for tuning */

#ifdef __cplusplus
}
#endif
#endif /* TXT_SOURCE_H */
