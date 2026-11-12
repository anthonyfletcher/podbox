/* The script libraries in lib/, compiled into a cart beside main.js and
 * run through pbgvm. */

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
const lib = path.join(here, '..', 'lib');
const exe = path.join(host, process.platform === 'win32' ? 'pbgvm.exe' : 'pbgvm');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-lib-'));
const CART = JSON.stringify({ id: '0000000000011b00', name: 'Libraries' });

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

let n = 0;
function run(libs, main, ticks = 1, extra = []) {
    const files = { 'cart.json': CART, 'main.js': main };
    for (const l of libs) files[l + '.js'] = fs.readFileSync(path.join(lib, l + '.js'), 'utf8');
    const res = compileFiles(files);
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'l' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', String(ticks)].concat(extra),
                        { encoding: 'utf8', timeout: 20000 });
    assert.equal(r.status, 0, r.stderr);
    return r.stdout.replace(/\r/g, '').trim().split('\n');
}

test('every library compiles alone and all together', () => {
    for (const l of ['dial', 'tween', 'text', 'palfx', 'gui', 'fx', 'rand'])
        run([l], 'function init() {}\nfunction update() {}');
    run(['dial', 'menu'], 'function init() {}\nfunction update() {}');
    run(['dial', 'tween', 'text', 'palfx', 'menu', 'fx', 'rand'], 'function init() {}\nfunction update() {}');
});

test('text: a whole-seconds clock, a time board\'s ms, a shadow, centring on the screen', () => {
    const out = run(['text'], `function init() {
    printh(text.clock(83.5, true) + " " + text.clock(83.5) + " " + text.lap(83456));
    cls(0); text.shadow("A", 10, 10, 7, 1);
    printh(pget(10, 10) + " " + pget(13, 14));
    cls(0); text.center("AB", null, 0, 7);
    printh(pget(78, 0));
}
function update() {}`);
    /* CAPS's A starts with ink at its top left; its shadow ends a pixel
     * further right and down; "AB" is 7 wide, centred from 77 */
    assert.equal(out[0], '1:23 1:23.5 1:23.45');
    assert.equal(out[1], '7 1');
    assert.equal(out[2], '7');
});

test('fx: a shake for its ticks, a hit-stop, and timers coming due', () => {
    const out = run(['fx'], `function init() { fx.shake(2, 2); fx.stop(1); fx.after(3, 7); fx.after(3, 8); fx.after(1, 9); }
function update() {
    fx.step();
    var due = "";
    for (var id = fx.due(); id >= 0; id = fx.due()) due += id;
    printh(frame() + " " + fx.stopped() + " " + (abs(fx.x) <= 2) + " " + due);
}`, 5);
    /* tick 1 stands still and counts nothing down; then 9 is due, then
     * 7 and 8 together, the shake over by tick 4 */
    assert.deepEqual(out, ['1 1 1 ', '2 0 1 9', '3 0 1 ', '4 0 1 87', '5 0 1']);
});

test('rand: int within its bounds, a shuffle that keeps every element, weights and a generator', () => {
    const out = run(['rand'], `var a = [1, 2, 3, 4, 5], g = new Int32Array(2), h = new Int32Array(2);
function init() {
    var lo = 99, hi = -99, picks = [0, 0];
    for (var i = 0; i < 400; i++) { var v = rand.int(3, 6); lo = min(lo, v); hi = max(hi, v); }
    for (var k = 0; k < 600; k++) picks[rand.weighted([5, 1])]++;
    rand.shuffle(a);
    var sum = 0;
    for (var j = 0; j < 5; j++) sum += a[j];
    rng.seed(g, 3); rng.seed(h, 3);
    printh(lo + " " + hi + " " + sum + " " + (picks[0] > picks[1] * 3) + " " + (rand.int(1, 1000, g) == rand.int(1, 1000, h)));
}
function update() {}`);
    assert.equal(out[0], '3 6 15 1 1');
});

test('dial: detents with the rest carried, the angle turned, the finger on an arc', () => {
    const out = run(['dial'], `function init() {}
function update() {
    var s = dial.steps(8);
    printh(s + " " + dial.angle() + " " + dial.paddle(0, 100, 72, 24));
}`, 5, ['--wheel', '0:5:72,1:5,2:5,3:-20:0,4:0:50']);
    /* eight detents a turn are twelve clicks each: 5, 10, then 15 makes
     * one with 3 over; -20 takes 3 - 20 = -17, one back with 5 owed */
    assert.deepEqual(out.map((l) => l.split(' ')[0]), ['0', '0', '1', '-1', '0']);
    /* the angle: 15 clicks, then -5 net */
    assert.equal(out[2].split(' ')[1], String(15 / 96).slice(0, 6));
    /* the arc from 72 round to 24: 72 is its start, 0 halfway, 50 off it */
    assert.deepEqual(out.map((l) => l.split(' ')[2]), ['0', '0', '0', '50', '-1']);
});

test('tween: a slot moved along a curve, replaced, and done', () => {
    const out = run(['tween'], `var a = new Int32Array(2), b = [0.0];
function init() {
    tween.start(a, 0, 100, 10, tween.linear);
    tween.start(b, 0, 1.0, 4, tween.out_quad);
}
var tk = 0;
function update() {
    tk++;
    var left = tween.step();
    if (tk == 2) printh(b[0]);
    if (tk == 5) { printh(a[0] + " " + left); tween.start(a, 0, 0, 5, tween.linear); }
    if (tk == 10) printh(a[0] + " " + tween.running(a, 0) + " " + tween.step());
}`, 10);
    /* out_quad at half way is three quarters; linear at five of ten is
     * 50, with one still running (b is done); restarted from 50 to 0 over
     * five ticks, it is there and done */
    assert.deepEqual(out, ['0.75', '50 1', '0 0 0']);
});

test('text: padding, a clock, centring and words wrapped in a box', () => {
    const out = run(['text'], `function init() {
    printh(text.pad(7, 3) + " " + text.pad(1234, 2) + " " + text.clock(75.53) + " " + text.width("ABC"));
    cls(0);
    printh(text.box("the quick brown fox", 0, 0, 40, 7));
    var top = -1, bottom = -1;
    for (var y = 0; y < 20; y++) for (var x = 0; x < 40; x++) if (pget(x, y) == 7) { if (top < 0) top = y; bottom = y; }
    printh(top + " " + bottom);
    cls(0);
    text.center("AB", 80, 0, 7);
    var l = 200, r = -1;
    for (x = 0; x < 160; x++) for (y = 0; y < 6; y++) if (pget(x, y) == 7) { l = min(l, x); r = max(r, x); }
    printh(l + " " + r);
}
function update() {}`);
    assert.equal(out[0], '007 1234 1:15.5 12');
    /* ten characters a line: "the quick" and "brown fox", two lines */
    assert.equal(out[1], '12');
    assert.match(out[2], /^0 1[01]$/);
    /* eight pixels wide, centred on 80 */
    assert.equal(out[3], '76 82');
});

test('text: in the current font and size, cutting UTF-8 whole', () => {
    const out = run(['text'], `function init() {
    font(FONT.CAPS, 2);
    printh(text.width("ABC") + " " + texth("A") + " " + texth("A\\nB"));
    cls(0);
    printh(text.box("the quick brown fox", 0, 0, 80, 7));
    font(FONT.WORLD);
    printh(texth("") + " " + text.box("Ωμέγα Ωμέγα", 0, 0, textw("Ωμέγα") + 2, 7));
    // no spaces: cut where it fills the box, never inside a character
    cls(0);
    var h = text.box("ЖЖЖЖЖЖ", 0, 0, textw("ЖЖЖ") + 2, 7);
    var ink = 0;
    for (var y = 0; y < 30; y++) for (var x = 0; x < 160; x++) if (pget(x, y) == 7) ink++;
    cls(0); print("ЖЖЖ\\nЖЖЖ", 0, 0, 7);
    var want = 0;
    for (y = 0; y < 30; y++) for (x = 0; x < 160; x++) if (pget(x, y) == 7) want++;
    printh(h + " " + (ink == want));
}
function update() {}`);
    /* CAPS at 2: 8 a character, 12 a line; ten a line in 80 */
    assert.deepEqual(out, ['24 12 24', '24', '12 24', '24 1']);
});

test('palfx: fades from the kept colours and back, and flashes', () => {
    const out = run(['palfx'], `function init() {
    setpal(7, 0xff8000);
    palfx.keep();
    palfx.fade(0.5, 0x000000);
    printh(getpal(7));
    palfx.flash(0xffffff);
    printh(getpal(3));
    palfx.fade(0, 0);
    printh(getpal(7));
    palfx.cycle(8, 3, 1);
}
function update() {}`);
    /* half of ff8000 is 7f4000 */
    assert.deepEqual(out, [String(0x7f4000), String(0xffffff), String(0xff8000)]);
});

test('wheelsteps: a row every 4 clicks slowly, many a step spun fast, back at once', () => {
    /* 12 clicks one a tick: three rows. Then 12 a tick, near four turns a
     * second: well past the 24 rows of 96 clicks taken slowly. Then a turn
     * back of 4: one row, the speed started again */
    const slow = [], fast = [];
    for (let t = 1; t <= 12; t++) slow.push(t + ':1');
    for (let t = 20; t < 28; t++) fast.push(t + ':12');
    const out = run([], `var a = 0, b = 0, c = 0;
function init() {}
function update() {
    var s = wheelsteps(), f = frame();
    if (f <= 15) a += s; else if (f < 40) b += s; else c += s;
    if (f == 45) printh(a + " " + (b > 24) + " " + c);
}`, 45, ['--wheel', slow.concat(fast, ['28:0', '40:-4', '41:0']).join(',')]);
    assert.deepEqual(out, ['3 1 -1']);
});

test('menu: the wheel and buttons move the choice, SELECT takes it', () => {
    const out = run(['dial', 'menu'], `var tk = 0;
function init() { menu.open(["PLAY", "OPTIONS", "QUIT"]); }
function update() {
    tk++;
    var chose = menu.update();
    if (chose >= 0) printh(tk + " chose " + chose);
    if (tk == 3) printh("at " + menu.choice());
}
function draw() { cls(0); menu.draw(10, 10, 7, 1); }`, 8, ['--wheel', '1:8', '--buttons', '4:2,5:0,7:16']);
    /* 8 clicks is two rows, 4 each: QUIT; NEXT wraps to PLAY; SELECT takes it */
    assert.deepEqual(out, ['at 2', '8 chose 0']);
});

test('gui: lines broken at newlines, spaces, and inside a long word', () => {
    const out = run(['gui'], `function init() {
    var l = gui.lines("THE QUICK BROWN FOX\\nJUMPS ABCDEFGHIJKLMNOP", 40);
    for (var i = 0; i < l.length; i++) printh("[" + l[i] + "]");
}
function update() {}`);
    assert.deepEqual(out, ['[THE QUICK]', '[BROWN FOX]', '[JUMPS]', '[ABCDEFGHIJ]', '[KLMNOP]']);
});

test('gui: a field typed with the wheel, Right and Left held; a list moved', () => {
    const out = run(['gui'], `var f, l, tk = 0;
function init() { f = gui.field(8, "XY"); l = gui.list(["A", "B", "C", "D"], 2); }
function update() {
    tk++;
    if (gui.field_update(f)) printh("[" + gui.field_text(f) + "]");
    var c = gui.list_update(l);
    if (c >= 0) printh("chose " + c + " top " + l.top);
}`, 30, ['--wheel', '1:6:0,2:0,4:12,5:0,8:-6,9:0',
         '--buttons', '2:2,3:0,10:1,27:0,28:16,29:0']);
    /* the wheel puts A after XY; Right keeps it; two on puts in A and
     * turns it to B, back one makes it A; Left held deletes one. The list,
     * a row every 4 clicks: one on, NEXT, three on (held at D), one back,
     * then PREV and its repeats leave A */
    assert.deepEqual(out, ['[XYA]', 'chose 0 top 0']);
});

/* The dialogs and the library draw the same pixels: each pair of carts
 * shows one dialog and the same thing built from gui */
test('gui: the same pixels as the dialogs', () => {
    const pairs = [
        ['ui.confirm("Sure?");',
         'gui.box(6, 47, 153, 71); print("Sure?", 10, 51, 7); gui.buttons(["Yes", "No"], 1, 1, 0, 60, 160);'],
        ['ui.text("Name", 8, "XYZ");',
         'gui.box(6, 41, 153, 78); print("Name", 10, 45, 7); gui.field_draw(gui.field(8, "XYZ"), 10, 54, 140, 1); gui.buttons(["OK", "Cancel"], 0, 0, 0, 67, 160);'],
        ['ui.choose("Pick one", ["Red", "Green", "Blue"]);',
         'gui.box(6, 39, 153, 79); print("Pick one", 10, 43, 7); gui.list_draw(gui.list(["Red", "Green", "Blue"], 3), 8, 52, 146);'],
        /* eleven rows of twelve, so with the scroll bar */
        ['ui.choose("Pick one", ["A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L"]);',
         'gui.box(6, 7, 153, 111); print("Pick one", 10, 11, 7); gui.list_draw(gui.list(["A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L"], 11), 8, 20, 146);'],
    ];
    for (const [dialog, built] of pairs) {
        const a = path.join(tmp, 'gui-a' + n + '.png'), b = path.join(tmp, 'gui-b' + n + '.png');
        run(['gui'], `function init() { cls(3); ${dialog} }\nfunction update() {}`, 1, ['--shot', a]);
        run(['gui'], `function init() {}\nfunction update() {}\nfunction draw() { cls(3); ${built} }`, 1, ['--shot', b]);
        assert.ok(fs.readFileSync(a).equals(fs.readFileSync(b)), dialog);
    }
});
