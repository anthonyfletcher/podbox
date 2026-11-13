/* wasm/arcade.wasm, the engine pbgrun and the IDE run, against pbgvm: the
 * same printed lines, units every tick and pixels from the same input;
 * and a dialog paused for its frames (the IDE's way) against one answered
 * at once. */

import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { compileFiles, readFolder, readAssets } from '../src/node.js';
import { Engine } from '../src/runner.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(here, '..');
const exe = path.join(root, 'host', process.platform === 'win32' ? 'pbgvm.exe' : 'pbgvm');
const wasmFile = path.join(root, 'wasm', 'arcade.wasm');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-wasm-'));
const CART = JSON.stringify({ id: '00000000000a5a50', name: 'Wasm' });

before(() => {
    const r = spawnSync('sh', [path.join(root, 'host', 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
    assert.ok(fs.existsSync(wasmFile),
              'no wasm/arcade.wasm: build it with sh wasm/build.sh (wasi-sdk and binaryen)');
    /* a module older than the sources it is built from is not the engine
     * tested; the player's own files, which it does not build, do not count */
    const built = fs.statSync(wasmFile).mtimeMs;
    const newer = wasmSources().filter((f) => fs.statSync(f).mtimeMs > built)
        .map((f) => path.basename(f));
    assert.deepEqual(newer, [], 'wasm/arcade.wasm is older than these; rebuild it');
});

/* What wasm/build.sh compiles -- its srcs -- and every file they include,
 * as paths */
function wasmSources() {
    const engine = path.join(root, '..', '..', 'apps-ipod', 'games', 'arcade');
    const script = fs.readFileSync(path.join(root, 'wasm', 'build.sh'), 'utf8');
    const files = /srcs="([^"]*)"/.exec(script)[1].trim().split(/\s+/).map((f) =>
        f.replace('$here/../host/', path.join(root, 'host') + path.sep)
         .replace('$here/', path.join(root, 'wasm') + path.sep)
         .replace('$engine/', engine + path.sep));
    for (let i = 0; i < files.length; i++) {
        const dir = path.dirname(files[i]);
        for (const m of fs.readFileSync(files[i], 'utf8').matchAll(/^#include "([^"]+)"/gm)) {
            const f = m[1].startsWith('games/arcade/')
                ? path.join(engine, m[1].slice('games/arcade/'.length)) : path.join(dir, m[1]);
            if (fs.existsSync(f) && !files.includes(f)) files.push(f);
        }
    }
    return files;
}

function build(name, files, assets) {
    const res = compileFiles(files, assets);
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, name + '.pbg');
    fs.writeFileSync(file, res.cart);
    return file;
}

const lf = (s) => s.replace(/\r/g, '');

function both(file, extra) {
    const a = path.join(tmp, 'a.png'), b = path.join(tmp, 'b.png');
    const vm = spawnSync(exe, [file, ...extra, '--units', '--shot', a], { encoding: 'utf8' });
    const run = spawnSync(process.execPath, [path.join(root, 'pbgrun.js'), file, ...extra,
                                             '--units', '--shot', b], { encoding: 'utf8' });
    assert.equal(lf(run.stdout), lf(vm.stdout));
    assert.equal(lf(run.stderr), lf(vm.stderr));
    assert.equal(run.status, vm.status);
    if (vm.status === 0) assert.deepEqual(pixels(b), pixels(a));
}

/* a shot's pixels, whatever compression wrote it */
function pixels(file) {
    const b = fs.readFileSync(file), idat = [];
    for (let at = 8; at < b.length;) {
        const n = b.readUInt32BE(at);
        if (b.toString('latin1', at + 4, at + 8) === 'IDAT') idat.push(b.subarray(at + 8, at + 8 + n));
        at += 12 + n;
    }
    return zlib.inflateSync(Buffer.concat(idat));
}

test('the language programs: the same lines and units', () => {
    for (const f of fs.readdirSync(path.join(here, 'lang'))) {
        const src = fs.readFileSync(path.join(here, 'lang', f), 'utf8');
        both(build('lang-' + f, { 'cart.json': CART, 'main.js': src }), ['--ticks', '3']);
    }
});

test('--store keeps a cart\'s files between runs, the same through both', () => {
    const file = build('store', { 'cart.json': CART, 'main.js': `function init() {
    var n = store.read("runs");
    store.write("runs", n == null ? 1 : n + 1);
    printh("run " + (n == null ? 0 : n) + " " + store.list().join(","));
}
function update() {}` });
    for (const d of ['sa', 'sb']) fs.rmSync(path.join(tmp, d), { recursive: true, force: true });
    for (let k = 0; k < 2; k++) {
        const vm = spawnSync(exe, [file, '--ticks', '1', '--store', path.join(tmp, 'sa')], { encoding: 'utf8' });
        const run = spawnSync(process.execPath, [path.join(root, 'pbgrun.js'), file, '--ticks', '1',
                                                 '--store', path.join(tmp, 'sb')], { encoding: 'utf8' });
        assert.equal(lf(run.stdout), lf(vm.stdout));
        assert.equal(lf(vm.stdout), k === 0 ? 'run 0 runs\n' : 'run 1 runs\n');
    }
});

test('a drawing cart, with input, the wheel and skipped frames', () => {
    const dir = path.join(here, 'carts', 'drawdemo');
    const file = build('drawdemo', readFolder(dir), readAssets(dir));
    both(file, ['--ticks', '40', '--dump', '0,0,160,120', '--dirty', '--buttons', '3:16,4:0,9:3,12:0',
                '--wheel', '5:20:30,6:0,20:-7:-1', '--skip', '15:2']);
});

test('a late last tick: --dirty is the rows the next present sends', () => {
    const file = build('late', { 'cart.json': CART, 'main.js': `function init() {}
function update() {}
function draw() { pset(0, frame() * 3, 7); }` });
    both(file, ['--ticks', '6', '--dirty', '--skip', '5:1']);
    const vm = spawnSync(exe, [file, '--ticks', '6', '--dirty', '--skip', '5:1'], { encoding: 'utf8' });
    const [lo, hi] = lf(vm.stdout).trim().split(' ').slice(1).map(Number);
    assert.ok(lo <= hi, vm.stdout);
});

test('watch() and prof(): the same lines and units', () => {
    const file = build('watch', { 'cart.json': CART, 'main.js': `function init() {}
function update() {
    watch("half", (frame() >> 1) + "");
    watch("same", "x");
    prof("outer"); prof("inner"); cls(frame() & 7); prof(); prof();
    prof();
}` });
    both(file, ['--ticks', '6']);
});

const DIALOGS = `var said = [];
function init() {}
function update() {
    if (frame() == 2) {
        font(FONT.WORLD);
        printh(ui.choose("Pick", ["Red", "Green", "Blue"]));
        printh(ui.text("Name", 8));
        printh(ui.confirm("Sure?"));
    }
}
function draw() { cls(3); circfill(80, 60, 20 + frame() % 10, 8); }`;

test('dialogs worked a tick at a time, and an error, as pbgvm reports them', () => {
    const file = build('dialogs', { 'cart.json': CART, 'main.js': DIALOGS });
    both(file, ['--ticks', '60', '--wheel', '4:8,5:0,12:6,13:0',
                '--buttons', '7:16,8:0,15:8,16:0,17:16,18:0,22:2,23:0,24:16,25:0']);
    both(build('fault', { 'cart.json': CART, 'main.js':
        'function init() {}\nfunction update() { var a = null; a.b = 1; }' }), ['--ticks', '3']);
});

test('a dialog paused for each frame ends as one answered at once', async () => {
    const cart = new Uint8Array(fs.readFileSync(build('dialogs', { 'cart.json': CART, 'main.js': DIALOGS })));
    const wasm = new WebAssembly.Module(fs.readFileSync(wasmFile));
    /* the same presses: SELECT on the frames given, PLAY on 8, the wheel
     * on others */
    const input = (n) => ({ held: n === 8 ? 8 : [3, 10, 14].includes(n) ? 16 : 0, wheel: n === 1 ? 4 : n === 6 ? 6 : 0 });
    async function play(paused) {
        let n = 0;
        const lines = [], units = [];
        const host = {
            print: (s) => lines.push(s),
            frame: (open) => {
                if (!open) return null;
                const r = input(n++);
                return paused ? new Promise((done) => setTimeout(() => done(r), 1)) : r;
            }
        };
        const e = new Engine(wasm, host);
        assert.ok(e.load(cart));
        assert.equal(await e.start(), true);
        for (let t = 0; t < 6; t++) {
            assert.equal(await e.tick(0, 0, -1), true, e.error());
            units.push(e.units());
        }
        return { lines, units, px: Buffer.from(e.pixels()), frames: n };
    }
    const now = await play(false), later = await play(true);
    assert.deepEqual(now.lines, ['1', 'A', '0']);
    assert.deepEqual(later, now);
});

test('the made-up music, library and beat: the same through both', () => {
    const cart = JSON.stringify({ id: '00000000000a5a51', name: 'Music', music: 'control' });
    const res = compileFiles({ 'cart.json': cart, 'main.js': `var st = new Int32Array(6), md = new Int32Array(3), b = new Int32Array(4), n = 0;
function init() { beat.listen(true); printh(playback.title() + " " + tracks.count({ genre: "Rock" })); }
function update() {
    n++;
    beat.state(st);
    if (n % 10 == 0) {
        beat.mood(md); playback.bands(b, 4);
        printh(n + " " + st[0] + " " + beat.next(beat.clock(), 0) + " " + beat.inbar() + " " +
               playback.elapsed() + " " + md[0] + " " + b[0]);
    }
    if (beat.press(4) >= 0) printh("press " + beat.press(4));
    if (n == 45) playback.next();
    if (playback.changed()) printh("now " + playback.title() + " " + playback.artist());
}` });
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'music.pbg');
    fs.writeFileSync(file, res.cart);
    /* --beat 0: the IDE's, each track at its own tempo */
    both(file, ['--ticks', '80', '--beat', '0', '--buttons', '30:16,31:0']);
    both(file, ['--ticks', '80', '--beat', '90', '--buttons', '30:16,31:0']);
    both(file, ['--ticks', '20', '--playing']);
});

test('the memory view: Engine.memory() as pbgvm --memory reports it', async () => {
    const main = `var big, words, shared;
function init() {
    big = new Int32Array(1000);
    words = [];
    for (var i = 0; i < 50; i++) words.push("word" + i);
    shared = [1, 2, 3];
}
function update() { var junk = [1, 2, 3, 4]; }`;
    const file = build('memory', { 'cart.json': CART, 'main.js': main });
    const vm = spawnSync(exe, [file, '--ticks', '5', '--memory'], { encoding: 'utf8' });
    assert.equal(vm.status, 0, vm.stderr);
    const want = { totals: {}, kinds: [], globals: [] };
    for (const w of lf(vm.stdout).split('\n').map((l) => l.split(' '))) {
        if (w[0] !== 'memory') continue;
        if (w[1] === 'kind') want.kinds.push([+w[3], +w[4]]);
        else if (w[1] === 'global') want.globals.push([+w[2], +w[3]]);
        else for (let i = 1; i < w.length; i += 2) want.totals[w[i]] = +w[i + 1];
    }
    const e = new Engine(new WebAssembly.Module(fs.readFileSync(wasmFile)),
                         { print: () => {}, frame: () => null });
    assert.ok(e.load(new Uint8Array(fs.readFileSync(file))));
    assert.equal(await e.start(), true);
    for (let t = 0; t < 5; t++) assert.equal(await e.tick(0, 0, -1), true, e.error());
    const m = e.memory();
    /* the engine's own tables are a 64-bit build's size in pbgvm, so only
     * what the cart's values take is compared */
    for (const k of ['held', 'garbage', 'constants', 'engine'])
        assert.equal(m[k], want.totals[k], k);
    assert.deepEqual(m.kinds.filter((k) => k.count).map((k) => [k.count, k.bytes]), want.kinds);
    assert.deepEqual([...m.globals].map((b, g) => [g, b]).filter(([, b]) => b), want.globals);
});
