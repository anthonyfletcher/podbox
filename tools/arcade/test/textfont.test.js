/* FONT.WORLD: text beyond ASCII, through pbgvm -- the engine's own glyphs,
 * and the font file's, from a small one made here. */

import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { compileFiles } from '../src/node.js';
import { fontGlyphs } from '../src/runner.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const host = path.join(here, '..', 'host');
const exe = path.join(host, process.platform === 'win32' ? 'pbgvm.exe' : 'pbgvm');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-textfont-'));

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

/* A font file holding one glyph, U+3042: advance 8, a full 8x8 block on
 * rows 1 to 8 -- the format gen_textfont.py --file writes */
function fontFile() {
    const head = Buffer.from([0x50, 0x42, 0x54, 0x46, 1, 12, 9, 0]);
    const table = Buffer.alloc(256 * 36);
    const offset = head.length + table.length;
    table.writeUInt32LE(offset, 0x30 * 36);
    table[0x30 * 36 + 4 + (0x42 >> 3)] |= 1 << (0x42 & 7);
    const glyph = Buffer.from([8, 0, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0]);
    const file = path.join(tmp, 'font.bin');
    fs.writeFileSync(file, Buffer.concat([head, table, glyph]));
    return file;
}

let n = 0;
function run(main, extra = []) {
    const res = compileFiles({
        'cart.json': JSON.stringify({ id: '00000000000f0e75', name: 'Text' }),
        'main.js': main + '\nfunction update() {}' });
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(tmp, 't' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '1'].concat(extra), { encoding: 'utf8' });
    return { out: r.stdout.replace(/\r/g, ''), err: r.stderr, status: r.status };
}

const COUNT = `function count(c) {
    var k = 0;
    for (var y = 0; y < 120; y++) for (var x = 0; x < 160; x++) if (pget(x, y) == c) k++;
    return k;
}`;

test('the engine\'s own letters: accented, Greek, Cyrillic', () => {
    const r = run(COUNT + `
function init() {
    font(FONT.WORLD);
    printh(textw("A") + " " + textw("AÅ") + " " + textw("Ж") + " " + textw("Ω") + " " + textw("??"));
    cls(0); print("Ж", 10, 10, 7); var ink = count(7);
    cls(0); print("Å", 10, 10, 7); var ring = count(7);
    cls(0); print("A", 10, 10, 7);
    printh((ink > 8) + " " + (ring > count(7)));
}`);
    assert.equal(r.status, 0, r.err);
    const [w, drawn] = r.out.trim().split('\n');
    const [a, aa, zhe, omega, qq] = w.split(' ').map(Number);
    /* Å is A with its ring above: as wide; Ж and Ω are their own, not ? */
    assert.equal(aa, 2 * a);
    assert.ok(zhe >= 5 && omega >= 4, w);
    assert.equal(drawn, '1 1');
    assert.ok(qq > 0);
});

test('a glyph from the font file, and ? without it', () => {
    const file = fontFile();
    const main = COUNT + `
function init() {
    font(FONT.WORLD);
    cls(0); print("あ", 0, 0, 7);
    printh(textw("あ") + " " + count(7) + " " + textw("ぃ") + " " + textw("?"));
}`;
    const withFile = run(main, ['--font', file]);
    assert.equal(withFile.status, 0, withFile.err);
    const [w, ink, other, q] = withFile.out.trim().split(' ').map(Number);
    /* the block drawn whole; a codepoint the file lacks is ? */
    assert.deepEqual([w, ink, other], [8, 64, q]);
    const without = run(main);
    assert.deepEqual(without.out.trim().split(' ').map(Number).slice(0, 1), [q]);
});

test('a damaged font file gives no glyph, never a short or stray one', () => {
    const whole = fs.readFileSync(fontFile());
    const damaged = {
        glyph: whole.subarray(0, whole.length - 5),
        table: whole.subarray(0, 8 + 100),
        magic: Buffer.concat([Buffer.from('PBTX'), whole.subarray(4)]),
        version: Buffer.concat([whole.subarray(0, 4), Buffer.from([2]), whole.subarray(5)])
    };
    const main = `function init() { font(FONT.WORLD); printh(textw("あ") + " " + textw("?")); }`;
    for (const [what, bytes] of Object.entries(damaged)) {
        const file = path.join(tmp, 'font-' + what + '.bin');
        fs.writeFileSync(file, bytes);
        const r = run(main, ['--font', file]);
        assert.equal(r.status, 0, what + ': ' + r.err);
        const [w, q] = r.out.trim().split(' ');
        assert.equal(w, q, what);
        const glyph = fontGlyphs(new Uint8Array(bytes));
        assert.equal(glyph(0x3042), undefined, what);
        assert.equal(glyph(0), undefined, what);
    }
    assert.equal(fontGlyphs(new Uint8Array(whole))(0x3042).length, 13);
});

test('a line is 12 rows, and font() knows WORLD', () => {
    const r = run(`function init() {
    font(FONT.WORLD);
    cls(0);
    cursor(0, 0);
    print("A\\nA", 7);
    // A's left side, on rows 4 to 8 of its line: the second line 12 down
    printh(pget(0, 5) + " " + pget(0, 17) + " " + pget(0, 11) + " " + (textw("ab\\nabcd") == textw("abcd")));
}`);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out.trim(), '7 7 0 1');
    assert.match(run('function init() { font(3); }').err, /FONT.CAPS, FONT.WORLD or one of the game's fonts/);
});

test('size 2 doubles either font, and is 1 or 2', () => {
    const r = run(COUNT + `
function init() {
    var out = "";
    font(FONT.CAPS); var c1 = textw("AB"); cls(0); print("A", 0, 0, 7); var i1 = count(7);
    font(FONT.CAPS, 2); var c2 = textw("AB"); cls(0); print("A", 0, 0, 7); var i2 = count(7);
    font(FONT.WORLD); var w1 = textw("AÅ"); cls(0); print("Ж", 0, 0, 7); var j1 = count(7);
    font(FONT.WORLD, 2); var w2 = textw("AÅ"); cls(0); print("Ж", 0, 0, 7); var j2 = count(7);
    cursor(0, 0); print("A\\nA", 7);
    printh(c2 / c1 + " " + i2 / i1 + " " + w2 / w1 + " " + j2 / j1 + " " + (textw("A\\nA") == w2 / 2));
    cls(0); cursor(0, 0); print("A\\nA", 7);
    // the second line 24 rows down: A's left side on rows 32 to 41
    printh(pget(0, 34) + " " + pget(0, 22));
}`);
    assert.equal(r.status, 0, r.err);
    assert.deepEqual(r.out.trim().split('\n'), ['2 4 2 4 1', '7 0']);
    assert.match(run('function init() { font(FONT.CAPS, 3); }').err, /size is 1 or 2/);
    assert.match(run('function init() { font(FONT.CAPS, 0); }').err, /size is 1 or 2/);
});
