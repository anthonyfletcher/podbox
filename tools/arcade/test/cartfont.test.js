/* A cart's own fonts: fonts/<name>.bdf, built into the FONT chunk and
 * drawn by pbgvm. */

import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { compileFiles } from '../src/node.js';
import zlib from 'node:zlib';
import { readCart, writeCart } from '../src/cart.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const host = path.join(here, '..', 'host');
const exe = path.join(host, process.platform === 'win32' ? 'pbgvm.exe' : 'pbgvm');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-cartfont-'));
const CART = JSON.stringify({ id: '00000000000f0e76', name: 'Fonts' });
const CAPS = fs.readFileSync(path.join(here, '..', 'fonts', 'caps.bdf'), 'utf8');

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

/* A BDF from glyphs: [codepoint, advance, w, h, xoff, yoff, rows of # and .] */
function bdf(ascent, descent, glyphs) {
    const out = ['STARTFONT 2.1', 'STARTPROPERTIES 2', 'FONT_ASCENT ' + ascent,
                 'FONT_DESCENT ' + descent, 'ENDPROPERTIES', 'CHARS ' + glyphs.length];
    for (const [cp, adv, w, h, xo, yo, rows] of glyphs) {
        out.push('STARTCHAR c' + cp, 'ENCODING ' + cp, 'DWIDTH ' + adv + ' 0',
                 'BBX ' + [w, h, xo, yo].join(' '), 'BITMAP');
        const nb = (w + 7) >> 3;
        for (const r of rows) {
            let v = 0n;
            for (let x = 0; x < w; x++) if (r[x] === '#') v |= 1n << BigInt(nb * 8 - 1 - x);
            out.push(v.toString(16).toUpperCase().padStart(nb * 2, '0'));
        }
        out.push('ENDCHAR');
    }
    out.push('ENDFONT');
    return out.join('\n');
}

let n = 0;
function build(main, fonts) {
    return compileFiles({ 'cart.json': CART, 'main.js': main + '\nfunction update() {}' },
                        { fonts: fonts });
}
function run(main, fonts, extra = []) {
    const res = build(main, fonts);
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(tmp, 'f' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '1'].concat(extra), { encoding: 'utf8' });
    return { out: r.stdout.replace(/\r/g, ''), err: r.stderr, status: r.status, file: file };
}
function shot(main, fonts) {
    const png = path.join(tmp, 's' + n + '.png');
    const r = run(main, fonts, ['--shot', png]);
    assert.equal(r.status, 0, r.err || JSON.stringify(r.errors));
    return fs.readFileSync(png);
}

const COUNT = `function count(c) {
    var k = 0;
    for (var y = 0; y < 120; y++) for (var x = 0; x < 160; x++) if (pget(x, y) == c) k++;
    return k;
}`;

test('caps.bdf as a cart font draws exactly FONT.CAPS, at both sizes', () => {
    let ascii = '';
    for (let c = 32; c < 127; c++) ascii += String.fromCharCode(c);
    ascii = ascii.replace(/\\/g, '\\\\').replace(/"/g, '\\"');
    const page = (f) => `function init() {
    cls(1);
    font(${f}); print("${ascii.slice(0, 40)}\\n${ascii.slice(40)}\\nÉTÉ", 0, 0, 7);
    font(${f}, 2); print("${ascii.slice(0, 20)}\\nWIDE", 0, 30, 8);
    clip(10, 80, 59, 88); print("CLIPPED AT BOTH ENDS", 6, 82, 9); clip();
    print("OFF THE EDGE", 140, 110, 10);
    printh(textw("Hello") + " " + texth("a\\nb") + " " + print("x", 0, 100));
}`;
    const mine = shot(page('FONT.caps'), { caps: CAPS });
    const engine = shot(page('FONT.CAPS'), { caps: CAPS });
    assert.ok(mine.equals(engine));
    assert.deepEqual(run(page('FONT.caps'), { caps: CAPS }).out,
                     run(page('FONT.CAPS'), {}).out);
});

/* a 20x24 block, its top 2 rows below the line's top and 3 in from the
 * left; a 2x2 dot standing on the baseline; and a ? */
const BIG = bdf(26, 4, [
    [0x41, 24, 20, 24, 3, 0, Array(24).fill('#'.repeat(20))],
    [0x2e, 4, 2, 2, 1, 0, ['##', '##']],
    [0x3f, 10, 8, 8, 0, 10, Array(8).fill('#.#.#.#.')]
]);

test('a big font: the line, each glyph\'s box and advance, ? for the rest', () => {
    const r = run(COUNT + `
function init() {
    font(FONT.big);
    printh(texth("A") + " " + texth("A\\nA") + " " + textw("A.") + " " + textw("AZ"));
    cls(0); print("A", 10, 10, 7); printh(count(7) + " " + pget(13, 12) + " " + pget(12, 12) + " " + pget(13, 11) + " " + pget(32, 35) + " " + pget(33, 35));
    cls(0); print(".", 0, 0, 7); printh(count(7) + " " + pget(1, 24) + " " + pget(1, 25) + " " + pget(1, 26));
    cls(0); print("Z", 0, 0, 7); printh(count(7));
    font(FONT.big, 2); cls(0); print("A", 0, 0, 7); printh(count(7) + " " + textw("A") + " " + texth("A"));
    cls(0); print("A", -10, -10, 7); printh(count(7));
}`, { big: BIG });
    assert.equal(r.status, 0, r.err || JSON.stringify(r.errors));
    assert.deepEqual(r.out.trim().split('\n'), [
        /* the line 26 + 4; A then . is 24 + 4; Z is the ?'s 10 */
        '30 60 28 34',
        /* 20x24 from (13, 10): inside at its corners, nothing outside */
        '480 7 0 0 7 0',
        /* the dot: rows 24 and 25, the baseline under 25 */
        '4 7 7 0',
        /* Z is the ?: 32 dots */
        '32',
        /* doubled: four times the dots */
        '1920 48 60',
        /* off the top left: from (-10 + 6, -10 + 4), 40x48, so 36 columns
         * and 42 rows on the screen */
        String(36 * 42)
    ]);
});

test('without a ?, a character the font lacks is left out', () => {
    const r = run(`function init() { font(FONT.dot); printh(textw("..x..") + " " + textw("....")); }`,
                  { dot: bdf(2, 0, [[0x2e, 3, 2, 2, 0, 0, ['##', '##']]]) });
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out.trim(), '12 12');
});

test('FONT.<name> numbers each font after the engine\'s; font() refuses others', () => {
    const r = run(`function init() { printh(FONT.CAPS + " " + FONT.WORLD + " " + FONT.a + " " + FONT.b); font(FONT.b); font(7); }`,
                  { b: CAPS, a: CAPS });
    assert.equal(r.out.trim(), '0 2 3 4');
    assert.match(r.err, /FONT.CAPS, FONT.WORLD or one of the game's fonts/);
    assert.equal(run('function init() { font(3); }', {}).status, 1);
});

test('a font that cannot be built says why', () => {
    const why = (fonts) => build('function init() {}', fonts).errors.map((e) => e.file + ': ' + e.message);
    assert.match(why({ CAPS: CAPS })[0], /fonts\/CAPS.bdf: FONT.CAPS is the engine's/);
    assert.match(why({ x: 'STARTFONT 2.1\nENDFONT' })[0], /FONT_ASCENT and FONT_DESCENT/);
    assert.match(why({ x: bdf(20, 20, [[65, 4, 1, 1, 0, 0, ['#']]]) })[0], /line .* is 1 to 32 pixels, not 40/);
    assert.match(why({ x: bdf(8, 0, [[65, 40, 33, 1, 0, 0, ['#'.repeat(33)]]]) })[0],
                 /U\+0041 is 33x1; a glyph is at most 32/);
    const many = [];
    for (let cp = 0x4e00; cp < 0x4e00 + 1700; cp++) many.push([cp, 16, 16, 16, 0, 0, Array(16).fill('#'.repeat(16))]);
    assert.match(why({ x: bdf(16, 0, many) })[0], /a cart's are at most 65536/);
    const fonts = {};
    for (let i = 0; i < 17; i++) fonts['f' + i] = CAPS;
    assert.match(why(fonts)[0], /at most 16 fonts/);
});

test('a dialog in a font that lacks its letters is drawn in WORLD', () => {
    const digits = bdf(5, 1, '0123456789'.split('').map((d) =>
        [d.charCodeAt(0), 4, 3, 5, 0, 0, ['###', '#.#', '#.#', '#.#', '###']]));
    const a = shot('function init() { cls(3); font(FONT.d); ui.message("Hello"); }', { d: digits });
    const b = shot('function init() { cls(3); font(FONT.WORLD); ui.message("Hello"); }', { d: digits });
    assert.ok(a.equals(b));
});

test('the cart keeps its BDF in SRC_, and a damaged FONT is refused, never crashes', () => {
    const res = build(`function init() {} function draw() { cls(0); font(FONT.big); print("A.?Z", 0, 0, 7); font(FONT.big, 2); print("A.", -5, 50, 8); }`,
                      { big: BIG });
    assert.deepEqual(res.errors, []);
    const { chunks, order } = readCart(res.cart);
    assert.ok(order.includes('FONT'));
    assert.ok(zlib.inflateRawSync(chunks.SRC_).toString().includes('fonts/big.bdf'));
    const file = path.join(tmp, 'mutant.pbg');
    let refused = 0, ran = 0;
    let seed = 99;
    const r = (k) => { seed = (seed * 1103515245 + 12345) >>> 0; return (seed >>> 8) % k; };
    for (let i = 0; i < 200; i++) {
        const data = chunks.FONT.slice();
        for (let k = 1 + r(3); k > 0; k--) data[r(data.length)] = r(256);
        fs.writeFileSync(file, writeCart(order.map((t) => ({ tag: t, data: t === 'FONT' ? data : chunks[t] }))));
        const out = spawnSync(exe, [file, '--ticks', '2'], { encoding: 'utf8', timeout: 20000 });
        assert.equal(out.signal, null, 'mutant ' + i + ' hung or was killed');
        assert.ok(out.status === 0 || out.status === 1, 'mutant ' + i + ' crashed: ' + out.status);
        if (/damaged/.test(out.stderr)) refused++; else ran++;
    }
    assert.ok(refused > 0 && ran > 0, 'refused ' + refused + ', ran ' + ran);
});
