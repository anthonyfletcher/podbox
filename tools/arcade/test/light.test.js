/* Light: light.draw over a scene, through pbgvm's --dump (one hex digit a
 * pixel, a row a line). */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-light-'));

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

let n = 0;
function run(draw, dump) {
    const res = compileFiles({
        'cart.json': JSON.stringify({ id: '00000000001167e5', name: 'Light' }),
        'main.js': `var black = [], half = [];
function init() { for (var c = 0; c < 16; c++) { black.push(0); half.push(1); } }
function update() {}
function draw() { cls(7); ${draw} }` });
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(tmp, 'l' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '1', '--dump', dump], { encoding: 'utf8' });
    return { out: r.stdout.replace(/\r/g, ''), err: r.stderr, status: r.status };
}

test('full light within half the reach, the darkest table past it all', () => {
    /* a light of reach 20 at (40, 40): a row through its centre */
    const r = run('light.draw(40, 40, 20, 1, [black, half]);', '10,40,61,1');
    assert.equal(r.status, 0, r.err);
    const row = r.out.trim();
    assert.equal(row.length, 61);
    /* (10..19) past the reach: black */
    assert.match(row.slice(0, 10), /^0+$/);
    /* (31..49) within half the reach: as drawn */
    assert.match(row.slice(21, 40), /^7+$/);
    /* between, dithered: only the scene's own colour, half's and black */
    assert.match(row.slice(10, 21), /^[017]+$/);
    assert.match(row.slice(10, 21), /1/);
    /* the right side the same way round */
    assert.match(row.slice(51), /^0+$/);
});

test('the nearest light wins, the camera moves them, and the clip holds', () => {
    let r = run('light.draw([20, 60], [20, 20], [10, 10], 2, [black]);', '0,20,80,1');
    const row = r.out.trim();
    assert.match(row.slice(17, 24), /^7+$/);
    assert.match(row.slice(57, 64), /^7+$/);
    assert.match(row.slice(35, 45), /^0+$/);
    /* the camera 10 to the right: the light drawn at 20 lands at 10 */
    r = run('camera(10, 0); light.draw(20, 20, 10, 1, [black]); camera();', '0,20,40,1');
    assert.match(r.out.trim().slice(7, 14), /^7+$/);
    assert.match(r.out.trim().slice(25, 40), /^0+$/);
    /* a clip: outside it nothing changes, not even to black */
    r = run('clip(0, 0, 29, 119); light.draw(100, 60, 10, 1, [black]); clip();', '20,60,20,1');
    assert.equal(r.out.trim(), '0000000000' + '7777777777');
});

test('no lights: all of it through the darkest table', () => {
    const r = run('light.draw(0, 0, 0, 0, [half]);', '0,0,8,2');
    assert.equal(r.out, '11111111\n11111111\n');
});

test('wrong arguments stop the cart', () => {
    assert.match(run('light.draw(1, 1, 1, 1, []);', '0,0,1,1').err, /a ramp: a list of 1 to 8/);
    assert.match(run('light.draw(1, 1, 1, 1, [5]);', '0,0,1,1').err, /ramp holds colour tables/);
    assert.match(run('light.draw([1], [1], [1], 3, [black]);', '0,0,1,1').err, /fewer x, y or r than lights/);
    assert.match(run('light.draw("a", 1, 1, 1, [black]);', '0,0,1,1').err, /needs x, y and r/);
});

test('more lights than one call takes stop the cart, rather than overrun', () => {
    const r = run('light.draw(80, 60, 20, 82595525, [black]);', '0,0,1,1');
    assert.equal(r.status, 1, r.err);
    assert.match(r.err, /at most 4096 lights/);
});

test('a small light on a pixel centre lights that pixel', () => {
    /* at x.5, y.5 a light's centre pixel is at distance 0: its stepping
     * must not wrap below the shade table's start */
    const r = run('light.draw([10.5], [60.5], [2.5], 1, [black, half]);', '10,60,1,1');
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out.trim(), '7');
});
