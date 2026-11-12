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

test('overlaps finds the same pairs, in the same order, as trying every pair', () => {
    const r = run(`function init() {
    srand(7);
    var a = actors.pool(300), b = actors.pool(200), out = new Int32Array(20000);
    for (var i = 0; i < 300; i++) { actors.spawn(a); a.x[i] = rnd(300); a.y[i] = rnd(200); a.w[i] = 2 + flr(rnd(20)); a.h[i] = 2 + flr(rnd(20)); if (i % 7 == 0) a.on[i] = 0; }
    for (i = 0; i < 200; i++) { actors.spawn(b); b.x[i] = rnd(300); b.y[i] = rnd(200); b.w[i] = 2 + flr(rnd(40)); b.h[i] = 2 + flr(rnd(8)); if (i % 5 == 0) b.on[i] = 0; }
    printh(same(a, b, out) + " " + same(a, a, out));
}
// every pair, as overlaps would find them in order
function same(p, q, out) {
    var k = actors.overlaps(p, q, out), at = 0, ok = 1;
    for (var i = 0; i < p.x.length; i++) {
        if (!p.on[i]) continue;
        for (var j = p == q ? i + 1 : 0; j < q.x.length; j++) {
            if (!q.on[j]) continue;
            if (p.x[i] < q.x[j] + q.w[j] && q.x[j] < p.x[i] + p.w[i] &&
                p.y[i] < q.y[j] + q.h[j] && q.y[j] < p.y[i] + p.h[i]) {
                if (at >= k || out[at * 2] != i || out[at * 2 + 1] != j) ok = 0;
                at++;
            }
        }
    }
    return ok && at == k ? "same " + k : "differ " + at + " " + k;
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    const [ab, aa] = r.out.trim().split(' same ');
    assert.match(r.out, /^same \d+ same \d+\n$/);
    assert.ok(Number(ab.split(' ')[1]) > 50 && Number(aa) > 50, r.out);
});

test('one-way cells and platforms stop an actor coming down, not going up', () => {
    const r = run(`var p, plat, out = new Int32Array(40), tk = 0;
function init() {
    // flag 4: one-way; a ledge of it along row 8 (y 64-71), and a one-way
    // platform at y 40, 40 wide, from x 60
    fset(3, 4);
    for (var c = 0; c < 6; c++) mset(c, 8, 3);
    plat = actors.pool(1); actors.spawn(plat);
    plat.x[0] = 60; plat.y[0] = 40; plat.w[0] = 40; plat.h[0] = 4; plat.oneway = 1;
    p = actors.pool(4);
    p.gravity = 0.5;
    p.platforms = plat;
    // falling onto the ledge; jumping up through it from below; walking
    // across it at its height; falling onto the platform
    actors.spawn(p); p.x[0] = 8; p.y[0] = 30;
    actors.spawn(p); p.x[1] = 24; p.y[1] = 90; p.dy[1] = -9;
    actors.spawn(p); p.x[2] = 0; p.y[2] = 64; p.dx[2] = 2; p.dy[2] = -0.5;
    actors.spawn(p); p.x[3] = 70; p.y[3] = 10;
}
function update() {
    tk++;
    plat.x[0] += 1;
    plat.dx[0] = 1;
    actors.move(p, 1, 0, out, 4);
    if (tk == 6) printh("up " + (p.y[1] < 64));
    if (tk == 40) printh(p.y[0] + " " + (p.x[2] > 48) + " " + p.y[3] + " " + (p.x[3] > 100));
}`, 40);
    assert.equal(r.status, 0, r.err);
    /* 0 lands on the ledge (64 - 8); 1 jumped up through it; 2 walked
     * through its side; 3 lands on the platform (40 - 8) and rides it */
    assert.deepEqual(r.out.trim().split('\n'), ['up 1', '56 1 32 1']);
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

/* Sprite 1's top row is colours 1 2 3 0: drawn shaded from colour 8, it is
 * 8 9 10 and see-through; flipped across, the other way round */
test('sprite particles draw their sprite in their colour ramp, flipped', () => {
    const r = run(`var p;
function init() {
    sset(8, 0, 1); sset(9, 0, 2); sset(10, 0, 3);
    p = particles.pool(2);
    p.w = 4; p.h = 1; p.shade = 3;
    printh(particles.emit(p, 2, 0, 0, 0, 0, 0, 9, 8, 1));
    printh(p.sp.join(",") + " " + (p.flip[0] >= 0 && p.flip[0] <= 3));
    p.x[0] = 10; p.y[0] = 20; p.flip[0] = 0;
    p.x[1] = 10; p.y[1] = 21; p.flip[1] = 1;
}
function update() {}
function draw() { cls(5); particles.draw(p); }`, 1, ['--dump', '10,20,4,2']);
    assert.equal(r.status, 0, r.err);
    assert.deepEqual(r.out.trim().split('\n'), ['2', '1,1 1', '89a5', '5a98']);
});

/* Half off the screen's left edge a sprite particle goes the clipped way
 * round; whole, the quick way; both draw the same pixels */
test('sprite particles clipped by the edge draw what is on the screen', () => {
    const r = run(`var p;
function init() {
    sset(8, 0, 1); sset(9, 0, 2); sset(10, 0, 3);
    p = particles.pool(2);
    p.w = 3; p.h = 1; p.shade = 3;
    particles.emit(p, 2, 0, 0, 0, 0, 0, 9, 8, 1);
    p.x[0] = -1; p.y[0] = 0; p.flip[0] = 0;
    p.x[1] = 4; p.y[1] = 0; p.flip[1] = 0;
}
function update() {}
function draw() { cls(5); particles.draw(p); }`, 1, ['--dump', '0,0,8,1']);
    assert.equal(r.status, 0, r.err);
    assert.deepEqual(r.out.trim().split('\n'), ['9a5589a5']);
});

test('particles bounce off the floor, and sprites turn as they spin', () => {
    const r = run(`var p;
function init() {
    p = particles.pool(3);
    p.floor = 30; p.bounce = 0.5; p.spin = 1;
    particles.emit(p, 1, 0, 29, 0, 2.0, 0, 50, 7, 1);
    particles.emit(p, 1, 0, 29, 0, 0.25, 0, 50, 7);
    p.flip[0] = 0;
}
function update() {
    particles.step(p);
    printh(p.y[0] + " " + p.dy[0] + " " + p.y[1] + " " + p.flip[0]);
}`, 2);
    assert.equal(r.status, 0, r.err);
    /* the fast one bounces at half its speed; the slow one falls through,
     * a quarter pixel's bounce being too little; flips go 0, 1, 3, 2 */
    const lines = r.out.trim().split('\n');
    assert.match(lines[0], /^30 -1 29\.25 [13]$/);
    assert.match(lines[1], /^29 -1 29\.5 [032]$/);
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

test('a FixArray holds fractions, wraps as a fraction does, and copies, fills and stores as one', () => {
    const r = run(`var a = new FixArray(4);
function init() {
    a[0] = 1.5; a[1] = 3; a[2] = -0.25; a[3] = 40000;
    printh(a.join(",") + " " + a.length + " " + (a[0] + a[1]) + " " + a.indexOf(3) + " " + a[9]);
    var b = new FixArray(4);
    copy(b, 0, a, 0, 4);
    fill(b, 0.5, 2);
    printh(b.join(","));
    a.reverse();
    printh("" + a);
}
function update() {}`, 1);
    assert.equal(r.status, 0, r.err);
    /* 40000 wraps to 40000 - 65536 */
    assert.equal(r.out, '1.5,3,-0.25,-25536 4 4.5 1 null\n1.5,3,0.5,0.5\n-25536,-0.25,3,1.5\n');
    const mixed = run('var a = new FixArray(2), b = new Int32Array(2);\nfunction init() { copy(a, 0, b, 0, 2); }\nfunction update() {}', 1);
    assert.equal(mixed.status, 1);
    assert.match(mixed.err, /copy\(\) needs two arrays of one kind/);
});

test('textfit cuts between characters to fit, with ... or its own ending, and leaves what fits alone', () => {
    const r = run(`function init() {
    printh(textfit("SHORT", 40) + "|" + textfit("A LONGER TITLE", 40) + "|" + textfit("A LONGER TITLE", 40, "~"));
    font(FONT.WORLD);
    var s = textfit("Café del Mar über alles", 60);
    printh(s + "|" + (textw(s) <= 60) + "|" + textfit("ééééééé", 20));
}
function update() {}`, 1);
    assert.equal(r.status, 0, r.err);
    const [caps, world] = r.out.trim().split('\n');
    /* CAPS is 4 a character: 40 holds 10, "..." 12 of them, so 7 with it;
     * the space before the cut goes */
    assert.equal(caps, 'SHORT|A LONGE...|A LONGER~');
    const [cut, fits, accents] = world.split('|');
    assert.ok(cut.endsWith('...') && fits === '1', world);
    /* never half a character: every é whole */
    assert.match(accents, /^(é)*\.\.\.$/);
});

test('ending() runs once as the cart closes, after its last tick', () => {
    const r = run(`var n = 0;
function init() {}
function update() { n++; }
function ending() { printh("ending after " + n); }`, 3);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, 'ending after 3\n');
    const quits = run(`function init() {}
function update() { quit(); }
function ending() { printh("ending"); }`, 5);
    assert.equal(quits.out, 'quit\nending\n');
});

test('rng generators keep their own streams, and rnd()\'s state is kept and put back', () => {
    const r = run(`var g = new Int32Array(2), h = new Int32Array(2), s = new Int32Array(2);
function init() {
    rng.seed(g, 7); rng.seed(h, 7);
    var a = rng.rnd(g, 100) + " " + rng.rnd(g, 100);
    srand(1);
    var x = rnd(1000);
    var b = rng.rnd(h, 100) + " " + rng.rnd(h, 100);
    rndstate(s);
    var y = rnd(1000), z = rnd(1000);
    srand(s);
    printh(a + "|" + b + "|" + (rnd(1000) == y) + (rnd(1000) == z) + "|" + rng.rnd(g, ["x"]) + "|" + (rng.rnd(g) < 1));
}
function update() {}`, 1);
    assert.equal(r.status, 0, r.err);
    const [a, b, same, pick, frac] = r.out.trim().split('|');
    /* one seed, one stream, whatever rnd() did between */
    assert.equal(a, b);
    assert.equal(same, '11');
    assert.equal(pick, 'x');
    assert.equal(frac, '1');
    const bad = run('function init() { rng.rnd(5); }\nfunction update() {}', 1);
    assert.match(bad.err, /rng\.rnd\(\) needs an Int32Array of 2/);
});

test('particles: bounds as drawn, cull off the screen, 16-pixel sprites, and a negative drag speeds up', () => {
    const r = run(`var p = particles.pool(8), q = particles.pool(2), box = new Int32Array(4);
function init() {
    printh(particles.bounds(p, box) + " " + box.join(","));
    particles.emit(p, 1, 10, 20, 0, 0, 0, 50, 7);
    particles.emit(p, 1, 40, 5, 0, 0, 0, 50, 7, 1);
    p.w = 16; p.h = 16;
    camera(5, 0);
    printh(particles.bounds(p, box) + " " + box.join(","));
    camera();
    p.cull = true; p.drag = -0.5;
    particles.emit(p, 1, 150, 60, 2, 0, 0, 50, 7);
    q.cull = true;
    particles.emit(q, 1, 300, 60, 0, 0, 0, 50, 7);
}
function update() {
    particles.step(p);
    particles.step(q);
    var x = 0;
    for (var i = 0; i < 8; i++) if (p.life[i] > 0 && p.x[i] > 100) x = p.x[i];
    printh(frame() + " " + x + " " + (q.life[0] > 0));
}
function draw() {
    particles.draw(p);
    camera(250, 0);              /* q is in the world, seen through this */
    particles.draw(q);
    camera();                    /* and the HUD drawn without it */
}`, 4);
    assert.equal(r.status, 0, r.err);
    const lines = r.out.trim().split('\n');
    /* none live: 0, the box untouched; then a dot at 10,20 and a 16x16
     * sprite at 40,5, both moved 5 left by the camera */
    assert.equal(lines[0], '0 0,0,0,0');
    assert.equal(lines[1], '2 5,5,50,20');
    /* moving 3 (2 less a drag of -0.5), then 4.5, then 6.75: past 160 on
     * the third step, it is culled as it is drawn; q, on the screen through
     * the camera it is drawn by, lives on though update() has none */
    assert.equal(lines[2], '1 153 1');
    assert.equal(lines[3], '2 157.5 1');
    assert.equal(lines[4], '3 164.25 1');
    assert.equal(lines[5], '4 0 1');
});

test('btnr, imgw and imgh, unitsnow', () => {
    const r = run(`var i = img(12, 7);
function init() { printh(imgw(i) + " " + imgh(i)); }
function update() { printh(frame() + " " + btnr(4) + " " + (unitsnow() > 0)); }`, 5, ['--buttons', '2:16,4:0']);
    assert.equal(r.status, 0, r.err);
    /* --buttons counts ticks from 0: Select held for frames 3 and 4, let
     * go for 5 */
    assert.equal(r.out, '12 7\n1 0 1\n2 0 1\n3 0 1\n4 0 1\n5 1 1\n');
});

test('path.view marks what path.sight sees within a radius, and a mark remembers', () => {
    const r = run(`var b = new Uint8Array(7 * 5), seen = new Uint8Array(7 * 5), known = new Uint8Array(7 * 5);
function init() {
    // a wall down column 3, rows 0 to 3; the viewer at 1,2
    for (var y = 0; y < 4; y++) b[y * 7 + 3] = 1;
    var n = path.view(b, 7, 0, 1, 2, 9, seen);
    var row = "";
    for (var y = 0; y < 5; y++) { for (var x = 0; x < 7; x++) row += seen[y * 7 + x]; row += "/"; }
    printh(n + " " + row);
    var agree = 1;
    for (var y = 0; y < 5; y++) for (var x = 0; x < 7; x++)
        if (b[y * 7 + x] == 0 && seen[y * 7 + x] != path.sight(b, 7, 0, 1, 2, x, y)) agree = 0;
    printh("agrees " + agree);
    known[0] = 9;
    path.view(b, 7, 0, 1, 2, 1, known, 5);
    printh(known[0] + " " + known[1 * 7 + 1] + " " + known[2 * 7 + 6]);
}
function update() {}`, 1);
    assert.equal(r.status, 0, r.err);
    const [first, agree, marks] = r.out.trim().split('\n');
    /* the wall's face is seen, its ends past a corner not, nor anything
     * behind it: the corner rule path.sight has */
    assert.equal(first, '18 1110000/1111000/1111000/1111000/1110000/');
    assert.equal(agree, 'agrees 1');
    /* radius 1 marks around 1,2 with 5, leaving the 9 at 0,0 */
    assert.equal(marks, '9 5 0');
});

test('actors walk up a ramp and hang on a ladder', () => {
    const r = run(`var p = actors.pool(2), hits = new Int32Array(16);
function init() {
    fset(1, true, 0);           // floor: flag bit 1
    fset(2, true, 1);           // a ramp rising right: flag bit 2
    fset(3, true, 2);           // ladder: flag bit 4
    for (var x = 0; x < 20; x++) mset(x, 10, 1);
    mset(5, 9, 2);
    for (var y = 5; y < 10; y++) mset(12, y, 3);
    p.gravity = 0.5; p.slope_up = 2; p.ladders = 4;
    actors.spawn(p); p.x[0] = 20; p.y[0] = 72; p.w[0] = 4; p.h[0] = 8; p.dx[0] = 1;
    actors.spawn(p); p.x[1] = 94; p.y[1] = 48; p.w[1] = 4; p.h[1] = 8;
}
function update() {
    actors.move(p, 1, 0, hits);
    p.dx[0] = 1;
    if (p.x[0] == 42) printh("ramp " + p.y[0]);
    if (frame() == 30) printh("ladder " + p.y[1] + " " + p.dy[1]);
}`, 30);
    assert.equal(r.status, 0, r.err);
    /* x 42: the feet's middle 44, 4 pixels into the ramp's cell at 40, so
     * the surface 72 + 3 and the actor 8 tall above it; the ladder's actor
     * where it began, gravity let go */
    assert.equal(r.out, 'ramp 67\nladder 48 0\n');
});

test('resumed tells of a pause once', () => {
    const r = run(`var n = 0;
function init() {}
function update() { n++; if (n == 3 || n == 5 || n == 6) printh(n + " " + resumed()); }`,
        6, ['--pause', '1,4']);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, 'paused\n3 1\npaused\n5 1\n6 0\n');
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

test('links: a cloth hangs, ripples in depth and shades, and tears', () => {
    const r = run(`var p, tk = 0;
function init() {
    p = links.pool(48, 200);
    p.gravity = 0.1;
    // 8 by 6, 4 apart: 48 points, 7*6 + 8*5 across and down, 2*7*5 diagonals
    printh(links.cloth(p, 20, 10, 8, 6, 4) + " " + p.points + " " + p.links + " " + p.pin[0] + p.pin[7] + p.pin[8]);
    printh(links.cloth(p, 0, 0, 2, 2, 4));
}
function update() {
    tk++;
    for (var i = 8; i < p.points; i++) p.z[i] += sin(tk * 0.02 + i * 0.1) * 0.3;
    links.step(p, 0);
    if (tk == 60) {
        cls(0);
        var cells = links.fill(p, [1, 2, 8, 14, 15]);
        var seen = [];
        for (var y = 0; y < 120; y++) for (var x = 0; x < 160; x++) { var c = pget(x, y); if (c && seen.indexOf(c) < 0) seen.push(c); }
        printh(cells + " " + (seen.length > 1) + " " + (p.y[47] > 20));
        // torn: pull the bottom row far down, past twice its links
        p.tear = 1;
        for (var k = 40; k < 48; k++) { p.y[k] += 40; p.oy[k] = p.y[k]; }
        links.step(p, 0);
        printh((p.broken > 0) + " " + (links.fill(p, [7]) < 35));
    }
}`, 60);
    assert.equal(r.status, 0, r.err);
    assert.deepEqual(r.out.trim().split('\n'), ['0 48 152 110', '-1', '35 1 1', '1 1']);
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

test('land: a ramp colours the ground by its height, not by its colours', () => {
    const r = run(`var hm, cm;
function init() {
    // flat ground with the pillar of the test above, its colours all 1
    hm = img(64, 64); cm = img(64, 64);
    target(cm); cls(1);
    target(hm); cls(0); rectfill(40, 30, 43, 33, 200);
    target(SCREEN);
    plane.view(0, 32, 20, 0, 50, 0.25);
    cls(0);
    // green below 100, red from there up
    var ramp = [];
    for (var h = 0; h < 256; h++) ramp.push(h < 100 ? 11 : 8);
    printh(land.draw(hm, cm, 200, 1, 1, ramp) > 0);
    printh(pget(10, 100) + " " + pget(80, 40) + " " + pget(80, 110));
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    assert.deepEqual(r.out.trim().split('\n'), ['1', '11 8 11']);
    const bad = run('function init() { plane.view(0, 0, 9, 0, 50, 0.25); land.draw(img(64, 64), img(64, 64), 100, 1, 1, 5); }\nfunction update() {}');
    assert.match(bad.err, /needs its ramp as an array of colours/);
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

test('beat.offset takes its calibration off each press, within -500 to 500', () => {
    const r = run(`function init() { printh(beat.offset() + " " + beat.offset(45) + " " + beat.offset(900)); beat.offset(45); }
function update() { if (beat.press(4) >= 0) printh("press " + beat.press(4) + " " + beat.clock()); }`,
                  60, ['--beat', '120', '--buttons', '50:16,51:0']);
    assert.equal(r.status, 0, r.err);
    /* the press the test below sees at 1545, judged 45 ms earlier */
    assert.equal(r.out, '0 45 500\npress 1500 1545\n');
});

test('beat: the clock, the lock, the next beat, its bar and a press', () => {
    const cart = `var st = new Int32Array(6), md = new Int32Array(3), tk = 0;
function init() { beat.listen(true); }
function update() {
    tk++;
    var locked = beat.state(st);
    if (tk == 10) printh(locked + " " + beat.next(beat.clock(), 0));
    if (tk == 40) {
        printh(beat.clock() + " " + locked + " " + st[1] + " " + st[2]);
        printh(beat.next(beat.clock(), 0) + " " + beat.inbar() + " " +
               beat.next(beat.clock(), 1) + " " + beat.inbar());
        printh(beat.downbeat() + " " + beat.margin());
        beat.listen(false);
    }
    if (beat.press(4) >= 0) printh("press " + beat.press(4) + " " + beat.clock());
    if (tk == 60) { beat.mood(md); printh(md[0] + " " + beat.left() + " " + beat.jumped()); }
}`;
    const r = run(cart, 60, ['--beat', '120', '--buttons', '50:16,51:0']);
    assert.equal(r.status, 0, r.err);
    /* 33 ticks a second: no lock at first, locked a second after listening
     * began, beats on the half-second, the press stamped with its tick */
    assert.deepEqual(r.out.trim().split('\n'), [
        '0 -1', '1212 1 120 500', '1500 3 2000 0', '0 30',
        /* the first track is 214 s long */
        'press 1545 1545', '30 212182 0']);
    /* with no music, the clock stands still and nothing locks */
    const q = run(cart, 45);
    assert.deepEqual(q.out.trim().split('\n'),
                     ['0 -1', '0 0 0 0', '-1 -1 -1 -1', '-1 0']);
});

test('playback: the spectrum bands and the track\'s Sound Index record', () => {
    const cart = `var b = new Int32Array(8), s = new Int32Array(SOUND.SIZE);
function init() {
    printh(playback.sound(s) + " " + s[SOUND.PERIOD] + " " + s[SOUND.TONIC] +
           " " + s[SOUND.MODE] + " " + SOUND.SIZE);
}
function update() {
    playback.bands(b, 4);
    printh(b[0] + " " + b[1] + " " + b[2] + " " + b[3]);
}`;
    const r = run(cart, 1, ['--beat', '120']);
    assert.equal(r.status, 0, r.err);
    assert.deepEqual(r.out.trim().split('\n'), ['1 500 9 1 31', '89 50 40 30']);
    const q = run(cart, 1);
    assert.deepEqual(q.out.trim().split('\n'), ['0 0 0 0 31', '0 0 0 0']);
    const bad = run('var b = new Int32Array(8);\nfunction init() { playback.bands(b, 9); }\nfunction update() {}');
    assert.match(bad.err, /1 to 8 bands/);
});

test('beat.ahead: the hits heard ahead, only while listening', () => {
    const cart = `var h = new Int32Array(30);
function init() { beat.listen(true); }
function update() {
    if (frame() != 30) return;
    var now = beat.clock(), n = beat.ahead(h, now, now + 1200);
    printh(now + " " + beat.horizon() + " " + n);
    for (var i = 0; i < n; i++) printh(h[i * 3] + " " + h[i * 3 + 1] + " " + h[i * 3 + 2]);
    beat.listen(false);
    printh(beat.ahead(h, now, now + 1200) + " " + beat.horizon());
}`;
    const r = run(cart, 31, ['--beat', '120']);
    assert.equal(r.status, 0, r.err);
    /* the stand-in's kick, hat, kick and snare, hat, and the bar's accent */
    assert.deepEqual(r.out.trim().split('\n'), ['909 2709 5', '1000 1 128',
        '1250 4 128', '1500 3 128', '1750 4 128', '2000 1 192', '0 -1']);
});

test('sys: a setting by its config name, and a phrase translated', () => {
    const cart = `function init() {
    var v = sys.setting("volume"), t = sys.setting("theme");
    printh(v + " " + (v + 1) + " " + t + " " + sys.setting("nothing"));
    printh(sys.translate("Playlist") + "|" + sys.translate("my own words"));
}
function update() {}`;
    const r = run(cart, 0, ['--setting', 'volume=-25', '--setting', 'theme=scrim']);
    assert.equal(r.status, 0, r.err);
    /* a number is a number; anything else text; unknown is null; with no
     * language but English a phrase comes back as it went */
    assert.deepEqual(r.out.trim().split('\n'),
                     ['-25 -24 scrim null', 'Playlist|my own words']);
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

test('playback.toggle, next and prev reach the host, for a cart that controls the music', () => {
    const main = 'function init() { playback.toggle(); playback.next(); playback.prev(); }\nfunction update() {}';
    const res = compileFiles({ 'cart.json': JSON.stringify({ id: '00000000000c0e70', name: 'Control', music: 'control' }),
                               'main.js': main });
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'control.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '1'], { encoding: 'utf8' });
    assert.equal(r.status, 0, r.stderr);
    assert.equal(r.stdout.replace(/\r/g, ''), 'control 0\ncontrol 1\ncontrol 2\n');
    const listen = run(main);
    assert.match(listen.errors[0].message, /needs the cart's music level to be control/);
});

test('plane: billboards fade into the fog with the ground', () => {
    const r = run(`function init() {
    // sprite 1 a solid red 8x8 with a transparent (0) column
    for (var y = 0; y < 8; y++) for (var x = 0; x < 8; x++) sset(8 + x, y, x == 3 ? 0 : 8);
    plane.view(0, 0, 8, 0, 60, 0.25);
    // one near, one in the fog's middle, one past its far end
    var xs = [20, 60, 140], ys = [0, 0, 0];
    plane.fog(40, 100, 12);
    cls(1);
    plane.billboards(xs, ys, 0, 1, 16, 0, 3);
    var near = 0, red = 0, blue = 0, through = 0;
    for (var y = 0; y < 120; y++) for (var x = 0; x < 160; x++) {
        var c = pget(x, y);
        if (c == 8) red++;
        if (c == 12) blue++;
    }
    printh((red > 0) + " " + (blue > 0));
    // without fog, nothing is the fog's colour
    plane.fog(0, 0, -1);
    cls(1);
    plane.billboards(xs, ys, 0, 1, 16, 0, 3);
    blue = 0;
    for (y = 0; y < 120; y++) for (x = 0; x < 160; x++) if (pget(x, y) == 12) blue++;
    printh(blue);
    // fog on, but not on the billboards: the ground's colour only
    plane.fog(40, 100, 12, false);
    cls(1);
    plane.billboards(xs, ys, 0, 1, 16, 0, 3);
    blue = 0;
    for (y = 0; y < 120; y++) for (x = 0; x < 160; x++) if (pget(x, y) == 12) blue++;
    printh(blue);
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, '1 1\n0\n0\n');
});

test('plane: a sky panorama sits on the horizon and turns with the view', () => {
    const r = run(`function init() {
    // a turn 64 wide, 10 tall: red for its first half, green for its second
    var sky = img(64, 10);
    target(sky); rectfill(0, 0, 31, 9, 8); rectfill(32, 0, 63, 9, 11); target(SCREEN);
    plane.view(0, 0, 8, 0.25, 60, 0.125);
    cls(0); plane.sky(sky);
    // its bottom row on the horizon's row above, nothing below it
    var a = pget(80, 59), above = pget(80, 49), below = pget(80, 60), top = pget(80, 50);
    plane.view(0, 0, 8, 0.75, 60, 0.125);
    cls(0); plane.sky(sky);
    printh(a + " " + top + " " + above + " " + below + " " + pget(80, 59));
    plane.view(0, 0, 8, 0.75, 70, 0.125);
    cls(0); plane.sky(sky, 5);
    printh(pget(80, 64) + " " + pget(80, 65));
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    /* a quarter turn looks at the image's three-quarter mark, green; three
     * quarters, its quarter mark, red; 10 rows from 50 to 59 */
    assert.equal(r.out, '11 11 0 0 8\n8 0\n');
});

test('walls: doors slide aside, faces along y take dark, and the fog hazes them', () => {
    const r = run(`function span() {
    var top = -1, bottom = -1;
    for (var y = 0; y < 120; y++) if (pget(80, y) != 0) { if (top < 0) top = y; bottom = y; }
    return top + "-" + bottom;
}
function init() {
    for (var y = 0; y < 8; y++) for (var x = 0; x < 8; x++) sset(8 + x, y, 8);
    fset(1, 1);
    // a wall of sprite 1 down column 5 (x 40-47), the camera at x 4 looking
    // along x: the wall is 36 away; cell 5,8 in front of it a door
    for (var r = 0; r < 16; r++) mset(5, r, 1);
    plane.view(4, 68, 4, 0, 60, 0.25);
    cls(0); walls.draw(1); var plain = span();
    cls(0); walls.draw(1, 8, 512, null, [5, 8, 0]); var shut = span();
    cls(0); walls.draw(1, 8, 512, null, [5, 8, 1]); var open = span();
    printh(plain + " " + shut + " " + open);
    // a wall across row 5 (y 40-47), looked at along y: its face is met
    // crossing a row, one of the faces dark is for
    for (r = 0; r < 16; r++) mset(5, r, 0);
    for (var c = 0; c < 16; c++) mset(c, 5, 1);
    plane.view(68, 4, 4, 0.75, 60, 0.25);
    var dark = [];
    for (var k = 0; k < 16; k++) dark.push(k == 8 ? 2 : k);
    cls(0); walls.draw(1); var lit = pget(80, 60);
    cls(0); walls.draw(1, 8, 512, dark); var shaded = pget(80, 60);
    plane.fog(0, 1, 12);
    cls(0); walls.draw(1); var fogged = pget(80, 60) + pget(81, 60) + pget(80, 61) + pget(81, 61);
    printh(lit + " " + shaded + " " + fogged);
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    const [doors, faces] = r.out.trim().split('\n');
    const [plain, shut, open] = doors.split(' ');
    const rows = (s) => s.split('-').map(Number);
    /* shut, the panel is 4 further than the wall's face: a shorter column */
    assert.ok(rows(shut)[1] - rows(shut)[0] < rows(plain)[1] - rows(plain)[0], doors);
    assert.equal(open, '-1--1', 'open, the ray passes: ' + doors);
    /* lit red, shaded the dark colour; wholly fogged, all four blue */
    assert.equal(faces, '8 2 48');
});

test('playback and tracks: the playing track, the library, the queue, and owning the music', () => {
    const main = `var ids = new Int32Array(8), near = [0, 0, 0], snd = new Int32Array(SOUND.SIZE), tk = 0;
function init() {
    // pbgvm's library (host/fakemusic.c): five albums of four, the last
    // of three and a spoken chapter; a queue of the first ten, playing the first
    printh(playback.track() + " " + playback.title() + "|" + playback.artist() + "|" + playback.album() + " " + playback.length());
    printh(tracks.count() + " " + tracks.count({ music: true }) + " " + tracks.count({ genre: "Jazz" }) + " " + tracks.count({ decade: 1980, music: true }) + " " + tracks.count({ minlen: 200000 }));
    var k = tracks.pick(3, { genre: "Pop" }, ids);
    printh(k + " " + ids[0] + " " + ids[1] + " " + ids[2]);
    printh(tracks.field(4, TRACK.TITLE) + "|" + tracks.field(4, TRACK.GENRE) + "|" + tracks.field(4, TRACK.YEAR) + "|" + tracks.field(4, TRACK.LENGTH) + "|" + tracks.field(99, TRACK.TITLE));
    printh(tracks.near(6, 3, near) + " " + near.join(",") + " " + tracks.count({ artist: "Mira Vale" }));
    printh(tracks.near(6, 3, near, true) + " " + near.join(",") + " " + tracks.near(19, 3, near, true));
    var many = new Int32Array(6);
    printh(tracks.near([6, 19], 3, many, true) + " " + many.join(","));
    printh(tracks.sound(3, snd) + " " + snd[0]);
    printh(tracks.queued() + " " + tracks.place() + " " + tracks.queue(2) + " " + tracks.queue(40));
    printh(tracks.enqueue(15, true) + " " + tracks.queued() + " " + tracks.queue(1));
    playback.seek(30000);
    playback.duck(128, 10);
}
function update() {
    tk++;
    if (tk == 2) playback.next();
    if (tk == 3) printh("changed " + playback.changed() + " " + playback.track() + " " + tracks.place());
    if (tk == 4) printh("still " + playback.changed());
    if (tk == 5) { printh(playback.play(12, 5000) + " " + playback.started() + " " + playback.track()); playback.stop(); }
}`;
    const res = compileFiles({ 'cart.json': JSON.stringify({ id: '0000000000007ac5', name: 'Tracks', music: 'own' }),
                               'main.js': main });
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'tracks.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '5'], { encoding: 'utf8' });
    assert.equal(r.status, 0, r.stderr);
    assert.deepEqual(r.stdout.replace(/\r/g, '').trim().split('\n'), [
        '0 Lighthouse|Harbour Lamps|Northern Lights 214000',
        /* 20 in all, 19 music; Jazz is Blue Hours, 4-7; the 80s Engine
         * Room, 12-15; under 200 s are 1, 7, 9 and 11, so 16 are not */
        '20 19 4 4 16',
        '3 8 9 10',
        'Half Past Midnight|Jazz|1962|326000|null',
        /* the Jazz nearest track 6; Mira Vale's four are Paper Planes */
        '3 5,7,4 4',
        /* by sound, the nearest tempo to 6's 64 bpm: 72, 84, 88; the
         * spoken chapter has none */
        '3 4,3,5 0',
        /* a list: 6's three, then -1 for the chapter's none */
        '3 4,3,5,-1,-1,-1',
        /* track 3 at 84 bpm: a beat every 714 ms */
        '1 714',
        '10 0 2 -1',
        '1 11 15',
        'seek 30000',
        'duck 128 over 10',
        'control 1',
        'changed 1 15 1',
        'still 0',
        'play 12 from 5000',
        '1 1 12',
        'stop'
    ]);
    /* a listen cart may read, not steer */
    const listen = run('function init() { playback.seek(0); }\nfunction update() {}');
    assert.match(listen.errors[0].message, /needs the cart's music level to be control/);
});

test('a platform moving into an actor pushes it, and crushes it against a wall', () => {
    const r = run(`var p, plat, out = new Int32Array(40), tk = 0;
function init() {
    fset(1, 1);
    for (var r = 0; r < 15; r++) mset(15, r, 1);      // a wall at x 120
    plat = actors.pool(2);
    actors.spawn(plat); actors.spawn(plat);
    // a pusher sliding right at row 20; a lift rising under an actor at 60
    plat.x[0] = 10; plat.y[0] = 20; plat.w[0] = 16; plat.h[0] = 16;
    plat.x[1] = 60; plat.y[1] = 100; plat.w[1] = 16; plat.h[1] = 8;
    p = actors.pool(2);
    p.platforms = plat;
    actors.spawn(p); p.x[0] = 40; p.y[0] = 24;
    actors.spawn(p); p.x[1] = 64; p.y[1] = 85;
}
function update() {
    tk++;
    plat.x[0] += 3; plat.dx[0] = 3;
    plat.y[1] -= 2; plat.dy[1] = -2;
    var k = actors.move(p, 1, 0, out);
    for (var i = 0; i < k; i++)
        if (out[i * 4 + 1] == HIT.CRUSH && tk < 40) { printh(tk + " crush " + out[i * 4] + " by " + out[i * 4 + 3]); tk = 99; }
    if (tk == 10) printh(p.x[0] + " " + (p.y[1] + 8 == plat.y[1]));
}`, 40);
    assert.equal(r.status, 0, r.err);
    const lines = r.out.trim().split('\n');
    /* by tick 10 the pusher's right edge is 10 + 30 + 16 = 56: the actor
     * flush against it; the lift has come up under the other and carries
     * it on its top */
    assert.equal(lines[0], '56 1');
    /* pressed on to the wall at 120, it is crushed (pusher 0) */
    assert.match(lines[1], /^\d+ crush 0 by 0$/);
});

test('links rest on a cells field\'s solid cells and slow in its liquid', () => {
    const r = run(`var rope, drop, f, R, tk = 0;
function init() {
    R = cells.rules();
    R.density[5] = 255; R.flags[5] = CELL.SOLID;
    R.move[12] = MOVE.LIQUID; R.density[12] = 100; R.flags[12] = CELL.LIQUID;
    // a stone ledge at y 60 from x 20 to 80, water below x 100 from y 40
    f = img(160, 100);
    target(f); cls(0); rectfill(20, 60, 80, 63, 5); rectfill(100, 40, 159, 99, 12); target(SCREEN);
    rope = links.pool(12, 12);
    rope.gravity = 0.3; rope.field = f; rope.rules = R;
    for (var i = 0; i < 12; i++) { links.point(rope, 30 + i * 4, 30); if (i) links.link(rope, i - 1, i); }
    // two lone points: one falling in air, one in the water
    drop = links.pool(2, 0);
    drop.gravity = 0.3; drop.field = f; drop.rules = R;
    links.point(drop, 90, 0); links.point(drop, 120, 41);
}
function update() {
    tk++;
    links.step(rope, 0);
    links.step(drop, 0);
    if (tk == 12) printh(flr(drop.y[0]) + " " + flr(drop.y[1]));
    if (tk == 80) {
        var low = 0;
        for (var i = 0; i < 12; i++) if (rope.y[i] > low) low = rope.y[i];
        printh(low < 61);
    }
}`, 80);
    assert.equal(r.status, 0, r.err);
    const [air, water] = r.out.trim().split('\n')[0].split(' ').map(Number);
    /* both fell for 12 ticks; the one in water far less far */
    assert.ok(air - 0 > 2 * (water - 41), r.out);
    /* the rope lies on the ledge (its top at 60), not through it */
    assert.equal(r.out.trim().split('\n')[1], '1');
});

test('plane: a floor drawn through a table of sprites, and a ceiling', () => {
    const r = run(`function init() {
    // sprite 1: colour 4; sprite 3: colour 9; sprite 5: colour 12
    for (var y = 0; y < 8; y++) for (var x = 0; x < 8; x++) { sset(8 + x, y, 4); sset(24 + x, y, 9); sset(40 + x, y, 12); }
    for (var r = 0; r < 64; r++) for (var c = 0; c < 128; c++) mset(c, r, 1);
}
function update() {}
function draw() {
    cls(0);
    plane.view(200, 200, 4, 0, 60, 0.2);
    plane.draw(60, 119, -1, 1, [0, 3]);
    plane.ceiling(8, [5, 5]);
    printh(pget(80, 119) + " " + pget(10, 100) + " " + pget(80, 0) + " " + pget(150, 10));
    cls(0);
    plane.view(200, 200, 12, 0, 60, 0.2);
    plane.ceiling(8, [5, 5]);
    printh(pget(80, 0));
}`);
    assert.equal(r.status, 0, r.err);
    /* every floor cell is sprite 1, drawn as 3 (colour 9); the ceiling 5
     * (colour 12); and from above it, no ceiling at all */
    assert.equal(r.out, '9 9 12 12\n0\n');
});

test('land: a billboard behind a hill shows only above the hill', () => {
    const body = (ridge) => `var hm, cm;
function init() {
    for (var y = 0; y < 8; y++) for (var x = 0; x < 8; x++) sset(8 + x, y, 8);
    hm = img(64, 64); cm = img(64, 64);
    target(hm); cls(0); target(cm); cls(11);
    // a low ridge across the view at x 30..31
    target(hm); if (${ridge}) rectfill(30, 0, 31, 63, 12);
    target(cm); rectfill(30, 0, 31, 63, 5);
    target(SCREEN);
    plane.view(0, 32, 20, 0, 50, 0.25);
    cls(1);
    land.draw(hm, cm, 200);
    // a tall figure well beyond the ridge, straight ahead
    plane.billboards([50], [32], 0, 1, 40, 0, 1);
    var n = 0;
    for (var y = 0; y < 120; y++) if (pget(80, y) == 8) n++;
    printh(n);
}
function update() {}`;
    const open = run(body(0)), hidden = run(body(1));
    assert.equal(open.status, 0, open.err);
    assert.equal(hidden.status, 0, hidden.err);
    const seen = Number(open.out.trim()), behind = Number(hidden.out.trim());
    /* the whole figure on open ground; behind the ridge, its top only */
    assert.ok(seen > 10, open.out);
    assert.ok(behind > 0 && behind < seen, seen + ' then ' + behind);
});

test('links: a pool given arrays longer than links.pool() allows is refused', () => {
    const r = run(`var p;
function init() {
    p = links.pool(4, 4);
    var pts = [], lks = [];
    for (var i = 0; i < 1600; i++) pts.push(0);
    for (var j = 0; j < 3000; j++) lks.push(0);
    p.x = pts.slice(0); p.y = pts.slice(0); p.z = pts.slice(0);
    p.ox = pts.slice(0); p.oy = pts.slice(0); p.oz = pts.slice(0);
    p.pin = pts.slice(0); p.points = 1600;
    p.a = lks.slice(0); p.b = lks.slice(0); p.len = lks.slice(0); p.links = 3000;
    links.step(p, 0);
}
function update() {}
function draw() {}`);
    assert.equal(r.status, 1, r.err);
    assert.match(r.err, /arrays are as links.pool\(\) made them/);
});

test('trifill wholly off the left or right costs the call, not its rows', () => {
    const cost = (t) => {
        const r = run(`function init() { prof("t"); ${t}; prof(); }
function update() {}
function draw() {}`, 1, ['--units']);
        assert.equal(r.status, 0, r.err);
        return Number(/prof t (\d+)/.exec(r.err)[1]);
    };
    assert.ok(cost('trifill(200,0, 600,0, 600,119, 8)') < 60);
    assert.ok(cost('trifill(-600,0, -200,0, -200,119, 8)') < 60);
    assert.ok(cost('camera(300, 0); trifill(200,0, 600,0, 600,119, 8)') > 600);
});

test('playback.volume() reads the volume, playback.adjust() turns it a step at a time, within 0 to 100', () => {
    const main = `function init() {
    // pbgvm's volume (host/fakemusic.c) starts at 60, two a step
    printh(playback.volume() + " " + playback.adjust(3) + " " + playback.volume());
    printh(playback.adjust(-100) + " " + playback.adjust(80) + " " + playback.adjust(-1));
}
function update() {}`;
    const res = compileFiles({ 'cart.json': JSON.stringify({ id: '0000000000007ac6', name: 'Volume', music: 'control' }),
                               'main.js': main });
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'volume.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '1'], { encoding: 'utf8' });
    assert.equal(r.status, 0, r.stderr);
    assert.deepEqual(r.stdout.replace(/\r/g, '').trim().split('\n').slice(0, 2), ['60 66 66', '0 100 98']);
    /* turning it is control's; reading it is anyone's */
    const listen = compileFiles({ 'cart.json': JSON.stringify({ id: '0000000000007ac7', name: 'Listen' }),
                                  'main.js': 'function init() { printh(playback.volume()); playback.adjust(1); }\nfunction update() {}' });
    assert.equal(listen.errors.length, 1);
    assert.match(listen.errors[0].message, /control/);
});
