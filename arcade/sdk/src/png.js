/* Indexed PNGs: the sheet and a cart's images are kept as their pixels'
 * palette indices, exactly. inflate and deflate are the zlib coders of the
 * host (Node's, or the IDE's), for the IDAT data with its zlib header. */

const SIGNATURE = [137, 80, 78, 71, 13, 10, 26, 10];

function u32be(b, i) {
    return (b[i] << 24 | b[i + 1] << 16 | b[i + 2] << 8 | b[i + 3]) >>> 0;
}

/* { width, height, px } with one index a pixel; throws an Error saying
 * what to change */
export function decodeIndexedPng(bytes, inflate) {
    for (let i = 0; i < 8; i++)
        if (bytes[i] !== SIGNATURE[i])
            throw new Error('not a PNG file');
    let at = 8, ihdr = null;
    const idat = [];
    while (at + 8 <= bytes.length) {
        const len = u32be(bytes, at);
        const type = String.fromCharCode(...bytes.subarray(at + 4, at + 8));
        const data = bytes.subarray(at + 8, at + 8 + len);
        if (data.length !== len) throw new Error('the PNG file is cut short');
        if (type === 'IHDR') ihdr = data;
        else if (type === 'IDAT') idat.push(data);
        else if (type === 'IEND') break;
        at += 12 + len;
    }
    if (!ihdr) throw new Error('the PNG file has no header');
    const width = u32be(ihdr, 0), height = u32be(ihdr, 4);
    const depth = ihdr[8], colour = ihdr[9], interlace = ihdr[12];
    const indexed = colour === 3 && [1, 2, 4, 8].includes(depth);
    const grey = colour === 0 && depth === 8;
    if (!indexed && !grey)
        throw new Error('save it as an indexed (palette) PNG: its pixels must ' +
                        'be palette numbers');
    if (interlace)
        throw new Error('save it without interlacing');
    let total = 0;
    for (const d of idat) total += d.length;
    const z = new Uint8Array(total);
    let o = 0;
    for (const d of idat) { z.set(d, o); o += d.length; }
    const raw = inflate(z);
    const stride = Math.ceil(width * depth / 8);
    if (raw.length < (stride + 1) * height)
        throw new Error('the PNG file\'s pixels are cut short');

    /* Unfilter, a byte to the pixel at most: bpp is 1 */
    const rows = new Uint8Array(stride * height);
    for (let y = 0; y < height; y++) {
        const f = raw[y * (stride + 1)];
        const src = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
        const cur = rows.subarray(y * stride, (y + 1) * stride);
        const up = y ? rows.subarray((y - 1) * stride, y * stride) : null;
        for (let x = 0; x < stride; x++) {
            const a = x ? cur[x - 1] : 0, b = up ? up[x] : 0;
            const c = x && up ? up[x - 1] : 0;
            let v = src[x];
            if (f === 1) v += a;
            else if (f === 2) v += b;
            else if (f === 3) v += (a + b) >> 1;
            else if (f === 4) {
                const pa = Math.abs(b - c), pb = Math.abs(a - c);
                const pc = Math.abs(a + b - 2 * c);
                v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            } else if (f !== 0)
                throw new Error('the PNG file has an unknown filter');
            cur[x] = v & 0xff;
        }
    }
    const px = new Uint8Array(width * height);
    const per = 8 / depth, mask = (1 << depth) - 1;
    for (let y = 0; y < height; y++)
        for (let x = 0; x < width; x++) {
            const byte = rows[y * stride + Math.floor(x / per)];
            px[y * width + x] = byte >> (8 - depth * (x % per + 1)) & mask;
        }
    return { width: width, height: height, px: px };
}

/* a chunk's CRC, over its type and then its data */
function crc(type, data) {
    let c = 0xffffffff;
    const add = (b) => {
        c ^= b;
        for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ 0xedb88320 : c >>> 1;
    };
    type.forEach(add);
    data.forEach(add);
    return (c ^ 0xffffffff) >>> 0;
}

/* An 8-bit indexed PNG of px, w x h, with rgb (0xrrggbb a colour) as its
 * palette -- as many entries as the pixels reach -- and index 0
 * transparent */
export function encodeIndexedPng(width, height, px, rgb, deflate) {
    let top = 0;
    for (const v of px) if (v > top) top = v;
    const n = Math.max(rgb.length, top + 1);
    const plte = new Uint8Array(n * 3);
    for (let i = 0; i < n; i++) {
        const c = rgb[i] || 0;
        plte[i * 3] = c >> 16 & 0xff; plte[i * 3 + 1] = c >> 8 & 0xff; plte[i * 3 + 2] = c & 0xff;
    }
    const raw = new Uint8Array(height * (width + 1));
    for (let y = 0; y < height; y++)
        raw.set(px.subarray(y * width, (y + 1) * width), y * (width + 1) + 1);
    const ihdr = new Uint8Array(13);
    const dv = new DataView(ihdr.buffer);
    dv.setUint32(0, width); dv.setUint32(4, height);
    ihdr.set([8, 3, 0, 0, 0], 8);
    const parts = [new Uint8Array(SIGNATURE)];
    const chunk = (type, data) => {
        const head = new Uint8Array(8);
        new DataView(head.buffer).setUint32(0, data.length);
        for (let i = 0; i < 4; i++) head[4 + i] = type.charCodeAt(i);
        const tail = new Uint8Array(4);
        new DataView(tail.buffer).setUint32(0, crc(head.subarray(4), data));
        parts.push(head, data, tail);
    };
    chunk('IHDR', ihdr);
    chunk('PLTE', plte);
    chunk('tRNS', new Uint8Array([0]));
    chunk('IDAT', deflate(raw));
    chunk('IEND', new Uint8Array(0));
    let len = 0;
    for (const p of parts) len += p.length;
    const out = new Uint8Array(len);
    let at = 0;
    for (const p of parts) { out.set(p, at); at += p.length; }
    return out;
}
