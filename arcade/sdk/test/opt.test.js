/* Options: the settings cart.json declares, opt.*, and the values the host
 * keeps between runs, through pbgvm. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-opt-'));

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

const OPTIONS = [
    { name: 'level', title: 'Difficulty', type: 'choice',
      choices: ['Easy', 'Normal', 'Hard'], default: 1, help: 'How fast it gets.' },
    { name: 'sound', type: 'onoff', default: true },
    { name: 'lives', type: 'number', min: 1, max: 9, step: 2, default: 3 }
];

function compile(main, opts = OPTIONS) {
    return compileFiles({
        'cart.json': JSON.stringify({ id: '00000000000005e7', name: 'Options',
                                      options: opts }),
        'main.js': main });
}

let n = 0;
function run(main, extra = {}) {
    const res = compile(main, extra.options);
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(tmp, 'o' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const args = [file, '--ticks', String(extra.ticks || 1)];
    if (extra.file) args.push('--options', extra.file);
    if (extra.set) args.push('--opt', extra.set);
    const r = spawnSync(exe, args, { encoding: 'utf8', timeout: 20000 });
    return { errors: [], out: r.stdout.replace(/\r/g, ''), err: r.stderr, status: r.status };
}

test('options are checked in cart.json', () => {
    const bad = (opts) => compile('function update() {}', opts)
        .errors.map((e) => e.message).join('\n');
    assert.match(bad({}), /options must be a list/);
    assert.match(bad([{ name: 'a', type: 'slider' }]), /type must be one of choice, onoff, number/);
    assert.match(bad([{ name: 'a', type: 'choice', choices: ['x'] }]), /choices must be 2 to 8 texts/);
    assert.match(bad([{ name: 'a', type: 'choice', choices: ['x', 'y'], default: 2 }]),
                 /default must be the index of one of its choices/);
    assert.match(bad([{ name: 'a', type: 'onoff', min: 0 }]), /only a number has min/);
    assert.match(bad([{ name: 'a', type: 'onoff', default: 'yes' }]), /default must be true or false/);
    assert.match(bad([{ name: 'a', type: 'number', min: 5, max: 5 }]), /min below max/);
    assert.match(bad([{ name: 'a', type: 'number', min: 0, max: 5, step: 0 }]), /step must be/);
    assert.match(bad([{ name: 'a', type: 'number', min: 0, max: 5, default: 6 }]),
                 /default must be a whole number from min to max/);
    assert.match(bad([{ name: 'a', type: 'onoff' }, { name: 'a', type: 'onoff' }]),
                 /a second option called a/);
    assert.match(bad([{ name: 'a', type: 'onoff', help: 'x'.repeat(301) }]), /help must be text/);
    assert.match(compile('function update() { opt.get(OPT.nope); }').errors[0].message, /nope/);
});

test('get gives each option its default until the owner changes it', () => {
    const r = run(`function init() { printh(opt.get(OPT.level) + " " + opt.get(OPT.sound) + " " + opt.get(OPT.lives) + " " + opt.changed()); }
function update() {}`);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, '1 1 3 0\n');
});

test('a change shows for the next tick only, kept within the range and steps', () => {
    const r = run(`function init() {}
function update() { printh(frame() + " " + opt.changed() + " " + opt.get(OPT.level) + " " + opt.get(OPT.lives)); }`,
                  { ticks: 4, set: '1:0:9,1:2:8,2:2:7,3:0:0' });
    assert.equal(r.status, 0, r.err);
    /* tick 1 reports the options as changed, so a cart reads them there;
     * tick 2: level 9 is brought down to Hard, lives 8 lands on 7 (1 + 3
     * steps of 2); tick 3: 7 again is no change; tick 4: Easy */
    assert.equal(r.out, '1 1 1 3\n2 1 2 7\n3 0 2 7\n4 1 0 7\n');
});

test('opt.set changes an option from the game, fitted, kept, and not reported as the owner\'s change', () => {
    const file = path.join(tmp, 'set.opt');
    const r = run(`function init() {}
function update() {
    if (frame() == 2) { opt.set(OPT.level, 0); opt.set(OPT.lives, 6); }
    printh(frame() + " " + opt.changed() + " " + opt.get(OPT.level) + " " + opt.get(OPT.lives));
}`, { ticks: 3, file });
    assert.equal(r.status, 0, r.err);
    /* 6 lands on 5, between 1 + 2 steps of 2 and 7 */
    assert.equal(r.out, '1 1 1 3\n2 0 0 5\n3 0 0 5\n');
    assert.equal(run('function init() { printh(opt.get(OPT.level) + " " + opt.get(OPT.lives)); }\nfunction update() {}',
                     { file }).out, '0 5\n');
    const bad = run('function init() { opt.set(9, 1); }\nfunction update() {}');
    assert.equal(bad.status, 1);
    assert.match(bad.err, /opt\.set\(\) needs one of the cart's options/);
});

test('an unknown option stops the cart', () => {
    const r = run('function init() { opt.get(5); }\nfunction update() {}');
    assert.equal(r.status, 1);
    assert.match(r.err, /opt\.get\(\) needs one of the cart's options/);
});

test('the host keeps the values between runs, matched by name', () => {
    const file = path.join(tmp, 'options.opt');
    const main = `function init() { printh(opt.get(OPT.level) + " " + opt.get(OPT.lives)); }
function update() {}`;
    assert.equal(run(main, { file, set: '0:0:0,0:2:9' }).out, '1 3\n');
    assert.equal(run(main, { file }).out, '0 9\n');
    /* reordered, and lives' range narrowed: each finds its value, and one
     * now out of range is brought within it */
    const changed = [Object.assign({}, OPTIONS[2], { max: 5 }), OPTIONS[0], OPTIONS[1]];
    assert.equal(run(main, { file, options: changed }).out, '0 5\n');
    fs.writeFileSync(file, Buffer.from('PBOP\x01\x09'));
    assert.equal(run(main, { file }).out, '1 3\n');
});
