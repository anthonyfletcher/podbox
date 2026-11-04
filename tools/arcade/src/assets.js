/* A cart's assets, from its folder into chunks:
 *
 *   sheet.png            the 128x128 sheet, an indexed PNG of palette numbers
 *   flags.json           the 256 sprite flags: an array, or { "n": flags }
 *   palettes/<name>.gpl  palettes, GIMP's text format; the first is loaded
 *                        when the cart starts
 *   maps/<name>.json     { width, height, rows: ["0a0b..", ...], objects:
 *                        [{ kind, x, y, w, h, props: { name: value } }] }
 *   images/<name>.png    other images, indexed PNGs
 *
 * Each palette, map and image is named after its file; maps and palettes
 * become MAP.<name> and PAL.<name> (numbered in name order), object kinds
 * OBJ.<kind>, and images IMG.<name>, an image value the loader makes.
 *
 * Chunks, little-endian (lengths and counts u16, sizes u32):
 *   PALS  count; each: entries, then r, g and b, a byte each
 *   SHET  width, height, LZ4 size, LZ4 pixels; then the 256 flags
 *   MAPS  count; each: width, height, LZ4 size, LZ4 cells, object count;
 *         each object: kind u8, x y w h i16, prop count u8; each prop:
 *         key (a string number), type u8 (0 an integer, 1 a string), then
 *         an i32 or a string number
 *   IMGS  count; each: width, height, LZ4 size, LZ4 pixels */

import { Writer } from './cart.js';
import { lz4Compress } from './lz4.js';
import { decodeIndexedPng } from './png.js';

const NAME = /^[A-Za-z_][\w]*$/;
const MAX_MAP = 256;
const MAX_IMAGE = 1024;

function sortedNames(obj) {
    return Object.keys(obj || {}).sort();
}

export function parseGpl(text) {
    const rgb = [];
    const lines = text.split(/\r?\n/);
    if (!/^GIMP Palette/.test(lines[0] || ''))
        throw new Error('a .gpl palette starts with "GIMP Palette"');
    for (let i = 1; i < lines.length; i++) {
        const l = lines[i].trim();
        if (!l || l[0] === '#' || /^(Name|Columns):/.test(l)) continue;
        const m = /^(\d+)\s+(\d+)\s+(\d+)/.exec(l);
        if (!m || +m[1] > 255 || +m[2] > 255 || +m[3] > 255)
            throw new Error('line ' + (i + 1) + ': a colour is three numbers ' +
                            'from 0 to 255');
        rgb.push([+m[1], +m[2], +m[3]]);
    }
    if (!rgb.length) throw new Error('the palette has no colours');
    if (rgb.length > 256) throw new Error('a palette has at most 256 colours');
    return rgb;
}

/* assets: { sheet, flags, palettes, maps, images } as the folder holds
 * them (PNGs as bytes, the rest as text); string(s) numbers a constant
 * string. Errors go to errors; returns { groups, images, chunks }. */
export function buildAssets(assets, inflate, string, errors) {
    const out = { groups: { MAP: {}, PAL: {}, OBJ: {} }, images: [], chunks: [] };
    if (!assets) return out;
    function err(file, message) {
        errors.push({ file: file, line: 1, column: 1, message: message });
    }
    function named(file, name) {
        if (NAME.test(name)) return true;
        err(file, 'a file name must be usable as a name in code: letters, ' +
            'digits and _');
        return false;
    }

    /* palettes */
    const pals = [];
    for (const name of sortedNames(assets.palettes)) {
        const file = 'palettes/' + name + '.gpl';
        if (!named(file, name)) continue;
        try {
            pals.push(parseGpl(assets.palettes[name]));
            out.groups.PAL[name] = { value: pals.length - 1, version: 1 };
        } catch (e) { err(file, e.message); }
    }
    if (pals.length) {
        const w = new Writer();
        w.u8(pals.length);
        for (const p of pals) {
            w.u16(p.length);
            for (const c of p) { w.u8(c[0]); w.u8(c[1]); w.u8(c[2]); }
        }
        out.chunks.push({ tag: 'PALS', data: w.done() });
    }

    /* the sheet and its flags */
    let sheet = null, flags = new Uint8Array(256);
    if (assets.sheet) {
        try {
            const img = decodeIndexedPng(assets.sheet, inflate);
            if (img.width !== 128 || img.height !== 128)
                err('sheet.png', 'the sheet is 128x128, not ' + img.width + 'x' +
                    img.height);
            else sheet = img.px;
        } catch (e) { err('sheet.png', e.message); }
    }
    if (assets.flags !== undefined) {
        try {
            const f = JSON.parse(assets.flags);
            const set = (n, v) => {
                if (!(n >= 0 && n < 256 && v >= 0 && v < 256 && v === Math.floor(v)))
                    throw new Error('sprite ' + n + ': flags are a number from 0 ' +
                                    'to 255, for sprites 0 to 255');
                flags[n] = v;
            };
            if (Array.isArray(f)) f.forEach((v, n) => set(n, v));
            else for (const k in f) set(+k, f[k]);
        } catch (e) { err('flags.json', e.message); }
    }
    if (sheet || assets.flags !== undefined) {
        const w = new Writer();
        const px = sheet || new Uint8Array(128 * 128);
        const z = lz4Compress(px);
        w.u16(128); w.u16(128); w.u32(z.length); w.bytes(z);
        w.bytes(flags);
        out.chunks.push({ tag: 'SHET', data: w.done() });
    }

    /* maps */
    const maps = [], kinds = new Set();
    for (const name of sortedNames(assets.maps)) {
        const file = 'maps/' + name + '.json';
        if (!named(file, name)) continue;
        try {
            const m = JSON.parse(assets.maps[name]);
            const W = m.width, H = m.height;
            if (!(W >= 1 && W <= MAX_MAP && H >= 1 && H <= MAX_MAP &&
                  W === Math.floor(W) && H === Math.floor(H)))
                throw new Error('width and height are 1 to ' + MAX_MAP + ' cells');
            const cells = new Uint8Array(W * H);
            const rows = m.rows || [];
            if (!Array.isArray(rows) || rows.length > H)
                throw new Error('rows is a list of at most height strings');
            rows.forEach((r, y) => {
                if (typeof r !== 'string' || r.length > W * 2 || r.length % 2 ||
                    !/^[0-9a-fA-F]*$/.test(r))
                    throw new Error('row ' + y + ': two hex digits a cell, at ' +
                                    'most width cells');
                for (let x = 0; x < r.length / 2; x++)
                    cells[y * W + x] = parseInt(r.substr(x * 2, 2), 16);
            });
            const objects = m.objects || [];
            if (!Array.isArray(objects) || objects.length > 65535)
                throw new Error('objects is a list');
            for (const o of objects) {
                if (typeof o.kind !== 'string' || !NAME.test(o.kind))
                    throw new Error('an object\'s kind is a name: letters, digits ' +
                                    'and _');
                for (const k of ['x', 'y', 'w', 'h'])
                    if (o[k] !== undefined && !(o[k] === Math.floor(o[k]) &&
                                                o[k] >= -32768 && o[k] <= 32767))
                        throw new Error('an object\'s ' + k + ' is a whole number ' +
                                        'of pixels');
                const props = o.props || {};
                if (Object.keys(props).length > 255)
                    throw new Error('an object has at most 255 properties');
                for (const k in props)
                    if (typeof props[k] !== 'string' &&
                        !(typeof props[k] === 'number' && props[k] === (props[k] | 0)))
                        throw new Error('property ' + k + ' is a whole number or ' +
                                        'a string');
                kinds.add(o.kind);
            }
            maps.push({ W: W, H: H, cells: cells, objects: objects });
            out.groups.MAP[name] = { value: maps.length - 1, version: 1 };
        } catch (e) { err(file, e.message); }
    }
    const kindList = [...kinds].sort();
    if (kindList.length > 255) err('maps', 'at most 255 object kinds');
    kindList.forEach((k, i) => { out.groups.OBJ[k] = { value: i, version: 1 }; });
    if (maps.length) {
        const w = new Writer();
        w.u16(maps.length);
        for (const m of maps) {
            const z = lz4Compress(m.cells);
            w.u16(m.W); w.u16(m.H); w.u32(z.length); w.bytes(z);
            w.u16(m.objects.length);
            for (const o of m.objects) {
                w.u8(kindList.indexOf(o.kind));
                for (const k of ['x', 'y', 'w', 'h']) w.u16((o[k] || 0) & 0xffff);
                const props = o.props || {};
                const keys = Object.keys(props);
                w.u8(keys.length);
                for (const k of keys) {
                    w.u16(string(k));
                    if (typeof props[k] === 'string') { w.u8(1); w.u16(string(props[k])); }
                    else { w.u8(0); w.u32(props[k] >>> 0); }
                }
            }
        }
        out.chunks.push({ tag: 'MAPS', data: w.done() });
    }

    /* images */
    const imgs = [];
    for (const name of sortedNames(assets.images)) {
        const file = 'images/' + name + '.png';
        if (!named(file, name)) continue;
        try {
            const img = decodeIndexedPng(assets.images[name], inflate);
            if (img.width > MAX_IMAGE || img.height > MAX_IMAGE)
                throw new Error('an image is at most ' + MAX_IMAGE + ' pixels a side');
            imgs.push(img);
            out.images.push(name);
        } catch (e) { err(file, e.message); }
    }
    if (imgs.length) {
        const w = new Writer();
        w.u16(imgs.length);
        for (const img of imgs) {
            const z = lz4Compress(img.px);
            w.u16(img.width); w.u16(img.height); w.u32(z.length); w.bytes(z);
        }
        out.chunks.push({ tag: 'IMGS', data: w.done() });
    }
    return out;
}
