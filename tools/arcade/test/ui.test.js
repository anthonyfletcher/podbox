/* Dialogs: ui.* through pbgvm, whose dialogs print what they showed and
 * answer from --ui. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-ui-'));

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

let n = 0;
function run(main, answers) {
    const res = compileFiles({
        'cart.json': JSON.stringify({ id: '00000000000000d1', name: 'Dialogs' }),
        'main.js': main + '\nfunction update() {}' });
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(tmp, 'u' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const args = [file, '--ticks', '1'].concat(answers === undefined ? [] : ['--ui', answers]);
    const r = spawnSync(exe, args, { encoding: 'utf8', timeout: 20000 });
    return { errors: [], out: r.stdout.replace(/\r/g, ''), err: r.stderr, status: r.status };
}

test('choose shows the items and gives the index, or -1', () => {
    const r = run(`function init() {
    printh(ui.choose("Pick", ["Red", "Green", 7]));
    printh(ui.choose("Again", ["a", "b"], 1));
    printh(ui.choose("Once more", ["a", "b"], 9));
}`, '2,-1');
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, 'choose Pick: Red|Green|7 at 0 -> 2\n2\n' +
                 'choose Again: a|b at 1 -> -1\n-1\n' +
                 'choose Once more: a|b at 0 -> -1\n-1\n');
});

test('confirm and message take the cart\'s name as their title unless given one', () => {
    const r = run(`function init() {
    printh(ui.confirm("Start over?"));
    printh(ui.confirm("Sure?", "Reset"));
    ui.message("Well done.");
    ui.message("Level 2", "Next");
}`, '1,0');
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, 'confirm Dialogs: Start over? -> 1\n1\n' +
                 'confirm Reset: Sure? -> 0\n0\n' +
                 'message Dialogs: Well done.\nmessage Next: Level 2\n');
});

test('number starts within range and gives what is chosen, kept within it', () => {
    const r = run(`function init() {
    printh(ui.number("Lives", 1, 9, 12));
    printh(ui.number("Speed", 0, 100, 50, 10));
    printh(ui.number("Back", 0, 5, 3));
}`, '4,200');
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, 'number Lives: 1 to 9 by 1 from 9 -> 4\n4\n' +
                 'number Speed: 0 to 100 by 10 from 50 -> 200\n100\n' +
                 'number Back: 0 to 5 by 1 from 3 -> 3\n3\n');
});

test('text gives the line typed, or null; the start is cut to the most', () => {
    const r = run(`function init() {
    printh(ui.text("Name", 8));
    printh(ui.text("Again", 3, "ABCDEF"));
    printh(ui.text("Gone", 8));
}`, 'ANN,XY,~');
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, 'text Name: [] up to 8 -> [ANN]\nANN\n' +
                 'text Again: [ABC] up to 3 -> [XY]\nXY\n' +
                 'text Gone: [] up to 8 -> cancelled\nnull\n');
});

test('wrong arguments stop the cart with what was wanted', () => {
    assert.match(run('function init() { ui.choose("x", 3); }').err,
                 /ui\.choose\(\) needs a list of items/);
    assert.match(run('function init() { ui.choose("x", []); }').err,
                 /ui\.choose\(\) takes 1 to 32 items/);
    assert.match(run('function init() { ui.number("x", 5, 5, 5); }').err,
                 /the least below the most/);
    assert.match(run('function init() { ui.text("x", 65); }').err,
                 /1 to 64 bytes/);
});

test('a high score named with ui.text', () => {
    const res = compileFiles({
        'cart.json': JSON.stringify({ id: '00000000000000d2', name: 'Named',
                                      boards: [{ name: 'best' }] }),
        'main.js': `function init() {
    var name = ui.text("Your name", 15);
    score.submit(BOARD.best, 120, name);
    printh(score.name(BOARD.best, 1));
}
function update() {}` });
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'named.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '1', '--ui', 'ZOE'], { encoding: 'utf8' });
    assert.equal(r.stdout.replace(/\r/g, ''), 'text Your name: [] up to 15 -> [ZOE]\nZOE\n');
});
