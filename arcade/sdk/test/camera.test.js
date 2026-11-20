/* camera(x, y, turn, [px], [py], [zoom]): the picture turned and grown
 * about a pivot, through pbgvm's --dump (one hex digit a pixel, a row a line). */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-camera-'));

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

let n = 0;
function run(draw, dump) {
    const res = compileFiles({
        'cart.json': JSON.stringify({ id: '00000000000ca3e7', name: 'Camera' }),
        'main.js': `function init() {}
function update() {}
function draw() { cls(0); ${draw} }` });
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(tmp, 'c' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '1', '--dump', dump], { encoding: 'utf8' });
    return { out: r.stdout.replace(/\r/g, ''), err: r.stderr, status: r.status };
}

test('a quarter turn moves a point clockwise about the middle of the screen', () => {
    /* pixel 90, 60's middle is 10.5, 0.5 from the pivot at 80, 60; turned
     * clockwise a quarter it is -0.5, 10.5 from it: pixel 79, 70 */
    const r = run('camera(0, 0, 0.25); pset(90, 60, 7);', '79,70,1,1');
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out.trim(), '7');
});

test('pget reads where pset drew, turn and all', () => {
    const r = run('camera(5, 3, 0.1); pset(40, 30, 9); printh(pget(40, 30));', '0,0,1,1');
    assert.equal(r.out.split('\n')[0], '9');
});

test('a pivot of its own, and camera() undoing it all', () => {
    /* about 0, 0 a half turn takes pixel 10, 10 (middle 10.5) to -10.5:
     * off the screen; camera() then draws it where it is */
    const turned = run('camera(0, 0, 0.5, 0, 0); pset(10, 10, 7);', '10,10,1,1');
    assert.equal(turned.out.trim(), '0');
    const back = run('camera(0, 0, 0.5, 0, 0); camera(); pset(10, 10, 7);', '10,10,1,1');
    assert.equal(back.out.trim(), '7');
});

test('a turned rectfill is the same as the turned outline filled', () => {
    const a = run('camera(0, 0, 0.07); rectfill(40, 30, 99, 79, 7);', '0,0,160,120');
    const b = run('camera(0, 0, 0.07); polyfill([40, 100, 100, 40], [30, 30, 80, 80], 4, 7);', '0,0,160,120');
    assert.equal(a.out, b.out);
    assert.ok(a.out.includes('7'));
});

test('sprites stay upright at their turned places', () => {
    /* a 1x1 sprite of colour 7 at 90, 60 lands where pset(90, 60) does */
    const r = run('sset(8, 0, 7); camera(0, 0, 0.25); spr(1, 90, 60, 0.125, 0.125);', '79,70,1,1');
    assert.equal(r.out.trim(), '7');
});

const count = (out, c) => out.split('').filter((d) => d === c).length;

test('a zoom grows shapes about the pivot', () => {
    /* 20 by 20 about the middle, twice the size: 40 by 40 from 60, 40 */
    const r = run('camera(0, 0, 0, 80, 60, 2); rectfill(70, 50, 89, 69, 7);', '0,0,160,120');
    assert.equal(r.status, 0, r.err);
    assert.equal(count(r.out, '7'), 1600);
    const edge = run('camera(0, 0, 0, 80, 60, 2); rectfill(70, 50, 89, 69, 7);', '59,40,2,1');
    assert.equal(edge.out.trim(), '07');
});

test('a zoom grows a circle about its middle, as it does an oval', () => {
    /* radius 10 at twice the size: radius 20, its middle (pixel 80's,
     * 80.5) half a pixel from the pivot and so a pixel off it, zoomed */
    const z = run('camera(0, 0, 0, 80, 60, 2); circfill(80, 60, 10, 7);', '0,0,160,120');
    const big = run('circfill(81, 61, 20, 7);', '0,0,160,120');
    assert.equal(z.status, 0, z.err);
    assert.equal(z.out, big.out);
    const half = run('camera(0, 0, 0, 80, 60, 0.5); circ(80, 60, 20, 7);', '0,0,160,120');
    const small = run('circ(80, 60, 10, 7);', '0,0,160,120');
    assert.equal(half.out, small.out);
    const huge = run('camera(0, 0, 0, 80, 60, 16); circfill(80, 60, 268435457, 7);',
                     '0,0,160,120');
    assert.equal(count(huge.out, '7'), 160 * 120);
});

test('a polygon fills by the middles of its pixels, near the screen or far past it', () => {
    /* a strip two pixels wide down the screen; one reaching thousands of
     * pixels past each side, filling the screen; a wedge brought on by the
     * camera from 20,000 away, as drawn where it lands */
    const thin = run('polyfill([20, 22, 22, 20], [0, 0, 119, 119], 4, 7);', '0,0,160,120');
    assert.equal(thin.status, 0, thin.err);
    assert.equal(count(thin.out, '7'), 2 * 119);
    const wide = run('polyfill([-31000, 31000, 31000, -31000], [0, 0, 120, 120], 4, 7);',
                     '0,0,160,120');
    assert.equal(count(wide.out, '7'), 160 * 120);
    const pts = 'var xs = [10, 150, 120, 30], ys = [5, 40, 115, 90];';
    const near = run(pts + ' polyfill(xs, ys, 4, 7);', '0,0,160,120');
    const far = run(pts + ' for (var i = 0; i < 4; i++) xs[i] += 20000;' +
                    ' camera(20000, 0); polyfill(xs, ys, 4, 7);', '0,0,160,120');
    assert.equal(near.out, far.out);
});

test('a plain fill draws as a patterned one of a single colour does', () => {
    /* a pattern of one colour twice over draws the same pixels the long
     * way: a wedge on the screen, and one whose edge reaches 31,000 past it */
    const shapes = ['[10, 150, 120, 30], [5, 40, 115, 90], 4',
                    '[-31000, 80, 159], [0, 119, 0], 3',
                    '[0, 159, 80], [-500, -500, 5], 3'];
    for (const s of shapes) {
        const plain = run(`polyfill(${s}, 7);`, '0,0,160,120');
        const pat = run(`fillp(1); polyfill(${s}, 7 + 256 * 7);`,
                        '0,0,160,120');
        assert.equal(plain.status, 0, plain.err);
        assert.equal(plain.out, pat.out, s);
        assert.ok(plain.out.includes('7'), s);
    }
});

test('a zoom of 1 draws as no zoom does', () => {
    const a = run('camera(0, 0, 0.07); circfill(60, 50, 20, 7); trifill(10, 10, 90, 20, 40, 90, 8);', '0,0,160,120');
    const b = run('camera(0, 0, 0.07, 80, 60, 1); circfill(60, 50, 20, 7); trifill(10, 10, 90, 20, 40, 90, 8);', '0,0,160,120');
    assert.equal(a.out, b.out);
});

test('rspr grows with the zoom, spr keeps its size', () => {
    const fill = 'for (var i = 0; i < 8; i++) for (var j = 0; j < 8; j++) sset(8 + i, j, 7);';
    const r = run(fill + ' camera(0, 0, 0, 80, 60, 2); rspr(1, 80, 60, 0);', '0,0,160,120');
    assert.equal(count(r.out, '7'), 256);
    const s = run(fill + ' camera(0, 0, 0, 80, 60, 2); spr(1, 76, 56);', '0,0,160,120');
    assert.equal(count(s.out, '7'), 64);
});

test('a zoom past 1/16 to 16 is an error', () => {
    const r = run('camera(0, 0, 0, 80, 60, 20);', '0,0,1,1');
    assert.notEqual(r.status, 0);
    assert.match(r.err, /zoom from 1\/16 to 16/);
});
