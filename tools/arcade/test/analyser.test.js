/* The beat analyser's share of a tick: reported with the cart's units while
 * it listens to playing music, and only then. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-analyser-'));

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

/* listens for ticks 1-3, then stops */
function units(rate, extra) {
    const res = compileFiles({
        'cart.json': JSON.stringify({ id: '0000000000a0a15e', name: 'Listen', rate: rate }),
        'main.js': `var n = 0;
function init() { beat.listen(true); }
function update() { n++; if (n == 3) beat.listen(false); }` });
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'listen' + rate + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '5', '--units'].concat(extra),
                        { encoding: 'utf8' });
    return r.stderr.replace(/\r/g, '').split('\n').filter((l) => l.startsWith('tick'))
        .map((l) => +l.split(' ')[2]);
}

test('a listening tick carries the analyser\'s share, by the rate', () => {
    for (const rate of [20, 33]) {
        const quiet = units(rate, []);
        const heard = units(rate, ['--beat', '120']);
        const share = Math.floor(208000 / rate);
        assert.deepEqual(heard.slice(0, 2), quiet.slice(0, 2).map((u) => u + share));
        /* the tick that stops listening and those after carry none */
        assert.deepEqual(heard.slice(2), quiet.slice(2));
    }
});
