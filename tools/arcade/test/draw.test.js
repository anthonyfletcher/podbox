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
    const clipped = pixels('clip(5,3,20,15); line(-50,-20,100,40,7); clip();',
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
                        'clip(12,0,3,2); rectfill(10,0,20,5,9);', [0, 0, 20, 4]),
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
                           'spr(0, ' + x + ', ' + y + ', 1, 1, ' + fx + ', ' + fy + ');',
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
