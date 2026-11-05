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

/* The plane. Sprites 1 and 2 have a different colour at every pixel, none
 * transparent, chequered over the map, so a pixel of the floor names the
 * world point it shows. */
const FLOOR = `
function floor() {
    for (var y = 0; y < 8; y++)
        for (var x = 0; x < 8; x++) {
            sset(8 + x, y, 1 + (x + y * 3) % 15);
            sset(16 + x, y, 1 + (x * 5 + y) % 15);
        }
    for (var r = 0; r < mapH(); r++)
        for (var c = 0; c < mapw(); c++) mset(c, r, 1 + (c + r) % 2);
}
`.replace('mapH()', '64');

test('plane: every floor pixel is the map under plane.pick, to within the steps', () => {
    const r = run(FLOOR + `
function init() {
    floor();
    plane.view(300.5, 200.25, 24, 0.13, 40, 0.25);
    cls(0);
    plane.draw();
    var at = [0, 0], bad = 0, n = 0;
    for (var y = 40; y < 120; y++)
        for (var x = 0; x < 160; x++) {
            if (!plane.pick(x, y, at)) { bad++; continue; }
            var wx = flr(at[0]), wy = flr(at[1]), want = 0;
            if (wx >= 0 && wy >= 0 && wx < 1024 && wy < 512) {
                var s = mget(flr(wx / 8), flr(wy / 8));
                want = sget((s % 16) * 8 + wx % 8, flr(s / 16) * 8 + wy % 8);
            }
            n++;
            if (pget(x, y) != want) bad++;
        }
    var sky = 0;
    for (y = 0; y < 40; y++) for (x = 0; x < 160; x++) if (pget(x, y) != 0) sky++;
    printh(n + " " + bad + " " + sky);
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    const [n, bad, sky] = r.out.trim().split(' ').map(Number);
    assert.equal(n, 80 * 160);
    assert.equal(sky, 0, 'nothing above the horizon');
    /* draw steps along a row and pick divides afresh: they part only
     * where a pixel's centre sits on a texel's edge */
    assert.ok(bad < 32, bad + ' of ' + n + ' differ');
});

test('plane: project and pick are each other\'s inverse on the floor', () => {
    const r = run(`function init() {
    plane.view(100, 100, 16, 0.3, 50, 0.2);
    var at = [0, 0], sc = [0, 0, 0, 0], worst = 0;
    for (var y = 52; y < 120; y += 7)
        for (var x = 3; x < 160; x += 13) {
            plane.pick(x, y, at);
            plane.project(at[0], at[1], 0, sc);
            worst = max(worst, max(abs(sc[0] - x), abs(sc[1] - (y + 0.0))));
        }
    printh(worst < 0.6 ? "ok" : "worst " + worst);
    printh(plane.project(100 - cos(0.3) * 10, 100 - sin(0.3) * 10, 0, sc) + " " + plane.pick(10, 49, at));
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    /* behind the camera, and above the horizon, are 0 */
    assert.equal(r.out, 'ok\n0 0\n');
});

test('plane: billboards stand on their point, far to near', () => {
    const r = run(`var xs = [0, 0, 0], ys = [0, 0, 0], zs = [0, 0, 0], sp = [3, 4, 5], size = [8, 8, 8], fl = [false, false, false];
function init() {
    for (var y = 0; y < 8; y++) for (var x = 0; x < 8; x++) { sset(24 + x, y, 8); sset(32 + x, y, 11); sset(40 + x, y, 12); }
    plane.view(0, 0, 8, 0, 60, 0.25);
    // ahead along x: 3 far, 4 near and in front of it, 5 behind the camera
    xs[0] = 40; xs[1] = 20; xs[2] = -10;
    cls(0);
    var k = plane.billboards(xs, ys, zs, sp, size, fl, 3), sc = [0, 0, 0, 0];
    plane.project(20, 0, 0, sc);
    printh(k + " " + flr(sc[0]) + " " + flr(sc[1]) + " " + pget(80, flr(sc[1]) - 1) + " " + pget(80, flr(sc[1]) + 1));
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    /* two drawn; the near one (11) covers the far one's centre and ends on
     * the ground line, which is the horizon less one plus z * scale */
    const [k, sx, sy, on, below] = r.out.trim().split(' ').map(Number);
    assert.equal(k, 2);
    assert.equal(sx, 80);
    assert.equal(on, 11);
    assert.equal(below, 0);
    assert.ok(sy > 60 && sy < 120, 'ground row ' + sy);
});

test('plane: the camera comes first, and off the map is left or filled', () => {
    const r = run('function init() { plane.draw(); }\nfunction update() {}');
    assert.match(r.err, /plane\.draw\(\) needs a camera first/);
    const r2 = run(FLOOR + `function init() {
    floor();
    plane.view(-200, -200, 20, 0.625, 30, 0.25);
    cls(7); plane.draw(); var a = pget(80, 119);
    cls(7); plane.draw(30, 119, 1); var b = pget(80, 119);
    printh(a + " " + (b != 7 ? "filled" : "left"));
}
function update() {}`);
    assert.equal(r2.status, 0, r2.err);
    /* looking away from the map: nothing drawn, then sprite 1 everywhere */
    assert.equal(r2.out, '7 filled\n');
});

/* The clock: rate(), setrate() and skipped(), beside the systems that
 * lean on it */
test('setrate changes the rate from the next tick and t() carries on', () => {
    const r = run(`var n = 0;
function init() { printh(rate()); }
function update() {
    n++;
    if (n == 4) { printh(t()); setrate(25); printh(t() + " " + rate()); }
    if (n == 5 || n == 29) printh(t());
}`, 29);
    assert.equal(r.status, 0, r.err);
    const [r0, before, after, at5, at29] = r.out.trim().split('\n');
    assert.equal(r0, '33');
    assert.equal(after, before + ' 25');
    /* one tick at 25 is 0.04 s, and 24 more make 1 s */
    assert.ok(Math.abs(Number(at5) - Number(before) - 0.04) < 0.001, at5);
    assert.ok(Math.abs(Number(at29) - Number(at5) - 0.96) < 0.001, at29);
    const bad = run('function init() { setrate(30); }\nfunction update() {}');
    assert.match(bad.err, /setrate\(\) takes 20, 25, 33 or 50/);
});

test('skipped counts the frames the host did not draw, once', () => {
    const r = run(`var n = 0;
function init() {}
function update() { n++; if (n == 3 || n == 8 || n == 9) printh(n + " " + skipped()); }`,
        9, ['--skip', '1:2,4:1,6:3']);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, '3 2\n8 4\n9 0\n');
});

test('sprites.draw draws from arrays of any kind, as spr would', () => {
    const SHEET = `for (var y = 0; y < 8; y++) for (var x = 0; x < 8; x++) { sset(8 + x, y, 1 + x); sset(16 + x, y, 9); }`;
    const batch = run(`function init() {
    ${SHEET}
    var sp = new Uint8Array(3), xs = new Int32Array(3), ys = [0, 10.5, 20], fl = [false, true, 0];
    sp[0] = 1; sp[1] = 1; sp[2] = 2; xs[0] = 0; xs[1] = 30; xs[2] = 156;
    cls(); camera(-2, 0);
    sprites.draw(sp, xs, ys, 3, fl);
    sprites.draw(2, [70], [40], 1);
}
function update() {}`, 1, ['--dump', '0,0,160,50']);
    const one = run(`function init() {
    ${SHEET}
    cls(); camera(-2, 0);
    spr(1, 0, 0); spr(1, 30, 10.5, 1, 1, true); spr(2, 156, 20); spr(2, 70, 40);
}
function update() {}`, 1, ['--dump', '0,0,160,50']);
    assert.equal(batch.status, 0, batch.err);
    assert.equal(one.status, 0, one.err);
    assert.equal(batch.out, one.out);
    const bad = run('function init() { sprites.draw([1], [0], [0], 2); }\nfunction update() {}');
    assert.match(bad.err, /n no longer than its arrays/);
});

test('plane: a billboard can be several sprites, and z lifts it', () => {
    const r = run(`function init() {
    // a 1x2 figure: sprite 1 on top (8), sprite 17 below it (11)
    for (var y = 0; y < 8; y++) for (var x = 0; x < 8; x++) { sset(8 + x, y, 8); sset(8 + x, 8 + y, 11); }
    plane.view(0, 0, 8, 0, 60, 0.25);
    var sc = [0, 0, 0, 0];
    plane.project(20, 0, 0, sc);
    var foot = flr(sc[1]);
    cls(0);
    plane.billboards([20], [0], 0, 1, 16, 0, 1, 1, 2);
    printh(foot + " " + pget(80, foot - 1) + " " + pget(80, foot - 40) + " " + pget(80, foot) + " " + pget(80 - 15, foot - 2) + " " + pget(80 - 17, foot - 2));
    cls(0);
    plane.billboards([20], [0], [4], [1], [16], [false], 1, [1], [2]);
    printh(pget(80, foot - 1) + " " + pget(80, foot - 17));
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    const [first, second] = r.out.trim().split('\n');
    /* 16 world pixels tall at scale 4 is 64 rows on the floor, 32 wide:
     * the lower sprite at the feet, the upper one 40 rows up */
    const [foot, low, high, below, inside, outside] = first.split(' ').map(Number);
    assert.ok(foot > 60 && foot < 120, 'foot ' + foot);
    assert.deepEqual([low, high, below, inside, outside], [11, 8, 0, 11, 0]);
    /* z 4 is 16 rows up: the feet are clear, the figure starts above */
    assert.equal(second, '0 11');
});

test('links: a pinned rope hangs to its length and stays out of walls', () => {
    const r = run(`var rope, tk = 0;
function init() {
    rope = links.pool(11, 10);
    rope.gravity = 0.2; rope.drag = 0.02;
    links.point(rope, 40, 10, true);
    for (var i = 1; i <= 10; i++) { links.point(rope, 40 + i * 4, 10); links.link(rope, i - 1, i); }
}
function update() {
    tk++;
    links.step(rope, 0);
    if (tk == 300) {
        var worst = 0;
        for (var k = 0; k < 10; k++) {
            var a = rope.a[k], b = rope.b[k];
            var dx = rope.x[b] - rope.x[a], dy = rope.y[b] - rope.y[a];
            worst = max(worst, abs(sqrt(dx * dx + dy * dy) - rope.len[k]));
        }
        printh(rope.x[0] + " " + rope.y[0] + " " + flr(rope.x[10] + 0.5) + " " + flr(rope.y[10] + 0.5) + " " + (worst < 0.5));
    }
}`, 300);
    assert.equal(r.status, 0, r.err);
    /* pinned at 40,10; ten links of 4 hang straight down to about 50 */
    const [px, py, ex, ey, tight] = r.out.trim().split(' ');
    assert.deepEqual([px, py], ['40', '10']);
    assert.ok(Math.abs(Number(ex) - 40) <= 1, 'below the pin: ' + ex);
    assert.ok(Number(ey) >= 48 && Number(ey) <= 52, 'its length down: ' + ey);
    assert.equal(tight, '1');
    const wall = run(`var rope, tk = 0;
function init() {
    fset(1, 1);
    // a block of map cells 5-9 across, row 6 (y 48-55)
    for (var c = 5; c < 10; c++) mset(c, 6, 1);
    rope = links.pool(12, 11);
    rope.gravity = 0.2;
    for (var i = 0; i < 12; i++) { links.point(rope, 44 + i * 3, 20); if (i) links.link(rope, i - 1, i); }
}
function update() {
    tk++;
    links.step(rope, 1);
    if (tk == 200) {
        var inside = 0;
        for (var i = 0; i < 12; i++) if (fget(mget(flr(rope.x[i] / 8), flr(rope.y[i] / 8))) & 1) inside++;
        printh(inside + " " + flr(rope.y[6]));
    }
}
function draw() { cls(); links.draw(rope, 7); }`, 200, ['--dump', '40,40,40,10']);
    assert.equal(wall.status, 0, wall.err);
    /* draped over the block: nothing inside it, the middle resting on top */
    const lines = wall.out.trim().split('\n');
    const [inside, mid] = lines[0].split(' ').map(Number);
    assert.equal(inside, 0);
    assert.ok(mid >= 44 && mid < 48, 'resting on the block: ' + mid);
    assert.ok(lines.slice(1).join('').includes('7'), 'drawn');
    const bad = run('function init() { links.step({ x: [] }, 0); }\nfunction update() {}');
    assert.match(bad.err, /links\.step\(\) needs a pool from links\.pool\(\)/);
});

test('land: ground from the camera, near hills hiding far ones, heights read back', () => {
    const r = run(`var hm, cm;
function init() {
    hm = img(64, 64); cm = img(64, 64);
    target(hm); cls(0); target(cm); cls(11);
    // a tall red pillar at 40..43, 30..33, straight ahead of the camera,
    // beyond the nearest ground the screen shows (about 23 out)
    target(hm); rectfill(40, 30, 43, 33, 200);
    target(cm); rectfill(40, 30, 43, 33, 8);
    target(SCREEN);
    plane.view(0, 32, 20, 0, 50, 0.25);
    cls(1);
    printh(land.draw(hm, cm, 200) > 0);
    // above the horizon untouched, the ground below it green, the pillar
    // standing above the horizon in red in the middle
    printh(pget(10, 30) + " " + pget(10, 100) + " " + pget(80, 40) + " " + pget(80, 110));
    printh(land.height(hm, 41, 31) + " " + land.height(hm, 41, 31, 0.5) + " " + land.height(hm, 105, 95, 0.5));
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    assert.deepEqual(r.out.trim().split('\n'), ['1', '1 11 8 11', '200 100 100']);
    const bad = run('function init() { land.draw(img(64, 64), img(64, 64)); }\nfunction update() {}');
    assert.match(bad.err, /needs a camera first/);
    const odd = run('function init() { plane.view(0, 0, 9, 0, 50, 0.25); land.draw(img(60, 64), img(60, 64)); }\nfunction update() {}');
    assert.match(odd.err, /a power of two/);
});

test('walls: a wall ahead at its height, billboards hidden behind it', () => {
    const r = run(`function init() {
    for (var y = 0; y < 8; y++) for (var x = 0; x < 8; x++) { sset(8 + x, y, 8); sset(16 + x, y, 11); }
    fset(1, 1);
    // a wall of sprite 1 across column 5 (x 40-47), the camera at x 4
    // looking along x, eye 4 up a wall 8 tall: the wall is 36 away
    for (var r = 0; r < 16; r++) mset(5, r, 1);
    plane.view(4, 64, 4, 0, 60, 0.25);
    cls(0);
    printh(walls.draw(1));
    var top = -1, bottom = -1;
    for (var y = 0; y < 120; y++) if (pget(80, y) == 8) { if (top < 0) top = y; bottom = y; }
    printh(top + " " + bottom);
    // a ghost (sprite 2) behind the wall, then one in front of it
    plane.billboards([60], [64], [0], [2], [8], [false], 1);
    printh(pget(80, 64));
    plane.billboards([20], [64], [0], [2], [8], [false], 1);
    printh(pget(80, 62));
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    const out = r.out.trim().split('\n');
    /* every column meets the wall; a wall 8 tall seen from 4 up, 36 away
     * at focal 80: 4 * 80 / 36 = 8.9 rows above and below the horizon */
    assert.equal(out[0], '160');
    const [top, bottom] = out[1].split(' ').map(Number);
    assert.ok(Math.abs(top - 50) <= 1 && Math.abs(bottom - 67) <= 1, out[1]);
    /* behind: the wall shows; in front: the ghost does */
    assert.deepEqual(out.slice(2), ['8', '11']);
    const bad = run('function init() { walls.draw(1); }\nfunction update() {}');
    assert.match(bad.err, /needs a camera first/);
});

test('playback.playing reports the owner music', () => {
    const quiet = run('function init() { printh(playback.playing()); }\nfunction update() {}');
    const loud = run('function init() { printh(playback.playing()); }\nfunction update() {}', 1, ['--playing']);
    assert.deepEqual([quiet.out, loud.out], ['0\n', '1\n']);
});

test('plane: fog covers the far ground, and coarse draws pixel pairs', () => {
    const r = run(FLOOR + `function init() {
    floor();
    plane.view(300.5, 200.25, 24, 0.13, 40, 0.25);
    plane.fog(0, 1, 9);
    cls(0); plane.draw();
    var fogged = 0;
    for (var y = 40; y < 120; y++) for (var x = 0; x < 160; x++) if (pget(x, y) == 9) fogged++;
    plane.fog(0, 0, -1);
    cls(0); plane.draw(40, 119, -1, 2);
    var pairs = 0;
    for (y = 40; y < 120; y++) for (x = 0; x < 160; x += 2) if (pget(x, y) == pget(x + 1, y)) pairs++;
    printh(fogged + " " + pairs);
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    /* every floor pixel fogged; every pair the same with coarse */
    assert.equal(r.out, '12800 6400\n');
});
