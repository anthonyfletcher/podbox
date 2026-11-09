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
