/* Circles: actors.bounce and actors.bounce_map, through pbgvm. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-circles-'));

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

let n = 0;
function run(main, ticks = 1) {
    const res = compileFiles({
        'cart.json': JSON.stringify({ id: '00000000000c1c1e', name: 'Circles' }),
        'main.js': main });
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(tmp, 'c' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', String(ticks)], { encoding: 'utf8', timeout: 20000 });
    return { errors: [], out: r.stdout.replace(/\r/g, ''), err: r.stderr, status: r.status };
}

/* r(v): a number in whole hundredths, for printing */
const R = 'function r(v) { return flr(v * 100 + 0.5); }\n';

test('two balls meeting head on trade their speeds', () => {
    const t = run(R + `function init() {
    var p = actors.pool(2), out = new Int32Array(4);
    actors.spawn(p); actors.spawn(p);
    p.x[0] = 10; p.y[0] = 20; p.dx[0] = 1;
    p.x[1] = 17; p.y[1] = 20; p.dx[1] = -0.5;
    printh(actors.bounce(p, p, out) + " " + out[0] + "," + out[1]);
    printh(r(p.dx[0]) + " " + r(p.dx[1]) + " " + r(p.x[1] - p.x[0]) + " " + r(p.dy[0]));
    printh(actors.bounce(p, p, out));
}
function update() {}`);
    assert.equal(t.status, 0, t.err);
    /* pushed apart to touching (8 between centres), speeds swapped */
    assert.equal(t.out, '1 0,1\n-50 100 800 0\n0\n');
});

test('a ball against a peg turns back along the line between them, give of it', () => {
    const t = run(R + `function init() {
    var b = actors.pool(1), peg = actors.pool(1), out = new Int32Array(2);
    actors.spawn(b); actors.spawn(peg);
    peg.x[0] = 40; peg.y[0] = 40;
    /* straight on, then aslant at 45 degrees, then with give 0.5 */
    b.x[0] = 33; b.y[0] = 40; b.dx[0] = 2; b.dy[0] = 0;
    actors.bounce(b, peg, out);
    printh(r(b.dx[0]) + " " + r(b.dy[0]) + " " + r(b.x[0]) + " " + r(peg.x[0]));
    b.x[0] = 35; b.y[0] = 35; b.dx[0] = 1; b.dy[0] = 1;
    actors.bounce(b, peg, out);
    printh(r(b.dx[0]) + " " + r(b.dy[0]));
    b.x[0] = 33; b.y[0] = 40; b.dx[0] = 2; b.dy[0] = 0;
    actors.bounce(b, peg, out, 0.5);
    printh(r(b.dx[0]));
    b.x[0] = 20; b.dx[0] = 2;
    printh(actors.bounce(b, peg, out));
}
function update() {}`);
    assert.equal(t.status, 0, t.err);
    assert.equal(t.out, '-200 0 3200 4000\n-100 -100\n-100\n0\n');
});

test('balls in a box keep their momentum among themselves', () => {
    const t = run(R + `var p, out = new Int32Array(200);
function init() {
    p = actors.pool(12);
    for (var i = 0; i < 12; i++) {
        actors.spawn(p);
        p.x[i] = 20 + (i % 4) * 14; p.y[i] = 20 + flr(i / 4) * 14;
        p.dx[i] = (i % 3) - 1; p.dy[i] = ((i + 1) % 3) - 1;
    }
}
function update() {
    var sx = 0, sy = 0;
    for (var i = 0; i < 12; i++) { p.x[i] += p.dx[i]; p.y[i] += p.dy[i]; }
    actors.bounce(p, p, out);
    for (var i = 0; i < 12; i++) { sx += p.dx[i]; sy += p.dy[i]; }
    if (frame() % 20 == 0) printh(r(sx) + " " + r(sy));
}`, 60);
    assert.equal(t.status, 0, t.err);
    /* rounding takes at most a little each bounce */
    for (const line of t.out.trim().split('\n')) {
        const [sx, sy] = line.split(' ').map(Number);
        assert.ok(Math.abs(sx) < 0.05 && Math.abs(sy - 0) < 0.05, line);
    }
});

test('a ball bounces off the map: a floor square on, a corner aslant', () => {
    const t = run(R + `var b, out = new Int32Array(30);
function init() {
    fset(1, 1);
    for (var c = 0; c < 20; c++) mset(c, 10, 1);
    mset(5, 5, 1);
    b = actors.pool(2);
    actors.spawn(b); actors.spawn(b);
    b.x[0] = 20; b.y[0] = 66; b.dx[0] = 0; b.dy[0] = 3;
    /* aimed at the top-left corner of cell (5, 5), from up and to the left */
    b.x[1] = 30; b.y[1] = 30; b.dx[1] = 2; b.dy[1] = 2;
}
function update() {
    var k = actors.bounce_map(b, 1, out);
    for (var i = 0; i < k; i++) printh(frame() + ": " + out[i * 3] + " hit " + out[i * 3 + 1] + "," + out[i * 3 + 2] + " now " + r(b.dx[out[i * 3]]) + "," + r(b.dy[out[i * 3]]));
}`, 8);
    assert.equal(t.status, 0, t.err);
    const lines = t.out.trim().split('\n');
    /* the first ball straight back up off the floor, row 10 */
    assert.ok(lines.some((l) => /^\d+: 0 hit 2,10 now 0,-300$/.test(l)), t.out);
    /* the second off the corner: thrown back up and to the left alike */
    const corner = lines.find((l) => / 1 hit 5,5 /.test(l));
    assert.ok(corner, t.out);
    const [vx, vy] = corner.split(' now ')[1].split(',').map(Number);
    assert.ok(vx < 0 && vy < 0 && Math.abs(vx - vy) < 10, corner);
});

test('bounce_map takes the pool\'s gravity and a give below one', () => {
    const t = run(R + `var b, out = new Int32Array(9);
function init() {
    fset(1, 1);
    for (var c = 0; c < 20; c++) mset(c, 10, 1);
    b = actors.pool(1);
    b.gravity = 0.5;
    actors.spawn(b);
    b.x[0] = 20; b.y[0] = 60;
}
function update() {
    if (actors.bounce_map(b, 1, out, 0.5)) printh(frame() + " " + r(b.dy[0]));
}`, 30);
    assert.equal(t.status, 0, t.err);
    /* each bounce comes back with half the speed it fell with */
    const lines = t.out.trim().split('\n');
    assert.ok(lines.length >= 2, t.out);
    for (const l of lines) assert.ok(Number(l.split(' ')[1]) < 0, l);
});
