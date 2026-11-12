/* The grid system, through pbgvm: each function on boards worked by hand. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-grid-'));
const CART = JSON.stringify({ id: '0000000000641d00', name: 'Grid' });

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

let n = 0;
function run(main, ticks = 1, extra = []) {
    const res = compileFiles({ 'cart.json': CART, 'main.js': main });
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(tmp, 's' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', String(ticks)].concat(extra),
                        { encoding: 'utf8', timeout: 20000 });
    return { errors: [], out: r.stdout.replace(/\r/g, ''), err: r.stderr, status: r.status };
}


/* A board from rows of digits, and printh of it back as the same */
const BOARD = `
function board(rows) {
    var w = rows[0].length, b = new Uint8Array(w * rows.length);
    for (var y = 0; y < rows.length; y++)
        for (var x = 0; x < w; x++) b[y * w + x] = rows[y].charCodeAt(x) - 48;
    return b;
}
function show(b, w) {
    var s = "";
    for (var i = 0; i < b.length; i++) s += (i && i % w == 0 ? "/" : "") + b[i];
    printh(s);
}
function ints(a, n) { var s = ""; for (var i = 0; i < n; i++) s += (i ? "," : "") + a[i]; printh(s); }
`;
function grid(body) {
    const r = run(BOARD + 'function init() {\n' + body + '\n}\nfunction update() {}');
    assert.equal(r.status, 0, r.err || JSON.stringify(r.errors));
    return r.out.trim().split('\n');
}

test('fill changes the 4-connected region, and region lists it', () => {
    const out = grid(`var b = board(["1100", "0102", "0111", "2000"]);
    printh(grid.fill(b, 4, 1, 0, 5) + " " + grid.fill(b, 4, 0, 1, 0)); show(b, 4);
    var o = new Int32Array(3);
    printh(grid.region(b, 4, 3, 3, o)); ints(o, 3);
    printh(grid.region(b, 4, 9, 9, o));`);
    /* 1s joined by sides only: the diagonal 2 at 3,1 stays */
    /* breadth first from the start cell */
    assert.deepEqual(out, ['6 0', '5500/0502/0555/2000', '3', '15,14,13', '0']);
});

test('runs finds row and column runs once each, full and collapse clear rows', () => {
    const out = grid(`var b = board(["11122", "30102", "30142", "34444"]);
    var o = new Int32Array(20);
    var n = grid.runs(b, 5, 3, o); ints(o, n);
    var t = board(["00100", "11111", "10101", "11111"]), rows = new Int32Array(4);
    n = grid.full(t, 5, rows); ints(rows, n);
    printh(grid.collapse(t, 5, rows, n)); show(t, 5);`);
    /* the top 1s, the 4s across the bottom, the 1s, 3s and 2s down */
    assert.equal(out[0], '0,1,2,4,5,7,9,10,12,14,15,16,17,18,19');
    assert.deepEqual(out.slice(1), ['1,3', '2', '00000/00000/00100/10101']);
});

test('fall drops cells through gaps, slide merges as 2048 does', () => {
    const out = grid(`var b = board(["120", "000", "302", "010"]);
    printh(grid.fall(b, 3)); show(b, 3);
    var s = board(["1120", "1111", "0111", "2202"]), m = new Int32Array(8);
    printh(grid.slide(s, 4, DIR.LEFT, m)); show(s, 4); ints(m, 6);
    printh(grid.slide(s, 4, DIR.RIGHT)); show(s, 4);
    var u = board(["10", "10", "20"]);
    printh(grid.slide(u, 2, DIR.DOWN)); show(u, 2);
    printh(grid.slide(u, 2, DIR.DOWN)); show(u, 2);
    printh(grid.slide(u, 2, DIR.DOWN));`);
    assert.deepEqual(out, [
        '4', '000/000/120/312',
        /* moves and merges; a merged tile does not merge again in the
         * same move */
        '10', '2200/2200/2100/3200', '0,4,5,8,12,-1',
        '8', '0003/0003/0021/0032',
        '1', '00/20/20',
        '1', '00/00/30',
        '0']);
});

test('shapes fit, place and rotate', () => {
    const out = grid(`var b = board(["0000", "0000", "1001"]);
    var l = board(["10", "10", "11"]), r = new Uint8Array(6);
    printh(grid.fits(b, 4, l, 2, 1, 0) + " " + grid.fits(b, 4, l, 2, 0, 0) + " " +
           grid.fits(b, 4, l, 2, 1, -2) + " " + grid.fits(b, 4, l, 2, 3, 0) + " " + grid.fits(b, 4, l, 2, 1, 1));
    printh(grid.place(b, 4, l, 2, 1, 0)); show(b, 4);
    printh(grid.rotate(l, 2, r, 1)); show(r, 3);
    printh(grid.rotate(l, 2, r, -1)); show(r, 3);
    printh(grid.rotate(l, 2, l, 2)); show(l, 2);`);
    assert.deepEqual(out, [
        /* over a 1; off the side; above the top is free; past the bottom */
        '1 0 1 0 0',
        '4', '0100/0100/1111',
        '3', '111/100', '3', '001/111', '2', '11/01/01']);
});

test('boards are checked', () => {
    const r = run(BOARD + 'function init() { grid.fall(new Uint8Array(10), 3); }\nfunction update() {}');
    assert.match(r.err, /grid\.fall\(\) needs a Uint8Array board and its width/);
    const r2 = run(BOARD + 'function init() { grid.slide(new Uint8Array(4), 2, 7); }\nfunction update() {}');
    assert.match(r2.err, /slides toward DIR/);
});
