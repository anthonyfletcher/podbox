/* The IDE's map editor reads and writes a map's JSON: written again, a map
 * keeps its cells, objects and any other fields, and the compiler takes it. */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readMap, writeMap } from '../ide/map.js';
import { buildAssets } from '../src/assets.js';

const MAP = JSON.stringify({
    width: 5, height: 3, note: 'kept',
    rows: ['0102030405', '0a0B', ''],
    objects: [{ kind: 'door', x: 8, y: 16, w: 8, h: 16, props: { to: 'cave', key: 2 } },
              { kind: 'coin', x: 3, y: 5 }]
});

test('a map read and written again is the same map', () => {
    const m = readMap(MAP), back = JSON.parse(writeMap(m)), was = JSON.parse(MAP);
    assert.equal(back.width, 5);
    assert.equal(back.height, 3);
    assert.equal(back.note, 'kept');
    assert.deepEqual(back.rows, ['0102030405', '0a0b000000', '0000000000']);
    assert.deepEqual(back.objects, was.objects);
    assert.equal(writeMap(readMap(writeMap(m))), writeMap(m));
});

test('a row to a line and an object to a line', () => {
    const lines = writeMap(readMap(MAP)).split('\n');
    assert.ok(lines.includes('  "0102030405",'));
    assert.ok(lines.includes('  { "kind": "coin", "x": 3, "y": 5 }'));
});

test('a broken map is refused with the reason', () => {
    assert.throws(() => readMap('{"width": 0, "height": 2}'), /width and height/);
    assert.throws(() => readMap('{"width": 2, "height": 2, "rows": ["0g"]}'), /row 0/);
});

test('the compiler takes what the editor writes', () => {
    const errors = [], strings = [];
    const r = buildAssets({ maps: { level: writeMap(readMap(MAP)) } }, null,
                          (s) => strings.push(s) - 1, errors);
    assert.deepEqual(errors, []);
    assert.equal(r.groups.MAP.level.value, 0);
    assert.deepEqual(Object.keys(r.groups.OBJ).sort(), ['coin', 'door']);
});
