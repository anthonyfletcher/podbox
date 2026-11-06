/* Suspending: a cart suspended between ticks and resumed from a copy of
 * its arena carries on exactly as one never suspended. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-suspend-'));

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

/* the heap churning (strings, records, a function value, arrays the
 * collector frees), random numbers, drawing and the saved values */
const CART = `
var things = [], words = {}, count = 0, step;
function triple(x) { return x * 3 + count; }
function init() { step = triple; dset(0, 7); }
function update() {
    count++;
    things.push({ x: rnd(160), y: rnd(120), name: "n" + count });
    if (things.length > 40) things.shift();
    words["w" + (count % 13)] = step(count) + " " + flr(rnd(1000));
    dset(1, dget(1) + count);
    if (count % 10 == 0) printh(count + " " + words["w3"] + " " + things.length + " " + dget(1) + " " + flr(rnd(100)));
}
function draw() {
    cls(1);
    for (var i = 0; i < things.length; i++) circfill(things[i].x, things[i].y, 2, 8 + i % 8);
    print(words["w" + (count % 13)], 2, 2, 7);
}`;

function run(extra) {
    const res = compileFiles({
        'cart.json': JSON.stringify({ id: '000000000000505e', name: 'Suspend' }),
        'main.js': CART });
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'suspend.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '120', '--dump', '0,0,160,120'].concat(extra),
                        { encoding: 'utf8', timeout: 20000 });
    assert.equal(r.status, 0, r.stderr);
    return r.stdout.replace(/\r/g, '');
}

test('a resumed cart carries on as one never suspended', () => {
    const straight = run([]);
    for (const at of ['1', '37', '119']) {
        const out = run(['--suspend-at', at]);
        assert.match(out, /^resumed$/m);
        assert.equal(out.replace(/^resumed\n/m, ''), straight, 'suspended at ' + at);
    }
});
