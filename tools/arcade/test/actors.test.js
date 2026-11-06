/* Actors with a world: platforms that carry, zones from map objects,
 * animation, and a cells field to stand, swim and burn in. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-actors-'));
const CART = JSON.stringify({ id: '00000000000ac705', name: 'Actors' });

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

let n = 0;
function run(main, ticks = 1, extra = []) {
    const res = compileFiles({ 'cart.json': CART, 'main.js': main });
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'a' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', String(ticks)].concat(extra),
                        { encoding: 'utf8', timeout: 20000 });
    assert.equal(r.status, 0, r.stderr);
    return r.stdout.replace(/\r/g, '').trim().split('\n');
}

test('a platform carries what stands on it, and stops what runs into it', () => {
    const out = run(`var lift, p, q, hits = new Int32Array(40), tk = 0;
function init() {
    lift = actors.pool(1);
    var l = actors.spawn(lift); lift.x[l] = 20; lift.y[l] = 60; lift.w[l] = 24; lift.h[l] = 4;
    p = actors.pool(2);
    p.gravity = 0.5;
    p.platforms = lift;
    var a = actors.spawn(p); p.x[a] = 28; p.y[a] = 30;
    q = actors.pool(1);
    q.platforms = lift;
    var b = actors.spawn(q); q.x[b] = 0; q.y[b] = 58; q.h[b] = 4; q.dx[b] = 2;
}
function update() {
    tk++;
    // the lift rises a pixel a tick from tick 20, and slides right from 40
    lift.dy[0] = tk >= 20 && tk < 40 ? -1 : 0;
    lift.dx[0] = tk >= 40 ? 1 : 0;
    actors.move(lift, 0, 0, hits);
    actors.move(p, 0, 0, hits);
    var k = actors.move(q, 0, 0, hits);
    if (tk == 19 || tk == 39 || tk == 59) printh(tk + " " + p.x[0] + " " + p.y[0] + " " + q.x[0]);
    for (var i = 0; i < k; i++) if (hits[i * 4 + 2] == -1 && tk < 12) printh("b hit " + hits[i * 4 + 1] + " " + hits[i * 4 + 3]);
}`, 59);
    /* a lands on the lift's top (y 60 - 8), rides up 20, then right 20 */
    assert.match(out.find((l) => l.startsWith('19 ')), /^19 28 52 /);
    assert.match(out.find((l) => l.startsWith('39 ')), /^39 28 32 /);
    assert.match(out.find((l) => l.startsWith('59 ')), /^59 48 32 /);
    /* b, running right along y 58, is stopped flush at the lift's left */
    assert.ok(out.some((l) => l === 'b hit 1 0'), out.join('|'));
    assert.match(out.find((l) => l.startsWith('19 ')), / 12$/);
});

test('zones report entering and leaving map objects', () => {
    const out = run(`var p, hits = new Int32Array(20), tk = 0;
var objs = [7, 40, 0, 16, 40,   9, 80, 0, 16, 40];
function init() { p = actors.pool(1); var a = actors.spawn(p); p.x[a] = 20; p.y[a] = 10; p.dx[a] = 2; }
function update() {
    tk++;
    actors.move(p, 0, 0, hits);
    var k = actors.zones(p, objs, 2, hits);
    for (var i = 0; i < k; i++) printh(tk + " " + hits[i * 4 + 1] + " " + hits[i * 4 + 2] + " " + hits[i * 4 + 3]);
}`, 40);
    /* the centre (x + 4) enters 40 at tick 8 and leaves at 56, enters 80
     * at 28 and leaves at 96 */
    assert.deepEqual(out, ['8 4 0 7', '16 5 0 7', '28 4 1 9', '36 5 1 9']);
});

test('frames animate an actor through the sprites after its own', () => {
    const out = run(`var p;
function init() {
    for (var k = 0; k < 3; k++) for (var y = 0; y < 8; y++) for (var x = 0; x < 8; x++) sset(8 + k * 8 + x, y, 7 + k);
    p = actors.pool(1);
    var a = actors.spawn(p); p.sp[a] = 1; p.frames[a] = 3; p.speed[a] = 2;
}
function update() {}
function draw() { cls(); actors.draw(p); printh(pget(3, 3)); }`, 8);
    /* two ticks each of sprites 1, 2, 3 (colours 7, 8, 9), round again:
     * each frame the same as the last or the next in turn */
    const seq = out.map(Number);
    for (let i = 1; i < seq.length; i++)
        assert.ok(seq[i] === seq[i - 1] || seq[i] === (seq[i - 1] - 6) % 3 + 7,
                  seq.join(','));
    assert.deepEqual([...new Set(seq)].sort(), [7, 8, 9]);
});

/* A field: stone floor at y 60, a pool of water x 40-79 from y 40, fire
 * at x 100, wood at x 120 */
const FIELD = `
var R, f, p, hits = new Int32Array(80), tk = 0;
function field() {
    R = cells.rules();
    R.density[5] = 255; R.flags[5] = CELL.SOLID;
    R.move[12] = MOVE.LIQUID; R.density[12] = 100; R.flags[12] = CELL.LIQUID;
    R.density[8] = 255; R.flags[8] = CELL.HARM;
    R.density[4] = 255; R.flags[4] = CELL.SOLID;
    // an actor of material 9 (a torch) beside wood: the wood becomes fire
    R.a[0] = 9; R.b[0] = 4; R.c[0] = 9; R.d[0] = 8; R.chance[0] = 255;
    f = img(160, 70);
    target(f); cls(0);
    rectfill(0, 60, 159, 69, 5);
    rectfill(40, 40, 79, 59, 12);
    rectfill(100, 52, 103, 59, 8);
    rectfill(120, 44, 127, 59, 4);
    target(SCREEN);
}
`;

test('in a field an actor stands on stone, floats or sinks in water, and is told of fire', () => {
    const out = run(FIELD + `
function init() {
    field();
    p = actors.pool(3);
    p.gravity = 0.25; p.field = f; p.rules = R; p.density = 60;
    var a = actors.spawn(p); p.x[a] = 10; p.y[a] = 20;
    var b = actors.spawn(p); p.x[b] = 56; p.y[b] = 20;
    var c = actors.spawn(p); p.x[c] = 98; p.y[c] = 20;
}
function update() {
    tk++;
    var k = actors.move(p, 0, 0, hits);
    for (var i = 0; i < k; i++) if (hits[i * 4 + 1] == HIT.TOUCH && tk < 60) printh("touch " + hits[i * 4] + " " + hits[i * 4 + 3]);
    if (tk == 120) printh("at " + flr(p.y[0]) + " " + flr(p.y[1]) + " " + flr(p.y[2]));
}`, 120);
    const at = out.find((l) => l.startsWith('at ')).split(' ').map(Number);
    /* a on the stone floor (60 - 8), b lighter than water bobbing near its
     * top (40) rather than on the floor, c standing on the floor by the fire */
    assert.equal(at[1], 52);
    assert.ok(at[2] < 48, 'floats: ' + at[2]);
    assert.ok(out.includes('touch 2 8'), out.join('|'));
    /* heavier than water, it sinks to the floor */
    const sink = run(FIELD + `
function init() {
    field();
    p = actors.pool(1);
    p.gravity = 0.25; p.field = f; p.rules = R; p.density = 300;
    var b = actors.spawn(p); p.x[b] = 56; p.y[b] = 20;
}
function update() { tk++; actors.move(p, 0, 0, hits); if (tk == 120) printh(flr(p.y[0])); }`, 120);
    assert.deepEqual(sink, ['52']);
});

test('an actor made of a material reacts with the field it touches', () => {
    const out = run(FIELD + `
function init() {
    field();
    p = actors.pool(1);
    p.field = f; p.rules = R;
    var a = actors.spawn(p); p.x[a] = 112; p.y[a] = 50; p.mat[a] = 9; p.dx[a] = 1;
}
function update() {
    tk++;
    var k = actors.move(p, 0, 0, hits);
    for (var i = 0; i < k; i++) if (hits[i * 4 + 1] == HIT.REACT) printh(tk + " react " + hits[i * 4 + 3]);
    target(f); var w = pget(120, 50); target(SCREEN);
    if (tk == 3) printh("cell " + w);
}`, 3);
    /* touching the wood at x 120 (actor 112-119, the ring at 120) the torch
     * lights it a cell a tick, the first round its edge (120, 50) becoming
     * fire (8); the actor is told 9 */
    assert.ok(out.some((l) => / react 9$/.test(l)), out.join('|'));
    assert.ok(out.includes('cell 8'), out.join('|'));
});

test('a pool names its platforms and field, checked when used', () => {
    const res = compileFiles({ 'cart.json': CART, 'main.js':
        'function init() { var p = actors.pool(1); p.platforms = { x: 1 }; actors.move(p, 0, 0, new Int32Array(4)); }\nfunction update() {}' });
    const file = path.join(tmp, 'bad.pbg');
    fs.writeFileSync(file, res.cart);
    const out = spawnSync(exe, [file, '--ticks', '1'], { encoding: 'utf8' });
    assert.match(out.stderr, /actors\.move\(\) needs a pool from actors\.pool\(\)/);
});

test('an actor with splash set pushes the water aside, and none is lost', () => {
    const body = (splash) => FIELD + `
function water() {
    var n = 0;
    target(f); for (var y = 0; y < 60; y++) for (var x = 30; x < 90; x++) if (pget(x, y) == 12) n++; target(SCREEN);
    return n;
}
function inside() {
    var n = 0, x0 = flr(p.x[0]), y0 = flr(p.y[0]);
    target(f); for (var y = y0; y < y0 + 8; y++) for (var x = x0; x < x0 + 8; x++) if (pget(x, y) == 12) n++; target(SCREEN);
    return n;
}
function init() {
    field();
    p = actors.pool(1);
    p.gravity = 0.25; p.field = f; p.rules = R; p.density = 300; p.splash = ${splash};
    var b = actors.spawn(p); p.x[b] = 56; p.y[b] = 20;
    printh(water());
}
function update() {
    tk++;
    actors.move(p, 0, 0, hits);
    if (tk == 40) printh(water() + " " + inside() + " " + flr(p.y[0]));
}`;
    const wet = run(body(1), 40);
    /* 40 x 20 of water: all of it still there, none inside the actor, which
     * has sunk to the floor (60 - 8) */
    assert.deepEqual(wet, ['800', '800 0 52']);
    /* without splash it sits in the water */
    const still = run(body(0), 40);
    assert.equal(still[1].split(' ')[0], '800');
    assert.notEqual(still[1].split(' ')[1], '0');
});
