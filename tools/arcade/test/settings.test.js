/* The IDE's cart settings form writes cart.json: in a fixed order, empty
 * texts, lists and unnamed flags left out, and taken by the compiler. */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { writeSettings } from '../ide/settings.js';
import { compileFiles } from '../src/node.js';

const MAIN = 'function init() {}\nfunction update() {}\n';

test('settings are written in order, the empty ones left out', () => {
    const text = writeSettings({
        rate: 25, name: 'Test', id: '0123456789abcdef', author: '', help: 'Hi',
        flags: ['solid', null, '', null], boards: [], options: [], achievements: [{ name: 'won' }]
    });
    const c = JSON.parse(text);
    assert.deepEqual(Object.keys(c), ['id', 'name', 'help', 'rate', 'flags', 'achievements']);
    assert.deepEqual(c.flags, ['solid']);
    assert.equal(writeSettings(JSON.parse(text)), text);
    const none = JSON.parse(writeSettings({ id: '0123456789abcdef', name: 'T', flags: [null, null] }));
    assert.equal(none.flags, undefined);
});

test('the compiler takes what the form writes', () => {
    const text = writeSettings({
        id: '0123456789abcdef', name: 'Test', rate: 50, menu_button: 'game', music: 'control',
        flags: [null, 'ladder'],
        boards: [{ name: 'points' }, { name: 'lap', title: 'Best Lap', wins: 'low', format: 'time', version: 2 }],
        options: [{ name: 'level', type: 'choice', choices: ['Easy', 'Hard'], default: 1 },
                  { name: 'trails', type: 'onoff', default: true },
                  { name: 'speed', type: 'number', min: 1, max: 10, step: 1, default: 3, help: 'How fast' }],
        achievements: [{ name: 'won', title: 'Winner', text: 'Win a game' }]
    });
    const r = compileFiles({ 'main.js': MAIN, 'cart.json': text }, {});
    assert.deepEqual(r.errors, []);
});

test('"suspend": false is kept in the header, and anything but true or false refused', () => {
    const cart = (s) => JSON.stringify(Object.assign({ id: '0123456789abcdef', name: 'T' }, s));
    const flag = (s) => {
        const r = compileFiles({ 'main.js': MAIN, 'cart.json': cart(s) }, {});
        assert.deepEqual(r.errors, []);
        const b = Buffer.from(r.cart), at = b.indexOf('PBG1') + 8;    /* the header's chunk, past its tag and length */
        return b[at + 7];
    };
    assert.equal(flag({}), 0);
    assert.equal(flag({ suspend: true }), 0);
    assert.equal(flag({ suspend: false }), 1);
    const bad = compileFiles({ 'main.js': MAIN, 'cart.json': cart({ suspend: 'no' }) }, {});
    assert.match(bad.errors.map((e) => e.message).join('\n'), /suspend must be true or false/);
    assert.equal(JSON.parse(writeSettings({ id: '0123456789abcdef', name: 'T', suspend: false })).suspend, false);
});
