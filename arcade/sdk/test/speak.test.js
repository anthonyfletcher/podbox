/* Speech: speak.* through pbgvm, which prints what is said. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-speak-'));

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

let n = 0;
function run(main, extra = []) {
    const res = compileFiles({
        'cart.json': JSON.stringify({ id: '0000000000005ea4', name: 'Speak' }),
        'main.js': main + '\nfunction update() {}' });
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(tmp, 's' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '1'].concat(extra),
                        { encoding: 'utf8', timeout: 20000 });
    return { errors: [], out: r.stdout.replace(/\r/g, ''), err: r.stderr, status: r.status };
}

test('say and queue join their values as one phrase', () => {
    const r = run(`function init() {
    speak.say("Level", 3);
    speak.queue("score", 1200, "points");
    speak.say("");
    printh(speak.busy() + " " + speak.on());
    speak.stop();
}`);
    assert.equal(r.status, 0, r.err);
    /* an empty phrase says nothing */
    assert.equal(r.out, 'say Level 3\nqueue score 1200 points\n0 0\nstop\n');
});

test('on follows the owner\'s spoken menus', () => {
    assert.equal(run('function init() { printh(speak.on()); }', ['--speech']).out, '1\n');
});

test('a long phrase is cut at a whole character', () => {
    const r = run(`function init() {
    var s = "x"; while (s.length < 509) s = s + "x";
    speak.say(s + "éé");
}`);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, 'say ' + 'x'.repeat(509) + 'é\n');
});
