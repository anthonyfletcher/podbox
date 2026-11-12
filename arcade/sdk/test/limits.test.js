/* What a cart cannot do: corrupt the engine, crash it or hang it, however
 * it calls the natives and the language. Each stops cleanly, or runs. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-limits-'));
const CART = JSON.stringify({ id: '00000000001141e5', name: 'Limits' });

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

let n = 0;
/* At the player's ceiling, and well inside the 20 s a hang would take */
function run(main, ticks = 1) {
    const res = compileFiles({ 'cart.json': CART, 'main.js': main });
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(tmp, 'l' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const t0 = Date.now();
    const r = spawnSync(exe, [file, '--ticks', String(ticks), '--ceiling',
                              '2000000'], { encoding: 'utf8', timeout: 20000 });
    return { errors: [], out: r.stdout.replace(/\r/g, ''), err: r.stderr,
             status: r.status, ms: Date.now() - t0 };
}

const update = '\nfunction update() {}';

test('a sort inside a comparator is refused, and typeof stays whole', () => {
    const r = run(`var a = [2, 1], b = [2, 1];
function inner(p, q) { return p - q; }
function outer(x, y) { b.sort(inner); return x - y; }
function init() { a.sort(outer); }` + update);
    assert.equal(r.status, 1);
    assert.match(r.err, /\.sort\(\) inside a sort's comparator \(in outer\)/);
    const ok = run(`function init() {
    var a = [3, 1, 2]; a.sort(function (x, y) { return x - y; });
    printh(a.join(",") + " " + typeof 5);
}` + update);
    assert.equal(ok.out, '1,2,3 number\n');
});

test('path.ray along a direction too long to square returns', () => {
    const r = run(`function init() {
    var o = [0, 0, 0, 0, 0];
    printh(path.ray(0, 0, 40000, 0, 1, o) + " " + path.ray(0, 0, -2147483647, 1, 1, o));
    fset(1, 1); mset(5, 0, 1);
    printh(path.ray(4, 4, 40000, 0, 1, o) + " " + o[2] + " " + path.ray(4, 4, 1, 0, 1, o, 2000000000));
}` + update);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, '0 0\n1 5 1\n');
});

test('the views drawn into an image wider than the screen stay in it', () => {
    const r = run(`function init() {
    var rules = cells.rules(), f = new Uint8Array(256);
    fset(1, 1);
    for (var y = 0; y < 64; y++) for (var x = 0; x < 128; x++) mset(x, y, 1);
    mset(10, 10, 0);
    plane.view(84, 84, 4, 0, 60, 0.25);
    target(img(1024, 120));
    walls.draw(1);
    plane.draw(2);
    target(img(4096, 16));
    land.draw(img(64, 64), img(64, 64), 512, 1, 1);
    sspr(0, 0, 8, 8, 0, 0, 4096, 8);
    target(SCREEN);
    printh(cells.step(f, 16, rules));
}` + update);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, '0\n');
});

test('counts and fields that are not what they claim are refused', () => {
    const zones = run(`function init() {
    actors.zones(actors.pool(1), [0, 0, 0, 0, 0], 858993460, new Int32Array(8));
}` + update);
    assert.match(zones.err, /actors\.zones\(\) needs the objects/);
    const links = run(`function init() { var p = links.pool(4, 4); p.x = 2000000000; links.step(p, 0); }` + update);
    assert.match(links.err, /links\.step\(\) needs a pool whose arrays/);
    const rows = run(`function init() { grid.collapse(new Uint8Array(16), 4, 1, 2147483647); }` + update);
    assert.match(rows.err, /grid\.collapse\(\) takes an array of rows/);
});

test('long work inside one call is charged as it goes, and stopped', () => {
    const cases = {
        indexOf: `function init() {
    var h = "a"; while (h.length < 65536) h = h + h;
    var k = h.substring(0, 32767) + "b";
    for (var i = 0; i < 20; i++) h.indexOf(k);
}`,
        compare: `function init() {
    var h = "a"; while (h.length < 65536) h = h + h;
    var g = h.substring(0, 65535) + "a", e = 0;
    for (var j = 0; j < 20000; j++) if (h == g) e++;
}`,
        typed: `function init() { for (var i = 0; i < 20000; i++) new Uint8Array(150000); }`,
        boxes: `function init() {
    var f = new Uint8Array(65536), r = cells.rules(), p = actors.pool(1024), ps = [];
    for (var i = 0; i < 1024; i++) { p.on[i] = 1; p.w[i] = 256; p.h[i] = 256; }
    for (var k = 0; k < 64; k++) ps.push(p);
    cells.step(f, 256, r, ps);
}`,
    };
    for (const [name, main] of Object.entries(cases)) {
        const r = run(main + update);
        assert.equal(r.status, 1, name + ': ' + r.err);
        assert.match(r.err, /The game stopped responding/, name);
        /* stopped rather than hung: well short of the 20 s, with room for
         * the whole suite running at once */
        assert.ok(r.ms < 15000, name + ' took ' + r.ms + ' ms');
    }
});

test('actors as tall as the world, moving as far as they can, move quickly', () => {
    const r = run(`function init() {
    var p = actors.pool(256), o = new Int32Array(64);
    for (var i = 0; i < 256; i++) { p.on[i] = 1; p.w[i] = 8; p.h[i] = 32767; p.dx[i] = 32767; }
    fset(1, 1); mset(30, 3, 1);
    printh(actors.move(p, 1, 0, o) + " " + p.x[0]);
}` + update);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, '16 232\n');
    assert.ok(r.ms < 5000, 'took ' + r.ms + ' ms');
});

test('an array joined into itself stops at its size', () => {
    const r = run(`var a = [];
function init() { for (var i = 0; i < 36; i++) a.push(a); a.join(""); }` + update);
    assert.equal(r.status, 1);
    assert.match(r.err, /text of too many elements/);
    assert.ok(r.ms < 5000, 'took ' + r.ms + ' ms');
});

test('a function needing more stack than a game has does not compile', () => {
    const big = [];
    for (let i = 0; i < 4100; i++) big.push(i);
    const r = run(`function init() { var a = [${big.join(',')}]; }` + update);
    assert.match(r.errors[0].message, /init needs more stack than a game has/);
});
