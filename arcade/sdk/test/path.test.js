/* The path system, through pbgvm: boards worked by hand, and random mazes. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-path-'));
const CART = JSON.stringify({ id: '00000000000a7400', name: 'Path' });

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
function paths(body) {
    const r = run(BOARD + 'function init() {\n' + body + '\n}\nfunction update() {}');
    assert.equal(r.status, 0, r.err || JSON.stringify(r.errors));
    return r.out.trim().split('\n');
}

/* 1 is a wall (solid 0: any non-zero cell) */
const MAZE = `var b = board(["00000", "01110", "00010", "11010", "00000"]);`;

test('field counts steps from its start round the walls, next walks it back', () => {
    const out = paths(MAZE + `
    var f = new Int32Array(25);
    printh(path.field(b, 5, 0, 0, 0, f));
    var s = ""; for (var i = 0; i < 25; i++) s += (i == 0 ? "" : i % 5 == 0 ? "/" : " ") + (f[i] < 0 ? "#" : f[i]); printh(s);
    var x = 0, y = 4, way = "";
    for (var k = 0; k < 20; k++) {
        var d = path.next(f, 5, x, y);
        if (d < 0) break;
        way += "LRUD".substring(d, d + 1);
        if (d == DIR.LEFT) x--; if (d == DIR.RIGHT) x++; if (d == DIR.UP) y--; if (d == DIR.DOWN) y++;
    }
    printh(way + " " + x + "," + y + " " + path.next(f, 5, 1, 1));`);
    assert.equal(out[0], '18');
    assert.equal(out[1], '0 1 2 3 4/1 # # # 5/2 3 4 # 6/# # 5 # 7/8 7 6 7 8');
    /* from the bottom-left corner home: right, up and round */
    assert.equal(out[2], 'RRUULLUU 0,0 -1');
});

test('field on a 128 by 128 board fits beside a heap mostly held', () => {
    /* 64 KB of answer and 240 KB held: the queue, two bytes a cell, is
     * the 32 KB left */
    const r = run(`var b, f, big;
function init() {
    b = new Uint8Array(128 * 128);
    f = new Int32Array(128 * 128);
    big = new Int32Array(60000);
    printh(path.field(b, 128, 0, 0, 0, f) + " " + f[128 * 128 - 1]);
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out.trim(), '16384 254');
});

test('find gives a shortest route, sight sees down open lines only', () => {
    const out = paths(MAZE + `
    var o = new Int32Array(20);
    var n = path.find(b, 5, 0, 0, 4, 4, 0, o); ints(o, n);
    printh(path.find(b, 5, 0, 0, 0, 0, 0, o) + " " + path.find(b, 5, 0, 0, 0, 1, 1, o));
    var shut = board(["010", "010", "010"]);
    printh(path.find(shut, 3, 0, 0, 0, 2, 2, o));
    printh(path.sight(b, 5, 0, 0, 0, 4, 0) + " " + path.sight(b, 5, 0, 0, 0, 0, 4) + " " +
           path.sight(b, 5, 0, 0, 2, 2, 2) + " " + path.sight(b, 5, 0, 0, 4, 4, 0) + " " +
           path.sight(b, 5, 0, 2, 2, 4, 4));
    var gap = board(["01", "10"]);
    printh(path.sight(gap, 2, 0, 0, 0, 1, 1));`);
    /* eight steps: along the bottom then up the right side */
    assert.equal(out[0], '21,22,23,24,19,14,9,4');
    /* already there; a wall as the goal; no way through */
    assert.equal(out[1], '0 -1');
    assert.equal(out[2], '-1');
    /* along the top row; down the left (the wall at 0,3); across row 2 to
     * the wall's neighbour; the diagonal through the block; from 2,2 to
     * the corner, past 3,3 (a wall) */
    assert.equal(out[3], '1 0 1 0 0');
    assert.equal(out[4], '0');
});

test('solid as flags reads the sprites on the map', () => {
    const out = paths(`fset(3, 4);
    for (var c = 0; c < 6; c++) mset(c, 1, c == 5 ? 0 : 3);
    var m = mem(MEM.MAP), o = new Int32Array(40);
    printh(path.find(m, mapw(), 4, 0, 0, 0, 2, o) + " " + path.find(m, mapw(), 2, 0, 0, 0, 2, o));`);
    /* row 1 is a wall of sprite 3 (flag 4) with a gap at column 5: round
     * it is 12 steps; with solid 2 it is no wall at all, 2 */
    assert.equal(out[0], '12 2');
});

/* A random maze from a seed: about a third walls */
function maze(seed, w, h) {
    let s = seed;
    const rnd = () => (s = (s * 1103515245 + 12345) >>> 0) / 4294967296;
    const rows = [];
    for (let y = 0; y < h; y++) {
        let r = '';
        for (let x = 0; x < w; x++) r += (x + y) && rnd() < 0.33 ? '1' : '0';
        rows.push(r);
    }
    return rows;
}

test('on random mazes find is as long as the field says, and walks open cells', () => {
    for (let seed = 1; seed <= 6; seed++) {
        const W = 23, H = 17, rows = maze(seed, W, H);
        const gx = W - 1, gy = H - 1;
        const out = paths(`var b = board(${JSON.stringify(rows)});
    var f = new Int32Array(${W * H}), o = new Int32Array(${W * H});
    path.field(b, ${W}, 0, 0, 0, f);
    var n = path.find(b, ${W}, 0, 0, 0, ${gx}, ${gy}, o);
    printh(f[${gy * W + gx}] + " " + n);
    if (n > 0) ints(o, n);`);
        const [want, got] = out[0].split(' ').map(Number);
        assert.equal(got, want, 'seed ' + seed);
        if (got <= 0) continue;
        let x = 0, y = 0;
        for (const c of out[1].split(',').map(Number)) {
            const cx = c % W, cy = Math.floor(c / W);
            assert.equal(Math.abs(cx - x) + Math.abs(cy - y), 1, 'a step, seed ' + seed);
            assert.equal(rows[cy][cx], '0', 'open, seed ' + seed);
            x = cx; y = cy;
        }
        assert.deepEqual([x, y], [gx, gy]);
    }
});

test('a ray meets the first solid map cell, with the face it met', () => {
    const out = paths(`fset(3, 1);
    // a wall of sprite 3 down column 10 (x 80-87), and a block at 4,1
    for (var r = 0; r < 12; r++) mset(10, r, 3);
    mset(4, 1, 3);
    var o = [0, 0, 0, 0, 0];
    var h = path.ray(20, 30, 1, 0, 1, o); printh(h + " " + o.join(" "));
    h = path.ray(20, 30.5, 1, 0.5, 1, o); printh(h + " " + flr(o[0]) + " " + flr(o[1]) + " " + o[2] + " " + o[3] + " " + o[4]);
    h = path.ray(20, 30, -1, 0, 1, o); printh(h);
    h = path.ray(20, 30, 1, 0, 1, o, 40); printh(h);
    h = path.ray(84, 30, 1, 0, 1, o); printh(h + " " + o[4]);
    h = path.ray(36, 30, 0, -1, 1, o); printh(h + " " + o.slice(1).join(" "));`);
    /* straight right from x 20: the wall's left face at x 80, cell 10,3 */
    assert.equal(out[0], '1 80 30 10 3 0');
    /* down and right at a half: meets the wall's left face (x 80) at y 60.5 */
    assert.equal(out[1], '1 80 60 10 7 0');
    /* left, and too short: clear */
    assert.deepEqual(out.slice(2, 4), ['0', '0']);
    /* starting inside the wall */
    assert.equal(out[4], '1 -1');
    /* straight up from x 36: the block at 4,1's bottom face (y 16) */
    assert.equal(out[5], '1 16 4 1 3');
});
