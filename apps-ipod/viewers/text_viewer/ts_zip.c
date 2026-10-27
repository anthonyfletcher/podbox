/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Just enough ZIP to walk an EPUB or DOCX container: central directory,
 * entry lookup, and stream-per-entry.
 ****************************************************************************/

#include "ts_internal.h"

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_END 2
#endif

#define EOCD_SIG 0x06054b50u
#define CEN_SIG  0x02014b50u
#define LOC_SIG  0x04034b50u
#define SCAN_MAX 66000          /* 64 KiB comment + EOCD record */
#define CHUNK    4096

typedef struct {
    uint32_t lho, csize, usize;
    uint16_t method;
    char    *name;
} zent;

struct ts_zip {
    bs_arena    *a;
    const bs_io *io;
    int          count;
    zent        *ent;
    bs_stream   *raw;      /* file window over the member's compressed bytes */
    bs_stream   *infl;     /* reusable inflate stage                         */
};

static int read_at(const bs_io *io, bs_off_t off, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t done = 0;

    if (io->seek(io->ctx, off, SEEK_SET) < 0) return BS_ERR_IO;
    while (done < n) {
        long got = io->read(io->ctx, p + done, n - done);
        if (got < 0) return BS_ERR_IO;
        if (!got) return BS_ERR_FORMAT;     /* short file */
        done += (size_t)got;
    }
    return BS_OK;
}

/* Walks backwards from EOF looking for the end-of-central-directory record. */
static int find_eocd(const bs_io *io, bs_off_t size, uint8_t *scratch,
                     bs_off_t *eocd)
{
    bs_off_t limit = size < SCAN_MAX ? size : SCAN_MAX;
    bs_off_t pos = size;

    while (pos > size - limit) {
        bs_off_t chunk = pos - (size - limit);
        bs_off_t start;
        size_t n, i;

        if (chunk > CHUNK) chunk = CHUNK;
        start = pos - chunk;
        n = (size_t)chunk;
        if (read_at(io, start, scratch, n) != BS_OK) return BS_ERR_IO;

        for (i = n; i >= 4; i--) {
            if (ts_le32(scratch + i - 4) == EOCD_SIG) {
                *eocd = start + (bs_off_t)i - 4;
                return BS_OK;
            }
        }
        if (start == size - limit) break;
        pos = start + 3;                    /* overlap for a split signature */
    }
    return BS_ERR_FORMAT;
}

int ts_zip_open(ts_zip **out, bs_arena *a, const bs_io *io)
{
    uint8_t hdr[46];
    uint8_t *scratch;
    bs_off_t size, eocd, cd;
    ts_zip *z;
    int i, rc;
    uint32_t cd_off;
    uint16_t total;

    *out = NULL;
    size = io->size(io->ctx);
    if (size < 22) return BS_ERR_FORMAT;

    z = bs_alloc(a, sizeof *z);
    scratch = bs_alloc(a, CHUNK);
    if (!z || !scratch) return BS_ERR_NOMEM;
    z->a = a; z->io = io;

    rc = find_eocd(io, size, scratch, &eocd);
    if (rc != BS_OK) return rc;

    rc = read_at(io, eocd, scratch, 22);
    if (rc != BS_OK) return rc;
    total  = ts_le16(scratch + 10);
    cd_off = ts_le32(scratch + 16);
    if (total == 0xFFFF || cd_off == 0xFFFFFFFFu) return BS_ERR_UNSUP; /* zip64 */
    if (!total) return BS_ERR_FORMAT;

    /* The scratch buffer is only needed for the EOCD hunt. */
    a->used -= CHUNK;

    z->count = total;
    z->ent = bs_alloc(a, sizeof(zent) * total);
    if (!z->ent) return BS_ERR_NOMEM;

    cd = cd_off;
    for (i = 0; i < total; i++) {
        uint16_t nlen, elen, clen;
        zent *e = &z->ent[i];

        rc = read_at(io, cd, hdr, sizeof hdr);
        if (rc != BS_OK) return rc;
        if (ts_le32(hdr) != CEN_SIG) return BS_ERR_FORMAT;

        e->method = ts_le16(hdr + 10);
        e->csize  = ts_le32(hdr + 20);
        e->usize  = ts_le32(hdr + 24);
        e->lho    = ts_le32(hdr + 42);
        nlen = ts_le16(hdr + 28);
        elen = ts_le16(hdr + 30);
        clen = ts_le16(hdr + 32);
        if (e->lho == 0xFFFFFFFFu || e->csize == 0xFFFFFFFFu) return BS_ERR_UNSUP;

        e->name = bs_alloc(a, (size_t)nlen + 1);
        if (!e->name) return BS_ERR_NOMEM;
        rc = read_at(io, cd + sizeof hdr, e->name, nlen);
        if (rc != BS_OK) return rc;
        e->name[nlen] = 0;

        cd += (bs_off_t)sizeof hdr + nlen + elen + clen;
    }

    z->raw = bs_file_stream(a, io, 0, 0);
    if (!z->raw) return BS_ERR_NOMEM;
    z->infl = bs_inflate_stream(a, z->raw, BS_INFLATE_RAW);
    if (!z->infl) return BS_ERR_NOMEM;

    *out = z;
    return BS_OK;
}

int ts_zip_count(ts_zip *z) { return z->count; }

const char *ts_zip_name(ts_zip *z, int i)
{
    return (i >= 0 && i < z->count) ? z->ent[i].name : NULL;
}

int ts_zip_find(ts_zip *z, const char *path)
{
    int i;
    for (i = 0; i < z->count; i++)
        if (strcmp(z->ent[i].name, path) == 0) return i;
    return BS_ERR_FORMAT;
}

int ts_zip_member(ts_zip *z, int idx, bs_stream **out)
{
    uint8_t loc[30];
    zent *e;
    bs_off_t data;
    int rc;

    if (idx < 0 || idx >= z->count) return BS_ERR_INVAL;
    e = &z->ent[idx];

    rc = read_at(z->io, e->lho, loc, sizeof loc);
    if (rc != BS_OK) return rc;
    if (ts_le32(loc) != LOC_SIG) return BS_ERR_FORMAT;

    data = (bs_off_t)e->lho + 30 + ts_le16(loc + 26) + ts_le16(loc + 28);
    bs_file_stream_retarget(z->raw, data, (bs_off_t)e->csize);

    if (e->method == 0) {                   /* stored */
        *out = z->raw;
        return BS_OK;
    }
    if (e->method != 8) return BS_ERR_UNSUP;

    bs_inflate_restart(z->infl, BS_INFLATE_RAW);
    *out = z->infl;
    return BS_OK;
}
