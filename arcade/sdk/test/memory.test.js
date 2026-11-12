/* The memory view: what the heap holds, by kind and by the global that
 * reaches it, as pbgvm --memory and the IDE report it -- and that looking
 * changes nothing the cart sees. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-memory-'));
const CART = JSON.stringify({ id: '00000000000a3e30', name: 'Memory' });

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

let n = 0;
function run(main, args) {
    const res = compileFiles({ 'cart.json': CART, 'main.js': main });
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'm' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, ...args], { encoding: 'utf8', timeout: 20000 });
    assert.equal(r.status, 0, r.stderr);
    return { out: r.stdout.replace(/\r/g, ''), globals: res.globals };
}

/* --memory's lines as { totals, kinds, globals } */
function parse(out) {
    const m = { totals: {}, kinds: {}, globals: {} };
    for (const line of out.split('\n')) {
        const w = line.split(' ');
        if (w[0] !== 'memory') continue;
        if (w[1] === 'kind') m.kinds[w[2]] = { count: +w[3], bytes: +w[4] };
        else if (w[1] === 'global') m.globals[+w[2]] = +w[3];
        else for (let i = 1; i < w.length; i += 2) m.totals[w[i]] = +w[i + 1];
    }
    return m;
}

const CODE = `
var big, words, shared, again;
function init() {
    big = new Int32Array(10000);
    words = [];
    for (var i = 0; i < 100; i++) words.push("word" + i);
    shared = [1, 2, 3];
    again = shared;
}
function update() { var junk = [1, 2, 3, 4]; printh(rnd(1000)); }
`;

test('the heap by kind and by global, held, garbage and free', () => {
    const { out, globals } = run(CODE, ['--ticks', '30', '--memory']);
    const m = parse(out);
    const slot = (name) => globals.indexOf(name);
    /* 10,000 numbers and the header */
    assert.equal(m.kinds.Int32Array.count, 1);
    assert.equal(m.globals[slot('big')], m.kinds.Int32Array.bytes);
    assert.ok(m.kinds.Int32Array.bytes >= 40000);
    /* the hundred strings, and the array and its storage */
    assert.equal(m.kinds.string.count, 100);
    assert.ok(m.globals[slot('words')] > m.kinds.string.bytes);
    /* what two globals reach is counted to the first */
    assert.ok(m.globals[slot('shared')] > 0);
    assert.equal(m.globals[slot('again')], undefined);
    /* every held byte is a global's or the engine's */
    const sum = Object.values(m.globals).reduce((a, b) => a + b, 0);
    assert.equal(sum + m.totals.engine + m.totals.constants, m.totals.held);
    /* the arrays each update made and dropped, not yet collected */
    assert.ok(m.totals.garbage > 0);
    assert.equal(m.totals.held + m.totals.garbage + m.totals.free, m.totals.heap);
    assert.ok(m.totals.largest <= m.totals.free);
    assert.equal(m.totals.tables + m.totals.heap, m.totals.arena);
});

test('looking at memory changes nothing the cart sees', () => {
    const plain = run(CODE, ['--ticks', '60']).out;
    const looked = run(CODE, ['--ticks', '60', '--memory']).out
        .split('\n').filter((l) => !l.startsWith('memory ')).join('\n');
    assert.equal(looked, plain);
});
