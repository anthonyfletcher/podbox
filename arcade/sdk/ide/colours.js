/* A picture from elsewhere brought to the palette: which of its colours
 * the palette lacks, the nearest match dithered, and a few colours chosen
 * to stand for many (median cut) when they are to be added. toPalette, in
 * pixels.js, is the plain nearest match. */

import { shown } from './pixels.js';

/* the opaque colours of RGBA pixels that are not in the palette, as the
 * player shows it or exactly: { colours (colour -> pixels), n, pixels } */
export function missing(rgba, rgb) {
    const have = new Set();
    for (const c of rgb) { have.add(c); have.add(shown(c)); }
    const colours = new Map();
    let pixels = 0;
    for (let i = 0; i < rgba.length; i += 4) {
        if (rgba[i + 3] < 128) continue;
        const c = rgba[i] << 16 | rgba[i + 1] << 8 | rgba[i + 2];
        if (have.has(c)) continue;
        colours.set(c, (colours.get(c) || 0) + 1);
        pixels++;
    }
    return { colours, n: colours.size, pixels };
}

function nearestIndex(rgb, r, g, b) {
    let best = 0, bestD = Infinity;
    for (let i = 0; i < rgb.length; i++) {
        const c = rgb[i], cr = c >> 16 & 255, cg = c >> 8 & 255, cb = c & 255;
        const rm = (r + cr) / 2, dr = r - cr, dg = g - cg, db = b - cb;
        const d = (2 + rm / 256) * dr * dr + 4 * dg * dg + (2 + (255 - rm) / 256) * db * db;
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

/* the nearest colours, each pixel's error spread to its neighbours yet to
 * come (Floyd and Steinberg), so a shade between two colours reads as a
 * mix of them; transparent pixels are 0 */
export function dither(rgba, w, h, rgb) {
    const err = new Float32Array(w * h * 3);
    const px = new Uint8Array(w * h);
    const spread = (x, y, er, eg, eb, f) => {
        if (x < 0 || x >= w || y >= h) return;
        const k = (y * w + x) * 3;
        err[k] += er * f; err[k + 1] += eg * f; err[k + 2] += eb * f;
    };
    for (let y = 0; y < h; y++)
        for (let x = 0; x < w; x++) {
            const i = y * w + x;
            if (rgba[i * 4 + 3] < 128) { px[i] = 0; continue; }
            const r = Math.max(0, Math.min(255, rgba[i * 4] + err[i * 3]));
            const g = Math.max(0, Math.min(255, rgba[i * 4 + 1] + err[i * 3 + 1]));
            const b = Math.max(0, Math.min(255, rgba[i * 4 + 2] + err[i * 3 + 2]));
            const k = nearestIndex(rgb, r, g, b), c = rgb[k];
            px[i] = k;
            const er = r - (c >> 16 & 255), eg = g - (c >> 8 & 255), eb = b - (c & 255);
            spread(x + 1, y, er, eg, eb, 7 / 16);
            spread(x - 1, y + 1, er, eg, eb, 3 / 16);
            spread(x, y + 1, er, eg, eb, 5 / 16);
            spread(x + 1, y + 1, er, eg, eb, 1 / 16);
        }
    return px;
}

/* k colours standing for many: the colours, weighted by how many pixels
 * have them, split box by box along their widest channel at the middle
 * pixel, each box then its average */
export function medianCut(colours, k) {
    const all = [...colours].map(([c, n]) => [c >> 16 & 255, c >> 8 & 255, c & 255, n]);
    if (all.length <= k) return all.map(([r, g, b]) => r << 16 | g << 8 | b);
    let boxes = [all];
    const widest = (box) => {
        let best = 0, axis = 0;
        for (let a = 0; a < 3; a++) {
            let lo = 255, hi = 0;
            for (const c of box) { lo = Math.min(lo, c[a]); hi = Math.max(hi, c[a]); }
            if (hi - lo > best) { best = hi - lo; axis = a; }
        }
        return { range: best, axis };
    };
    while (boxes.length < k) {
        /* the box with the most to split: its range times its pixels */
        let pick = -1, score = -1, axis = 0;
        boxes.forEach((box, i) => {
            if (box.length < 2) return;
            const wd = widest(box), n = box.reduce((s, c) => s + c[3], 0);
            if (wd.range * n > score) { score = wd.range * n; pick = i; axis = wd.axis; }
        });
        if (pick < 0) break;
        const box = boxes[pick].sort((p, q) => p[axis] - q[axis]);
        const half = box.reduce((s, c) => s + c[3], 0) / 2;
        let run = 0, cut = 1;
        for (let i = 0; i < box.length - 1; i++) {
            run += box[i][3];
            if (run >= half) { cut = i + 1; break; }
            cut = i + 1;
        }
        boxes.splice(pick, 1, box.slice(0, cut), box.slice(cut));
    }
    return boxes.map((box) => {
        const n = box.reduce((s, c) => s + c[3], 0);
        const avg = (a) => Math.round(box.reduce((s, c) => s + c[a] * c[3], 0) / n);
        return avg(0) << 16 | avg(1) << 8 | avg(2);
    });
}
