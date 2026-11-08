/* The IDE's sound editor: what its generators make and what it writes the
 * compiler takes, and what it plays is what the build stores. */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { GENERATORS, generate, readSound, writeSound, rendered } from '../ide/sound.js';
import { buildAssets } from '../src/assets.js';

/* a repeatable random() */
function seeded(s) {
    return () => { s = Math.imul(s ^ (s >>> 15), 2246822507) + 0x6d2b79f5 | 0; return (s >>> 0) / 4294967296; };
}

test('every generator makes sounds the compiler takes', () => {
    const errors = [], sounds = {};
    for (const [kind] of GENERATORS)
        for (let i = 0; i < 20; i++) {
            const p = generate(kind, generate('blip', null, seeded(i)), seeded(i * 31 + kind.length));
            sounds[kind + i] = writeSound({ params: p, rest: {} });
        }
    const r = buildAssets({ sounds }, null, () => 0, errors);
    assert.deepEqual(errors, []);
    assert.equal(Object.keys(r.groups.SFX).length, GENERATORS.length * 20);
});

test('a sound read and written again keeps its settings and jsfxr\'s extras', () => {
    const text = JSON.stringify({ wave_type: 2, p_base_freq: 0.45, p_freq_ramp: -0.2, sample_rate: 44100 });
    const back = JSON.parse(writeSound(readSound(text)));
    assert.equal(back.wave_type, 2);
    assert.equal(back.p_base_freq, 0.45);
    assert.equal(back.p_freq_ramp, -0.2);
    assert.equal(back.sample_rate, 44100);
    assert.equal(writeSound(readSound(writeSound(readSound(text)))), writeSound(readSound(text)));
});

test('a broken sound is refused with the reason', () => {
    assert.throws(() => readSound('{"p_base_freq": 2}'), /p_base_freq/);
    assert.throws(() => readSound('[1]'), /object/);
});

test('what the editor plays is what the build stores', () => {
    const text = writeSound({ params: generate('explosion', null, seeded(7)), rest: {} });
    const r = buildAssets({ sounds: { boom: text } }, null, () => 0, []);
    const chunk = r.chunks.find((c) => c.tag === 'SFX_').data;
    const n = chunk[2] | chunk[3] << 8 | chunk[4] << 16 | chunk[5] << 24, scale = chunk[6] | chunk[7] << 8;
    const pcm = rendered(readSound(text).params, 'boom');
    assert.equal(pcm.length, n);
    for (let i = 0; i < n; i += 97)
        assert.equal(Math.round(pcm[i] * 32768 / scale), (chunk[8 + i] << 24) >> 24);
});
