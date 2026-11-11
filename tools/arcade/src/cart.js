/* The .pbg container: little-endian chunks, each a 4-byte tag, a u32
 * length, the data and a CRC32 of the data. The first chunk is PBG1, which
 * doubles as the file's magic. */

const CRC_TABLE = (function () {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
        let c = n;
        for (let k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
        t[n] = c >>> 0;
    }
    return t;
})();

export function crc32(bytes) {
    let c = 0xffffffff;
    for (let i = 0; i < bytes.length; i++)
        c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
}

/* A growable little-endian byte buffer. */
export class Writer {
    constructor() { this.buf = new Uint8Array(256); this.len = 0; }
    room(n) {
        if (this.len + n <= this.buf.length) return;
        let size = this.buf.length * 2;
        while (size < this.len + n) size *= 2;
        const b = new Uint8Array(size);
        b.set(this.buf.subarray(0, this.len));
        this.buf = b;
    }
    u8(v) { this.room(1); this.buf[this.len++] = v & 0xff; }
    u16(v) { this.u8(v); this.u8(v >> 8); }
    u32(v) { this.u16(v); this.u16(v >>> 16); }
    bytes(b) { this.room(b.length); this.buf.set(b, this.len); this.len += b.length; }
    /* u16 length, then UTF-8 */
    str(s) { const b = utf8(s); this.u16(b.length); this.bytes(b); }
    patch16(at, v) { this.buf[at] = v & 0xff; this.buf[at + 1] = (v >> 8) & 0xff; }
    done() { return this.buf.slice(0, this.len); }
}

export function utf8(s) { return new TextEncoder().encode(s); }

export function writeCart(chunks) {
    const w = new Writer();
    for (const c of chunks) {
        if (c.tag.length !== 4) throw new Error('chunk tag ' + c.tag);
        for (let i = 0; i < 4; i++) w.u8(c.tag.charCodeAt(i));
        w.u32(c.data.length);
        w.bytes(c.data);
        w.u32(crc32(c.data));
    }
    return w.done();
}

/* Returns { chunks: { tag: data }, order: tags in file order, list: each
 * { tag, data } in file order, chunks a tag repeats (MAPC, IMGC) all
 * there }; throws on a malformed file. */
export function readCart(bytes) {
    const chunks = {};
    const order = [];
    const list = [];
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let at = 0;
    while (at < bytes.length) {
        if (at + 8 > bytes.length) throw new Error('truncated chunk header');
        const tag = String.fromCharCode(bytes[at], bytes[at + 1],
                                        bytes[at + 2], bytes[at + 3]);
        const len = dv.getUint32(at + 4, true);
        if (at + 12 + len > bytes.length)
            throw new Error('chunk ' + tag + ' runs past the end');
        const data = bytes.subarray(at + 8, at + 8 + len);
        if (dv.getUint32(at + 8 + len, true) !== crc32(data))
            throw new Error('chunk ' + tag + ' fails its CRC');
        if (order.length === 0 && tag !== 'PBG1')
            throw new Error('not a cart: no PBG1 header');
        chunks[tag] = data;
        order.push(tag);
        list.push({ tag: tag, data: data });
        at += 12 + len;
    }
    return { chunks: chunks, order: order, list: list };
}
