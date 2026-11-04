/* Reading an indexed PNG: the sheet and a cart's images are kept as their
 * pixels' palette indices, exactly. inflate is the zlib decoder of the
 * host (Node's, or the IDE's), given the IDAT data with its zlib header. */

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
