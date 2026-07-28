/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Shared internals of the ts_* engine -- stream types and per-stage
 * restart hooks. Not part of the public API.
 ****************************************************************************/
#ifndef TS_INTERNAL_H
#define TS_INTERNAL_H

#include "txt_source.h"
#include <stdint.h>
#include <string.h>

/* ---- bump arena ------------------------------------------------------ */

typedef struct {
    uint8_t *base;
    size_t   size, used;
} ts_arena;

void *ts_alloc(ts_arena *a, size_t n);          /* pointer-aligned, zeroed */

/* ---- byte stream ----------------------------------------------------- */

/* pull() fills up to n bytes. *out == 0 means clean EOF. Returns TS_OK or a
 * negative error. Streams are chained; each stage owns bounded state in the
 * arena, so a whole pipeline costs a fixed number of kilobytes regardless of
 * how large the document is. */
typedef struct ts_stream {
    int (*pull)(struct ts_stream *s, uint8_t *buf, size_t n, size_t *out);
    int (*reset)(struct ts_stream *s);          /* may be NULL */
    void *st;
} ts_stream;

static inline int ts_pull(ts_stream *s, uint8_t *b, size_t n, size_t *o)
{
    *o = 0;
    return s->pull(s, b, n, o);
}

/* ---- file-backed stream --------------------------------------------- */

/* Reads [start, start+len) of the file. len < 0 means "to end of file". */
ts_stream *ts_file_stream(ts_arena *a, const ts_io *io,
                          ts_off_t start, ts_off_t len);
int ts_file_stream_retarget(ts_stream *s, ts_off_t start, ts_off_t len);

/* ---- charset stage --------------------------------------------------- */

ts_stream *ts_charset_stream(ts_arena *a, ts_stream *src, ts_charset cs);
void ts_charset_restart(ts_stream *s, ts_stream *src, ts_charset cs);
/* Sniffs `head` (typically 4 KiB from offset 0). Sets *bom_skip to the number
 * of BOM bytes to drop. Never returns TS_CS_AUTO. */
ts_charset ts_charset_sniff(const uint8_t *head, size_t n,
                            ts_charset fallback, size_t *bom_skip);
uint32_t   ts_sb_to_cp(ts_charset cs, uint8_t b);
ts_charset ts_charset_from_name(const char *name);
ts_charset ts_charset_decl(const uint8_t *head, size_t n, ts_charset current);

/* ---- inflate stage --------------------------------------------------- */

#define TS_INFLATE_RAW  0
#define TS_INFLATE_ZLIB 1
ts_stream *ts_inflate_stream(ts_arena *a, ts_stream *src, int wrapper);
int  ts_inflate_finished(const ts_stream *s);
void ts_inflate_restart(ts_stream *s, int wrapper);   /* reuse, no realloc */
/* Bytes of the source consumed so far -- lets the PDF scanner resume after a
 * stream whose /Length was an indirect reference. */
ts_off_t ts_inflate_src_used(const ts_stream *s);

/* ---- pdf ascii armour ------------------------------------------------ */

#define TS_FILT_RAW 0
#define TS_FILT_A85 1
#define TS_FILT_AHX 2
ts_stream *ts_filt_stream(ts_arena *a, ts_stream *src);
void       ts_filt_restart(ts_stream *s, int mode);
ts_off_t   ts_filt_used(ts_stream *s);     /* source bytes consumed */

/* ---- pdf fonts ------------------------------------------------------- */

/* Subset fonts assign glyph codes arbitrarily, so their /ToUnicode CMap is
 * the only way to recover characters. This layer scans the file for the font
 * dictionaries, their CMaps and the page resource maps that name them. */
typedef struct ts_pdffonts ts_pdffonts;

int ts_pdffonts_build(ts_pdffonts **out, ts_arena *a, const ts_io *io,
                      ts_stream *win, ts_stream *filt, ts_stream *infl);
/* NULL when the content stream's page or font resource isn't known. */
const void *ts_pdffonts_cmap(ts_pdffonts *f, int content_obj, const char *name);
/* 1 when this object is a page's content stream (or when no page structure
 * was found, in which case every stream is a candidate). */
int ts_pdffonts_is_content(ts_pdffonts *f, int obj);
int ts_cmap_bytes(const void *cmap);                       /* 1 or 2 */
/* 0 = code not mapped (the font's CMap is incomplete). */
int ts_cmap_map(const void *cmap, uint32_t code, uint32_t *u0, uint32_t *u1);

/* ---- zip container --------------------------------------------------- */

typedef struct ts_zip ts_zip;

int  ts_zip_open(ts_zip **out, ts_arena *a, const ts_io *io);
/* Case-sensitive exact match on the stored path. */
int  ts_zip_find(ts_zip *z, const char *path);      /* index, or TS_ERR_FORMAT */
const char *ts_zip_name(ts_zip *z, int idx);
int  ts_zip_count(ts_zip *z);
/* Opens member `idx` as a stream. The returned stream is owned by `z` and is
 * invalidated by the next ts_zip_member() call. */
int  ts_zip_member(ts_zip *z, int idx, ts_stream **out);

/* ---- markup stage ---------------------------------------------------- */

typedef enum {
    TS_TAGS_HTML = 0,   /* HTML / XHTML */
    TS_TAGS_FB2,
    TS_TAGS_DOCX
} ts_tag_profile;

ts_stream *ts_tags_stream(ts_arena *a, ts_stream *src,
                          ts_tag_profile p, const ts_config *cfg);
/* Re-point the filter at the next document of a spine, keeping the arena. */
void ts_tags_restart(ts_stream *s, ts_stream *src);
void ts_tags_rewind(ts_stream *s);      /* forget the document-boundary state */

/* ---- format back ends ------------------------------------------------ */

/* Each returns the head of a finished pipeline emitting UTF-8. `head`/`hlen`
 * is the probe buffer already read from offset 0. */
typedef struct {
    ts_arena       *arena;
    const ts_io    *io;
    const ts_config *cfg;
    const uint8_t  *head;
    size_t          hlen;
    ts_charset      detected;
    ts_format       resolved;   /* set by container back ends (epub/docx/fb2z) */
} ts_ctx;

int ts_open_text (ts_ctx *c, ts_format fmt, ts_stream **out);  /* plain/md/rtf */
int ts_open_html (ts_ctx *c, ts_stream **out);
int ts_open_fb2  (ts_ctx *c, ts_stream **out);
int ts_open_epub (ts_ctx *c, ts_stream **out);
int ts_open_docx (ts_ctx *c, ts_stream **out);
int ts_open_pdf  (ts_ctx *c, ts_stream **out);

/* ---- small helpers --------------------------------------------------- */

size_t ts_utf8_put(uint8_t *dst, uint32_t cp);      /* 1..4, 0 if invalid */
int    ts_ascii_casecmp(const char *a, const char *b);
int    ts_ascii_ncasecmp(const char *a, const char *b, size_t n);
/* Decodes &name; / &#nn; / &#xnn; at `s` (pointing just past '&').
 * Returns bytes consumed including ';', or 0 if not an entity. */
size_t ts_entity(const char *s, size_t n, uint32_t *cp);

static inline uint16_t ts_le16(const uint8_t *p)
{ return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t ts_le32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Emitter: shared pending-output ring used by every filter stage. A filter
 * consumes input only while the ring has headroom, which is what keeps the
 * whole engine streaming without unbounded buffers. */
#define TS_PEND 512
typedef struct {
    uint8_t buf[TS_PEND];
    size_t  head, tail;
} ts_pend;

static inline size_t ts_pend_len(const ts_pend *p)
{ return p->tail - p->head; }
static inline size_t ts_pend_free(const ts_pend *p)
{ return TS_PEND - ts_pend_len(p); }
static inline void ts_pend_reset(ts_pend *p)
{ p->head = p->tail = 0; }

void   ts_emit(ts_pend *p, const uint8_t *b, size_t n);
void   ts_emit_cp(ts_pend *p, uint32_t cp);
size_t ts_pend_take(ts_pend *p, uint8_t *dst, size_t n);

#endif /* TS_INTERNAL_H */
