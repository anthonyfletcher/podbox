/* The IDE's font editor reads and writes BDF: a font read and written
 * again keeps every glyph, as the compiler reads them. */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { readFont, writeFont } from '../ide/font.js';
import { parseBdf } from '../src/bdf.js';

const here = path.dirname(fileURLToPath(import.meta.url));

/* each glyph's ink as the compiler places it: cp, advance and its dots
 * in line coordinates */
function dots(text) {
    return parseBdf(text).glyphs.map((g) => {
        const on = [];
        g.rows.forEach((row, r) => {
            for (let c = 0; c < g.w; c++)
                if (row[c >> 3] & (0x80 >> (c & 7))) on.push((g.x + c) + ',' + (g.y + r));
        });
        return g.cp + ':' + g.adv + ':' + on.join(' ');
    });
}

test('CAPS read and written again is the same font', () => {
    const caps = fs.readFileSync(path.join(here, '..', 'fonts', 'caps.bdf'), 'utf8');
    const again = writeFont(readFont(caps));
    assert.deepEqual(dots(again), dots(caps));
    assert.equal(parseBdf(again).line, parseBdf(caps).line);
});

test('WORLD\'s own glyphs, accents above and below the line, come back the same', () => {
    const world = fs.readFileSync(path.join(here, '..', 'fonts', 'world.bdf'), 'utf8');
    assert.deepEqual(dots(writeFont(readFont(world))), dots(world));
});

test('a glyph drawn in its cell is written trimmed to its ink', () => {
    const f = readFont(fs.readFileSync(path.join(here, '..', 'fonts', 'caps.bdf'), 'utf8'));
    const g = f.glyphs.find((x) => x.cp === 46);   /* . */
    g.cell.px.fill(0);
    g.cell.px[1 * g.cell.w + 2] = 1;                /* one dot, row 1, column 2 */
    const out = parseBdf(writeFont(f)).glyphs.find((x) => x.cp === 46);
    assert.deepEqual([out.w, out.h, out.x, out.y], [1, 1, 2, 1]);
});
