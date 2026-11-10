/* The IDE's side of a cart: building it in the browser, opening a .pbg,
 * and the names the editor completes. The engine's definitions come in as
 * text when the IDE is built, so the compiler here is pbgc's exactly. */

import { deflateSync, inflateSync, unzlibSync, zlibSync } from 'fflate';
import { compile } from '../src/compiler.js';
import { parseOpcodes, parseApi } from '../src/defs.js';
import { load } from '../src/bytecode.js';
import { unpackCart, DEFAULT_RGB } from '../src/unpack.js';
import { parseGpl } from '../src/assets.js';
import { decodeIndexedPng, encodeIndexedPng } from '../src/png.js';
import { compileInput, isBinary, newCartId } from '../src/folder.js';
import { musicCalls } from '../src/webpage.js';
import opcodesText from '../../../apps-ipod/games/arcade/opcodes.def';
import apiText from '../../../apps-ipod/games/arcade/arcade_api.def';

const isa = parseOpcodes(opcodesText);
const api = parseApi(apiText);

/* a built cart's functions, as the engine numbers them: each its name,
 * file and [offset, line] pairs, from the cart's LINE chunk */
export function functions(cart) {
    return load(cart).fns.map((f) => ({ name: f.name || '?', file: f.file, lines: f.lines || [] }));
}

/* an API call's name, by its slot, as code writes it */
export function apiName(slot) {
    const n = api.bySlot[slot];
    return n ? (n.label || n.name) + '()' : 'call ' + slot;
}

/* the owner's-music calls a built cart makes, which keep it off the web */
export function musicUsed(cart) {
    return musicCalls(cart, isa, api);
}

/* { cart, errors, stats } from the folder's files */
export function build(folder) {
    const { files, assets } = compileInput(folder);
    return compile({ files: files, assets: assets, isa: isa, api: api,
                     deflate: (b) => deflateSync(b, { level: 9 }),
                     inflate: (b) => unzlibSync(b) });
}

/* the folder a .pbg was built from: text as text, pictures as bytes */
export function open(bytes) {
    const files = unpackCart(bytes, (b) => inflateSync(b), (b) => zlibSync(b, { level: 9 }));
    for (const name of Object.keys(files))
        if (!isBinary(name)) files[name] = new TextDecoder().decode(files[name]);
    return files;
}

/* The cart's palette, as 0xRRGGBB: its first palette by name, or the
 * engine's sixteen */
export function palette(folder) {
    const first = Object.keys(folder).filter((f) => /^palettes\/\w+\.gpl$/.test(f)).sort()[0];
    if (first) {
        try {
            return parseGpl(String(folder[first])).map(([r, g, b]) => r << 16 | g << 8 | b);
        } catch (e) { /* a broken palette: the build says so */ }
    }
    return DEFAULT_RGB.slice();
}

/* a picture's palette numbers, or the error that stops it being read */
export function readPicture(bytes) {
    return decodeIndexedPng(bytes, (b) => unzlibSync(b));
}

export function writePicture(w, h, px, rgb) {
    return encodeIndexedPng(w, h, px, rgb, (b) => zlibSync(b, { level: 9 }));
}

/* What the editor offers: the natives, constants and groups at the top,
 * and after a dot each system's functions or a group's constants. The
 * cart's own groups (MAP., FONT.<name>) come from its last build. */
export function names(built) {
    const members = {};
    for (const [ns, fns] of Object.entries(api.namespaces)) members[ns] = Object.keys(fns);
    for (const [g, m] of Object.entries(api.groups)) members[g] = Object.keys(m);
    for (const [g, m] of Object.entries((built && built.groups) || {}))
        members[g] = [...new Set([...(members[g] || []), ...Object.keys(m)])];
    const top = [...Object.keys(api.natives), ...Object.keys(api.consts), ...Object.keys(members)];
    return { top: [...new Set(top)].sort(), members: members };
}

export const NEW_CART = () => {
    const id = newCartId();
    return {
        'cart.json': JSON.stringify({ id: id, name: 'New Game', author: '', version: '1', rate: 33 },
                                    null, 2) + '\n',
        'main.js': 'function init() {\n}\n\nfunction update() {\n}\n\nfunction draw() {\n' +
                   '    cls(1);\n    print("hello", 66, 56, 7);\n}\n'
    };
};
