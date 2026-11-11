/* Drawing, pixel for pixel: small cases checked exactly, and a scene of
 * every drawing function checked against a hash recorded after looking at
 * it. The scene's picture is pbgvm test/carts/drawdemo.pbg --shot. */

import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { compileFiles, readFolder } from '../src/node.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const host = path.join(here, '..', 'host');
const exe = path.join(host, process.platform === 'win32' ? 'pbgvm.exe' : 'pbgvm');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-draw-'));
const CART = JSON.stringify({ id: '0123456789abcdef', name: 'Draw' });

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

let n = 0;

/* The screen rectangle x,y,w,h after one tick whose draw() is `body`. */
function pixels(body, rect, files) {
    const res = compileFiles(Object.assign({ 'cart.json': CART, 'main.js':
        'function init() {}\nfunction update() {}\nfunction draw() { cls(); ' +
        body + ' }' }, files || {}));
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'd' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '1', '--dump', rect.join(',')],
                        { encoding: 'utf8', timeout: 20000 });
    assert.equal(r.status, 0, r.stderr);
    return r.stdout.replace(/\r/g, '').trimEnd();
}

function picture(rows) {
    return rows.join('\n');
}

test('filled circles stay round at small sizes', () => {
    assert.equal(pixels('circfill(3,3,0,1); circfill(10,3,1,1); ' +
                        'circfill(18,3,2,1); circfill(27,3,3,1);', [0, 0, 31, 7]),
    picture([
        '0000000000000000000000000011100',
        '0000000000000000011100000111110',
        '0000000000100000111110001111111',
        '0001000001110000111110001111111',
        '0000000000100000111110001111111',
        '0000000000000000011100000111110',
        '0000000000000000000000000011100']));
});

test('outlined circles and ovals are connected and symmetric', () => {
    assert.equal(pixels('circ(4,4,4,7); oval(10,0,17,8,7); oval(20,2,29,6,7);',
                        [0, 0, 31, 9]),
    picture([
        '0007770000000770000000000000000',
        '0770007700007007000000000000000',
        '0700000700070000700000007700000',
        '7000000070070000700007770077700',
        '7000000070700000070070000000070',
        '7000000070070000700007770077700',
        '0700000700070000700000007700000',
        '0770007700007007000000000000000',
        '0007770000000770000000000000000']));
});

test('lines round evenly and triangles include their corners', () => {
    assert.equal(pixels('line(0,0,9,3,7); line(12,0,14,6,7); ' +
                        'trifill(16,0,24,6,20,6,7);', [0, 0, 26, 7]),
    picture([
        '77000000000070007000000000',
        '00777000000070000700000000',
        '00000777000007000777000000',
        '00000000770007000077700000',
        '00000000000007000007770000',
        '00000000000000700007777700',
        '00000000000000700000777770']));
});

test('a clipped line is exactly the pixels of the whole line', () => {
    const whole = pixels('line(-50,-20,100,40,7);', [0, 0, 40, 30]);
    const clipped = pixels('clip(5,3,24,17); line(-50,-20,100,40,7); clip();',
                           [0, 0, 40, 30]);
    const rows = whole.split('\n'), crows = clipped.split('\n');
    for (let y = 0; y < rows.length; y++)
        for (let x = 0; x < rows[y].length; x++) {
            const inside = x >= 5 && x < 25 && y >= 3 && y < 18;
            assert.equal(crows[y][x], inside ? rows[y][x] : '0', x + ',' + y);
        }
});

test('text, and rectangles thin enough to be all edge', () => {
    assert.equal(pixels('print("Aa!",0,0,7); rect(14,0,17,3,7); rect(20,0,21,3,7);',
                        [0, 0, 24, 5]),
    picture([
        '777077700700007777007700',
        '707070700700007007007700',
        '777077700700007007007700',
        '707070700000007777007700',
        '707070700700000000000000']));
});

test('fill pattern, camera and clip', () => {
    assert.equal(pixels('fillp(0x5a5a); rectfill(0,0,7,3,0x0907); fillp(); ' +
                        'camera(-2,-1); pset(0,0,8); camera(); ' +
                        'clip(12,0,14,1); rectfill(10,0,20,5,9);', [0, 0, 20, 4]),
    picture([
        '79797979000099900000',
        '97879797000099900000',
        '79797979000000000000',
        '97979797000000000000']));
});

test('a pattern bit with no second colour draws nothing: 0xf0f0 is stripes', () => {
    assert.equal(pixels('rectfill(0,0,7,1,2); fillp(0xf0f0); rectfill(0,0,7,1,7);',
                        [0, 0, 8, 2]),
    picture(['22222222', '77777777']));
});

test('sprites: transparency, flipping, scaling and the draw palette', () => {
    /* Sprite 0's top-left 2x2 is 7 8 / 0 9, and 0 is transparent. */
    assert.equal(pixels('sset(0,0,7); sset(1,0,8); sset(0,1,0); sset(1,1,9); ' +
                        'rectfill(0,0,11,3,5); spr(0,0,0); spr(0,0,2,1,1,true); ' +
                        'pal(7,12); sspr(0,0,2,2,10,0,2,4); pal();', [0, 0, 12, 4]),
    picture([
        '7855555555c8',
        '5955555555c8',
        '555555875559',
        '555555955559']));
});

test('sheet() makes an image the sprites\' sheet, its whole cells numbered along its rows', () => {
    /* A 20x16 image is 2 by 2 sprites; its last 4 columns are no sprite's
     * but sspr reaches them. */
    assert.equal(pixels('sset(0,0,7); var im = img(20, 16); target(im); ' +
                        'pset(8,0,8); pset(0,8,9); pset(16,0,10); target(SCREEN); ' +
                        'rectfill(0,0,11,0,5); sheet(im); spr(1,0,0); spr(2,1,0); ' +
                        'spr(4,2,0); sspr(16,0,1,1,3,0); sset(1,0,12); pset(4,0,sget(1,0)); ' +
                        'sprites.draw(1, [10], [0], 1); sheet(); spr(0,5,0); ' +
                        'pset(6,0,sget(1,0));', [0, 0, 12, 1]),
    '895ac7055585');
});

test('the map draws only sprites whose flags hold the layers', () => {
    assert.equal(pixels('sset(8,0,7); sset(16,0,8); fset(2,1,1); ' +
                        'mset(0,0,1); mset(1,0,2); mset(2,0,0); ' +
                        'map(0,0,0,0,3,1); map(0,0,0,2,3,1,2);', [0, 0, 24, 3]),
    picture([
        '700000008000000000000000',
        '000000000000000000000000',
        '000000008000000000000000']));
});

test('tline steps through the map', () => {
    assert.equal(pixels('for (var i = 0; i < 8; i++) sset(8 + i, 0, i + 1); ' +
                        'mset(0,0,1); mset(1,0,1); ' +
                        'tline(0,0,15,0,0,0); tline(0,1,7,1,0,0,0.25,0);',
                        [0, 0, 16, 2]),
    picture(['1234567812345678', '1357135700000000']));
});

test('images: drawing into one, then blitting it', () => {
    assert.equal(pixels('var i = img(3, 2); target(i); cls(4); pset(0,0,9); ' +
                        'target(SCREEN); blit(i, 1, 0); blit(i, 5, 0, true); ' +
                        'blitpart(i, 0, 0, 2, 1, 9, 0);', [0, 0, 12, 2]),
    picture(['094404490940', '044404440000']));
});

test('every drawing function: the scene matches its recorded picture', () => {
    const res = compileFiles(readFolder(path.join(here, 'carts', 'drawdemo')));
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'drawdemo.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '1', '--dump', '0,0,160,120'],
                        { encoding: 'utf8', timeout: 20000 });
    assert.equal(r.status, 0, r.stderr);
    const hash = crypto.createHash('sha256').update(r.stdout.replace(/\r/g, ''))
        .digest('hex');
    assert.equal(hash, '67cbf0f76b94b9bcfc43202f71abf7e668007e7f9085d566bde13c0994eafe4a',
                 'the scene changed: look at it with pbgvm --shot, and if it is ' +
                 'right, record the new hash');
});

test('drawing reports a wrong argument by name', () => {
    const res = compileFiles({ 'cart.json': CART, 'main.js':
        'function init() {}\nfunction update() {}\nfunction draw() {\n  rect(0,0,"x",4);\n}' });
    const file = path.join(tmp, 'bad.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '1'], { encoding: 'utf8' });
    assert.match(r.stderr, /main\.js:4: rect\(\) needs a number, not a string \(in draw\)/);
});

test('scaled sprites pick the same source pixels as the exact formula', () => {
    /* Each sheet pixel a distinct colour, never 0 so nothing is
     * transparent; then shrunk, stretched and flipped to odd sizes. */
    const cases = [[16, 16, 5, 7, false], [16, 16, 23, 9, true],
                   [12, 10, 12, 10, true], [10, 14, 3, 31, false]];
    for (const [w, h, dw, dh, flip] of cases) {
        const got = pixels('for (var y = 0; y < 16; y++) for (var x = 0; x < 16; x++) ' +
                           'sset(x, y, 1 + (x * 5 + y * 3) % 15); ' +
                           'sspr(0, 0, ' + w + ', ' + h + ', 0, 0, ' + dw + ', ' + dh +
                           ', ' + flip + ');', [0, 0, dw, dh]);
        const want = [];
        for (let v = 0; v < dh; v++) {
            let row = '';
            const ty = Math.floor(v * h / dh);
            for (let u = 0; u < dw; u++) {
                const tx = Math.floor(u * w / dw);
                const x = flip ? w - 1 - tx : tx;
                row += (1 + (x * 5 + ty * 3) % 15).toString(16);
            }
            want.push(row);
        }
        assert.equal(got, want.join('\n'), w + 'x' + h + ' to ' + dw + 'x' + dh);
    }
});

test('unscaled sprites clip and flip exactly at every edge', () => {
    /* Sprite 0: each pixel a distinct non-zero colour. */
    const colour = (x, y) => 1 + (x * 3 + y * 5) % 15;
    const placements = [[-3, 2, false, false], [-5, 0, true, false],
                        [155, 1, false, true], [154, -4, true, true]];
    for (const [x, y, fx, fy] of placements) {
        const got = pixels('for (var j = 0; j < 8; j++) for (var i = 0; i < 8; i++) ' +
                           'sset(i, j, 1 + (i * 3 + j * 5) % 15); ' +
                           'spr(0, ' + x + ', ' + y + ', 1, 1, ' + ((fx ? 1 : 0) | (fy ? 2 : 0)) + ');',
                           [x < 0 ? 0 : 150, 0, 10, 8]);
        const want = [];
        for (let row = 0; row < 8; row++) {
            let line = '';
            for (let col = 0; col < 10; col++) {
                const sx = (x < 0 ? 0 : 150) + col - x, sy = row - y;
                let c = 0;
                if (sx >= 0 && sx < 8 && sy >= 0 && sy < 8 && sx + x < 160)
                    c = colour(fx ? 7 - sx : sx, fy ? 7 - sy : sy);
                line += c.toString(16);
            }
            want.push(line);
        }
        assert.equal(got, want.join('\n'), 'spr at ' + x + ',' + y);
    }
});

test('mem() hands out the engine arrays; writes to the screen mark its rows', () => {
    const run = (draw, extra) => {
        const res = compileFiles({ 'cart.json': CART, 'main.js':
            'var scr;\nfunction init() { scr = mem(MEM.SCREEN); }\n' +
            'function update() {}\nfunction draw() { ' + draw + ' }' });
        assert.deepEqual(res.errors, []);
        const file = path.join(tmp, 'm' + n++ + '.pbg');
        fs.writeFileSync(file, res.cart);
        const r = spawnSync(exe, [file, '--ticks', '2', '--dirty'].concat(extra || []),
                            { encoding: 'utf8', timeout: 20000 });
        return { out: r.stdout.replace(/\r/g, ''), err: r.stderr, status: r.status };
    };
    // One pixel through the view: row 3 only, and the pixel lands.
    let r = run('scr[3 * 160 + 5] = 8;', ['--dump', '4,3,3,1']);
    assert.equal(r.status, 0, r.err);
    assert.match(r.out, /^080\ndirty 3 3\n/);
    // pset and the view address the same pixel.
    r = run('pset(7, 2, 9); printh(scr[2 * 160 + 7] + " " + scr.length);');
    assert.match(r.out, /^9 19200\n/);
    // fill and copy mark exactly the rows they wrote.
    r = run('fill(scr, 12, 160 * 10, 320);');
    assert.match(r.out, /dirty 10 11/);
    r = run('var a = new Uint8Array(4); fill(a, 7); copy(scr, 160 * 50 - 2, a, 0, 4); printh(scr[160 * 50 + 1]);');
    assert.match(r.out, /^7\n7\ndirty 49 50/);
    // A drawing call and a view write together: the union.
    r = run('pset(0, 100); scr[160 * 20] = 1;');
    assert.match(r.out, /dirty 20 100/);
    // The sheet, map and flags are the arrays sget, mget and fget read.
    r = run('mem(MEM.SHEET)[3 * 128 + 2] = 11; mem(MEM.MAP)[128 + 4] = 33; ' +
            'mem(MEM.FLAGS)[33] = 5; printh(sget(2, 3) + " " + mget(4, 1) + " " + fget(33));');
    assert.match(r.out, /^11 33 5\n/);
    // copy between kinds, past an end, and a bad region are errors.
    r = run('copy(scr, 0, new Int32Array(4), 0, 4);');
    assert.match(r.err, /copy\(\) needs two arrays of one kind/);
    r = run('copy(scr, 19198, scr, 0, 4);');
    assert.match(r.err, /copy\(\) runs past the end of an array/);
    r = run('fill(scr, 1, 19000, 300);');
    assert.match(r.err, /fill\(\) runs past the end of an array/);
    r = run('mem(9);');
    assert.match(r.err, /mem\(\) needs MEM.SCREEN/);
    // Arrays of values copy and fill too, overlapping as memmove does.
    r = run('var v = [1, 2, 3, 4, 5]; copy(v, 1, v, 0, 4); fill(v, "x", 4); printh(v.join(","));');
    assert.match(r.out, /^1,1,2,3,x\n/);
});

/* A whole cart, one tick: what it prints, then any --dump */
function runCart(main, extra = []) {
    const res = compileFiles({ 'cart.json': CART, 'main.js': main });
    assert.deepEqual(res.errors, []);
    const file = path.join(tmp, 'c' + n++ + '.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', '1'].concat(extra),
                        { encoding: 'utf8', timeout: 20000 });
    assert.equal(r.status, 0, r.stderr);
    return r.stdout.replace(/\r/g, '');
}

test('rspr at angle 0 and scale 1 is spr, and a quarter turn turns it', () => {
    const SHEET = `for (var y = 0; y < 8; y++) for (var x = 0; x < 16; x++) sset(8 + x, y, x < 8 ? 1 + (x + y * 3) % 15 : (x + y) % 2 ? 9 : 0);`;
    const a = runCart(`function init() { ${SHEET} cls(); camera(-3, -2); spr(1, 10, 20, 2, 1); spr(1, 40, 7, 1, 1, true); }\nfunction update() {}`, ['--dump', '0,0,70,40']);
    const b = runCart(`function init() { ${SHEET} cls(); camera(-3, -2); rspr(1, 18, 24, 0, 1, 2, 1); rspr(1, 44, 11, 0, 1, 1, 1, true); }\nfunction update() {}`, ['--dump', '0,0,70,40']);
    assert.equal(b, a);
    /* a quarter turn anticlockwise: the sprite's top row becomes its left
     * column read bottom to top, its right column its top row */
    const q = runCart(`function init() { ${SHEET} cls(); rspr(1, 20, 20, 0.25); var s = "";
        for (var x = 0; x < 8; x++) s += pget(16, 23 - x) == sget(8 + x, 0) ? "" : x;
        for (var y = 0; y < 8; y++) s += pget(16 + y, 16) == sget(8 + 7, y) ? "" : "r" + y;
        printh(s == "" ? "turned" : s); }\nfunction update() {}`);
    assert.equal(q, 'turned\n');
});

test('one flip number everywhere: 1 across, 2 up and down, 3 both', () => {
    const SHEET = `for (var y = 0; y < 8; y++) for (var x = 0; x < 8; x++) sset(8 + x, y, 1 + (x + y * 3) % 15);`;
    /* spr flipped up and down reads the sheet's rows bottom to top */
    const v = runCart(`function init() { ${SHEET} cls(); spr(1, 0, 0, 1, 1, 2); spr(1, 10, 0, 1, 1, 3); var s = "";
        for (var y = 0; y < 8; y++) for (var x = 0; x < 8; x++) {
            if (pget(x, y) != sget(8 + x, 7 - y)) s += "v";
            if (pget(10 + x, y) != sget(15 - x, 7 - y)) s += "b";
        }
        printh(s == "" ? "flipped" : s); }\nfunction update() {}`);
    assert.equal(v, 'flipped\n');
    /* every other call draws the same as spr with that number */
    for (const f of [1, 2, 3]) {
        const want = runCart(`function init() { ${SHEET} cls(); spr(1, 4, 4, 1, 1, ${f}); }\nfunction update() {}`, ['--dump', '0,0,16,16']);
        for (const call of [`sspr(8, 0, 8, 8, 4, 4, 8, 8, ${f})`, `rspr(1, 8, 8, 0, 1, 1, 1, ${f})`,
                            `var i = img(8, 8); target(i); spr(1, 0, 0); target(SCREEN); blit(i, 4, 4, ${f})`,
                            `var i = img(8, 8); target(i); spr(1, 0, 0); target(SCREEN); blitpart(i, 0, 0, 8, 8, 4, 4, ${f})`,
                            `sprites.draw([1], [4], [4], 1, [${f}])`]) {
            const got = runCart(`function init() { ${SHEET} cls(); ${call}; }\nfunction update() {}`, ['--dump', '0,0,16,16']);
            assert.equal(got, want, call);
        }
    }
});

test('a FixArray serves wherever an array of numbers does', () => {
    const SHEET = `for (var y = 0; y < 8; y++) for (var x = 0; x < 8; x++) sset(8 + x, y, 1 + (x + y * 3) % 15);`;
    const draw = (kind) => runCart(`function init() { ${SHEET} cls();
        var xs = new ${kind}(3), ys = new ${kind}(3), fl = new ${kind}(3);
        for (var i = 0; i < 3; i++) { xs[i] = 4 + i * 12; ys[i] = 2 + i; fl[i] = i; }
        sprites.draw(1, xs, ys, 3, fl); }\nfunction update() {}`, ['--dump', '0,0,40,16']);
    assert.equal(draw('FixArray'), draw('Int32Array'));
});

test('clip takes two corners, both inside, as rect does', () => {
    const r = runCart(`function init() { cls(); clip(10, 20, 12, 21); rectfill(0, 0, 159, 119, 7); clip(); var n = 0;
        for (var y = 0; y < 120; y++) for (var x = 0; x < 160; x++) if (pget(x, y) == 7) n++;
        printh(n + " " + pget(10, 20) + " " + pget(12, 21) + " " + pget(13, 21) + " " + pget(12, 22)); }\nfunction update() {}`);
    assert.equal(r, '6 7 7 0 0\n');
});

test('fset takes the value before the flag, which is optional as in fget', () => {
    const r = runCart(`function init() { fset(5, true, 3); var a = fget(5) + " " + fget(5, 3);
        fset(5, false, 3); fset(6, 9); printh(a + " " + fget(5) + " " + fget(6)); }\nfunction update() {}`);
    assert.equal(r, '8 1 0 9\n');
});

test('color(c) is the pen, and color.mix() and the rest beside it', () => {
    const r = runCart(`function init() { cls(); color(9); pset(0, 0); printh(pget(0, 0) + " " + color.mix(0, 0xffffff, 0.5)); }\nfunction update() {}`);
    assert.equal(r, '9 8421504\n');
});

test('rspr scales about its centre, rblit turns an image', () => {
    const r = runCart(`function init() {
    for (var y = 0; y < 8; y++) for (var x = 0; x < 8; x++) sset(8 + x, y, 12);
    cls(); rspr(1, 40, 40, 0, 2);
    var w = 0; for (var x = 0; x < 80; x++) if (pget(x, 40) == 12) w++;
    var first = pget(32, 40), before = pget(31, 40);
    var im = img(10, 4); target(im); cls(7); target(SCREEN);
    cls(); rblit(im, 40, 40, 0.25);
    var tall = 0; for (var y = 0; y < 80; y++) if (pget(40, y) == 7) tall++;
    printh(w + " " + first + " " + before + " " + tall);
}
function update() {}`);
    /* 16 wide from 32 to 47; the 10x4 image stands 10 tall */
    assert.equal(r, '16 12 0 10\n');
});
