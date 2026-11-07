/* A cart's assets, from its folder into chunks:
 *
 *   sheet.png            the 128x128 sheet, an indexed PNG of palette numbers
 *   flags.json           the 256 sprite flags: an array, or { "n": flags }
 *   palettes/<name>.gpl  palettes, GIMP's text format; the first is loaded
 *                        when the cart starts
 *   maps/<name>.json     { width, height, rows: ["0a0b..", ...], objects:
 *                        [{ kind, x, y, w, h, props: { name: value } }] }
 *   images/<name>.png    other images, indexed PNGs
 *   sounds/<name>.json   sound effects: sfxr parameters in jsfxr's JSON,
 *                        rendered here and stored as 8-bit PCM at 22,050 Hz
 *   fonts/<name>.bdf     fonts, BDF; bdf.js has the FONT chunk
 *
 * Each palette, map and image is named after its file; maps and palettes
 * become MAP.<name> and PAL.<name> (numbered in name order), object kinds
 * OBJ.<kind>, sounds SFX.<name>, fonts FONT.<name> (after the engine's
 * CAPS and WORLD), and images IMG.<name>, an image value
 * the loader makes.
 *
 * Chunks, little-endian (lengths and counts u16, sizes u32):
 *   PALS  count; each: entries, then r, g and b, a byte each
 *   SHET  width, height, LZ4 size, LZ4 pixels; then the 256 flags
 *   MAPS  count; each: width, height, LZ4 size, LZ4 cells, object count;
 *         each object: kind u8, x y w h i16, prop count u8; each prop:
 *         key (a string number), type u8 (0 an integer, 1 a string), then
 *         an i32 or a string number
 *   IMGS  count; each: width, height, LZ4 size, LZ4 pixels
 *   SFX_  count; each: samples (u32), scale (u16), then a signed byte a
 *         sample: the 16-bit sample is the byte times the scale, which is
 *         as small as lets the loudest sample fit */

import { Writer } from './cart.js';
import { lz4Compress } from './lz4.js';
import { decodeIndexedPng } from './png.js';
import { sfxrParams, sfxrRender, halve } from './sfxr.js';
import { parseBdf, fontChunk, MAX_FONTS, MAX_FONT_BYTES } from './bdf.js';

/* FONT.<name> numbers: the engine's fonts are 0 to 2 */
const FIRST_FONT = 3, ENGINE_FONTS = ['CAPS', 'WORLD'];

const NAME = /^[A-Za-z_][\w]*$/;
const MAX_MAP = 256;
const MAX_IMAGE = 1024;

function sortedNames(obj) {
    return Object.keys(obj || {}).sort();
}

/* 16-bit samples as signed bytes and a scale: the smallest whole scale
 * that fits the loudest, so a quiet effect keeps its detail */
export function quantise(pcm) {
    let peak = 1;
    for (const v of pcm) peak = Math.max(peak, Math.abs(v));
    const scale = Math.ceil(peak / 127);
    const bytes = new Uint8Array(pcm.length);
    for (let i = 0; i < pcm.length; i++)
        bytes[i] = Math.max(-128, Math.min(127, Math.round(pcm[i] / scale))) & 0xff;
    return { scale: scale, bytes: bytes };
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
    const out = { groups: { MAP: {}, PAL: {}, OBJ: {}, SFX: {}, DATA: {}, FONT: {} },
                  images: [], chunks: [] };
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
        if (pals.length === 255) {
            err(file, 'a cart holds at most 255 palettes');
            continue;
        }
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

    /* sounds: rendered now, each with noise seeded from its name */
    const sounds = [];
    for (const name of sortedNames(assets.sounds)) {
        const file = 'sounds/' + name + '.json';
        if (!named(file, name)) continue;
        try {
            const params = sfxrParams(JSON.parse(assets.sounds[name]));
            let seed = 2166136261;
            for (const ch of name) seed = Math.imul(seed ^ ch.charCodeAt(0), 16777619);
            sounds.push(quantise(halve(sfxrRender(params, seed >>> 0))));
            out.groups.SFX[name] = { value: sounds.length - 1, version: 1 };
        } catch (e) { err(file, e.message); }
    }
    if (sounds.length) {
        const w = new Writer();
        w.u16(sounds.length);
        for (const s of sounds) {
            w.u32(s.bytes.length);
            w.u16(s.scale);
            w.bytes(s.bytes);
        }
        out.chunks.push({ tag: 'SFX_', data: w.done() });
    }

    /* fonts */
    const fonts = [];
    for (const name of sortedNames(assets.fonts)) {
        const file = 'fonts/' + name + '.bdf';
        if (!named(file, name)) continue;
        if (ENGINE_FONTS.includes(name)) {
            err(file, 'FONT.' + name + ' is the engine\'s; call the file something else');
            continue;
        }
        if (fonts.length === MAX_FONTS) {
            err(file, 'a cart holds at most ' + MAX_FONTS + ' fonts');
            continue;
        }
        try {
            fonts.push(parseBdf(assets.fonts[name]));
            out.groups.FONT[name] = { value: FIRST_FONT + fonts.length - 1, version: 1 };
        } catch (e) { err(file, e.message); }
    }
    if (fonts.length) {
        const data = fontChunk(fonts);
        if (data.length > MAX_FONT_BYTES)
            err('fonts/', 'the fonts come to ' + data.length + ' bytes; a cart\'s are at most ' +
                MAX_FONT_BYTES + ' -- leave out the characters it does not show');
        else out.chunks.push({ tag: 'FONT', data: data });
    }

    /* data files: JSON checked and stored without its spaces, text as it
     * is; DATA.<name> numbers each, JSON and text together by name */
    const files = [];
    const texts = assets.texts || {};
    for (const name of sortedNames(Object.assign({}, assets.data, texts))) {
        const json = assets.data && name in assets.data;
        const file = 'data/' + name + (json ? '.json' : '.txt');
        if (!named(file, name)) continue;
        if (json && name in texts) {
            err(file, 'a data file and a text file are both called ' + name);
            continue;
        }
        if (!json) {
            files.push({ name: name, kind: 1, bytes: utf8Bytes(texts[name]) });
        } else {
            try {
                const value = JSON.parse(assets.data[name]);
                const why = dataFault(value, 0);
                if (why) { err(file, why); continue; }
                files.push({ name: name, kind: 0,
                             bytes: utf8Bytes(JSON.stringify(value)) });
            } catch (e) { err(file, e.message); continue; }
        }
        out.groups.DATA[name] = { value: files.length - 1, version: 1 };
    }
    if (files.length) {
        const w = new Writer();
        w.u16(files.length);
        for (const f of files) {
            w.u8(f.kind);
            w.str(f.name);
            w.u32(f.bytes.length);
            w.bytes(f.bytes);
        }
        out.chunks.push({ tag: 'DATA', data: w.done() });
    }
    return out;
}

function utf8Bytes(s) {
    return new TextEncoder().encode(s);
}

/* What in a data file's value a cart cannot hold, or null: numbers past
 * its range, lists more than 64 deep */
function dataFault(v, depth) {
    if (depth > 64) return 'more than 64 lists inside one another';
    if (typeof v === 'number') {
        if (Number.isInteger(v))
            return v >= -2147483648 && v <= 2147483647 ? null
                : v + ' is beyond the whole numbers a cart has';
        return v > -32768 && v < 32768 ? null
            : v + ' is beyond the fractions a cart has (-32768 to 32767)';
    }
    if (v && typeof v === 'object') {
        for (const k in v) {
            const why = dataFault(v[k], depth + 1);
            if (why) return why;
        }
    }
    return null;
}
