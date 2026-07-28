/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Detects the input encoding (BOM, UTF-8 validity, codepage heuristics)
 * and converts it to UTF-8.
 ****************************************************************************/

#include "ts_internal.h"

/* ---- single byte tables ---------------------------------------------- */

static const uint16_t cp1252_hi[32] = {   /* 0x80..0x9F */
    0x20AC,0,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,
    0x02C6,0x2030,0x0160,0x2039,0x0152,0,0x017D,0,
    0,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
    0x02DC,0x2122,0x0161,0x203A,0x0153,0,0x017E,0x0178
};

static const uint16_t l2_hi[96] = {       /* ISO-8859-2, 0xA0..0xFF */
    0x00A0,0x0104,0x02D8,0x0141,0x00A4,0x013D,0x015A,0x00A7,
    0x00A8,0x0160,0x015E,0x0164,0x0179,0x00AD,0x017D,0x017B,
    0x00B0,0x0105,0x02DB,0x0142,0x00B4,0x013E,0x015B,0x02C7,
    0x00B8,0x0161,0x015F,0x0165,0x017A,0x02DD,0x017E,0x017C,
    0x0154,0x00C1,0x00C2,0x0102,0x00C4,0x0139,0x0106,0x00C7,
    0x010C,0x00C9,0x0118,0x00CB,0x011A,0x00CD,0x00CE,0x010E,
    0x0110,0x0143,0x0147,0x00D3,0x00D4,0x0150,0x00D6,0x00D7,
    0x0158,0x016E,0x00DA,0x0170,0x00DC,0x00DD,0x0162,0x00DF,
    0x0155,0x00E1,0x00E2,0x0103,0x00E4,0x013A,0x0107,0x00E7,
    0x010D,0x00E9,0x0119,0x00EB,0x011B,0x00ED,0x00EE,0x010F,
    0x0111,0x0144,0x0148,0x00F3,0x00F4,0x0151,0x00F6,0x00F7,
    0x0159,0x016F,0x00FA,0x0171,0x00FC,0x00FD,0x0163,0x02D9
};

static const uint16_t l7_hi[96] = {       /* ISO-8859-7, 0xA0..0xFF */
    0x00A0,0x2018,0x2019,0x00A3,0x20AC,0x20AF,0x00A6,0x00A7,
    0x00A8,0x00A9,0x037A,0x00AB,0x00AC,0x00AD,0xFFFD,0x2015,
    0x00B0,0x00B1,0x00B2,0x00B3,0x0384,0x0385,0x0386,0x00B7,
    0x0388,0x0389,0x038A,0x00BB,0x038C,0x00BD,0x038E,0x038F,
    0x0390,0x0391,0x0392,0x0393,0x0394,0x0395,0x0396,0x0397,
    0x0398,0x0399,0x039A,0x039B,0x039C,0x039D,0x039E,0x039F,
    0x03A0,0x03A1,0xFFFD,0x03A3,0x03A4,0x03A5,0x03A6,0x03A7,
    0x03A8,0x03A9,0x03AA,0x03AB,0x03AC,0x03AD,0x03AE,0x03AF,
    0x03B0,0x03B1,0x03B2,0x03B3,0x03B4,0x03B5,0x03B6,0x03B7,
    0x03B8,0x03B9,0x03BA,0x03BB,0x03BC,0x03BD,0x03BE,0x03BF,
    0x03C0,0x03C1,0x03C2,0x03C3,0x03C4,0x03C5,0x03C6,0x03C7,
    0x03C8,0x03C9,0x03CA,0x03CB,0x03CC,0x03CD,0x03CE,0xFFFD
};

static const uint16_t cp1251_hi[128] = {  /* 0x80..0xFF */
    0x0402,0x0403,0x201A,0x0453,0x201E,0x2026,0x2020,0x2021,
    0x20AC,0x2030,0x0409,0x2039,0x040A,0x040C,0x040B,0x040F,
    0x0452,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
    0xFFFD,0x2122,0x0459,0x203A,0x045A,0x045C,0x045B,0x045F,
    0x00A0,0x040E,0x045E,0x0408,0x00A4,0x0490,0x00A6,0x00A7,
    0x0401,0x00A9,0x0404,0x00AB,0x00AC,0x00AD,0x00AE,0x0407,
    0x00B0,0x00B1,0x0406,0x0456,0x0491,0x00B5,0x00B6,0x00B7,
    0x0451,0x2116,0x0454,0x00BB,0x0458,0x0405,0x0455,0x0457,
    0x0410,0x0411,0x0412,0x0413,0x0414,0x0415,0x0416,0x0417,
    0x0418,0x0419,0x041A,0x041B,0x041C,0x041D,0x041E,0x041F,
    0x0420,0x0421,0x0422,0x0423,0x0424,0x0425,0x0426,0x0427,
    0x0428,0x0429,0x042A,0x042B,0x042C,0x042D,0x042E,0x042F,
    0x0430,0x0431,0x0432,0x0433,0x0434,0x0435,0x0436,0x0437,
    0x0438,0x0439,0x043A,0x043B,0x043C,0x043D,0x043E,0x043F,
    0x0440,0x0441,0x0442,0x0443,0x0444,0x0445,0x0446,0x0447,
    0x0448,0x0449,0x044A,0x044B,0x044C,0x044D,0x044E,0x044F
};

uint32_t ts_sb_to_cp(ts_charset cs, uint8_t b);
static uint32_t sb_to_cp(ts_charset cs, uint8_t b);

uint32_t ts_sb_to_cp(ts_charset cs, uint8_t b) { return sb_to_cp(cs, b); }

static uint32_t sb_to_cp(ts_charset cs, uint8_t b)
{
    if (b < 0x80) return b;

    switch (cs) {
    case TS_CS_CP1252:
        if (b < 0xA0) { uint32_t c = cp1252_hi[b - 0x80]; return c ? c : 0xFFFD; }
        return b;
    case TS_CS_ISO8859_1:
        return b;
    case TS_CS_ISO8859_2:
        return (b < 0xA0) ? 0xFFFD : l2_hi[b - 0xA0];
    case TS_CS_ISO8859_7:
        return (b < 0xA0) ? 0xFFFD : l7_hi[b - 0xA0];
    case TS_CS_ISO8859_9:
        switch (b) {                        /* Latin-1 with six swaps */
        case 0xD0: return 0x011E; case 0xDD: return 0x0130;
        case 0xDE: return 0x015E; case 0xF0: return 0x011F;
        case 0xFD: return 0x0131; case 0xFE: return 0x015F;
        default:   return b;
        }
    case TS_CS_ISO8859_5:
        if (b < 0xA0) return 0xFFFD;
        if (b == 0xA0) return 0x00A0;
        if (b <= 0xAC) return 0x0400 + (b - 0xA0);
        if (b == 0xAD) return 0x00AD;
        if (b <= 0xAF) return 0x0400 + (b - 0xA0);
        if (b <= 0xEF) return 0x0410 + (b - 0xB0);
        if (b == 0xF0) return 0x2116;
        if (b <= 0xFC) return 0x0450 + (b - 0xF0);
        if (b == 0xFD) return 0x00A7;
        return 0x045E + (b - 0xFE);
    case TS_CS_CP1251:
        return cp1251_hi[b - 0x80];
    default:
        return b;
    }
}

/* ---- sniffing -------------------------------------------------------- */

static int utf8_plausible(const uint8_t *p, size_t n)
{
    size_t i = 0, seq = 0;

    while (i < n) {
        uint8_t b = p[i];
        size_t need;

        if (b < 0x80) { i++; continue; }
        if ((b & 0xE0) == 0xC0) need = 2;
        else if ((b & 0xF0) == 0xE0) need = 3;
        else if ((b & 0xF8) == 0xF0) need = 4;
        else return 0;

        if (i + need > n) return 1;         /* truncated at the window edge */
        for (size_t k = 1; k < need; k++)
            if ((p[i + k] & 0xC0) != 0x80) return 0;
        i += need;
        seq++;
    }
    (void)seq;
    return 1;
}

ts_charset ts_charset_sniff(const uint8_t *h, size_t n, ts_charset fb,
                            size_t *bom_skip)
{
    size_t zero_even = 0, zero_odd = 0, i;

    *bom_skip = 0;
    if (n >= 3 && h[0] == 0xEF && h[1] == 0xBB && h[2] == 0xBF) {
        *bom_skip = 3; return TS_CS_UTF8;
    }
    if (n >= 2 && h[0] == 0xFF && h[1] == 0xFE) { *bom_skip = 2; return TS_CS_UTF16LE; }
    if (n >= 2 && h[0] == 0xFE && h[1] == 0xFF) { *bom_skip = 2; return TS_CS_UTF16BE; }

    /* BOM-less UTF-16 gives itself away: half the bytes are NUL, and which
     * half tells us the byte order. */
    for (i = 0; i + 1 < n; i += 2) {
        if (!h[i])     zero_even++;
        if (!h[i + 1]) zero_odd++;
    }
    if (n >= 16) {
        size_t pairs = n / 2;
        if (zero_odd * 4 > pairs * 3 && zero_even * 8 < pairs) return TS_CS_UTF16LE;
        if (zero_even * 4 > pairs * 3 && zero_odd * 8 < pairs) return TS_CS_UTF16BE;
    }
    if (utf8_plausible(h, n)) return TS_CS_UTF8;
    return (fb == TS_CS_AUTO) ? TS_CS_CP1252 : fb;
}

/* ---- declared encodings ---------------------------------------------- */

static const struct { const char *n; ts_charset cs; } cs_names[] = {
    { "utf-8", TS_CS_UTF8 }, { "utf8", TS_CS_UTF8 }, { "us-ascii", TS_CS_UTF8 },
    { "ascii", TS_CS_UTF8 },
    { "utf-16", TS_CS_UTF16LE }, { "utf-16le", TS_CS_UTF16LE },
    { "utf-16be", TS_CS_UTF16BE }, { "ucs-2", TS_CS_UTF16LE },
    { "iso-8859-1", TS_CS_ISO8859_1 }, { "latin1", TS_CS_ISO8859_1 },
    { "iso-8859-15", TS_CS_ISO8859_1 },
    { "windows-1252", TS_CS_CP1252 }, { "cp1252", TS_CS_CP1252 },
    { "iso-8859-2", TS_CS_ISO8859_2 }, { "latin2", TS_CS_ISO8859_2 },
    { "windows-1250", TS_CS_ISO8859_2 },
    { "iso-8859-5", TS_CS_ISO8859_5 },
    { "windows-1251", TS_CS_CP1251 }, { "cp1251", TS_CS_CP1251 },
    { "koi8-r", TS_CS_CP1251 },       /* approximation, better than mojibake */
    { "iso-8859-7", TS_CS_ISO8859_7 }, { "windows-1253", TS_CS_ISO8859_7 },
    { "iso-8859-9", TS_CS_ISO8859_9 }, { "windows-1254", TS_CS_ISO8859_9 },
    { NULL, TS_CS_AUTO }
};

ts_charset ts_charset_from_name(const char *name)
{
    int i;
    for (i = 0; cs_names[i].n; i++)
        if (ts_ascii_casecmp(cs_names[i].n, name) == 0) return cs_names[i].cs;
    return TS_CS_AUTO;
}

/* Looks for encoding="..." (XML prolog) or charset=... (HTML meta) in the
 * probe window. A declaration beats the statistical guess. */
ts_charset ts_charset_decl(const uint8_t *h, size_t n, ts_charset current)
{
    static const char *keys[] = { "encoding=", "charset=", NULL };
    char val[32];
    size_t i, k;

    if (current == TS_CS_UTF16LE || current == TS_CS_UTF16BE) return current;
    if (n > 2048) n = 2048;

    for (k = 0; keys[k]; k++) {
        size_t kl = strlen(keys[k]);
        for (i = 0; i + kl < n; i++) {
            size_t j = 0, p;
            if (ts_ascii_ncasecmp((const char *)h + i, keys[k], kl)) continue;
            p = i + kl;
            if (p < n && (h[p] == '"' || h[p] == '\'')) p++;
            while (p < n && j < sizeof val - 1) {
                uint8_t c = h[p];
                if (c == '"' || c == '\'' || c == ' ' || c == '>' ||
                    c == ';' || c == '?' || c == '\r' || c == '\n') break;
                val[j++] = (char)c;
                p++;
            }
            val[j] = 0;
            if (j) {
                ts_charset cs = ts_charset_from_name(val);
                if (cs != TS_CS_AUTO) return cs;
            }
        }
    }
    return current;
}

/* ---- conversion stage ------------------------------------------------ */

#define CS_IN 256

typedef struct {
    ts_stream *src;
    ts_charset cs;
    uint8_t    in[CS_IN];
    size_t     ipos, ilen;
    int        src_eof, bom_done;
    ts_pend    out;
} cs_st;

/* Ensures at least `want` bytes are buffered unless the source is dry. */
static int cs_need(cs_st *s, size_t want)
{
    if (s->ilen - s->ipos >= want) return TS_OK;

    if (s->ipos) {
        memmove(s->in, s->in + s->ipos, s->ilen - s->ipos);
        s->ilen -= s->ipos;
        s->ipos = 0;
    }
    while (!s->src_eof && s->ilen < want) {
        size_t got = 0;
        int rc = ts_pull(s->src, s->in + s->ilen, CS_IN - s->ilen, &got);
        if (rc != TS_OK) return rc;
        if (!got) { s->src_eof = 1; break; }
        s->ilen += got;
    }
    return TS_OK;
}

static void cs_skip_bom(cs_st *s)
{
    size_t avail = s->ilen - s->ipos;
    const uint8_t *p = s->in + s->ipos;

    s->bom_done = 1;
    if (s->cs == TS_CS_UTF8 && avail >= 3 &&
        p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
        s->ipos += 3;
    else if (s->cs == TS_CS_UTF16LE && avail >= 2 && p[0] == 0xFF && p[1] == 0xFE)
        s->ipos += 2;
    else if (s->cs == TS_CS_UTF16BE && avail >= 2 && p[0] == 0xFE && p[1] == 0xFF)
        s->ipos += 2;
}

/* Decodes one character into the pending ring. Returns 1 if it produced
 * output, 0 at end of input. */
static int cs_step(cs_st *s, int *err)
{
    int rc = cs_need(s, 4);
    size_t avail;

    *err = TS_OK;
    if (rc != TS_OK) { *err = rc; return 0; }
    if (!s->bom_done) cs_skip_bom(s);

    avail = s->ilen - s->ipos;
    if (!avail) return 0;

    if (s->cs == TS_CS_UTF16LE || s->cs == TS_CS_UTF16BE) {
        uint32_t u, lo;
        int be = (s->cs == TS_CS_UTF16BE);

        if (avail < 2) { s->ipos = s->ilen; return 0; }   /* dangling byte */
        u = be ? (uint32_t)(s->in[s->ipos] << 8 | s->in[s->ipos + 1])
               : (uint32_t)(s->in[s->ipos + 1] << 8 | s->in[s->ipos]);
        s->ipos += 2;
        if (u >= 0xD800 && u <= 0xDBFF && avail >= 4) {
            lo = be ? (uint32_t)(s->in[s->ipos] << 8 | s->in[s->ipos + 1])
                    : (uint32_t)(s->in[s->ipos + 1] << 8 | s->in[s->ipos]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                s->ipos += 2;
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            }
        }
        ts_emit_cp(&s->out, u);
        return 1;
    }

    if (s->cs == TS_CS_UTF8) {
        uint8_t b = s->in[s->ipos];
        size_t need, k;
        uint32_t cp;

        if (b < 0x80)             { s->ipos++; ts_emit_cp(&s->out, b); return 1; }
        if ((b & 0xE0) == 0xC0)   { need = 2; cp = b & 0x1F; }
        else if ((b & 0xF0) == 0xE0) { need = 3; cp = b & 0x0F; }
        else if ((b & 0xF8) == 0xF0) { need = 4; cp = b & 0x07; }
        else { s->ipos++; ts_emit_cp(&s->out, 0xFFFD); return 1; }

        if (avail < need) { s->ipos = s->ilen; ts_emit_cp(&s->out, 0xFFFD); return 1; }
        for (k = 1; k < need; k++) {
            uint8_t c = s->in[s->ipos + k];
            if ((c & 0xC0) != 0x80) { s->ipos++; ts_emit_cp(&s->out, 0xFFFD); return 1; }
            cp = (cp << 6) | (c & 0x3F);
        }
        s->ipos += need;
        ts_emit_cp(&s->out, cp);
        return 1;
    }

    ts_emit_cp(&s->out, sb_to_cp(s->cs, s->in[s->ipos++]));
    return 1;
}

static int cs_pull(ts_stream *st, uint8_t *buf, size_t n, size_t *out)
{
    cs_st *s = st->st;
    size_t done = 0;

    while (done < n) {
        int err;
        done += ts_pend_take(&s->out, buf + done, n - done);
        if (done == n) break;
        if (ts_pend_free(&s->out) < 4) continue;
        if (!cs_step(s, &err)) {
            if (err != TS_OK) return err;
            done += ts_pend_take(&s->out, buf + done, n - done);
            break;                              /* end of input */
        }
    }
    *out = done;
    return TS_OK;
}

static int cs_reset(ts_stream *st)
{
    cs_st *s = st->st;
    int rc = s->src->reset ? s->src->reset(s->src) : TS_ERR_UNSUP;

    if (rc != TS_OK) return rc;
    s->ipos = s->ilen = 0;
    s->src_eof = s->bom_done = 0;
    ts_pend_reset(&s->out);
    return TS_OK;
}

/* Re-point at the next member of a container without touching the arena. */
void ts_charset_restart(ts_stream *st, ts_stream *src, ts_charset cs)
{
    cs_st *s = st->st;

    s->src = src;
    if (cs != TS_CS_AUTO) s->cs = cs;
    s->ipos = s->ilen = 0;
    s->src_eof = s->bom_done = 0;
    ts_pend_reset(&s->out);
}

ts_stream *ts_charset_stream(ts_arena *a, ts_stream *src, ts_charset cs)
{
    ts_stream *st = ts_alloc(a, sizeof *st);
    cs_st *s = ts_alloc(a, sizeof *s);

    if (!st || !s) return NULL;
    s->src = src;
    s->cs = (cs == TS_CS_AUTO) ? TS_CS_UTF8 : cs;
    st->pull = cs_pull; st->reset = cs_reset; st->st = s;
    return st;
}
