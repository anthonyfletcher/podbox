/* High scores: the boards cart.json declares, score.*, and the table the
 * host keeps between runs, through pbgvm. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-score-'));

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

const BOARDS = [
    { name: 'points', title: 'Points' },
    { name: 'lap', title: 'Best Lap', wins: 'low', format: 'time' },
    { name: 'run', wins: 'low', format: 'ticks' }
];

function compile(main, boards = BOARDS) {
    return compileFiles({
        'cart.json': JSON.stringify({ id: '000000000005c0e5', name: 'Scores',
                                      boards: boards }),
        'main.js': main });
}

let n = 0;
function run(main, opts = {}) {
    const res = compile(main, opts.boards);
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(tmp, 's' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const extra = opts.scores ? ['--scores', opts.scores] : [];
    const r = spawnSync(exe, [file, '--ticks', String(opts.ticks || 1)].concat(extra),
                        { encoding: 'utf8', timeout: 20000 });
    return { errors: [], out: r.stdout.replace(/\r/g, ''), err: r.stderr, status: r.status };
}

test('boards are checked in cart.json', () => {
    const bad = (boards) => compile('function update() {}', boards)
        .errors.map((e) => e.message).join('\n');
    assert.match(bad({}), /boards must be a list/);
    assert.match(bad([{ name: 'a b' }]), /name must be usable in code/);
    assert.match(bad([{ name: 'a' }, { name: 'a' }]), /a second board called a/);
    assert.match(bad([{ name: 'a', wins: 'most' }]), /wins must be "high" or "low"/);
    assert.match(bad([{ name: 'a', format: 'money' }]), /format must be one of number, time, ticks/);
    assert.match(bad([{ name: 'a', version: 0 }]), /version must be a whole number/);
    assert.match(bad([{ name: 'a', title: 'x'.repeat(24) }]), /title must be text of 1 to 23 bytes/);
    assert.match(bad([{ name: 'a', colour: 1 }]), /unknown setting colour/);
    assert.match(bad(Array.from({ length: 9 }, (_, i) => ({ name: 'b' + i }))),
                 /at most 8 boards/);
    assert.match(compile('function update() { score.best(BOARD.nope); }')
                 .errors[0].message, /nope/);
});

test('submit ranks a value among the ten, ties after the earlier one', () => {
    const r = run(`function init() {
    var b = BOARD.points;
    printh(score.count(b) + " " + score.best(b) + " " + score.get(b, 1));
    printh(score.submit(b, 5) + "," + score.submit(b, 9) + "," + score.submit(b, 1) + "," + score.submit(b, 9));
    printh(score.best(b) + " " + score.count(b) + " " + score.get(b, 2) + " " + score.get(b, 4) + " " + score.get(b, 5) + " " + score.get(b, 0));
    for (var i = 0; i < 6; i++) score.submit(b, 10 + i);
    printh(score.count(b) + " " + score.submit(b, 0) + " " + score.submit(b, 100) + " " + score.get(b, 10));
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, '0 null null\n1,1,3,2\n9 4 9 1 null null\n10 0 1 5\n');
});

test('low wins on a low board; names are kept, cut to 15 bytes', () => {
    const r = run(`function init() {
    var b = BOARD.lap;
    printh(score.submit(b, 3000, "ANN") + " " + score.submit(b, 2000) + " " + score.submit(b, 2500, "A very long name indeed"));
    printh(score.get(b, 1) + " [" + score.name(b, 1) + "] [" + score.name(b, 3) + "] [" + score.name(b, 2) + "] " + score.name(b, 9));
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, '1 1 2\n2000 [] [ANN] [A very long nam] null\n');
});

test('a name cut at 15 bytes loses a character split by the cut', () => {
    const r = run(`function init() {
    score.submit(BOARD.points, 1, "ABCDEFGHIJKLMNé");
    score.submit(BOARD.points, 0, "ABCDEFGHIJKLMé");
    printh("[" + score.name(BOARD.points, 1) + "] [" + score.name(BOARD.points, 2) + "]");
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, '[ABCDEFGHIJKLMN] [ABCDEFGHIJKLMé]\n');
});

test('show prints the board as the engine shows it, the new score marked', () => {
    const r = run(`function init() {
    score.submit(BOARD.lap, 83450);
    score.submit(BOARD.lap, 3723456);
    score.submit(BOARD.lap, 61000);
    score.show(BOARD.lap);
    score.submit(BOARD.run, 66);
    score.submit(BOARD.points, 1234567);
    score.show(BOARD.run);
}
function update() {}`);
    assert.equal(r.status, 0, r.err);
    /* the second show replaces the first */
    assert.equal(r.out, 'show run\n*1 0:02.00  20261103\n');
    const s = run(`function init() {}
function update() {
    score.submit(BOARD.lap, 83450);
    score.submit(BOARD.lap, 3723456);
    score.submit(BOARD.lap, 61000);
    score.show(BOARD.lap);
}`);
    assert.equal(s.out, 'show Best Lap\n*1 1:01.00  20261103\n2 1:23.45  20261103\n' +
                 '3 1:02:03.45  20261103\n');
});

test('an unknown board stops the cart', () => {
    const r = run('function init() { score.submit(7, 1); }\nfunction update() {}');
    assert.equal(r.status, 1);
    assert.match(r.err, /score\.submit\(\) needs one of the cart's boards/);
});

test('the host keeps the tables between runs, matched by name and version', () => {
    const file = path.join(tmp, 'scores.dat');
    const main = `function init() { printh(score.best(BOARD.points) + " " + score.best(BOARD.lap)); score.submit(BOARD.points, 40); score.submit(BOARD.lap, 900); }
function update() {}`;
    assert.equal(run(main, { scores: file }).out, 'null null\n');
    assert.equal(run(main, { scores: file }).out, '40 900\n');
    /* the boards in another order: each still finds its own table */
    const swapped = [BOARDS[1], BOARDS[0], BOARDS[2]];
    assert.equal(run(main, { scores: file, boards: swapped }).out, '40 900\n');
    /* a new version of lap starts it afresh; points keeps its table */
    const bumped = [BOARDS[0], Object.assign({}, BOARDS[1], { version: 2 }), BOARDS[2]];
    assert.equal(run(main, { scores: file, boards: bumped }).out, '40 null\n');
    /* a damaged file is ignored rather than trusted */
    fs.writeFileSync(file, Buffer.from('PBSC\x01\x21\x09'));
    assert.equal(run(main, { scores: file }).out, 'null null\n');
});

const ACHS = [
    { name: 'first', title: 'First Steps', text: 'Start a game.' },
    { name: 'ten', title: 'Ten in a Row' }
];

function runAch(main, scores, achs = ACHS) {
    const res = compileFiles({
        'cart.json': JSON.stringify({ id: '00000000000ac4e5', name: 'Achieve',
                                      achievements: achs }),
        'main.js': main });
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'ach' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '1', '--scores', scores], { encoding: 'utf8' });
    assert.equal(r.status, 0, r.stderr);
    return r.stdout.replace(/\r/g, '');
}

test('achievements are checked in cart.json', () => {
    const bad = (achs) => compileFiles({
        'cart.json': JSON.stringify({ id: '00000000000ac4e5', name: 'A', achievements: achs }),
        'main.js': 'function update() {}' }).errors.map((e) => e.message).join('\n');
    assert.match(bad({}), /achievements must be a list/);
    assert.match(bad([{ name: 'a b' }]), /usable in code as ACH/);
    assert.match(bad([{ name: 'a' }, { name: 'a' }]), /a second achievement called a/);
    assert.match(bad([{ name: 'a', title: 'x'.repeat(32) }]), /1 to 31 bytes/);
    assert.match(bad([{ name: 'a', text: 'x'.repeat(96) }]), /at most 95 bytes/);
    assert.match(bad(Array.from({ length: 33 }, (_, i) => ({ name: 'a' + i }))), /at most 32/);
});

test('earning one is announced once and kept between runs', () => {
    const file = path.join(tmp, 'ach.sco');
    const main = `function init() {
    printh(ach.has(ACH.first) + " " + ach.has(ACH.ten));
    printh(ach.earn(ACH.first) + " " + ach.earn(ACH.first));
}
function update() {}`;
    assert.equal(runAch(main, file), '0 0\n1 0\nachieved First Steps\n');
    assert.equal(runAch(main, file), '1 0\n0 0\n');
    /* reordered, a new one added: each finds its own */
    const more = [{ name: 'new', title: 'New' }, ACHS[1], ACHS[0]];
    assert.equal(runAch(main, file, more), '1 0\n0 0\n');
});

test('a scores file from before achievements still reads', () => {
    /* version 1: no achievements after the boards */
    const file = path.join(tmp, 'v1.sco');
    const id = Buffer.from('00000000000ac4e5', 'hex');
    const v1 = Buffer.concat([Buffer.from('PBSC'), Buffer.from([1, 33, 0, 0]), id,
                              Buffer.from([7, 0, 0, 0, 0, 0, 0, 0, 1, 65])]);
    fs.writeFileSync(file, v1);
    assert.equal(runAch(`function init() { printh(ach.has(ACH.first)); ach.earn(ACH.ten); }
function update() {}`, file), '0\nachieved Ten in a Row\n');
    const v2 = fs.readFileSync(file);
    assert.equal(v2[4], 2);
    assert.equal(v2.readUInt32LE(16), 7);   /* v1's plays, carried over */
});
