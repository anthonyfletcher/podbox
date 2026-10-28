/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Shared internals of the ts_* engine -- the stages and back ends, and
 * their restart hooks. Not part of the public API. The arena, the stream
 * and inflate are in files/byte_stream.h.
 ****************************************************************************/
#ifndef TS_INTERNAL_H
#define TS_INTERNAL_H

#include "txt_source.h"
#include <stdint.h>
#include <string.h>

/* ---- charset stage --------------------------------------------------- */

bs_stream *ts_charset_stream(bs_arena *a, bs_stream *src, ts_charset cs);
void ts_charset_restart(bs_stream *s, bs_stream *src, ts_charset cs);
/* Sniffs `head` (typically 4 KiB from offset 0). Sets *bom_skip to the number
 * of BOM bytes to drop. Never returns TS_CS_AUTO. */
ts_charset ts_charset_sniff(const uint8_t *head, size_t n,
                            ts_charset fallback, size_t *bom_skip);
uint32_t   ts_sb_to_cp(ts_charset cs, uint8_t b);
ts_charset ts_charset_from_name(const char *name);
ts_charset ts_charset_decl(const uint8_t *head, size_t n, ts_charset current);

/* ---- pdf ascii armour ------------------------------------------------ */

#define TS_FILT_RAW 0
#define TS_FILT_A85 1
#define TS_FILT_AHX 2
bs_stream *ts_filt_stream(bs_arena *a, bs_stream *src);
void       ts_filt_restart(bs_stream *s, int mode);
bs_off_t   ts_filt_used(bs_stream *s);     /* source bytes consumed */

/* ---- pdf fonts ------------------------------------------------------- */

/* Subset fonts assign glyph codes arbitrarily, so their /ToUnicode CMap is
 * the only way to recover characters. This layer scans the file for the font
 * dictionaries, their CMaps and the page resource maps that name them. */
typedef struct ts_pdffonts ts_pdffonts;

int ts_pdffonts_build(ts_pdffonts **out, bs_arena *a, const bs_io *io,
                      bs_stream *win, bs_stream *filt, bs_stream *infl);
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

int  ts_zip_open(ts_zip **out, bs_arena *a, const bs_io *io);
/* Case-sensitive exact match on the stored path. */
int  ts_zip_find(ts_zip *z, const char *path);      /* index, or BS_ERR_FORMAT */
const char *ts_zip_name(ts_zip *z, int idx);
int  ts_zip_count(ts_zip *z);
/* Opens member `idx` as a stream. The returned stream is owned by `z` and is
 * invalidated by the next ts_zip_member() call. */
int  ts_zip_member(ts_zip *z, int idx, bs_stream **out);

/* ---- markup stage ---------------------------------------------------- */

typedef enum {
    TS_TAGS_HTML = 0,   /* HTML / XHTML */
    TS_TAGS_FB2,
    TS_TAGS_DOCX
} ts_tag_profile;

bs_stream *ts_tags_stream(bs_arena *a, bs_stream *src,
                          ts_tag_profile p, const ts_config *cfg);
/* Re-point the filter at the next document of a spine, keeping the arena. */
void ts_tags_restart(bs_stream *s, bs_stream *src);
void ts_tags_rewind(bs_stream *s);      /* forget the document-boundary state */

/* ---- format back ends ------------------------------------------------ */

/* Each returns the head of a finished pipeline emitting UTF-8. `head`/`hlen`
 * is the probe buffer already read from offset 0. */
typedef struct {
    bs_arena       *arena;
    const bs_io    *io;
    const ts_config *cfg;
    const uint8_t  *head;
    size_t          hlen;
    ts_charset      detected;
    ts_format       resolved;   /* set by container back ends (epub/docx/fb2z) */
    bs_stream      *parts;      /* the spine stream, for a zip back end */
} ts_ctx;

int ts_open_text (ts_ctx *c, ts_format fmt, bs_stream **out);  /* plain/md/rtf */
int ts_open_html (ts_ctx *c, bs_stream **out);
int ts_open_fb2  (ts_ctx *c, bs_stream **out);
int ts_open_epub (ts_ctx *c, bs_stream **out);
int ts_open_docx (ts_ctx *c, bs_stream **out);
/* On the stream ts_open_epub() returns: read spine item `part` alone, from its
 * start, until another seek; and how many there are. */
int ts_zipdoc_seek_part(bs_stream *st, int part);
int ts_zipdoc_parts(const bs_stream *st);
int ts_open_pdf  (ts_ctx *c, bs_stream **out);
int ts_open_lyrics(ts_ctx *c, bs_stream **out);

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
