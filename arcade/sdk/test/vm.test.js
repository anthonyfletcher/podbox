/* Differential tests: each program runs in the engine's VM (host/pbgvm,
 * built from apps-ipod/games/arcade) and in the Node reference
 * (reference.js), and the two must print the same lines -- and, when one
 * stops with an error, so must the other, at the same point. */

import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { compileFiles } from '../src/node.js';
import { runReference } from './reference.js';
import { readCart, writeCart } from '../src/cart.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const host = path.join(here, '..', 'host');
const exe = path.join(host, process.platform === 'win32' ? 'pbgvm.exe' : 'pbgvm');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-'));
const CART = JSON.stringify({ id: '0123456789abcdef', name: 'Test' });

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

function runVm(src, ticks, name) {
    const res = compileFiles({ 'cart.json': CART, 'main.js': src });
    assert.deepEqual(res.errors, [], 'compile errors in ' + name);
    const file = path.join(tmp, name + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', String(ticks)],
                        { encoding: 'utf8', timeout: 30000 });
    assert.equal(r.signal, null, name + ' timed out');
    const out = r.stdout.replace(/\r/g, '').split('\n');
    out.pop();
    return { out: out, error: r.status === 0 ? null : r.stderr.trim() };
}

function same(src, ticks, name) {
    const ref = runReference(src, ticks);
    const got = runVm(src, ticks, name);
    const n = Math.max(ref.out.length, got.out.length);
    for (let i = 0; i < n; i++)
        assert.equal(got.out[i], ref.out[i], name + ': line ' + (i + 1) + ' differs');
    assert.equal(!!got.error, !!ref.error,
                 name + ': VM error ' + got.error + ', reference error ' + ref.error);
    return got;
}

for (const f of fs.readdirSync(path.join(here, 'lang'))) {
    test('same as the reference: ' + f, () => {
        const got = same(fs.readFileSync(path.join(here, 'lang', f), 'utf8'), 60, f);
        assert.equal(got.error, null);
    });
}

test('runtime errors name the file, line and function', () => {
    const r = runVm('var a = [1];\nfunction init() {\n  var z = 0;\n' +
                    '  printh(a[0] / z);\n}\nfunction update() {}', 0, 'err');
    assert.match(r.error, /^main\.js:4: division by zero \(in init\)$/);
    const s = runVm('function init() {}\nfunction update() {\n  var o = null;\n' +
                    '  o.x = 1;\n}', 1, 'err2');
    assert.match(s.error, /^main\.js:4: can't set \.x of null \(in update\)$/);
});

test('a property of a number stops the cart, in the reference as in the VM', () => {
    /* through a parameter, so the compiler cannot see it is a number */
    const got = same('function rx(o) { return o.x; }\nfunction init() {\n  printh("before");\n' +
                     '  printh(rx(3));\n}\nfunction update() {}', 0, 'propnum');
    assert.equal(got.out[0], 'before');
    assert.ok(got.error, 'the VM stops');
});

test('deep recursion stops cleanly', () => {
    const r = runVm('function f(n) { return f(n + 1) + 1; }\n' +
                    'function init() { f(0); }\nfunction update() {}', 0, 'deep');
    assert.match(r.error, /too much recursion/);
});

test('a runaway loop is stopped', () => {
    const file = path.join(tmp, 'spin.pbg');
    const res = compileFiles({ 'cart.json': CART, 'main.js':
        'function init() {}\nfunction update() { while (true) {} }' });
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '1', '--ceiling', '100000'],
                        { encoding: 'utf8', timeout: 20000 });
    assert.match(r.stderr, /stopped responding/);
});

test('running out of memory is an error, not a crash', () => {
    const r = runVm('var keep = [];\nfunction init() {\n' +
                    '  while (true) keep.push(new Int32Array(1000));\n}\n' +
                    'function update() {}', 0, 'oom');
    assert.match(r.error, /out of memory/);
});

/* ---- random expressions ------------------------------------------------ */

function rng(seed) {
    let s = seed >>> 0;
    return (n) => {
        s = (Math.imul(s, 1103515245) + 12345) >>> 0;
        return (s >>> 8) % n;
    };
}

const EDGES = ['0', '1', '-1', '2', '7', '31', '32', '255', '65536', '0x7fffffff',
               '-0x80000000', '0x55555555', '1000003', '0.5', '-1.25', '3.0',
               '0.0001', '32767.5', '-32768.0', '100.125'];
const BIN = ['+', '-', '*', '/', '%', '&', '|', '^', '<<', '>>', '>>>',
             '<', '<=', '>', '>=', '==', '!=', '&&', '||'];

function expr(r, depth) {
    if (depth <= 0 || r(4) === 0) {
        const k = r(3);
        return k === 0 ? EDGES[r(EDGES.length)] : ['a', 'b', 'c'][r(3)];
    }
    switch (r(8)) {
    case 0: return '-(' + expr(r, depth - 1) + ')';
    case 1: return '~(' + expr(r, depth - 1) + ')';
    case 2: return '!(' + expr(r, depth - 1) + ')';
    case 3: return '(' + expr(r, depth - 1) + ' ? ' + expr(r, depth - 1) + ' : ' +
        expr(r, depth - 1) + ')';
    default: {
        const op = BIN[r(BIN.length)];
        const right = op === '/' || op === '%'
            ? '((' + expr(r, depth - 1) + ') | 1)' : expr(r, depth - 1);
        return '(' + expr(r, depth - 1) + ' ' + op + ' ' + right + ')';
    }
    }
}

for (let p = 0; p < 12; p++) {
    test('random expressions, program ' + p, () => {
        const r = rng(1000 + p);
        const lines = [];
        for (let i = 0; i < 40; i++)
            lines.push('    printh("' + i + ' " + (' + expr(r, 5) + '));');
        const src = 'var vals = [' + EDGES.join(', ') + '];\n' +
            'function init() {\n  for (var i = 0; i < vals.length; i += 3) {\n' +
            '    var a = vals[i], b = vals[(i * 7 + 1) % vals.length], ' +
            'c = vals[(i * 5 + 2) % vals.length];\n' + lines.join('\n') +
            '\n  }\n}\nfunction update() {}\n';
        same(src, 0, 'fuzz' + p);
    });
}

test('mutated carts are refused or run, never crash or hang', () => {
    const src = fs.readFileSync(path.join(here, 'lang', 'data.js'), 'utf8') +
        fs.readFileSync(path.join(here, 'lang', 'control.js'), 'utf8')
            .replace(/function (init|update)\(/g, 'function $1_c(');
    const res = compileFiles({ 'cart.json': CART, 'main.js': src });
    assert.deepEqual(res.errors, []);
    const { chunks, order } = readCart(res.cart);
    const r = rng(77);
    const file = path.join(tmp, 'mutant.pbg');
    let refused = 0, ran = 0;
    for (let i = 0; i < 300; i++) {
        const tag = ['CODE', 'CODE', 'CODE', 'CNST', 'GLOB'][r(5)];
        const data = chunks[tag].slice();
        for (let k = 1 + r(3); k > 0; k--)
            data[r(data.length)] = r(256);
        fs.writeFileSync(file, writeCart(order.map((t) =>
            ({ tag: t, data: t === tag ? data : chunks[t] }))));
        const out = spawnSync(exe, [file, '--ticks', '3', '--ceiling', '3000000'],
                              { encoding: 'utf8', timeout: 20000 });
        assert.equal(out.signal, null, 'mutant ' + i + ' hung or was killed');
        assert.ok(out.status === 0 || out.status === 1,
                  'mutant ' + i + ' crashed with status ' + out.status);
        if (/damaged|newer PodBox|not a game/.test(out.stderr)) refused++;
        else ran++;
    }
    assert.ok(refused > 0 && ran > 0, 'refused ' + refused + ', ran ' + ran);
});

test('pause-menu items call back, relabel and keep the menu open', () => {
    const src = 'var sound = 1, resets = 0;\n' +
        'function toggle(b) { sound = 1 - sound; ' +
        'menuitem(1, "Sound: " + (sound ? "on" : "off"), toggle); return true; }\n' +
        'function reset(b) { resets++; printh("reset by " + b); }\n' +
        'function boom() { return 1 / 0; }\n' +
        'function init() { menuitem(1, "Sound: on", toggle); menuitem(2, "Reset", reset); ' +
        'menuitem(3, "Bad", boom); menuitem(5, "Gone"); menuitem(5); }\n' +
        'function update() {}';
    const res = compileFiles({ 'cart.json': CART, 'main.js': src });
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'menu.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--menu', '1', '--menu', '2', '--menu', '4', '--menu', '3'],
                        { encoding: 'utf8', timeout: 20000 });
    const out = r.stdout.replace(/\r/g, '');
    assert.match(out, /^menu: 1=Sound: on 2=Reset 3=Bad\nchose 1: 1\n/);
    assert.match(out, /menu: 1=Sound: off 2=Reset 3=Bad\nreset by 4\nchose 2: 0\n/);
    assert.match(out, /chose 4: 0\n/);
    assert.match(out, /chose 3: -1/);
    assert.match(r.stderr, /division by zero \(in boom\)/);
});
