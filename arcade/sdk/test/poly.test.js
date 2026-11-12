/* polyfill: outlines filled by the even-odd rule, through pbgvm's --dump
 * (one hex digit a pixel, a row a line). */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-poly-'));

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

let n = 0;
function run(draw, dump, extra = []) {
    const res = compileFiles({
        'cart.json': JSON.stringify({ id: '0000000000901f11', name: 'Poly' }),
        'main.js': `function init() {}
function update() {}
function draw() { cls(0); ${draw} }` });
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(tmp, 'p' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '1', '--dump', dump].concat(extra),
                        { encoding: 'utf8' });
    return { out: r.stdout.replace(/\r/g, ''), err: r.stderr, status: r.status };
}

test('a square fills the pixels whose middles are inside it', () => {
    const r = run('polyfill([1, 5, 5, 1], [1, 1, 4, 4], 4, 7);', '0,0,7,6');
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, '0000000\n0777700\n0777700\n0777700\n0000000\n0000000\n');
});

test('a star that crosses itself leaves its middle empty, and an outline inside another is a hole', () => {
    const star = run('polyfill([30, 38, 15, 45, 22], [5, 35, 15, 15, 35], 5, 7);', '30,19,1,1');
    assert.equal(star.out.trim(), '0');
    const holed = run('polyfill([0, 20, 20, 0, 5, 15, 15, 5], [0, 0, 20, 20, 5, 5, 15, 15], 8, 7, [4, 8]);',
                      '0,10,20,1');
    assert.equal(holed.out.trim(), '77777000000000077777');
});

test('curve points bend the outline: four make a rounded square, inside the corners', () => {
    const r = run('polyfill([0, 40, 40, 0], [0, 0, 40, 40], 4, 7, null, [1, 1, 1, 1]);', '0,0,40,40');
    assert.equal(r.status, 0, r.err);
    const rows = r.out.trim().split('\n');
    assert.equal(rows[0][0], '0');               /* the corner is cut */
    assert.equal(rows[20][1], '7');              /* the sides bulge out to it */
    assert.equal(rows[20][20], '7');
});

test('the camera moves it, the clip holds, and fillp patterns it', () => {
    const r = run('camera(-10, 0); clip(10, 0, 13, 3); polyfill([0, 8, 8, 0], [0, 0, 8, 8], 4, 7);',
                  '8,0,8,1');
    assert.equal(r.out.trim(), '0077770000'.slice(0, 8));
    const p = run('fillp(0x5a5a); polyfill([0, 8, 8, 0], [0, 0, 8, 8], 4, 7);', '0,0,4,1');
    assert.notEqual(p.out.trim(), '7777');
});

test('wrong arguments stop the cart', () => {
    assert.match(run('polyfill([1, 2], [1, 2], 3, 7);', '0,0,1,1').err, /fewer x or y than points/);
    assert.match(run('polyfill(1, 2, 3, 7);', '0,0,1,1').err, /arrays of x and y/);
    assert.match(run('polyfill([0, 9, 9, 0], [0, 0, 9, 9], 4, 7, [3, 2]);', '0,0,1,1').err, /ends must rise/);
    assert.match(run('var a = []; for (var i = 0; i < 9000; i++) a.push(0); polyfill(a, a, 9000, 7);',
                     '0,0,1,1').err, /at most 8192 points/);
});

test('an outline off the screen costs its edges, not the screen\'s rows', () => {
    const r = run('prof("t"); polyfill([300, 400, 400, 300], [0, 0, 119, 119], 4, 7); prof();', '0,0,1,1',
                  ['--units']);
    const units = Number(/prof t (\d+)/.exec(r.err)[1]);
    assert.ok(units < 200, String(units));
});

test('polyline draws what line() draws, a colour a line, closed and turned alike', () => {
    const xs = '[10, 70, 130, 90, 20, 40, 60, 50]', ys = '[10, 30, 15, 100, 90, 50, 50, 70]';
    const cs = '[7, 8, 9, 10, 11, 12, 13, 14]';
    for (const cam of ['', 'camera(0, 0, 0.07, 80, 60);']) {
        const one = run(`${cam} polyline(${xs}, ${ys}, 8, ${cs}, [5, 8], true);`, '0,0,160,120');
        const many = run(`${cam} var x = ${xs}, y = ${ys}, c = ${cs};
            for (var i = 0; i < 5; i++) line(x[i], y[i], x[(i + 1) % 5], y[(i + 1) % 5], c[i]);
            for (var j = 5; j < 8; j++) line(x[j], y[j], x[j < 7 ? j + 1 : 5], y[j < 7 ? j + 1 : 5], c[j]);`,
                         '0,0,160,120');
        assert.equal(one.status, 0, one.err);
        assert.ok(/[789abcde]/.test(one.out), 'something drawn');
        assert.equal(one.out, many.out, cam || 'unturned');
    }
    /* open by default: no line back to the start */
    const open = run('polyline([10, 30, 30], [10, 10, 30], 3, 7);', '10,10,21,21');
    const rows = open.out.trim().split('\n');
    assert.equal(rows[10][10], '0');
});

test('an outline reaching past both sides fills between them, and keeps its hole', () => {
    const r = run('polyfill([-100, 300, 300, -100, 60, 100, 100, 60], [10, 10, 30, 30, 15, 15, 25, 25], 8, 7, [4, 8]);',
                  '0,20,160,1');
    assert.equal(r.out.trim(), '7'.repeat(60) + '0'.repeat(40) + '7'.repeat(60));
    const all = run('polyfill([-500, 700, 700, -500], [-500, -500, 700, 700], 4, 7);', '0,0,160,1');
    assert.equal(all.out.trim(), '7'.repeat(160));
    const turned = run('camera(0, 0, 0.1); polyfill([-300, 460, 460, -300], [-300, -300, 420, 420], 4, 7);',
                       '0,60,160,1');
    assert.equal(turned.out.trim(), '7'.repeat(160));
});

test('a nearly level edge crosses its row where it should', () => {
    /* from (0, 10.5 - e) to (300, 10.5 + e): it crosses row 10's middle
     * halfway, at x 150, so pixels 0 to 149 of that row are inside */
    const e = 100 / 65536;
    const r = run(`polyfill([0, 300, 0], [${10.5 - e}, ${10.5 + e}, 50], 3, 7);`, '0,10,160,1');
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out.trim(), '7'.repeat(150) + '0'.repeat(10));
});

test('a turned rectangle in an image taller than 256 rows fills as in a short one', () => {
    /* the tall image takes its rows from scratch memory, the short one
     * from the held tables: the same pixels either way */
    const count = (h) => {
        const r = run(`target(img(64, ${h}));
camera(0, 0, 0.05, 32, 60);
rectfill(10, 20, 50, 120, 7);
camera();
var n = 0;
for (var y = 0; y < ${h}; y++) for (var x = 0; x < 64; x++) if (pget(x, y) == 7) n++;
target(SCREEN);
printh(n);`, '0,0,1,1');
        assert.equal(r.status, 0, r.err);
        return r.out.trim().split('\n')[0];
    };
    assert.equal(count(300), count(200));
    assert.ok(Number(count(300)) > 3000);
});

/* a long ragged ground, a cave in it and a frame round both, for boxes */
const WORLD = `var X = [], Y = [], E = [];
for (var i = 0; i <= 300; i++) { X.push(i * 5 - 400); Y.push(60 + (i * 37) % 23 - (i % 31 < 3 ? 30 : 0)); }
X.push(1100); Y.push(400); X.push(-400); Y.push(400); E.push(X.length);
for (var i = 0; i < 80; i++) { X.push(500 + cos(i / 80) * 200); Y.push(150 + sin(i / 80) * 40); }
E.push(X.length);
for (var i = 0; i < 100; i++) { X.push(-600 + i * 18); Y.push(-300); }
for (var i = 0; i < 100; i++) { X.push(1200 - i * 18); Y.push(600); }
E.push(X.length);
X.push(10); Y.push(10); X.push(30); Y.push(12); X.push(20); Y.push(30);`;

test('polyboxes: a fill with the boxes draws what one without does, however the camera stands', () => {
    const views = ['camera(-30, 0);', 'camera(200, 40);', 'camera(700, -100);', 'camera(-500, 300);',
                   'camera(100, 20, 0.12, 80, 60);', 'camera(450, 80, -0.3, 80, 60, 0.25);',
                   'camera(0, 0, 0.5, 80, 60, 4);', 'clip(20, 10, 90, 70); camera(300, 30, 0.07);'];
    for (const v of views) {
        const fill = (boxes) => run(`${WORLD}
${boxes ? 'var B = polyboxes(X, Y, X.length, E);' : ''}
${v} polyfill(X, Y, X.length, 7, E, null, ${boxes ? 'B' : 'null'}); camera(); clip();`, '0,0,160,120');
        const a = fill(true), b = fill(false);
        assert.equal(a.status, 0, a.err);
        assert.equal(a.out, b.out, v);
        assert.ok(/7/.test(a.out) || v.includes('-500'), v);
    }
});

test('polyboxes: runs of 16 lines in each outline, and a box round each', () => {
    const r = run(`var b = polyboxes([0, 10, 10, 0, 50, 60, 55], [0, 0, 10, 10, 0, 0, 9], 7, [4]);
printh(b.length + " " + b[0] + " " + b[1] + " " + b[3] / 65536 + " " + b[6] / 65536 + " " + b[7] / 65536 + " " + b[10] / 65536);`,
                  '0,0,1,1');
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out.split('\n')[0], '11 7 2 0 10 50 9');
    const long = run(`var x = [], y = []; for (var i = 0; i < 40; i++) { x.push(i); y.push(i % 3); }
printh(polyboxes(x, y, 40)[1]);`, '0,0,1,1');
    assert.equal(long.out.split('\n')[0], '3');
});

test('polyboxes: the boxes must be for these points, and for outlines without bend', () => {
    const sq = 'var x = [0, 9, 9, 0], y = [0, 0, 9, 9], b = polyboxes(x, y, 4);';
    assert.match(run(sq + 'polyfill(x, y, 3, 7, null, null, b);', '0,0,1,1').err, /other points or ends/);
    assert.match(run(sq + 'polyfill(x, y, 4, 7, [2], null, b);', '0,0,1,1').err, /other points or ends/);
    /* two outlines of a run each, against one outline of two runs */
    const two = 'var x = [], y = []; for (var i = 0; i < 32; i++) { x.push(i); y.push(i % 5); }' +
                'var b = polyboxes(x, y, 32, [16]);';
    assert.match(run(two + 'polyfill(x, y, 32, 7, null, null, b);', '0,0,1,1').err, /other points or ends/);
    assert.match(run(sq + 'polyfill(x, y, 4, 7, null, [1, 1, 1, 1], b);', '0,0,1,1').err, /without bend/);
    assert.match(run(sq + 'polyfill(x, y, 4, 7, null, null, [1, 2]);', '0,0,1,1').err, /Int32Array from polyboxes/);
    assert.match(run('polyboxes([0, 9, 9, 0], [0, 0, 9, 9], 4, [3, 2]);', '0,0,1,1').err, /ends must rise/);
});

test('polyboxes: the runs out of view cost a run each, not their points', () => {
    const cost = (boxes) => {
        const r = run(`${WORLD}
var B = polyboxes(X, Y, X.length, E);
prof("t"); polyfill(X, Y, X.length, 7, E, null, ${boxes ? 'B' : 'null'}); prof();`, '0,0,1,1', ['--units']);
        assert.equal(r.status, 0, r.err);
        return Number(/prof t (\d+)/.exec(r.err)[1]);
    };
    const with_ = cost(true), without = cost(false);
    assert.ok(with_ < without * 0.6, with_ + ' against ' + without);
});
