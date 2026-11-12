/* A cart's fonts: fonts/<name>.bdf, read by the rules arcade/sdk/bdf.py
 * reads the engine's own by, into the FONT chunk.
 *
 *   FONT  count u8; each font: its line (pixels, u8) and glyph count u16,
 *         then a u32 codepoint and u32 offset a glyph, in codepoint order,
 *         then the glyphs the offsets point into (from the first glyph's
 *         start): advance u8, x i8, y i8 (from the line's top), w u8, h u8,
 *         then h rows of (w + 7) / 8 bytes, the high bit on the left
 *
 * A BDF's line is FONT_ASCENT + FONT_DESCENT rows; a glyph's place in it
 * comes from its BBX, and its advance from DWIDTH. */

import { Writer } from './cart.js';

export const MAX_FONTS = 16;
export const MAX_FONT_BYTES = 65536;
const MAX_SIDE = 32;

/* { line, glyphs: [{ cp, adv, x, y, w, h, rows }] } in codepoint order,
 * rows as byte arrays; throws an Error saying what to change */
export function parseBdf(text) {
    let ascent = null, descent = null, ch = null, line = 0;
    const glyphs = new Map();
    const fail = (why) => { throw new Error('line ' + line + ': ' + why); };
    for (const raw of text.split(/\r?\n/)) {
        line++;
        const p = raw.trim().split(/\s+/);
        if (ch && ch.rows) {
            if (p[0] === 'ENDCHAR') {
                if (ch.rows.length < ch.h) fail('a glyph has fewer rows than its BBX');
                if (ch.cp >= 0) glyphs.set(ch.cp, ch);
                ch = null;
            } else {
                if (!/^[0-9A-Fa-f]+$/.test(p[0]) || p[0].length % 2)
                    fail('a bitmap row is whole bytes in hex');
                if (ch.rows.length < ch.h) ch.rows.push(p[0]);
            }
            continue;
        }
        switch (p[0]) {
        case 'FONT_ASCENT': ascent = +p[1]; break;
        case 'FONT_DESCENT': descent = +p[1]; break;
        case 'STARTCHAR': ch = { cp: null, adv: null, bbx: null, rows: null }; break;
        case 'ENCODING': if (ch) ch.cp = +p[1]; break;
        case 'DWIDTH': if (ch) ch.adv = +p[1]; break;
        case 'BBX': if (ch) ch.bbx = p.slice(1, 5).map(Number); break;
        case 'BITMAP':
            if (!ch || ch.cp === null || ch.adv === null || !ch.bbx)
                fail('a glyph needs ENCODING, DWIDTH and BBX before BITMAP');
            [ch.w, ch.h, ch.xo, ch.yo] = ch.bbx;
            ch.rows = [];
            break;
        }
    }
    if (!Number.isInteger(ascent) || !Number.isInteger(descent))
        throw new Error('a font needs FONT_ASCENT and FONT_DESCENT');
    const height = ascent + descent;
    if (height < 1 || height > MAX_SIDE)
        throw new Error('a font\'s line (FONT_ASCENT + FONT_DESCENT) is 1 to ' +
                        MAX_SIDE + ' pixels, not ' + height);
    const out = [];
    for (const cp of [...glyphs.keys()].sort((a, b) => a - b)) {
        const g = glyphs.get(cp);
        const where = 'U+' + cp.toString(16).toUpperCase().padStart(4, '0');
        if (g.w < 0 || g.h < 0 || g.w > MAX_SIDE || g.h > MAX_SIDE)
            throw new Error(where + ' is ' + g.w + 'x' + g.h + '; a glyph is at most ' +
                            MAX_SIDE + ' pixels a side');
        if (g.adv < 0 || g.adv > 255 || Math.abs(g.xo) > 127)
            throw new Error(where + '\'s advance or offset is out of range');
        const y = ascent - (g.yo + g.h), stride = (g.w + 7) >> 3;
        if (y < -128 || y > 127) throw new Error(where + ' is far outside its line');
        const rows = g.rows.map((hex) => {
            const b = new Uint8Array(stride);
            for (let i = 0; i < stride && i * 2 < hex.length; i++)
                b[i] = parseInt(hex.substr(i * 2, 2), 16);
            /* the bits past w are not the glyph's */
            if (g.w & 7) b[stride - 1] &= 0xff << (8 - (g.w & 7));
            return b;
        });
        out.push({ cp: cp, adv: g.adv, x: g.xo, y: y, w: g.w, h: g.h, rows: rows });
    }
    if (!out.length) throw new Error('a font needs at least one glyph');
    return { line: height, glyphs: out };
}

export function fontChunk(fonts) {
    const w = new Writer();
    w.u8(fonts.length);
    for (const f of fonts) {
        w.u8(f.line);
        w.u16(f.glyphs.length);
        let off = 0;
        for (const g of f.glyphs) {
            w.u32(g.cp);
            w.u32(off);
            off += 5 + g.h * ((g.w + 7) >> 3);
        }
        for (const g of f.glyphs) {
            w.u8(g.adv); w.u8(g.x & 0xff); w.u8(g.y & 0xff); w.u8(g.w); w.u8(g.h);
            for (const r of g.rows) w.bytes(r);
        }
    }
    return w.done();
}
