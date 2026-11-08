/* Data files (data.read) and a cart's stored files (store.*), through
 * pbgvm; JSON both ways. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-data-'));

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

function compile(main, assets) {
    return compileFiles({
        'cart.json': JSON.stringify({ id: '000000000000da7a', name: 'Data' }),
        'main.js': main + '\nfunction update() {}' }, assets);
}

let n = 0;
function run(main, assets, extra = []) {
    const res = compile(main, assets);
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(tmp, 'd' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '1'].concat(extra),
                        { encoding: 'utf8', timeout: 20000 });
    return { errors: [], out: r.stdout.replace(/\r/g, ''), err: r.stderr, status: r.status };
}

const LEVELS = JSON.stringify({
    name: 'Caves é "deep"\n',
    waves: [[1, 2, 3], [], [{ kind: 'bat', speed: 1.25, hp: -40 }]],
    big: 2147483647, small: -2147483648, tiny: 0.0625, half: -0.5,
    on: true, off: false, none: null, sci: 1.5e3
});

test('a data file reads as the cart\'s values, a text file as a string', () => {
    const r = run(`function init() {
    var d = data.read(DATA.levels);
    printh(d.name);
    printh(d.waves.length + " " + d.waves[0].join(",") + " " + d.waves[1].length + " " + d.waves[2][0].kind + " " + d.waves[2][0].speed + " " + d.waves[2][0].hp);
    printh(d.big + " " + d.small + " " + d.tiny + " " + d.half + " " + d.on + " " + d.off + " " + d.none + " " + d.sci);
    printh(data.read(DATA.story));
}`, { data: { levels: LEVELS }, texts: { story: 'Once, a cave.\nThe end.' } });
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, 'Caves é "deep"\n\n3 1,2,3 0 bat 1.25 -40\n' +
                 '2147483647 -2147483648 0.0625 -0.5 1 0 null 1500\nOnce, a cave.\nThe end.\n');
});

test('a data file\'s number is a fraction or an integer as written', () => {
    const r = run(`function init() {
    var d = data.read(DATA.n);
    printh(d.a / 4 + " " + d.b / 4 + " " + d.c / 4 + " " + d.d / 4);
}`, { data: { n: '{ "a": 2.0, "b": 2, "c": 1.50e1, "d": 1.5e1 }' } });
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, '0.5 0 3.75 3\n');
});

test('data files are checked when the cart is built', () => {
    const errs = (assets) => compile('function init() {}', assets).errors.map((e) => e.file + ': ' + e.message).join('\n');
    assert.match(errs({ data: { a: '{"x": 1e12}' } }), /data\/a\.json: 1000000000000 is beyond the whole numbers/);
    assert.match(errs({ data: { a: '[40000.5]' } }), /beyond the fractions a cart has/);
    assert.match(errs({ data: { a: '[40000.0]' } }), /beyond the fractions a cart has/);
    assert.equal(errs({ data: { a: '[40000, 4e4, "40000.0", 0.0e9]' } }), '');
    assert.match(errs({ data: { a: '{"x": ' } }), /data\/a\.json: /);
    assert.match(errs({ data: { a: '1' }, texts: { a: 'x' } }), /both called a/);
    assert.match(errs({ data: { 'a b': '1' } }), /usable as a name in code/);
    assert.match(compile('function init() { data.read(DATA.nope); }', {}).errors[0].message, /nope/);
});

test('store keeps values as JSON, and lists, reads back and removes them', () => {
    const dir = fs.mkdtempSync(path.join(tmp, 'store-'));
    const r = run(`function init() {
    printh(store.read("slot1") + " " + store.list().length);
    var v = { level: 3, pos: [1.5, -0.25, 100.75], name: "Zoë\\t", items: [{ a: 1 }, null, "x"], flags: new Int32Array(3) };
    v.flags[1] = -7;
    printh(store.write("slot1", v) + " " + store.write("b-2", "just text") + " " + store.write("a_1", 12));
    printh(store.list().join(","));
    var w = store.read("slot1");
    printh(w.level + " " + w.pos.join(",") + " " + w.name + " " + w.items[0].a + " " + w.items[1] + " " + w.items[2] + " " + w.flags.join(","));
    printh(store.read("b-2") + " " + store.read("a_1") + " " + store.remove("a_1") + " " + store.remove("a_1") + " " + store.list().join(","));
}`, {}, ['--store', dir]);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, 'null 0\n1 1 1\na_1,b-2,slot1\n' +
                 '3 1.5,-0.25,100.75 Zoë\t 1 null x 0,-7,0\n' +
                 'just text 12 1 0 b-2,slot1\n');
    assert.equal(fs.readFileSync(path.join(dir, 'slot1.json'), 'utf8'),
                 '{"level":3,"pos":[1.5,-0.25,100.75],"name":"Zoë\\t",' +
                 '"items":[{"a":1},null,"x"],"flags":[0,-7,0]}');
});

test('a fraction written and read back keeps every bit', () => {
    const dir = fs.mkdtempSync(path.join(tmp, 'bits-'));
    const r = run(`function init() {
    var a = [], bad = 0;
    for (var i = 0; i < 2000; i++) a.push(rnd(65536) - 32768);
    store.write("f", a);
    var b = store.read("f");
    for (var i = 0; i < 2000; i++) if (b[i] != a[i]) bad++;
    printh(bad);
}`, {}, ['--store', dir]);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, '0\n');
});

test('store\'s limits: names, what JSON can hold, size and count', () => {
    const dir = fs.mkdtempSync(path.join(tmp, 'limits-'));
    assert.match(run('function init() { store.write("a/b", 1); }', {}, ['--store', dir]).err,
                 /store\.write\(\) needs a name of 1 to 32 letters/);
    assert.match(run('function init() { store.write("f", [init]); }', {}, ['--store', dir]).err,
                 /store\.write\(f\): cannot keep a function/);
    const r = run(`function init() {
    var s = "x"; while (s.length < 40000) s = s + s;
    printh(store.write("big", s) + " " + store.write("ok1", s.substring(0, 60000)) + " " + store.write("ok2", s.substring(0, 60000)));
    var n = 0;
    for (var i = 0; i < 70; i++) if (store.write("n" + i, i)) n++;
    printh(n + " " + store.list().length);
}`, {}, ['--store', dir]);
    assert.equal(r.status, 0, r.err);
    /* over 64 KB as JSON; then within the area's 256 KB, two of 60 KB; and
     * 64 files in all */
    assert.equal(r.out, '0 1 1\n62 64\n');
    fs.writeFileSync(path.join(dir, 'broken.json'), '{"a": [1, 2');
    assert.match(run('function init() { store.read("broken"); }', {}, ['--store', dir]).err,
                 /store broken: the text ends early at byte 11/);
});

test('random nested JSON reads in and writes back the same', () => {
    let seed = 12345;
    const rand = (k) => { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; return seed % k; };
    function make(depth) {
        const r = rand(depth > 5 ? 4 : 7);
        if (r === 0) return rand(2000000) - 1000000;
        if (r === 1) return 'k' + rand(50) + (rand(4) ? '' : ' "q" é\n');
        if (r === 2) return null;
        if (r === 3) return rand(2) ? [] : {};
        if (r < 5) { const a = []; for (let i = rand(6); i > 0; i--) a.push(make(depth + 1)); return a; }
        const o = {}; for (let i = rand(6); i > 0; i--) o['f' + rand(40)] = make(depth + 1); return o;
    }
    for (let t = 0; t < 12; t++) {
        const v = make(0);
        const dir = fs.mkdtempSync(path.join(tmp, 'rt-'));
        const r = run('function init() { store.write("v", data.read(DATA.v)); }',
                      { data: { v: JSON.stringify(v) } }, ['--store', dir]);
        assert.equal(r.status, 0, r.err);
        assert.equal(fs.readFileSync(path.join(dir, 'v.json'), 'utf8'), JSON.stringify(v), 'case ' + t);
    }
});
