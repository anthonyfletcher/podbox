/* Sound: sfxr rendered at build time, 8-bit samples, and the engine's voices
 * as pbgvm --wav records them, sample for sample against a model of the
 * engine's decoder and interpolation. */

import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { compileFiles, readFolder, readAssets } from '../src/node.js';
import { sfxrParams, sfxrRender, halve, SFXR_RATE } from '../src/sfxr.js';
import { quantise } from '../src/assets.js';
import { readCart } from '../src/cart.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const host = path.join(here, '..', 'host');
const exe = path.join(host, process.platform === 'win32' ? 'pbgvm.exe' : 'pbgvm');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-sound-'));
const CART = JSON.stringify({ id: '0000000000050a1d', name: 'Sound', rate: 25 });

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

const BLIP = { wave_type: 0, p_env_attack: 0, p_env_sustain: 0.1, p_env_decay: 0.15,
               p_base_freq: 0.4, p_duty: 0.2, sound_vol: 0.5 };
const ZAP = { wave_type: 1, p_env_attack: 0.05, p_env_sustain: 0.12, p_env_punch: 0.4,
              p_env_decay: 0.2, p_base_freq: 0.6, p_freq_ramp: -0.3, p_lpf_freq: 0.7,
              p_lpf_resonance: 0.3, p_hpf_freq: 0.05, p_pha_offset: 0.1, sound_vol: 0.6 };

/* A cart folder with these sounds and main.js; its cart and run */
let n = 0;
function cart(sounds, main, ticks, extra) {
    const dir = path.join(tmp, 'c' + n++);
    fs.mkdirSync(path.join(dir, 'sounds'), { recursive: true });
    fs.writeFileSync(path.join(dir, 'cart.json'), CART);
    fs.writeFileSync(path.join(dir, 'main.js'), main);
    for (const k in sounds)
        fs.writeFileSync(path.join(dir, 'sounds', k + '.json'),
                         typeof sounds[k] === 'string' ? sounds[k] : JSON.stringify(sounds[k]));
    const res = compileFiles(readFolder(dir), readAssets(dir));
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(dir, 'cart.pbg'), wav = path.join(dir, 'out.wav');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', String(ticks), '--wav', wav].concat(extra || []),
                        { encoding: 'utf8', timeout: 30000 });
    assert.equal(r.status, 0, r.stderr);
    const b = fs.readFileSync(wav);
    const pcm = new Int16Array(b.buffer.slice(b.byteOffset + 44, b.byteOffset + b.length));
    const left = pcm.filter((_, i) => !(i & 1));
    return { errors: [], out: r.stdout.replace(/\r/g, ''), left: left, cart: res.cart };
}

/* The engine's playback of samples at 22,050 Hz into 44.1 kHz:
 * synth.c's interpolation, with its pitch */
function model(samples, pitch, frames) {
    const out = new Int16Array(frames);
    const step = Math.floor(Math.floor((22050 * 65536) / 44100) * pitch / 65536);
    let next = 0, s0 = 0, s1 = samples.length ? samples[next++] : 0, frac = 0, done = 0, on = true;
    for (let f = 0; f < frames && on; f++) {
        out[f] = Math.max(-32768, Math.min(32767, s0 + (((s1 - s0) * (frac >> 2)) >> 14)));
        frac += step;
        while (frac >= 0x10000) {
            frac -= 0x10000;
            s0 = s1;
            if (next >= samples.length && done++) { on = false; break; }
            s1 = next < samples.length ? samples[next++] : 0;
        }
    }
    return out;
}

test('sfxr renders as long as its envelope, and 8 bits with a scale keep it close', () => {
    const p = sfxrParams(BLIP);
    const pcm = sfxrRender(p, 1);
    const env = Math.trunc(0.1 * 0.1 * 100000) + Math.trunc(0.15 * 0.15 * 100000);
    assert.ok(Math.abs(pcm.length - env) <= 3, pcm.length + ' against ' + env);
    assert.equal(SFXR_RATE, 44100);
    const half = halve(sfxrRender(sfxrParams(ZAP), 7));
    const q = quantise(half);
    const back = Array.from(q.bytes, (b) => (b << 24 >> 24) * q.scale);
    let sig = 0, noise = 0;
    for (let i = 0; i < half.length; i++) {
        sig += half[i] * half[i];
        noise += (half[i] - back[i]) ** 2;
    }
    assert.ok(10 * Math.log10(sig / noise) > 40, 'SNR ' + 10 * Math.log10(sig / noise));
    assert.throws(() => sfxrParams({ p_base_freq: 2 }), /p_base_freq is a number from 0 to 1/);
});

test('sfx plays the effect exactly as built, and sfxp an octave up takes half as long', () => {
    const r = cart({ blip: BLIP, zap: ZAP },
                   'function init() { sfx(SFX.zap); }\nfunction update() {}', 25);
    assert.deepEqual(r.errors, []);
    const chunks = readCart(r.cart).chunks.SFX_;
    /* the zap is the second effect: walk past the first */
    const dv = new DataView(chunks.buffer, chunks.byteOffset, chunks.byteLength);
    let at = 2;
    const read = () => {
        const n = dv.getUint32(at, true), scale = dv.getUint16(at + 4, true);
        const bytes = chunks.subarray(at + 6, at + 6 + n);
        at += 6 + n;
        return Array.from(bytes, (b) => (b << 24 >> 24) * scale);
    };
    read();
    const zap = read();
    const want = model(zap, 65536, r.left.length);
    assert.deepEqual(Array.from(r.left), Array.from(want));
    const heard = (pcm) => {
        let last = 0;
        pcm.forEach((v, i) => { if (v) last = i; });
        return last;
    };
    const up = cart({ blip: BLIP, zap: ZAP },
                    'function init() { sfxp(SFX.zap, 12); }\nfunction update() {}', 25);
    const ratio = heard(up.left) / heard(r.left);
    assert.ok(Math.abs(ratio - 0.5) < 0.01, 'an octave up lasts ' + ratio);
    assert.deepEqual(Array.from(up.left), Array.from(model(zap, 131072, up.left.length)));
});

test('sfxat tells what each channel plays, and a muted game mixes silence that ends on time', () => {
    const main = 'var tk = 0;\nfunction init() { sfx(SFX.blip, 2); tone(1, 300, 4, 100, WAVE.SINE); }\n' +
                 'function update() { tk++; printh(tk + " " + sfxat(0) + " " + sfxat(1) + " " + sfxat(2)); }';
    const r = cart({ blip: BLIP }, main, 8);
    const lines = r.out.trim().split('\n');
    /* the effect is SFX.blip, 0; the tone -2 until its 4 ticks are mixed */
    assert.equal(lines[0], '1 -1 -2 0');
    assert.match(lines[7], /^8 -1 -1 /);
    assert.ok(r.left.some((v) => v !== 0));
    const opts = path.join(tmp, 'muted.opt');
    fs.writeFileSync(opts, Buffer.from('PBOP\x01\x00\x01\x00', 'latin1'));
    const muted = cart({ blip: BLIP }, main, 8, ['--options', opts]);
    assert.ok(muted.left.every((v) => v === 0), 'a muted game sounded');
    assert.deepEqual(muted.out, r.out);
});

test('four voices mix, sfx(-1) stops one, and tone plays its note for its ticks', () => {
    const r = cart({}, 'var tk = 0;\nfunction init() { tone(0, 441, 5, 255, WAVE.SQUARE); }\n' +
                   'function update() {}', 10);
    /* 5 ticks at 25 a second is 0.2 s: 8820 frames of a 100-frame square */
    let flips = 0, lastOn = 0;
    for (let i = 1; i < r.left.length; i++) {
        if ((r.left[i] > 0) !== (r.left[i - 1] > 0)) flips++;
        if (r.left[i]) lastOn = i;
    }
    assert.ok(Math.abs(lastOn - 8820) <= 2, 'tone ends at ' + lastOn);
    assert.ok(Math.abs(flips - 176) <= 2, flips + ' half-periods');
    assert.equal(Math.max(...r.left), 16320);
    const stop = cart({ blip: BLIP },
                      'var tk = 0;\nfunction init() { sfx(SFX.blip, 2); tone(1, 300, 50, 100, WAVE.SINE); }\n' +
                      'function update() { if (++tk == 3) { sfx(-1, 2); sfx(-1, 1); } }', 10);
    const tick = 44100 / 25;
    assert.ok(stop.left.slice(0, 2 * tick).some((v) => v !== 0));
    assert.ok(stop.left.slice(3 * tick).every((v) => v === 0), 'still sounding after sfx(-1)');
    /* 100 ticks is 4 s: past the 65536 samples a 32-bit length held */
    const long = cart({}, 'function init() { tone(0, 441, 100, 255, WAVE.SQUARE); }\n' +
                      'function update() {}', 110);
    let end = 0;
    for (let i = 0; i < long.left.length; i++)
        if (long.left[i]) end = i;
    assert.ok(Math.abs(end - 176400) <= 2, 'a long tone ends at ' + end);
});

test('click reaches the host, and bad sounds and calls are named', () => {
    const r = cart({}, 'function init() { click(); click(1); }\nfunction update() {}', 1);
    assert.equal(r.out, 'click 0\nclick 1\n');
    const bad = cart({ loud: { sound_vol: 3 }, broken: '{' }, 'function init() {}\nfunction update() {}', 1);
    const by = Object.fromEntries(bad.errors.map((e) => [e.file, e.message]));
    assert.match(by['sounds/loud.json'], /sound_vol is a number from 0 to 1/);
    assert.ok(by['sounds/broken.json']);
    const dir = path.join(tmp, 'calls');
    fs.mkdirSync(dir, { recursive: true });
    for (const [src, msg] of [['sfx(0);', /sfx\(\) needs one of the cart's sounds/],
                              ['tone(4, 440, 1, 100, 0);', /tone\(\) takes a channel from 0 to 3/],
                              ['tone(0, 440, 1, 100, 9);', /tone\(\) needs a WAVE/]]) {
        const res = compileFiles({ 'cart.json': CART,
                                   'main.js': 'function init() { ' + src + ' }\nfunction update() {}' });
        const file = path.join(dir, 'x.pbg');
        fs.writeFileSync(file, res.cart);
        const s = spawnSync(exe, [file, '--ticks', '1'], { encoding: 'utf8' });
        assert.match(s.stderr, msg);
    }
});
