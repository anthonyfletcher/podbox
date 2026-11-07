/* A cart back into its folder: the files SRC_ holds, as they were
 * written, and the pictures made again from SHET and IMGS as indexed PNGs
 * in the cart's first palette. Built again, the folder gives the same
 * cart byte for byte.
 *
 * inflateRaw undoes SRC_'s deflate; deflate is zlib's, for the PNGs. */

import { readCart } from './cart.js';
import { lz4Decompress } from './lz4.js';
import { encodeIndexedPng } from './png.js';

/* The engine's sixteen, for a cart without palettes */
const DEFAULT_RGB = [
    0x000000, 0x1a2244, 0x6b2a5e, 0x0f6640, 0x8a5236, 0x4f4a52, 0xb8b4bc,
    0xf6f2ea, 0xe8304a, 0xf5962a, 0xf8e43c, 0x3cd250, 0x2f9cf0, 0x7e7aa8,
    0xf27ab4, 0xf9c89a
];

function reader(bytes) {
    let at = 0;
    const r = {
        u8: () => bytes[at++],
        u16: () => { const v = bytes[at] | bytes[at + 1] << 8; at += 2; return v; },
        u32: () => { const v = (r.u16() | r.u16() << 16) >>> 0; return v; },
        bytes: (n) => { const b = bytes.subarray(at, at + n); at += n; return b; },
        str: () => new TextDecoder().decode(r.bytes(r.u16())),
        left: () => bytes.length - at
    };
    return r;
}

function firstPalette(pals) {
    if (!pals) return DEFAULT_RGB;
    const r = reader(pals);
    r.u8();
    const n = r.u16(), rgb = [];
    for (let i = 0; i < n; i++) rgb.push(r.u8() << 16 | r.u8() << 8 | r.u8());
    return rgb;
}

/* { path: Uint8Array } for every file of the cart's folder */
export function unpackCart(bytes, inflateRaw, deflate) {
    const { chunks } = readCart(bytes);
    if (!chunks.SRC_) throw new Error('the cart holds no sources');
    const src = reader(inflateRaw(chunks.SRC_));
    const files = {};
    for (let n = src.u16(); n > 0; n--) {
        const name = src.str();
        files[name] = src.bytes(src.u32()).slice();
    }
    const rgb = firstPalette(chunks.PALS);
    const picture = (r) => {
        const w = r.u16(), h = r.u16(), z = r.bytes(r.u32());
        return encodeIndexedPng(w, h, lz4Decompress(z, w * h), rgb, deflate);
    };
    if ('sheet.png' in files) {
        if (!chunks.SHET) throw new Error('the cart names a sheet it lacks');
        files['sheet.png'] = picture(reader(chunks.SHET));
    }
    const images = Object.keys(files).filter((f) => /^images\/.*\.png$/.test(f)).sort();
    if (images.length) {
        const r = reader(chunks.IMGS || new Uint8Array(2));
        if (r.u16() !== images.length) throw new Error('the cart\'s images do not match its sources');
        for (const f of images) files[f] = picture(r);
    }
    return files;
}
