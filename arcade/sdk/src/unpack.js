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
export const DEFAULT_RGB = [
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

/* The names compile() writes into SRC_; any other is refused, so no name
 * reaches outside the folder */
const SOURCE_NAME = new RegExp('^(cart\\.json|sheet\\.png|logo\\.png|flags\\.json|objects\\.json|' +
    '[^/\\\\:*?"<>|\\x00-\\x1f.][^/\\\\:*?"<>|\\x00-\\x1f]*\\.js|' +
    '(palettes/[A-Za-z_]\\w*\\.gpl|(maps|sounds|data)/[A-Za-z_]\\w*\\.json|' +
    'data/[A-Za-z_]\\w*\\.txt|fonts/[A-Za-z_]\\w*\\.bdf|editors/[A-Za-z_]\\w*\\.html|' +
    'images/[A-Za-z_]\\w*\\.png))$');

/* The cart's logo as { width, height, rgb }, each pixel 0xRRGGBB in the
 * cart's first palette; null when it has none */
export function cartLogo(bytes) {
    const { chunks } = readCart(bytes);
    if (!chunks.LOGO) return null;
    const r = reader(chunks.LOGO), w = r.u16(), h = r.u16();
    const px = lz4Decompress(r.bytes(r.u32()), w * h), pal = firstPalette(chunks.PALS);
    const rgb = new Uint32Array(w * h);
    for (let i = 0; i < px.length; i++) rgb[i] = pal[px[i]] || 0;
    return { width: w, height: h, rgb: rgb };
}

/* { path: Uint8Array } for every file of the cart's folder */
export function unpackCart(bytes, inflateRaw, deflate) {
    const { chunks, list } = readCart(bytes);
    if (!chunks.SRC_) throw new Error('the cart holds no sources');
    const src = reader(inflateRaw(chunks.SRC_));
    const files = {};
    for (let n = src.u16(); n > 0; n--) {
        const name = src.str();
        if (!SOURCE_NAME.test(name))
            throw new Error('the cart names a file no cart folder holds: ' +
                            JSON.stringify(name));
        files[name] = src.bytes(src.u32()).slice();
    }
    /* a cart built before SRC_ kept its assets has them only as chunks */
    const has = (prefix) => Object.keys(files).some((f) => f.startsWith(prefix));
    const lost = [['PALS', 'palettes/'], ['MAPS', 'maps/'], ['IMGS', 'images/'],
                  ['SFX_', 'sounds/'], ['DATA', 'data/'], ['FONT', 'fonts/']]
        .filter(([tag, dir]) => chunks[tag] && !has(dir));
    if ((chunks.SHET && !has('sheet.png') && !has('flags.json')) || lost.length)
        throw new Error('this cart was built before carts kept their pictures, maps ' +
                        'and sounds whole; build it again from its folder with pbgc');
    const rgb = firstPalette(chunks.PALS);
    const picture = (r) => {
        const w = r.u16(), h = r.u16(), z = r.bytes(r.u32());
        return encodeIndexedPng(w, h, lz4Decompress(z, w * h), rgb, deflate);
    };
    if ('sheet.png' in files) {
        if (!chunks.SHET) throw new Error('the cart names a sheet it lacks');
        files['sheet.png'] = picture(reader(chunks.SHET));
    }
    if ('logo.png' in files) {
        if (!chunks.LOGO) throw new Error('the cart names a logo it lacks');
        files['logo.png'] = picture(reader(chunks.LOGO));
    }
    const images = Object.keys(files).filter((f) => /^images\/.*\.png$/.test(f)).sort();
    if (images.length) {
        const r = reader(chunks.IMGS || new Uint8Array(2));
        if (r.u16() !== images.length) throw new Error('the cart\'s images do not match its sources');
        /* format 3 flags each image, a lazy one's pixels in an IMGC */
        const format3 = chunks.PBG1 && (chunks.PBG1[0] | chunks.PBG1[1] << 8) >= 3;
        const lazy = list.filter((c) => c.tag === 'IMGC').map((c) => c.data);
        for (const f of images) {
            const w = r.u16(), h = r.u16(), n = r.u32();
            const z = format3 && r.u8() ? lazy.shift() : r.bytes(n);
            if (!z || z.length !== n) throw new Error('the cart\'s images are damaged');
            files[f] = encodeIndexedPng(w, h, lz4Decompress(z, w * h), rgb, deflate);
        }
    }
    return files;
}
