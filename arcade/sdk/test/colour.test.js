/* playback.colors() and the color.* calls, through pbgvm's printh: the
 * made-up album's colours while the music plays and the theme's when it
 * does not, tones that land where asked with the hue kept, a turn that
 * keeps the tone, mixing and contrast. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-colour-'));

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

let n = 0;
function run(body, extra = []) {
    const res = compileFiles({
        'cart.json': JSON.stringify({ id: '00000000000c0100', name: 'Colour' }),
        'main.js': 'var c = new Int32Array(3);\nfunction init() {\n' + body +
            '\n}\nfunction update() {}\n' });
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'c' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '1'].concat(extra), { encoding: 'utf8' });
    return { out: r.stdout.replace(/\r/g, '').trim().split('\n'), err: r.stderr };
}

/* the hue of 0xRRGGBB in degrees, for checking it is kept */
function hue(c) {
    const r = (c >> 16 & 255) / 255, g = (c >> 8 & 255) / 255, b = (c & 255) / 255;
    const max = Math.max(r, g, b), d = max - Math.min(r, g, b);
    if (!d) return 0;
    const h = max === r ? (g - b) / d : max === g ? 2 + (b - r) / d : 4 + (r - g) / d;
    return (h * 60 + 360) % 360;
}

test('playback.colors: the album\'s while the music plays, the theme\'s when it stops', () => {
    const body = 'printh(playback.colors(c) + " " + c[0] + " " + c[1] + " " + c[2]);';
    assert.deepEqual(run(body).out, ['0 0 16777215 16777215']);
    assert.deepEqual(run(body, ['--playing']).out,
                     ['1 ' + 0x14283c + ' ' + 0xe8f0f8 + ' ' + 0x3cc8a0]);
    assert.match(run('playback.colors([1, 2]);').err, /Int32Array of 3/);
});

test('color.tone: black 0, white 100, mid grey about half', () => {
    const r = run('printh(color.tone(0) + " " + color.tone(0xffffff) + " " + color.tone(0x777777));');
    const [black, white, grey] = r.out[0].split(' ').map(Number);
    assert.equal(black, 0);
    assert.equal(white, 100);
    assert.ok(grey > 45 && grey < 55, 'grey ' + grey);
});

test('color.toned: lands on the tone asked, with the hue kept', () => {
    const tones = [10, 25, 50, 75, 90];
    const r = run('var t = [' + tones + '];\nfor (var i = 0; i < 5; i++) { var k = color.toned(0xe0306a, t[i]); ' +
                  'printh(k + " " + color.tone(k)); }');
    r.out.forEach((line, i) => {
        const [k, tone] = line.split(' ').map(Number);
        assert.ok(Math.abs(tone - tones[i]) <= 1, 'tone ' + tone + ' for ' + tones[i]);
        assert.ok(Math.abs(hue(k) - hue(0xe0306a)) < 12, 'hue ' + hue(k) + ' at tone ' + tones[i]);
    });
    assert.deepEqual(run('printh(color.toned(0x336699, 0) + " " + color.toned(0x336699, 100));').out,
                     ['0 16777215']);
});

test('color.turn: half a turn moves the hue round and keeps the tone', () => {
    const r = run('var k = color.turn(0x3cc8a0, 0.5);\nprinth(k + " " + color.tone(k) + " " + color.tone(0x3cc8a0) + " " + color.turn(0x808080, 0.25));');
    const [k, tone, was, grey] = r.out[0].split(' ').map(Number);
    assert.ok(Math.abs(tone - was) <= 2, tone + ' against ' + was);
    const d = Math.abs(hue(k) - hue(0x3cc8a0));
    assert.ok(Math.min(d, 360 - d) > 120, 'hue moved ' + d);
    assert.equal(grey, 0x808080, 'a grey has no hue to turn');
});

test('color.mix and color.contrast', () => {
    const r = run('printh(color.mix(0x000000, 0xffffff, 0.5) + " " + color.mix(0x102030, 0x405060, 0) + " " + ' +
                  'color.mix(0x102030, 0x405060, 1.0) + " " + color.contrast(0, 0xffffff) + " " + color.contrast(0x777777, 0x777777));');
    const v = r.out[0].split(' ').map(Number);
    assert.equal(v[0], 0x808080);
    assert.equal(v[1], 0x102030);
    assert.equal(v[2], 0x405060);
    assert.ok(Math.abs(v[3] - 21) < 0.01, 'black on white ' + v[3]);
    assert.equal(v[4], 1);
});
