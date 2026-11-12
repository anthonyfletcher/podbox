/* The IDE's colour matching: what a picture has that the palette lacks,
 * the dithered match, and the colours chosen to stand for many. */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { missing, dither, medianCut } from '../ide/colours.js';
import { toPalette, shown } from '../ide/pixels.js';

const rgba = (cs) => {
    const out = new Uint8Array(cs.length * 4);
    cs.forEach((c, i) => {
        out.set(c === null ? [0, 0, 0, 0] : [c >> 16, c >> 8 & 255, c & 255, 255], i * 4);
    });
    return out;
};

test('missing: colours not in the palette, as the player shows it or exactly; clear ones ignored', () => {
    const rgb = [0x000000, 0xffffff, 0x1a2244];
    const m = missing(rgba([0x000000, 0xff0000, 0xff0000, null, shown(0x1a2244), 0x00ff00]), rgb);
    assert.equal(m.n, 2);
    assert.equal(m.pixels, 3);
    assert.equal(m.colours.get(0xff0000), 2);
});

test('toPalette: exact entries kept, the rest the nearest, clear as 0', () => {
    const { px, near } = toPalette(rgba([0xffffff, 0xf0f0f0, null, 0x101010]), [0x000000, 0x777777, 0xffffff]);
    assert.deepEqual([...px], [2, 2, 0, 0]);
    assert.equal(near, 2);
});

test('dither: a grey between black and white becomes about half of each', () => {
    const w = 16, h = 16, px = dither(rgba(new Array(w * h).fill(0x808080)), w, h, [0x000000, 0xffffff]);
    const white = [...px].filter((v) => v === 1).length;
    assert.ok(white > w * h * 0.4 && white < w * h * 0.6, white + ' of ' + w * h);
    assert.ok([...px].every((v) => v === 0 || v === 1));
});

test('medianCut: as many colours as asked, each standing for its share of the pixels', () => {
    const colours = new Map();
    /* reds and blues, many pixels each, and two greens */
    for (let k = 0; k < 10; k++) colours.set((200 + k) << 16, 100);
    for (let k = 0; k < 10; k++) colours.set(200 + k, 100);
    colours.set(0x00ff00, 1).set(0x00f000, 1);
    const two = medianCut(colours, 2);
    assert.equal(two.length, 2);
    /* the two big groups win: one red, one blue */
    const reds = two.filter((c) => (c >> 16) > 150), blues = two.filter((c) => (c & 255) > 150);
    assert.equal(reds.length + blues.length, 2, two.map((c) => c.toString(16)).join(' '));
    assert.deepEqual(medianCut(colours, 30).length, 22);
});
