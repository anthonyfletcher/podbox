/* The noise system, through pbgvm. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-noise-'));
const CART = JSON.stringify({ id: '00000000000a0153', name: 'Noise' });

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


/* A board from rows of digits, and printh of it back as the same */
const BOARD = `
function board(rows) {
    var w = rows[0].length, b = new Uint8Array(w * rows.length);
    for (var y = 0; y < rows.length; y++)
        for (var x = 0; x < w; x++) b[y * w + x] = rows[y].charCodeAt(x) - 48;
    return b;
}
function show(b, w) {
    var s = "";
    for (var i = 0; i < b.length; i++) s += (i && i % w == 0 ? "/" : "") + b[i];
    printh(s);
}
function ints(a, n) { var s = ""; for (var i = 0; i < n; i++) s += (i ? "," : "") + a[i]; printh(s); }
`;
function noisy(body) {
    const r = run(BOARD + 'function init() {\n' + body + '\n}\nfunction update() {}');
    assert.equal(r.status, 0, r.err || JSON.stringify(r.errors));
    return r.out.trim().split('\n');
}


test('fill is at() at every cell, in range, and smooth', () => {
    const out = noisy(`var b = new Uint8Array(12 * 9), bad = 0, top = 0, jump = 0;
    noise.fill(b, 12, 5, -3, 4, 3, 77, 200);
    for (var y = 0; y < 9; y++)
        for (var x = 0; x < 12; x++) {
            var want = flr(noise.at((5 + x) / 4.0, (-3 + y) / 4.0, 77, 3) * 200);
            var v = b[y * 12 + x];
            if (v != want) bad++;
            top = max(top, v);
            if (x > 0) jump = max(jump, abs(v - b[y * 12 + x - 1]));
        }
    printh(bad + " " + (top < 200) + " " + (jump < 60));
    printh((noise.at(3.5, 2.25, 1) == noise.at(3.5, 2.25, 1)) + " " + (noise.at(3.5, 2.25, 1) != noise.at(3.5, 2.25, 2)));`);
    assert.deepEqual(out, ['0 1 1', '1 1']);
});

test('a ramp picks its values, and an image fills as a board does', () => {
    const out = noisy(`var b = new Uint8Array(16 * 16), seen = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    noise.fill(b, 16, 0, 0, 3, 2, 9, [1, 12, 15, 3, 11]);
    for (var i = 0; i < 256; i++) seen[b[i]]++;
    var s = ""; for (i = 0; i < 16; i++) if (seen[i]) s += i + " "; printh(s);
    var im = img(16, 16);
    noise.fill(im, 0, 0, 0, 3, 2, 9, [1, 12, 15, 3, 11]);
    cls(); blit(im, 0, 0);
    var diff = 0;
    for (i = 0; i < 256; i++) if (pget(i % 16, flr(i / 16)) != b[i]) diff++;
    printh(diff);`);
    /* only the ramp's values appear, and some of each end */
    assert.match(out[0], /^(1 )?(3 )?(11 )?(12 )?(15 )?$/);
    assert.ok(out[0].split(' ').filter(Boolean).length >= 3, out[0]);
    assert.equal(out[1], '0');
});

test('noise arguments are checked', () => {
    const r = run(BOARD + 'function init() { noise.fill(new Uint8Array(4), 2, 0, 0, 4, 1, 0, 0); }\nfunction update() {}');
    assert.match(r.err, /needs a ramp/);
    const r2 = run(BOARD + 'function init() { noise.fill(new Uint8Array(4), 2, 0, 0, 0, 1, 0, 4); }\nfunction update() {}');
    assert.match(r2.err, /scale of at least/);
});
