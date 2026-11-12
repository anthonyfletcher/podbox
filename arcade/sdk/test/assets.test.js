/* A cart's assets: LZ4 both ways, indexed PNGs, and a folder with a sheet,
 * flags, palettes, maps with objects and an image, compiled and run in
 * pbgvm; then the compiler's and the loader's refusals. */

import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { compileFiles, readFolder, readAssets } from '../src/node.js';
import { lz4Compress, lz4Decompress } from '../src/lz4.js';
import { decodeIndexedPng } from '../src/png.js';
import { crc32, readCart, writeCart } from '../src/cart.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const host = path.join(here, '..', 'host');
const exe = path.join(host, process.platform === 'win32' ? 'pbgvm.exe' : 'pbgvm');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-assets-'));
const inflate = (z) => new Uint8Array(zlib.inflateSync(z));

before(() => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building pbgvm failed:\n' + r.stdout + r.stderr);
});

/* An indexed (or, with colour 0, grey) PNG of the given pixels. */
function png(w, h, px, depth = 8, colour = 3) {
    const chunk = (type, data) => {
        const t = Buffer.from(type, 'latin1');
        const len = Buffer.alloc(4);
        len.writeUInt32BE(data.length);
        const crc = Buffer.alloc(4);
        crc.writeUInt32BE(crc32(Buffer.concat([t, data])));
        return Buffer.concat([len, t, data, crc]);
    };
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(w, 0);
    ihdr.writeUInt32BE(h, 4);
    ihdr[8] = depth;
    ihdr[9] = colour;
    const stride = Math.ceil(w * depth / 8), per = 8 / depth;
    const raw = Buffer.alloc((stride + 1) * h);
    let last = null;
    for (let y = 0; y < h; y++) {
        const f = y % 5;                        /* every filter type */
        raw[y * (stride + 1)] = f;
        const row = Buffer.alloc(stride);
        for (let x = 0; x < w; x++)
            row[Math.floor(x / per)] |= px[y * w + x] << (8 - depth * (x % per + 1));
        /* filtered as the row claims, so the reader must undo it */
        for (let x = 0; x < stride; x++) {
            const a = x ? row[x - 1] : 0;
            const up = last ? last[x] : 0, c = x && last ? last[x - 1] : 0;
            let p = 0;
            if (f === 1) p = a;
            else if (f === 2) p = up;
            else if (f === 3) p = (a + up) >> 1;
            else if (f === 4) {
                const pa = Math.abs(up - c), pb = Math.abs(a - c), pc = Math.abs(a + up - 2 * c);
                p = pa <= pb && pa <= pc ? a : pb <= pc ? up : c;
            }
            raw[y * (stride + 1) + 1 + x] = (row[x] - p) & 0xff;
        }
        last = row;
    }
    const sig = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
    const plte = Buffer.alloc(256 * 3);
    return new Uint8Array(Buffer.concat([sig, chunk('IHDR', ihdr),
        colour === 3 ? chunk('PLTE', plte.subarray(0, 3 << depth)) : Buffer.alloc(0),
        chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
}

function run(dir, ticks = 1) {
    const res = compileFiles(readFolder(dir), readAssets(dir));
    if (res.errors.length) return { errors: res.errors };
    const file = path.join(dir, 'cart.pbg');
    fs.writeFileSync(file, res.cart);
    const r = spawnSync(exe, [file, '--ticks', String(ticks)],
                        { encoding: 'utf8', timeout: 20000 });
    return { errors: [], out: r.stdout.replace(/\r/g, ''), err: r.stderr, status: r.status,
             cart: res.cart };
}

function folder(name, files) {
    const dir = path.join(tmp, name);
    fs.mkdirSync(dir, { recursive: true });
    for (const f in files) {
        fs.mkdirSync(path.dirname(path.join(dir, f)), { recursive: true });
        fs.writeFileSync(path.join(dir, f), files[f]);
    }
    return dir;
}

const CART = JSON.stringify({ id: '00000000000a55e7', name: 'Assets' });

test('LZ4 round-trips data of every shape, and refuses what is malformed', () => {
    const cases = [new Uint8Array(0), Uint8Array.of(7), new Uint8Array(13),
                   new Uint8Array(300).fill(9), new Uint8Array(70000).fill(1)];
    let seed = 1;
    const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) >>> 24;
    for (const n of [5, 12, 13, 17, 64, 1000, 20000]) {
        const a = new Uint8Array(n);
        for (let i = 0; i < n; i++) a[i] = i % 7 < 3 ? rnd() : a[Math.max(0, i - 40)];
        cases.push(a);
    }
    for (const c of cases) {
        const z = lz4Compress(c);
        assert.deepEqual(lz4Decompress(z, c.length), c, 'size ' + c.length);
        assert.equal(lz4Decompress(z, c.length + 1), null);
    }
    assert.equal(lz4Decompress(Uint8Array.of(0x10, 1, 0, 0), 5), null);
    assert.ok(lz4Compress(new Uint8Array(70000)).length < 400);
});

test('indexed PNGs read as palette numbers at every depth', () => {
    for (const depth of [1, 2, 4, 8]) {
        const w = 13, h = 7, px = new Uint8Array(w * h);
        for (let i = 0; i < px.length; i++) px[i] = (i * 7 + (i >> 3)) % (1 << depth);
        const img = decodeIndexedPng(png(w, h, px, depth), inflate);
        assert.equal(img.width, w);
        assert.deepEqual(img.px, px, 'depth ' + depth);
    }
    const grey = new Uint8Array([0, 50, 255, 3]);
    assert.deepEqual(decodeIndexedPng(png(2, 2, grey, 8, 0), inflate).px, grey);
    const rgb = png(2, 2, new Uint8Array(4), 8, 2);
    assert.throws(() => decodeIndexedPng(rgb, inflate), /indexed \(palette\) PNG/);
});

function assetCart() {
    const sheet = new Uint8Array(128 * 128);
    for (let i = 0; i < sheet.length; i++) sheet[i] = (i * 3 + (i >> 7)) & 15;
    const title = new Uint8Array(20 * 10);
    for (let i = 0; i < title.length; i++) title[i] = 1 + i % 9;
    const row = (cells) => cells.map((c) => c.toString(16).padStart(2, '0')).join('');
    return {
        'cart.json': CART,
        'sheet.png': png(128, 128, sheet),
        'flags.json': JSON.stringify({ 3: 5, 200: 128 }),
        'palettes/a_main.gpl': 'GIMP Palette\nName: main\n# a comment\n' +
            '0 0 0 black\n255 0 0 red\n0 255 0\n',
        'palettes/b_night.gpl': 'GIMP Palette\n10 20 30\n40 50 60\n',
        'maps/one.json': JSON.stringify({ width: 4, height: 3,
            rows: [row([1, 2, 3, 4]), row([5, 6]), row([255, 0, 7, 8])],
            objects: [{ kind: 'spawn', x: 16, y: 8, props: { hp: -3, name: 'start' } },
                      { kind: 'door', x: 24, y: 0, w: 8, h: 16 }] }),
        'maps/two.json': JSON.stringify({ width: 200, height: 2,
            rows: ['00'.repeat(199) + '2a'], objects: [] }),
        'images/title.png': png(20, 10, title),
        'main.js': `
var obj = new Int32Array(10);
function init() {
    printh("sheet " + sget(0, 0) + " " + sget(5, 2) + " flags " + fget(3) + " " + fget(200) + " " + fget(4));
    printh("pal " + getpal(1) + " " + getpal(2) + " " + getpal(3));
    printh("map " + mapw() + "x" + maph() + " " + mget(0, 0) + " " + mget(3, 0) + " " + mget(1, 1) + " " + mget(2, 1) + " " + mget(0, 2) + " " + mget(9, 9));
    printh("objs " + mapobj(obj, 2) + " " + obj.join(","));
    printh("kinds " + OBJ.door + " " + OBJ.spawn);
    printh("props " + mapprop(0, "hp") + " " + mapprop(0, "name") + " " + mapprop(0, "nope") + " " + mapprop(1, "hp"));
    cls();
    blit(IMG.title, 3, 4);
    printh("img " + pget(3, 4) + " " + pget(4, 4) + " " + pget(22, 13) + " " + pget(23, 14));
    target(IMG.title);
    pset(0, 0, 12);
    target(SCREEN);
    blit(IMG.title, 50, 50);
    printh("drawn " + pget(50, 50));
    loadpal(PAL.b_night);
    printh("night " + getpal(0) + " " + getpal(1) + " " + getpal(2));
    loadmap(MAP.two);
    printh("map " + mapw() + "x" + maph() + " " + mget(199, 0) + " " + mget(0, 0) + " objs " + mapobj(obj, 2));
    printh("view " + mem(MEM.MAP).length + " " + mem(MEM.MAP)[199]);
    loadmap(MAP.one);
    printh("back " + mapw() + " " + mget(3, 2));
    var cells = new Uint8Array(400);
    printh("cells " + loadmap(MAP.two, cells) + " " + cells[199] + " " + mapw() + " " + mget(3, 2));
    var row = new Uint8Array(2);
    row[1] = mget(1, 0);
    cls(); map(1, 0, 0, 0, 1, 1, 0, row, 2);
    var a = pget(2, 2) * 1000 + pget(5, 6);
    cls(); map(1, 0, 0, 0, 1, 1);
    printh("drawn alike " + (a == pget(2, 2) * 1000 + pget(5, 6)));
}
function update() {}
`
    };
}

test('a cart\'s sheet, flags, palettes, maps, objects and images reach the game', () => {
    const r = run(folder('assets', assetCart()));
    assert.deepEqual(r.errors, []);
    assert.equal(r.status, 0, r.err);
    const sheet0 = 0, sheet52 = (2 * 128 + 5) * 3 + 2 & 15;
    assert.equal(r.out, [
        'sheet ' + sheet0 + ' ' + sheet52 + ' flags 5 128 0',
        'pal 16711680 65280 ' + 0x0f6640,
        'map 4x3 1 4 6 0 255 0',
        'objs 2 1,16,8,0,0,0,24,0,8,16',
        'kinds 0 1',
        'props -3 start null null',
        'img 1 2 ' + (1 + (9 * 20 + 19) % 9) + ' 0',
        'drawn 12',
        'night 660510 2634300 65280',
        'map 200x2 42 0 objs 0',
        'view 400 42',
        'back 4 8',
        /* MAP.two unpacked into cells, 200 wide; the active map still one */
        'cells 200 42 4 8',
        'drawn alike 1',
        ''].join('\n'));
});

test('a cart with no assets keeps the empty 128x64 map and the default palette', () => {
    const r = run(folder('none', { 'cart.json': CART, 'main.js':
        'function init() { printh(mapw() + "x" + maph() + " " + mem(MEM.MAP).length + " " + getpal(8)); }\nfunction update() {}' }));
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out, '128x64 8192 ' + 0xe8304a + '\n');
});

test('asset files that cannot be used are named with what to change', () => {
    const files = assetCart();
    files['sheet.png'] = png(64, 64, new Uint8Array(64 * 64));
    files['maps/bad.json'] = JSON.stringify({ width: 300, height: 2 });
    files['maps/rows.json'] = JSON.stringify({ width: 2, height: 1, rows: ['0g00'] });
    files['images/rgb.png'] = png(2, 2, new Uint8Array(4), 8, 2);
    files['palettes/x.gpl'] = 'GIMP Palette\n300 0 0\n';
    files['maps/my-level.json'] = JSON.stringify({ width: 1, height: 1 });
    const r = run(folder('bad', files));
    const by = Object.fromEntries(r.errors.map((e) => [e.file, e.message]));
    assert.match(by['sheet.png'], /128x128, not 64x64/);
    assert.match(by['maps/bad.json'], /1 to 256 cells/);
    assert.match(by['maps/rows.json'], /two hex digits a cell/);
    assert.match(by['images/rgb.png'], /indexed \(palette\) PNG/);
    assert.match(by['palettes/x.gpl'], /three numbers from 0 to 255/);
    assert.match(by['maps/my-level.json'], /usable as a name in code/);
});

test('objects.json holds the maps to its kinds and properties', () => {
    const kinds = { spawn: { help: 'Where a player starts',
                             props: { hp: { default: 3, help: 'Lives' },
                                      name: { default: '' } } },
                    door: {} };
    const files = assetCart();
    files['objects.json'] = JSON.stringify(kinds);
    /* assetCart's maps hold a spawn with hp and name, and a door */
    assert.deepEqual(run(folder('objs-ok', files)).errors, []);

    files['maps/c_odd.json'] = JSON.stringify({ width: 1, height: 1, objects: [
        { kind: 'coin', x: 0, y: 0 }] });
    files['maps/d_prop.json'] = JSON.stringify({ width: 1, height: 1, objects: [
        { kind: 'spawn', x: 0, y: 0, props: { speed: 2 } }] });
    files['maps/e_type.json'] = JSON.stringify({ width: 1, height: 1, objects: [
        { kind: 'spawn', x: 0, y: 0, props: { hp: 'many' } }] });
    let by = Object.fromEntries(run(folder('objs-bad', files)).errors
                                .map((e) => [e.file, e.message]));
    assert.match(by['maps/c_odd.json'], /kind coin is not in objects\.json/);
    assert.match(by['maps/d_prop.json'], /spawn has no property speed/);
    assert.match(by['maps/e_type.json'], /spawn\.hp is a whole number/);

    const bad = assetCart();
    bad['objects.json'] = JSON.stringify({ spawn: { props: { hp: { default: 1.5 } } } });
    by = Object.fromEntries(run(folder('objs-file', bad)).errors
                            .map((e) => [e.file, e.message]));
    assert.match(by['objects.json'], /spawn\.hp: \{ "default": a whole number or text/);
});

/* A 256 by 256 map of cells that do not pack: about 64 KB in the file */
function noiseMap(seed) {
    let s = seed;
    const rows = [];
    for (let y = 0; y < 256; y++) {
        let r = '';
        for (let x = 0; x < 256; x++) {
            s = (s * 1103515245 + 12345) >>> 0;
            r += ((s >>> 16) & 255).toString(16).padStart(2, '0');
        }
        rows.push(r);
    }
    return { width: 256, height: 256, rows: rows, objects: [{ kind: 'start', x: seed, y: 0 }] };
}

test('maps stay in the cart file until loadmap, so four big ones fit beside a big heap', () => {
    const files = { 'cart.json': CART };
    const want = [];
    for (let k = 0; k < 4; k++) {
        const m = noiseMap(k + 1);
        files['maps/m' + k + '.json'] = JSON.stringify(m);
        want.push(parseInt(m.rows[200].slice(2 * 100, 2 * 100 + 2), 16));
    }
    /* 260 KB of maps: held whole beside 200 KB of heap, they would not fit */
    files['main.js'] = `var big, obj = new Int32Array(5);
function init() {
    big = new Int32Array(50000);
    var s = "";
    for (var k = 0; k < 4; k++) {
        loadmap(k);
        mapobj(obj, 1);
        s += mget(100, 200) + "/" + obj[1] + " ";
    }
    printh(s);
}
function update() {}`;
    const r = run(folder('lazymaps', files));
    assert.deepEqual(r.errors, []);
    assert.equal(r.status, 0, r.err);
    assert.equal(r.out.trim(), want.map((v, k) => v + '/' + (k + 1)).join(' '));
    assert.equal(readCart(r.cart).list.filter((c) => c.tag === 'MAPC').length, 4);
});

test('a kept game reads its maps and lazy images from the file after it resumes', () => {
    const files = {
        'cart.json': JSON.stringify({ id: '00000000000a55ea', name: 'Keep', lazy_images: ['sky'] }),
        'maps/a.json': JSON.stringify({ width: 2, height: 1, rows: ['0102'] }),
        'maps/b.json': JSON.stringify({ width: 2, height: 1, rows: ['0708'] }),
        'images/sky.png': png(2, 2, Uint8Array.of(5, 6, 7, 8)),
        'main.js': `function init() {}
function update() {
    if (frame() == 5) {
        loadmap(MAP.b);
        cls(); blit(img.load(IMG.sky), 0, 0);
        printh(mget(1, 0) + " " + pget(1, 1));
    }
}`
    };
    const dir = folder('lazykeep', files);
    const res = compileFiles(readFolder(dir), readAssets(dir));
    assert.deepEqual(res.errors, []);
    const file = path.join(dir, 'keep.pbg');
    fs.writeFileSync(file, res.cart);
    for (const extra of [[], ['--suspend-at', '3']]) {
        const r = spawnSync(exe, [file, '--ticks', '6'].concat(extra), { encoding: 'utf8', timeout: 20000 });
        assert.equal(r.status, 0, r.stderr);
        assert.ok(r.stdout.replace(/\r/g, '').split('\n').includes('8 8'), extra.join(' ') + ': ' + r.stdout);
    }
});

test('a lazy image is read by img.load and let go by img.drop', () => {
    const px = new Uint8Array(20 * 10);
    for (let i = 0; i < px.length; i++) px[i] = 1 + i % 9;
    const files = {
        'cart.json': JSON.stringify({ id: '00000000000a55e8', name: 'Lazy', lazy_images: ['sky'] }),
        'images/sky.png': png(20, 10, px),
        'images/hud.png': png(4, 4, new Uint8Array(16).fill(3)),
        'main.js': `var kept;
function init() {
    printh("size " + imgw(IMG.sky) + "x" + imgh(IMG.sky) + " " + imgw(IMG.hud));
    var a = img.load(IMG.sky), b = img.load(IMG.sky);
    cls(); blit(IMG.sky, 0, 0);
    printh("loaded " + (a == b) + " " + (a == IMG.sky) + " " + pget(0, 0) + " " + pget(19, 9));
    printh("hud " + (img.load(IMG.hud) == IMG.hud));
    kept = IMG.sky;
    img.drop(IMG.sky);
    cls(); blit(kept, 0, 0);
    printh("kept " + pget(0, 0) + " " + (kept == IMG.sky));
    blit(IMG.sky, 0, 0);
}
function update() {}`
    };
    const r = run(folder('lazyimg', files));
    assert.deepEqual(r.errors, []);
    assert.deepEqual(r.out.trim().split('\n'), ['size 20x10 4', 'loaded 1 1 1 ' + (1 + 199 % 9),
                                               'hud 1', 'kept 1 0']);
    assert.match(r.err, /blit\(\) needs SCREEN or an image, not a lazy image not yet loaded with img\.load\(\)/);
    const bad = run(folder('lazybad', Object.assign({}, files, {
        'cart.json': JSON.stringify({ id: '00000000000a55e9', name: 'Lazy', lazy_images: ['nope'] }) })));
    assert.match(bad.errors.map((e) => e.message).join(), /lazy_images names nope/);
});

test('the loader refuses damaged assets, and loadmap and mapprop check their arguments', () => {
    const dir = folder('damaged', assetCart());
    const good = run(dir);
    const { list } = readCart(good.cart);
    /* a map's height, the sheet's packed size, an image's height, a
     * palette's length; and a byte of the first map's cells, its checksum
     * left as it was, as when the file changes after the cart loads */
    const at = { MAPS: 4, SHET: 4, IMGS: 4, PALS: 1, MAPC: 3 };
    for (const tag in at) {
        let first = true;
        const chunks = list.map((c) => {
            if (c.tag !== tag || !first) return c;
            first = false;
            const d = Uint8Array.from(c.data);
            d[at[tag]] ^= 0x5a;
            return { tag: c.tag, data: d };
        });
        let bytes = writeCart(chunks);
        if (tag === 'MAPC') {
            bytes = Uint8Array.from(good.cart);
            const k = list.findIndex((c) => c.tag === 'MAPC');
            const start = list.slice(0, k).reduce((n, c) => n + 12 + c.data.length, 0);
            bytes[start + 8 + at.MAPC] ^= 0x5a;
        }
        const file = path.join(dir, 'damaged.pbg');
        fs.writeFileSync(file, bytes);
        const r = spawnSync(exe, [file, '--ticks', '1'], { encoding: 'utf8', timeout: 20000 });
        assert.notEqual(r.status, 0, tag + ' was accepted');
        assert.match(r.stderr, /damaged/, tag);
    }
    const bad = (src) => {
        const r = run(folder('args' + Math.abs(src.length), { 'cart.json': CART,
            'main.js': 'function init() { ' + src + ' }\nfunction update() {}' }));
        return r.err;
    };
    assert.match(bad('loadmap(0);'), /loadmap\(\) needs one of the cart's maps/);
    assert.match(bad('mapprop(0, "x");'), /mapprop\(\) needs an object of the active map/);
    assert.match(bad('loadpal(0);'), /loadpal\(\) needs one of the cart's palettes/);
});

test('cart.json names the sprite flags: FLAG.<name> is the flag as a set of one', () => {
    const cart = (flags) => JSON.stringify({ id: '00000000000f1a65', name: 'Flags', flags: flags });
    const main = 'function init() { printh(FLAG.solid + " " + FLAG.ladder + " " + (FLAG.solid | FLAG.ladder)); }\nfunction update() {}';
    const ok = compileFiles({ 'cart.json': cart(['solid', null, 'ladder']), 'main.js': main });
    assert.deepEqual(ok.errors, []);
    assert.deepEqual(ok.groups.FLAG, { solid: { value: 1, version: 1 }, ladder: { value: 4, version: 1 } });
    const why = (flags, code) => compileFiles({ 'cart.json': cart(flags),
        'main.js': (code || 'function init() {}') + '\nfunction update() {}' }).errors.map((e) => e.message);
    assert.match(why('solid')[0], /flags must be a list/);
    assert.match(why(['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i'])[0], /at most 8 names/);
    assert.match(why(['solid', 'solid'])[0], /a second flag called solid/);
    assert.match(why(['no way'])[0], /usable in code as FLAG/);
    assert.ok(why(['solid'], 'function init() { printh(FLAG.ladder); }').length > 0);
});
