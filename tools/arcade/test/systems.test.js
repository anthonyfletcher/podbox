/* The systems: actors and particles, through pbgvm. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-systems-'));
const CART = JSON.stringify({ id: '0000000000515e75', name: 'Systems' });

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

/* A map: a floor of sprite 1 (flag 1) along row 10, a wall of it at
 * column 12 rows 0-9, and a trigger cell of sprite 2 (flag 2) at 5,9 */
const WORLD = `
function world() {
    fset(1, 1); fset(2, 2);
    for (var c = 0; c < 20; c++) mset(c, 10, 1);
    for (var r = 0; r < 10; r++) mset(12, r, 1);
    mset(5, 9, 2);
}
`;

test('system functions live in their namespace', () => {
    assert.match(run('function init() { var p = actors; }\nfunction update() {}').errors[0].message,
                 /actors is a set of built-in functions; call one/);
    assert.match(run('function init() { actors.nope(1); }\nfunction update() {}').errors[0].message,
                 /actors has no function nope/);
    assert.match(run('function init() { actors_pool(1); }\nfunction update() {}').errors[0].message,
                 /actors_pool is not declared/);
    assert.match(run('function init() { actors.pool(); }\nfunction update() {}').errors[0].message,
                 /actors\.pool\(\) takes 1 argument/);
});

test('a pool is arrays the cart reads, and spawn fills it in order', () => {
    const r = run(`function init() {
    var p = actors.pool(3);
    printh(p.x.length + " " + p.w[2] + " " + p.h[0] + " " + p.sp[1] + " " + p.cell[0] + " " + p.on.join(","));
    printh(actors.spawn(p) + " " + actors.spawn(p) + " " + actors.spawn(p) + " " + actors.spawn(p));
    p.on[1] = 0;
    printh(actors.spawn(p) + " " + p.on.join(","));
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, '3 8 8 -1 -1 0,0,0\n0 1 2 -1\n1 1,1,1\n');
});

test('move falls, lands flush and hits walls, never tunnels, and reports triggers once', () => {
    const r = run(WORLD + `
var p, out = new Int32Array(40), tk = 0;
function init() {
    world();
    p = actors.pool(4);
    p.gravity = 0.5;
    var a = actors.spawn(p); p.x[a] = 16; p.y[a] = 40;
    var b = actors.spawn(p); p.x[b] = 80; p.y[b] = 72; p.dx[b] = 3;
    var c = actors.spawn(p); p.x[c] = 60; p.y[c] = 0; p.dx[c] = 20;
    var d = actors.spawn(p); p.x[d] = 2; p.y[d] = 72; p.dx[d] = -1;
}
function update() {
    tk++;
    var k = actors.move(p, 1, 2, out);
    for (var i = 0; i < k; i++)
        printh(tk + ": " + out[i * 4] + " " + out[i * 4 + 1] + " " + out[i * 4 + 2] + "," + out[i * 4 + 3]);
    if (tk == 12) printh("at " + p.y[0] + " " + p.x[1] + " " + p.x[2] + " " + p.x[3]);
}`, 12);
    assert.equal(r.status, 0, r.err);
    const lines = r.out.trim().split('\n');
    const at = lines.pop();
    /* a lands at y 72 (row 10 is y 80), b and c stop flush left of
     * column 12 (x 96 - 8), d walks off the map's left edge unhindered */
    assert.equal(at, 'at 72 88 88 -10');
    /* c, 20 px a tick, would jump the one-cell wall from x 80; it stops */
    assert.ok(lines.includes('2: 2 1 12,0'), 'c stopped at the wall: ' + lines);
    assert.ok(lines.some((l) => / 0 3 2,10$/.test(l)), 'a landed: ' + lines);
    assert.ok(lines.some((l) => / 1 1 12,9$/.test(l)), 'b hit the wall: ' + lines);
    /* the landed actors keep pressing on the floor, a contact a tick */
    assert.equal(lines.filter((l) => / 0 3 2,10$/.test(l)).length, 2, 'landed at tick 11, pressing at 12');
    /* an actor walking across the trigger cell */
    const trig = run(WORLD + `
var p, out = new Int32Array(40), tk = 0;
function init() { world(); p = actors.pool(1); var a = actors.spawn(p); p.x[a] = 24; p.y[a] = 72; p.dx[a] = 1; }
function update() {
    tk++;
    var k = actors.move(p, 1, 2, out);
    for (var i = 0; i < k; i++) if (out[i * 4 + 1] == HIT.ENTER) printh(tk + " enter " + out[i * 4 + 2] + "," + out[i * 4 + 3] + " cell " + p.cell[0]);
}`, 30);
    assert.equal(trig.status, 0, trig.err);
    /* centre x+4 reaches 40 (column 5) at t 12, and leaves at 48 */
    /* the cell is row * mapw() + column: 9 * 128 + 5 */
    assert.equal(trig.out, '12 enter 5,9 cell 1157\n');
});

test('overlaps pairs actors across pools and within one', () => {
    const r2 = run(`function init() {
    var a = actors.pool(3), b = actors.pool(2), out = new Int32Array(20);
    for (var i = 0; i < 3; i++) { actors.spawn(a); a.x[i] = i * 6; a.y[i] = 0; }
    actors.spawn(b); b.x[0] = 10; b.y[0] = 4;
    actors.spawn(b); b.x[1] = 100; b.y[1] = 100;
    var k = actors.overlaps(a, b, out), s = "";
    for (var j = 0; j < k; j++) s += out[j * 2] + "-" + out[j * 2 + 1] + " ";
    printh(k + " " + s);
    k = actors.overlaps(a, a, out);
    s = "";
    for (j = 0; j < k; j++) s += out[j * 2] + "-" + out[j * 2 + 1] + " ";
    printh(k + " " + s);
}
function update() {}`);
    assert.equal(r2.status, 0, r2.err);
    /* a at x 0, 6, 12 (8 wide); b0 at 10: overlaps a1 and a2. Within a:
     * 0-1 and 1-2 overlap, 0-2 do not */
    assert.equal(r2.out, '2 1-0 2-0 \n2 0-1 1-2 \n');
});

test('particles emit from free slots, step and draw as pixels', () => {
    const r = run(`var p;
function init() {
    p = particles.pool(5);
    printh(particles.emit(p, 3, 10, 20, 1, 0, 0, 4, 9) + " " + particles.emit(p, 9, 0, 0, 0, 0, 0, 2, 7));
    printh(p.life.join(",") + " " + p.c.join(","));
}
function update() { particles.step(p); }
function draw() { cls(); particles.draw(p); }`, 2, ['--dump', '10,20,4,1']);
    assert.equal(r.status, 0, r.err);
    const lines = r.out.trim().split('\n');
    assert.equal(lines[0], '3 2');
    /* lives within a quarter of 4, then 2 */
    assert.match(lines[1], /^[345],[345],[345],[12],[12] 9,9,9,7,7$/);
    /* after two steps the first three are at x 12, y 20 */
    assert.equal(lines[2], '0090');
});

test('pools are checked when handed back', () => {
    const r = run('function init() { actors.move({ x: 1 }, 1, 0, new Int32Array(4)); }\nfunction update() {}');
    assert.match(r.err, /actors\.move\(\) needs a pool from actors\.pool\(\)/);
    const r2 = run('function init() { var p = actors.pool(2); actors.move(p, 1, 0, []); }\nfunction update() {}');
    assert.match(r2.err, /writes its contacts to an Int32Array/);
    const r3 = run('function init() { var p = actors.pool(2); p.x = [1]; actors.move(p, 1, 0, new Int32Array(4)); }\nfunction update() {}');
    assert.match(r3.err, /arrays of one length/);
    const r4 = run('function init() { particles.step(actors.pool(2)); }\nfunction update() {}');
    assert.match(r4.err, /needs a pool from particles\.pool\(\)/);
});
