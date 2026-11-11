/* The cells system, through pbgvm. */

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
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-cells-'));
const CART = JSON.stringify({ id: '00000000000ce115', name: 'Cells' });

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

let n = 0;
function run(main, ticks = 1, extra = []) {
    const res = compileFiles({ 'cart.json': CART, 'main.js': main });
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(tmp, 's' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', String(ticks)].concat(extra),
                        { encoding: 'utf8', timeout: 20000 });
    return { errors: [], out: r.stdout.replace(/\r/g, ''), err: r.stderr, status: r.status };
}


/* A board from rows of digits, and printh of it back as the same */
const BOARD = `
function board(rows) {
    var w = rows[0].length, b = new Uint8Array(w * rows.length);
    for (var y = 0; y < rows.length; y++)
        for (var x = 0; x < w; x++) b[y * w + x] = rows[y].charCodeAt(x) - 48;
    return b;
}
function show(b, w) {
    var s = "";
    for (var i = 0; i < b.length; i++) s += (i && i % w == 0 ? "/" : "") + b[i];
    printh(s);
}
`;
function cellrun(body, ticks = 1) {
    const r = run(BOARD + 'var R;\n' + body + '\n', ticks);
    assert.equal(r.status, 0, r.err || JSON.stringify(r.errors));
    return r.out.trim().split('\n');
}

/* Materials: 0 air, 1 sand, 2 water, 3 wall, 4 steam, 5 fire, 6 wood,
 * 7 smoke */
const RULES = `
function rules() {
    R = cells.rules();
    R.move[1] = MOVE.POWDER; R.density[1] = 200;
    R.move[2] = MOVE.LIQUID; R.density[2] = 100; R.spread[2] = 3;
    R.density[3] = 255;
    R.move[4] = MOVE.GAS; R.density[4] = 5;
    R.density[5] = 255; R.decay[5] = 32; R.becomes[5] = 7;
    R.density[6] = 255;
    R.move[7] = MOVE.GAS; R.density[7] = 4;
    // fire beside wood: both fire
    R.a[0] = 5; R.b[0] = 6; R.c[0] = 5; R.d[0] = 5; R.chance[0] = 255;
}
`;

test('sand falls and piles, sinks through water, and steam rises', () => {
    const out = cellrun(RULES + `var b;
function init() {
    rules();
    b = board(["01110", "00000", "00000", "00000", "00000", "33333"]);
}
var tk = 0;
function update() {
    tk++;
    var n = cells.step(b, 5, R);
    if (tk == 12) { show(b, 5); printh(n); }
}`, 12);
    /* three grains settle on the floor, one beside another or on top */
    const rows = out[0].split('/');
    assert.equal(rows[5], '33333');
    assert.equal(out[0].split('1').length - 1, 3, out[0]);
    assert.ok(/^0+$/.test(rows.slice(0, 3).join('')), out[0]);
    assert.equal(out[1], '0', 'settled');
    const dense = cellrun(RULES + `var b;
function init() { rules(); b = board(["010", "020", "020", "333"]); }
var tk = 0;
function update() { tk++; cells.step(b, 3, R); if (tk == 12) show(b, 3); }`, 12);
    /* the grain has sunk to the bottom row, the water beside it */
    assert.equal(dense[0].split('/')[2].split('').sort().join(''), '122', dense[0]);
    const gas = cellrun(RULES + `var b;
function init() { rules(); b = board(["000", "000", "040"]); }
var tk = 0;
function update() { tk++; cells.step(b, 3, R); if (tk == 3) show(b, 3); }`, 3);
    assert.match(gas[0], /^\d\d\d\/000\/000$/);
});

test('water spreads level, fire burns wood and dies to smoke', () => {
    const water = cellrun(RULES + `var b;
function init() { rules(); b = board(["0000000", "0002000", "0002000", "0002000", "3333333"]); }
var tk = 0;
function update() { tk++; cells.step(b, 7, R); if (tk == 30) show(b, 7); }`, 30);
    /* all three cells of water on the bottom open row */
    assert.equal(water[0].split('/')[3].split('2').length - 1, 3, water[0]);
    assert.equal(water[0].split('/').slice(0, 3).join(''), '0'.repeat(21), water[0]);
    assert.equal(water[0].split('2').length - 1, 3);
    const fire = cellrun(RULES + `var b;
function init() { rules(); b = board(["66666", "56666"]); }
var tk = 0;
function update() { tk++; cells.step(b, 5, R); if (tk == 40) show(b, 5); }`, 40);
    /* the wood has burned, and the fires have died to smoke */
    assert.equal(fire[0].split('6').length - 1, 0, fire[0]);
    assert.ok(/7/.test(fire[0]), fire[0]);
});

test('a settled field sleeps, and drawing into it wakes it', () => {
    const out = cellrun(RULES + `var f;
function init() {
    rules();
    f = img(64, 48);
    target(f); cls(0); rectfill(0, 47, 63, 47, 3); target(SCREEN);
}
var tk = 0;
function update() {
    tk++;
    if (tk == 2) { target(f); pset(40, 3, 1); target(SCREEN); }
    var n = cells.step(f, 0, R);
    if (tk == 1 || tk == 3 || tk == 70) printh(tk + " " + n);
}`, 70);
    /* nothing to move at first; the grain drawn falls; then rests */
    assert.deepEqual(out, ['1 0', '3 1', '70 0']);
});

test('life: a blinker blinks, a cave smooths', () => {
    const out = cellrun(`function init() {
    var b = board(["00000", "00100", "00100", "00100", "00000"]);
    printh(cells.life(b, 5, 8, 12)); show(b, 5);
    cells.life(b, 5, 8, 12); show(b, 5);
    var c = board(["11111", "10001", "10101", "10001", "11111"]);
    cells.life(c, 5, 0x1e0, 0x1f0); show(c, 5);
}
function update() {}`);
    assert.deepEqual(out.slice(0, 3), ['4', '00000/00000/01110/00000/00000', '00000/00100/00100/00100/00000']);
    /* the lone pillar in the middle has too few neighbours and goes */
    assert.equal(out[3].split('/')[2][2], '0');
});

test('rules are checked', () => {
    const r = run(BOARD + 'function init() { cells.step(new Uint8Array(4), 2, {}); }\nfunction update() {}');
    assert.match(r.err, /needs rules from cells\.rules\(\)/);
    const r2 = run(BOARD + 'function init() { var R = cells.rules(); R.move = 1; cells.step(new Uint8Array(4), 2, R); }\nfunction update() {}');
    assert.match(r2.err, /the rules' move is no longer its Uint8Array/);
});

test('water levels across chunks, and a full layer sleeps', () => {
    /* a column of water on a floor four chunks wide ends level, its top
     * part-full layer still running between the walls; 128 cells fill two
     * rows exactly and go still */
    const run2 = (rows, ticks) => cellrun(`var b, W = 64, H = 40, tk = 0;
function init() {
    R = cells.rules();
    R.move[2] = MOVE.LIQUID; R.density[2] = 100;
    R.density[3] = 255;
    b = new Uint8Array(W * H);
    for (var x = 0; x < W; x++) b[(H - 1) * W + x] = 3;
    for (var y = 0; y < ${rows}; y++) for (x = 0; x < 4; x++) b[y * W + x] = 2;
}
function update() {
    tk++;
    var n = cells.step(b, W, R);
    if (tk == ${ticks}) {
        var lo = H, hi = 0;
        for (var x = 0; x < W; x++) {
            var top = H - 1;
            for (var y = 0; y < H - 1; y++) if (b[y * W + x] == 2) { top = y; break; }
            lo = min(lo, top); hi = max(hi, top);
        }
        printh(n + " " + (hi - lo));
    }
}`, ticks);
    /* 96 cells: a row and a half, the half still moving */
    const part = run2(24, 600)[0].split(' ').map(Number);
    assert.ok(part[0] > 0, 'the top layer runs');
    assert.equal(part[1], 1, 'within a cell of level');
    /* 128 cells: two rows, nowhere to go */
    assert.deepEqual(run2(32, 2000), ['0 0']);
});

test('a film one cell deep on a floor comes to rest, and a deep block still levels', () => {
    /* 42 cells on a floor 118 wide cannot make a full layer: once each
     * rests on stone with nowhere to fall it stays, so the field sleeps;
     * a block three deep runs out to one layer of 90 first */
    const film = (fill) => run(`const STONE = 5, WATER = 12;
var R, field;
function init() {
    R = cells.rules();
    R.move[STONE] = MOVE.STATIC; R.density[STONE] = 255;
    R.move[WATER] = MOVE.LIQUID; R.density[WATER] = 100; R.spread[WATER] = 4;
    field = img(120, 120);
    target(field);
    rectfill(0, 110, 119, 119, STONE);
    rect(0, 0, 119, 119, STONE);
    ${fill}
    target(SCREEN);
}
function update() {
    var m = cells.step(field, 0, R);
    if (frame() == 400) {
        target(field);
        var low = 0, above = 0;
        for (var x = 1; x < 119; x++) {
            if (pget(x, 109) == WATER) low++;
            for (var y = 1; y < 109; y++) if (pget(x, y) == WATER) above++;
        }
        target(SCREEN);
        printh(m + " " + low + " " + above);
    }
}`, 400).out.trim();
    assert.equal(film('rectfill(10, 100, 30, 101, WATER);'), '0 42 0');
    assert.equal(film('rectfill(40, 107, 69, 109, WATER);'), '0 90 0');
});

test('cells.step gives the box round what changed: stepped, drawn on, or settled into', () => {
    const r = run(`const STONE = 5, SAND = 15;
var R, field, box = new Int32Array(4), p = particles.pool(4);
function init() {
    R = cells.rules();
    R.move[STONE] = MOVE.STATIC; R.density[STONE] = 255;
    R.move[SAND] = MOVE.POWDER; R.density[SAND] = 200;
    field = img(128, 64);
    target(field);
    rectfill(0, 60, 127, 63, STONE);
    rectfill(70, 10, 73, 13, SAND);
    target(SCREEN);
    p.field = field;
}
function show(t) { printh(t + " " + box.join(",")); }
function update() {
    var f = frame();
    fill(box, -1);
    cells.step(field, 0, R, null, box);
    if (f == 1 || f == 5 || f == 199) show(f);
    if (f == 200) { target(field); pset(20, 20, STONE); target(SCREEN); }
    if (f == 201) show(f);
    if (f == 202) particles.emit(p, 1, 100.5, 40, 0, 4, 0, 50, SAND);
    if (f > 202) { particles.step(p); }
    if (f == 210) show(f);
}`, 210);
    assert.equal(r.status, 0, r.err);
    assert.deepEqual(r.out.trim().split('\n'), [
        /* a field first stepped: all of it */
        '1 0,0,127,63',
        /* the sand falling, in its chunks */
        '5 64,0,79,31',
        /* settled: nothing changed, the box left as it was */
        '199 -1,-1,-1,-1',
        /* drawn into: its chunk, and the cell drawn */
        '201 16,16,31,31',
        /* a grain settled on the floor at x 100, then that chunk awake */
        '210 96,48,111,63']);
});

/* Materials for the layers: 0 air, 5 stone, 12 water, 15 sand */
const LAYERS = `
function rules2() {
    R = cells.rules();
    R.move[15] = MOVE.POWDER; R.density[15] = 200;
    R.move[12] = MOVE.LIQUID; R.density[12] = 100;
    R.density[5] = 255;
}
`;

test('actors are obstacles to the field: sand rests on one and falls when it goes', () => {
    const out = cellrun(LAYERS + `var f, p, tk = 0;
function init() {
    rules2();
    f = img(32, 32);
    target(f); cls(0); rectfill(0, 31, 31, 31, 5); rectfill(14, 0, 17, 3, 15); target(SCREEN);
    p = actors.pool(1);
    var a = actors.spawn(p); p.x[a] = 12; p.y[a] = 16; p.w[a] = 8; p.h[a] = 8;
}
function at(x, y) { target(f); var v = pget(x, y); target(SCREEN); return v; }
function update() {
    tk++;
    if (tk == 60) p.x[0] = 24;
    cells.step(f, 0, R, p);
    if (tk == 59) printh(at(15, 15) + " " + at(15, 16) + " " + at(15, 30));
    if (tk == 120) printh(at(15, 15) + " " + at(15, 30));
}`, 120);
    /* resting on the actor's top (y 16), none inside or under it; once it
     * has gone the sand is on the floor */
    assert.deepEqual(out, ['15 0 0', '0 15']);
});

test('particles settle into their field, and a lift throws loose cells out', () => {
    const out = cellrun(LAYERS + `var f, sp, tk = 0;
function init() {
    rules2();
    f = img(96, 32);
    target(f); cls(0); rectfill(0, 28, 95, 31, 5); rectfill(40, 20, 47, 27, 15); rectfill(48, 24, 55, 27, 12); target(SCREEN);
    sp = particles.pool(200);
    sp.gravity = 0.2; sp.field = f;
    printh(cells.lift(f, R, 44, 26, 6, sp, 2));
    var stone = 0, live = 0;
    target(f); for (var x = 0; x < 96; x++) for (var y = 28; y < 32; y++) if (pget(x, y) == 5) stone++; target(SCREEN);
    for (var i = 0; i < 200; i++) if (sp.life[i] > 0) live++;
    printh(stone + " " + live);
}
function update() {
    tk++;
    particles.step(sp);
    cells.step(f, 0, R);
    if (tk == 150) {
        var live = 0, cells_ = 0;
        for (var i = 0; i < 200; i++) if (sp.life[i] > 0) live++;
        target(f); for (var x = 0; x < 96; x++) for (var y = 0; y < 28; y++) if (pget(x, y) != 0) cells_++; target(SCREEN);
        printh(live + " " + cells_);
    }
}`, 150);
    const lifted = Number(out[0]);
    assert.ok(lifted > 50, 'lifted ' + lifted);
    /* the stone floor untouched, every lifted cell a live particle */
    assert.equal(out[1], '384 ' + lifted);
    /* all landed again: no particle left in flight, and every cell back --
     * one landing where another just has settles beside or above it */
    const [live, back] = out[2].split(' ').map(Number);
    assert.equal(live, 0);
    assert.equal(back, 64 + 32);
});
