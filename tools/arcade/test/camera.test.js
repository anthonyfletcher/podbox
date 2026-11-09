/* camera(x, y, turn, [px], [py]): the picture turned about a pivot,
 * through pbgvm's --dump (one hex digit a pixel, a row a line). */

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
