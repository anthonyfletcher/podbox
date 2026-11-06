/* FONT.TEXT: text beyond ASCII, through pbgvm -- the engine's own glyphs,
 * and the font file's, from a small one made here. */

import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { compileFiles } from '../src/node.js';

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
    font(FONT.TEXT);
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
    font(FONT.TEXT);
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

test('a line is 12 rows, and font() knows TEXT', () => {
    const r = run(`function init() {
    font(FONT.TEXT);
    cls(0);
    cursor(0, 0);
    print("A\\nA", 7);
    // A's left side, on rows 4 to 8 of its line: the second line 12 down
    printh(pget(0, 5) + " " + pget(0, 17) + " " + pget(0, 11) + " " + (textw("ab\\nabcd") == textw("abcd")));
}`);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out.trim(), '7 7 0 1');
    assert.match(run('function init() { font(3); }').err, /FONT.SMALL, FONT.BIG or FONT.TEXT/);
});
