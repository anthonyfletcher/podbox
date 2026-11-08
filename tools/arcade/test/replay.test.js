/* The IDE's replays: a tape read back only when it is one, a replay's store
 * kept in memory, and the engine giving the same screens for the same
 * input and seed, which is what a replay rests on. */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { hashScreen, memoryStore, readTape, VERSION } from '../ide/replay.js';
import { compileFiles } from '../src/node.js';
import { Engine } from '../src/runner.js';

const here = path.dirname(fileURLToPath(import.meta.url));

test('a tape is read back only when it is one', () => {
    assert.ok(readTape('nonsense') instanceof Error);
    assert.ok(readTape(JSON.stringify({ version: VERSION + 1, steps: [], cart: {}, seed: '1' })) instanceof Error);
    const t = readTape(JSON.stringify({ version: VERSION, steps: [{ b: [0, 0, -1] }], cart: { id: 'x' }, seed: '5' }));
    assert.equal(t.seed, '5');
});

test('a replay\'s store starts as recorded and changes only in memory', () => {
    const s = memoryStore({ best: '12' });
    assert.equal(new TextDecoder().decode(s.read('best')), '12');
    s.write('new', new TextEncoder().encode('x'));
    assert.deepEqual(s.list(), ['best', 'new']);
    assert.equal(s.remove('best'), true);
    assert.equal(s.read('best'), undefined);
});

test('the same seed and input give the same screens; other input does not', async () => {
    const main = `var x = 80, y = 60;
function init() {}
function update() {
    if (btn(1)) x++;
    if (btn(0)) x--;
    y = (y + flr(rnd(3)) - 1 + 120) % 120;
}
function draw() { cls(); pset(x, y, 7); }`;
    const r = compileFiles({ 'cart.json': JSON.stringify({ id: '0123456789abcdef', name: 'R' }), 'main.js': main }, {});
    assert.deepEqual(r.errors, []);
    const wasm = new WebAssembly.Module(fs.readFileSync(path.join(here, '..', 'wasm', 'arcade.wasm')));
    const play = async (held) => {
        const e = new Engine(wasm, {});
        assert.ok(e.load(r.cart, 42n));
        assert.ok(await e.start());
        const out = [];
        for (const h of held) { assert.ok(await e.tick(h, 0, -1, true, 30)); out.push(hashScreen(e.pixels())); }
        return out;
    };
    const NEXT = 2;
    const a = await play([0, NEXT, NEXT, 0, 0]), b = await play([0, NEXT, NEXT, 0, 0]);
    assert.deepEqual(a, b);
    const c = await play([0, 0, NEXT, 0, 0]);
    assert.notDeepEqual(a, c);
});
