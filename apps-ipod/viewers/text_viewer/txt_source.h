/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Public API of the ts_* text-extraction engine: open a document, read
 * UTF-8 out of it, close it. The only header a caller needs.
 ****************************************************************************/
#ifndef TXT_SOURCE_H
#define TXT_SOURCE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long long ts_off_t;   /* map to off_t on Rockbox targets */

/* ---- errors ---------------------------------------------------------- */

enum {
    TS_OK          =  0,
    TS_ERR_IO      = -1,   /* read/seek failed                            */
    TS_ERR_NOMEM   = -2,   /* arena too small                             */
    TS_ERR_FORMAT  = -3,   /* file is corrupt or not what it claimed      */
    TS_ERR_UNSUP   = -4,   /* recognised, but this variant isn't handled  */
    TS_ERR_INVAL   = -5    /* bad argument                                */
};

const char *ts_strerror(int err);

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
    TS_FMT_PDF
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

/* ---- I/O vtable ------------------------------------------------------ */

/* Rockbox: wrap rb->read / rb->lseek / rb->close.
 * Host: use ts_io_stdio() from ts_io_stdio.c. */
typedef struct ts_io {
    long      (*read )(void *ctx, void *buf, size_t n);      /* <0 on error  */
    ts_off_t  (*seek )(void *ctx, ts_off_t off, int whence); /* SEEK_* codes */
    ts_off_t  (*size )(void *ctx);
    void      (*close)(void *ctx);
    void       *ctx;
} ts_io;

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
 * for deflate's mandatory 32 KiB history window.
 *
 *   plain / markdown / html / rtf / fb2      8.5 KiB
 *   pdf                                       57 KiB
 *   docx                                      50 KiB
 *   epub, 3 chapters                          50 KiB
 *   epub, 200 chapters                        65 KiB
 *   epub, 500 chapters                        96 KiB
 *
 * Only the zip formats scale with the document, at roughly 100 bytes per
 * archive member (the central directory index and the manifest ids). If the
 * arena is too small ts_open returns TS_ERR_NOMEM before reading anything,
 * so a caller with memory to spare can simply retry with a larger one. */
#define TS_ARENA_PLAIN        (12 * 1024)   /* also markdown, html, rtf, fb2 */
#define TS_ARENA_MARKUP       (12 * 1024)
#define TS_ARENA_PDF          (64 * 1024)
#define TS_ARENA_ZIP          (96 * 1024)   /* EPUB / DOCX, up to ~500 parts */
#define TS_ARENA_RECOMMENDED  (96 * 1024)   /* safe for every format         */

/* ---- lifecycle ------------------------------------------------------- */

typedef struct ts_source ts_source;

/* `io` is copied; the engine takes ownership of io->ctx and will call
 * io->close on ts_close(). `name` is used only for extension-based probing
 * and may be NULL. `arena` must stay valid and untouched until ts_close(). */
int  ts_open(ts_source **out, const ts_io *io, const char *name,
             void *arena, size_t arena_size, const ts_config *cfg);

/* Returns bytes written (>0), 0 at end of document, or a negative TS_ERR_*.
 * Never splits a UTF-8 sequence across calls. `n` must be >= 8. */
long ts_read(ts_source *s, char *buf, size_t n);

/* Restart from the beginning. Cheap for every format. */
int  ts_rewind(ts_source *s);

void ts_close(ts_source *s);

ts_format ts_source_format(const ts_source *s);
ts_charset ts_source_charset(const ts_source *s);   /* as detected */
size_t     ts_arena_used(const ts_source *s);       /* for tuning */

#ifdef __cplusplus
}
#endif
#endif /* TXT_SOURCE_H */
