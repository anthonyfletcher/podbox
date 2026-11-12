/* Dialogs: ui.* through pbgvm, whose dialogs print what they showed and
 * answer from --ui, or are worked tick by tick with --buttons and --wheel
 * as on the player. */

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
function run(main, answers, extra = [], ticks = 1) {
    const res = compileFiles({
        'cart.json': JSON.stringify({ id: '00000000000000d1', name: 'Dialogs' }),
        'main.js': main + '\nfunction update() {}' });
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(tmp, 'u' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const args = [file, '--ticks', String(ticks)]
        .concat(answers === undefined ? [] : ['--ui', answers], extra);
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
                 'choose Once more: a|b at 0 -> none\n-1\n');
});

test('confirm and message put a title given on the prompt\'s first line', () => {
    const r = run(`function init() {
    printh(ui.confirm("Start over?"));
    printh(ui.confirm("Sure?", "Reset"));
    ui.message("Well done.");
    ui.message("Level 2", "Next");
}`, '1,0');
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, 'confirm Start over? -> 1\n1\n' +
                 'confirm Reset / Sure? -> 0\n0\n' +
                 'message Well done.\nmessage Next / Level 2\n');
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
                 'number Back: 0 to 5 by 1 from 3 -> none\n3\n');
});

test('text gives the line typed, or null; the start is cut to the most', () => {
    const r = run(`function init() {
    printh(ui.text("Name", 8));
    printh(ui.text("Again", 3, "ABCDEF"));
    printh(ui.text("Gone", 8));
}`, 'ANN,XY,~');
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, 'text Name: [] up to 8 -> ANN\nANN\n' +
                 'text Again: [ABC] up to 3 -> XY\nXY\n' +
                 'text Gone: [] up to 8 -> ~\nnull\n');
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
    assert.match(run('function init() { ui.style(1, 7); }').err ||
                 run('function init() { ui.style(1, 7, 300); }').err,
                 /three colours, 0 to 255/);
});

/* Worked as on the player: the dialog opens in init, each of its frames a
 * tick. Masks: 1 PREV, 2 NEXT, 4 MENU, 16 SELECT; 4 wheel clicks a row,
 * as the player's lists, 6 a character. */
test('choose is worked with the wheel and SELECT', () => {
    const r = run('function init() { printh(ui.choose("Pick", ["a", "b", "c", "d"])); }',
                  undefined, ['--wheel', '1:8:0,2:0', '--buttons', '3:16,4:0'], 10);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, 'choose Pick: a|b|c|d at 0\n2\n');
});

test('confirm is No until moved to Yes; MENU backs out', () => {
    const yes = run('function init() { printh(ui.confirm("Sure?")); }',
                    undefined, ['--buttons', '1:1,2:0,3:16,4:0'], 10);
    assert.equal(yes.out, 'confirm Sure?\n1\n');
    const no = run('function init() { printh(ui.confirm("Sure?")); }',
                   undefined, ['--buttons', '1:16,2:0'], 10);
    assert.equal(no.out, 'confirm Sure?\n0\n');
    const menu = run('function init() { printh(ui.number("N", 0, 9, 4)); }',
                     undefined, ['--buttons', '1:2,2:0,3:4,4:0'], 10);
    assert.equal(menu.out, 'number N: 0 to 9 by 1 from 4\n4\n');
});

test('a buttons already held when it opens count once let go', () => {
    /* SELECT down from before: not an answer until pressed again */
    const r = run('function init() { printh(ui.confirm("Sure?")); }',
                  undefined, ['--buttons', '0:16,3:0,4:1,5:0,6:16,7:0'], 12);
    assert.equal(r.out, 'confirm Sure?\n1\n');
});

test('the press that answers a dialog does not reach the game', () => {
    /* PLAY opens it at tick 1; SELECT answers it at 4 and is held through
     * the game's ticks after, where it would otherwise be a fresh press */
    const res = compileFiles({
        'cart.json': JSON.stringify({ id: '00000000000000d3', name: 'Press' }),
        'main.js': `function init() {}
function update() {
    if (btnp(3)) ui.confirm("Sure?");
    if (btnp(4)) printh("select");
}` });
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'press.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '40', '--buttons', '1:8,2:0,4:16,30:0,32:16,33:0'],
                        { encoding: 'utf8' });
    /* the press at 32, after letting go, is the game's */
    assert.equal(r.stdout.replace(/\r/g, ''), 'confirm Sure?\nselect\n');
});

test('text: the wheel puts a letter in and turns it, Right keeps it, SELECT takes it', () => {
    /* 6 clicks: A; Right; 12 clicks: B; SELECT */
    const r = run('function init() { printh(ui.text("Name", 8)); }', undefined,
                  ['--wheel', '1:6:0,2:0,4:12,5:0',
                   '--buttons', '2:2,3:0,6:16,7:0'], 14);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, 'text Name: [] up to 8\nAB\n');
    /* PLAY goes down to OK and Cancel, MENU back up, PLAY, SELECT on OK */
    const ok = run('function init() { printh(ui.text("Name", 8, "XY")); }', undefined,
                   ['--buttons', '1:8,2:0,3:4,4:0,5:8,6:0,7:16,8:0'], 14);
    assert.equal(ok.out, 'text Name: [XY] up to 8\nXY\n');
    /* Left held half a second deletes; Cancel gives null */
    const del = run('function init() { printh(ui.text("Name", 8, "XYZ")); }', undefined,
                    ['--buttons', '1:1,17:0,18:8,19:0,20:2,21:0,22:16,23:0'], 30);
    assert.equal(del.out, 'text Name: [XYZ] up to 8\nnull\n');
    const kept = run('function init() { printh(ui.text("Name", 8, "XYZ")); }', undefined,
                     ['--buttons', '1:1,17:0,18:16,19:0'], 30);
    assert.equal(kept.out, 'text Name: [XYZ] up to 8\nXY\n');
});

test('the game\'s pixels and drawing state come back after', () => {
    const r = run(`function init() {
    cls(8);
    camera(10, 0);
    ui.message("Hello there");
    var same = 0;
    for (var y = 0; y < 120; y++) for (var x = 0; x < 160; x++) if (pget(x + 10, y) == 8) same++;
    pset(10, 0, 7);
    camera();
    printh(same + " " + pget(0, 0));
}`, '');
    assert.equal(r.out, 'message Hello there\n19200 7\n');
});

test('ticks running out back out of it, and --shot shows it', () => {
    const shot = path.join(tmp, 'dialog.png');
    const r = run('function init() { cls(3); printh(ui.choose("Pick one", ["Red", "Green"])); }',
                  undefined, ['--shot', shot], 5);
    assert.equal(r.out, 'choose Pick one: Red|Green at 0\n-1\n');
    assert.ok(fs.statSync(shot).size > 0);
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
    assert.equal(r.stdout.replace(/\r/g, ''), 'text Your name: [] up to 15 -> ZOE\nZOE\n');
});
