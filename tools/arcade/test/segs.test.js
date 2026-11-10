/* Segment sets and the bag: links.step against a segs.make() set, the
 * contact it reports, a moving set, links.ring, slack links, gravity
 * across and damp -- through pbgvm's printh. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-segs-'));

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

let n = 0;
function run(main, ticks = 1) {
    const res = compileFiles({
        'cart.json': JSON.stringify({ id: '0000000000005e65', name: 'Segs' }),
        'main.js': main + '\nfunction draw() {}\n' });
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(tmp, 's' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', String(ticks)], { encoding: 'utf8' });
    return { out: r.stdout.replace(/\r/g, '').trim().split('\n'), err: r.stderr, status: r.status };
}

/* one point falling onto a floor line at y 50 from x 0 to 100 */
const FLOOR = `var p, s, out = [0, 0, 0];
function init() {
    p = links.pool(1, 0);
    links.point(p, 50, 10);
    p.gravity = 0.5; p.radius = 2;
    s = segs.make([0, 100], [50, 50], 2, [3, 3]);
    p.segs = s;
}`;

test('a point falls onto a line and rests on it, reporting its material and the way out', () => {
    const r = run(FLOOR + `
function update() {
    links.step(p, 0, out);
    if (frame() == 60) printh(flr(p.y[0] * 10) + " " + out[0] + " " + out[1] + " " + out[2]);
}`, 60);
    assert.equal(r.status, 0, r.err);
    /* 2 above the line, material 3 plus one, out straight up */
    assert.equal(r.out[0], '480 4 0 -1');
});

test('a fast point does not pass through a thin line', () => {
    const r = run(FLOOR.replace('links.point(p, 50, 10);',
                                'links.point(p, 50, 10); p.oy[0] = -200;') + `
function update() {
    links.step(p, 0, out);
    if (frame() == 3) printh(p.y[0] < 50 ? "above" : "through");
}`, 3);
    assert.equal(r.out[0], 'above');
});

test('a moving set carries a point resting on it when friction holds, not when it slides', () => {
    const carry = (f) => run(FLOOR.replace('p.radius = 2;', 'p.radius = 2; p.friction = ' + f + ';') + `
function update() {
    if (frame() > 40) segs.move(s, 1, 0);
    links.step(p, 0, out);
    if (frame() == 80) printh(flr(p.x[0]));
}`, 80).out[0];
    assert.ok(Number(carry(1)) >= 85, carry(1));
    assert.ok(Number(carry(0)) <= 55, carry(0));
});

test('links.ring speeds a squashed ring back toward its area, by the next step', () => {
    const r = run(`var p;
function init() {
    p = links.pool(8, 0);
    for (var i = 0; i < 8; i++) links.point(p, 80 + cos(i * 0.125) * 20, 60 + sin(i * 0.125) * 5);
}
function update() {
    var before = links.ring(p, 0, 8, 1000, 1);
    links.step(p, 0);                // the ring speeds; the step moves
    var after = links.ring(p, 0, 8, 1000, 0);
    printh(flr(before) + " " + flr(after));
}`);
    assert.equal(r.status, 0, r.err);
    const [before, after] = r.out[0].split(' ').map(Number);
    assert.ok(before < 400, String(before));
    assert.ok(Math.abs(after - 1000) < Math.abs(before - 1000) / 2, r.out[0]);
});

test('a slack link pulls but never pushes; gravity across moves points sideways', () => {
    const r = run(`var p;
function init() {
    p = links.pool(2, 1);
    links.point(p, 50, 50); links.point(p, 52, 50);
    links.link(p, 0, 1, 10);
    p.slack = 1; p.gx = 0.25;
}
function update() {
    links.step(p, 0);
    printh(flr((p.x[1] - p.x[0]) * 100) + " " + flr(p.x[0] * 100));
}`);
    /* the link, 2 apart and 10 long, pushed nothing; both drifted right */
    assert.equal(r.out[0], '200 5025');
});

test('damp stills a body\'s wobble but keeps its motion as a whole', () => {
    const r = run(`var p;
function init() {
    p = links.pool(2, 0);
    links.point(p, 0, 0); links.point(p, 10, 0);
    p.ox[0] = -2; p.ox[1] = 10;     // one moving 2 a tick, one still
    p.damp = 1;
}
function update() {
    links.step(p, 0);
    printh(flr((p.x[0] - p.ox[0]) * 100) + " " + flr((p.x[1] - p.ox[1]) * 100));
}`);
    /* both moving at the mean, 1 a tick */
    assert.equal(r.out[0], '100 100');
});

test('wrong arguments stop the cart', () => {
    assert.match(run('function init() { segs.make([1], [1, 2], 2); }\nfunction update() {}').err,
                 /fewer x or y than points/);
    assert.match(run('function init() { segs.move(5, 1, 1); }\nfunction update() {}').err,
                 /a set from segs.make/);
    assert.match(run(FLOOR + '\nfunction update() { links.step(p, 0, [0]); }').err,
                 /three numbers a point/);
    assert.match(run('var p; function init() { p = links.pool(2, 0); links.ring(p, 0, 2, 10, 1); }\nfunction update() {}').err,
                 /3 or more points/);
});

test('a bag dropped on ragged ground never ends a step inside it', () => {
    /* random ground, gravity tilting, the ring pressing out: every point
     * of every tick tested against the ground's outline */
    const main = fs.readFileSync(path.join(here, 'carts', 'leak.js'), 'utf8');
    const res = compileFiles({ 'cart.json': JSON.stringify({ id: '0000000000001ea6', name: 'Leak' }),
                               'main.js': main });
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'leak.pbg');
    fs.writeFileSync(file, res.cart);
    for (let seed = 1; seed <= 40; seed++) {
        const r = spawnSync(exe, [file, '--ticks', '200', '--seed', String(seed)], { encoding: 'utf8' });
        assert.equal(r.stdout.replace(/\r/g, '').trim(), 'leaks 0', 'seed ' + seed);
    }
});

const MAP = (rows) => `function init() {
    fset(1, 1); fset(2, 3);
    var rows = ${JSON.stringify(rows)};
    for (var r = 0; r < rows.length; r++)
        for (var c = 0; c < rows[r].length; c++) {
            var ch = rows[r].charCodeAt(c);
            mset(c, r, ch == 35 ? 1 : ch == 36 ? 2 : 0);
        }
    var o = segs.trace(1, 2, SMOOTH, 0, 0, 8, ${rows.length});
    printh(o.n + " " + o.ends.length + " " + o.x[0] + "," + o.y[0]);
}
function update() {}`;

test('segs.trace: a block\'s corners cut by half a cell, or kept square where sharp', () => {
    const block = run(MAP(['....', '.##.', '.##.', '....']).replace('SMOOTH', '0'));
    assert.equal(block.status, 0, block.err);
    /* four corners, each cut into two points */
    assert.equal(block.out[0].split(' ').slice(0, 2).join(' '), '8 1');
    const sharp = run(MAP(['....', '.$$.', '.$$.', '....']).replace('SMOOTH', '0'));
    assert.equal(sharp.out[0], '4 1 8,8');
});

test('segs.trace: a hole is an outline of its own, and cells meeting at a corner stay apart', () => {
    const ring = run(MAP(['#####', '#...#', '#####']).replace('SMOOTH', '0'));
    assert.equal(ring.out[0].split(' ')[1], '2');
    const pinch = run(MAP(['#.', '.#']).replace('SMOOTH', '0'));
    assert.equal(pinch.out[0].split(' ')[1], '2');
});

test('segs.trace: each smoothing pass doubles the points, the sharp ones kept', () => {
    const one = run(MAP(['....', '.##.', '.##.', '....']).replace('SMOOTH', '1'));
    assert.equal(one.out[0].split(' ')[0], '16');
});

test('segs.trace with shapes: a shallow slope run is one line, its ends not cut', () => {
    /* a 6 then a 7 on two full cells: the slope from (0,8) to (16,0) whole,
     * the two square corners below each cut into two */
    const r = run(`function init() {
    for (var k = 1; k < 14; k++) fset(k, 1);
    mset(0, 0, 6); mset(1, 0, 7); mset(0, 1, 1); mset(1, 1, 1);
    var o = segs.trace(1, 0, 0, 0, 0, 2, 2, [0, 1, 2, 3, 4, 5, 6, 7]);
    var s = "";
    for (var i = 0; i < o.n; i++) s += " " + o.x[i] + "," + o.y[i];
    printh(o.n + s);
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out[0], '6 16,0 16,12 12,16 4,16 0,12 0,8');
});

test('segs.trace with outside: the air\'s edges, inside a far rectangle', () => {
    /* an empty 4 by 3 area: the far rectangle, then the air, square where
     * it meets the solid beyond */
    const r = run(`function init() {
    fset(1, 1);
    var o = segs.trace(1, 0, 2, 0, 0, 4, 3, null, true);
    var s = "";
    for (var i = 0; i < o.n; i++) s += " " + o.x[i] + "," + o.y[i];
    printh(o.n + " " + o.ends.join(",") + s);
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out[0], '8 4,8 -4096,-4096 4128,-4096 4128,4120 -4096,4120 ' +
                 '0,0 0,24 32,24 32,0');
    /* left, right and below only (2 + 4 + 16): one outline, open above */
    const u = run(`function init() {
    fset(1, 1);
    var o = segs.trace(1, 0, 2, 0, 0, 4, 3, null, 22);
    var s = "";
    for (var i = 0; i < o.n; i++) s += " " + o.x[i] + "," + o.y[i];
    printh(o.n + " " + o.ends.join(",") + s);
}
function update() {}`);
    assert.equal(u.out[0], '8 8 -4096,-4096 0,-4096 0,24 32,24 32,-4096 ' +
                 '4128,-4096 4128,4120 -4096,4120');
});

test('segs.trace of a small area needs little memory, however full the heap', () => {
    /* 360 KB of the 512 KB held: a working block sized for the most points
     * any trace can make would not fit beside it */
    const r = run(`var big;
function init() {
    fset(1, 1);
    mset(1, 1, 1);
    big = new Int32Array(90000);
    var o = segs.trace(1, 0, 2, 0, 0, 3, 3);
    printh("n " + o.n);
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out[0], 'n 32');
});

test('a frictionless curve of lines keeps a sliding point\'s height, and a pressed bag never gains', () => {
    /* a round bowl of 48 lines; tops of each swing, after the first landing */
    const r = run(`var one, bag, ground, gx = [], gy = [], N = 20, top = 999.0, btop = 999.0, side = 0, bside = 0;
function init() {
    for (var i = 0; i <= 48; i++) {
        var a = 0.5 + i / 96.0;
        gx.push(80 + cos(a) * 70); gy.push(40 + sin(a) * 70);
    }
    gx.push(160, 0); gy.push(200, 200);
    ground = segs.make(gx, gy, gx.length);
    one = links.pool(1, 0);
    links.point(one, 19.4, 74);
    one.gravity = 0.16; one.radius = 1; one.segs = ground;
    bag = links.pool(N, N);
    for (var k = 0; k < N; k++) links.point(bag, 30 + cos(k * 0.05) * 8, 70 + sin(k * 0.05) * 8);
    for (var j = 0; j < N; j++) links.link(bag, j, (j + 1) % N, 2.64);
    bag.gravity = 0.16; bag.damp = 0.3; bag.slack = 1; bag.passes = 6; bag.radius = 1; bag.segs = ground;
}
function update() {
    links.step(one, 0);
    links.ring(bag, 0, N, 160.8, 0.8);
    links.step(bag, 0);
    var bx = 0.0, by = 0.0;
    for (var i = 0; i < N; i++) { bx += bag.x[i]; by += bag.y[i]; }
    bx = bx / N; by = by / N;
    top = min(top, one.y[0]); btop = min(btop, by);
    var s = one.x[0] < 80 ? -1 : 1, bs = bx < 80 ? -1 : 1;
    if (s != side) { if (side != 0) printh("point " + top); top = 999.0; side = s; }
    if (bs != bside) { if (bside != 0) printh("bag " + btop); btop = 999.0; bside = bs; }
}`, 900);
    assert.equal(r.status, 0, r.err);
    const tops = (k) => r.out.filter((l) => l.startsWith(k)).map((l) => Number(l.split(' ')[1]));
    const p = tops('point'), b = tops('bag');
    /* the point: within a pixel and a half of its second swing, swing after swing */
    for (const y of p.slice(2)) assert.ok(Math.abs(y - p[1]) < 1.5, p.join(' '));
    /* the bag: each top no higher than the last but one, on its side */
    for (let i = 3; i < b.length; i++) assert.ok(b[i] >= b[i - 2] - 0.5, b.join(' '));
});
