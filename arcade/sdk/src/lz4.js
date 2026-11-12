/* The LZ4 block format, which the cart's images and maps are stored in:
 * a greedy compressor, and a decompressor the tests check it with. The
 * engine's own decoder is lz4_decode() in loader.c. */

const MINMATCH = 4;
const LAST_LITERALS = 5;    /* a block ends with at least this many literals */
const MFLIMIT = 12;         /* and its last match starts this far from its end */

function read32(b, i) {
    return (b[i] | b[i + 1] << 8 | b[i + 2] << 16 | b[i + 3] << 24) >>> 0;
}

function hash(v) {
    return Math.imul(v, 2654435761) >>> 18;     /* 14 bits */
}

export function lz4Compress(src) {
    const n = src.length, out = [];
    const table = new Int32Array(1 << 14).fill(-1);
    let anchor = 0, i = 0;

    function length(extra) {
        for (; extra >= 255; extra -= 255) out.push(255);
        out.push(extra);
    }

    function sequence(lit, offset, mlen) {
        const ml = mlen ? mlen - MINMATCH : 0;
        out.push((lit >= 15 ? 15 : lit) << 4 | (mlen && ml >= 15 ? 15 : ml));
        if (lit >= 15) length(lit - 15);
        for (let k = 0; k < lit; k++) out.push(src[anchor + k]);
        if (!mlen) return;
        out.push(offset & 0xff, offset >> 8);
        if (ml >= 15) length(ml - 15);
    }

    while (i + MFLIMIT <= n) {
        const v = read32(src, i), h = hash(v), ref = table[h];
        table[h] = i;
        if (ref >= 0 && i - ref < 65536 && read32(src, ref) === v) {
            let len = MINMATCH;
            while (i + len < n - LAST_LITERALS && src[ref + len] === src[i + len])
                len++;
            sequence(i - anchor, i - ref, len);
            i += len;
            anchor = i;
        } else
            i++;
    }
    sequence(n - anchor, 0, 0);
    return Uint8Array.from(out);
}

/* null when the data is malformed or does not make exactly size bytes */
export function lz4Decompress(src, size) {
    const d = new Uint8Array(size);
    let s = 0, o = 0;
    while (s < src.length) {
        const t = src[s++];
        let lit = t >> 4;
        if (lit === 15) {
            let b;
            do {
                if (s >= src.length) return null;
                b = src[s++];
                lit += b;
            } while (b === 255);
        }
        if (lit > src.length - s || lit > size - o) return null;
        d.set(src.subarray(s, s + lit), o);
        s += lit;
        o += lit;
        if (s >= src.length) break;
        if (src.length - s < 2) return null;
        const off = src[s] | src[s + 1] << 8;
        s += 2;
        if (!off || off > o) return null;
        let m = (t & 15) + MINMATCH;
        if ((t & 15) === 15) {
            let b;
            do {
                if (s >= src.length) return null;
                b = src[s++];
                m += b;
            } while (b === 255);
        }
        if (m > size - o) return null;
        for (let k = 0; k < m; k++, o++) d[o] = d[o - off];
    }
    return o === size ? d : null;
}
