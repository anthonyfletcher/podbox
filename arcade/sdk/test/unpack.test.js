/* pbgc unpack and pack: a cart into its folder and back, exactly. */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import zlib from 'node:zlib';
import { compileFiles } from '../src/node.js';
import { unpackCart, cartLogo } from '../src/unpack.js';
import { encodeIndexedPng, decodeIndexedPng } from '../src/png.js';
import { readCart, writeCart } from '../src/cart.js';
import { compileInput, asNewGame } from '../src/folder.js';

const deflate = (b) => new Uint8Array(zlib.deflateSync(b));
const inflate = (b) => new Uint8Array(zlib.inflateSync(b));
const inflateRaw = (b) => new Uint8Array(zlib.inflateRawSync(b));
const text = (b) => new TextDecoder().decode(b);

function picture(w, h, seed) {
    const px = new Uint8Array(w * h);
    for (let i = 0; i < px.length; i++) px[i] = (i * 7 + seed) % 23;
    return px;
}

/* A cart with one of every kind of file */
const CODE = {
    'cart.json': JSON.stringify({ id: '00000000000a7b0c', name: 'Every File' }, null, 2) + '\n',
    'main.js': '// the main file, its comments kept\nfunction init() { printh(enemy.speed()); }\nfunction update() {}\n',
    'enemy.js': 'function speed() {\n    return 3; // a namespace\n}\n'
};
const ASSETS = {
    sheet: encodeIndexedPng(128, 128, picture(128, 128, 1), [0x102030], deflate),
    logo: encodeIndexedPng(44, 44, picture(44, 44, 4), [0x102030], deflate),
    flags: '{ "1": 3, "9": 128 }\n',
    objects: '{ "door": { "help": "A way out", "props": { "to": { "default": "" },\n' +
             '  "key": { "default": 0 } } } }\n',
    palettes: { day: 'GIMP Palette\nName: day\n#\n  0   0   0 black\n255 255 255 white\n',
                night: 'GIMP Palette\n10 10 40\n200 200 255\n' },
    maps: { level1: JSON.stringify({ width: 4, height: 2, rows: ['01020304', '05060708'],
                                     objects: [{ kind: 'door', x: 8, y: 0, w: 8, h: 8,
                                                 props: { to: 'level2', key: 3 } }] }, null, 1) },
    images: { title: encodeIndexedPng(40, 30, picture(40, 30, 2), [0xff0000], deflate),
              hud: encodeIndexedPng(5, 300, picture(5, 300, 3), [], deflate) },
    sounds: { coin: '{"wave_type":0,"p_env_sustain":0.08,"p_env_decay":0.12,"p_base_freq":0.45}' },
    data: { waves: '[\n  { "count": 3 },\n  { "count": 5 }\n]\n' },
    texts: { story: 'Once, upon a time.\nThe end.\n' },
    fonts: { tiny: 'STARTFONT 2.1\nSTARTPROPERTIES 2\nFONT_ASCENT 2\nFONT_DESCENT 0\nENDPROPERTIES\n' +
                   'CHARS 1\nSTARTCHAR a\nENCODING 65\nDWIDTH 3 0\nBBX 2 2 0 0\nBITMAP\nC0\nC0\nENDCHAR\nENDFONT\n' }
};

function build(files, assets) {
    const res = compileFiles(files, assets);
    assert.deepEqual(res.errors, []);
    return res.cart;
}

test('unpack gives back every file as written, and pack the same cart', () => {
    /* the title lazy: its pixels in a chunk of their own */
    const code = Object.assign({}, CODE, { 'cart.json': JSON.stringify(
        { id: '00000000000a7b0c', name: 'Every File', lazy_images: ['title'] }, null, 2) + '\n' });
    const cart = build(code, ASSETS);
    const files = unpackCart(cart, inflateRaw, deflate);
    assert.deepEqual(Object.keys(files).sort(), [
        'cart.json', 'data/story.txt', 'data/waves.json', 'enemy.js', 'flags.json',
        'fonts/tiny.bdf', 'images/hud.png', 'images/title.png', 'logo.png', 'main.js', 'maps/level1.json',
        'objects.json', 'palettes/day.gpl', 'palettes/night.gpl', 'sheet.png', 'sounds/coin.json']);
    for (const [name, want] of Object.entries(code)) assert.equal(text(files[name]), want, name);
    assert.deepEqual(decodeIndexedPng(files['images/title.png'], inflate).px, picture(40, 30, 2));
    assert.equal(text(files['flags.json']), ASSETS.flags);
    assert.equal(text(files['objects.json']), ASSETS.objects);
    assert.equal(text(files['maps/level1.json']), ASSETS.maps.level1);
    assert.equal(text(files['data/waves.json']), ASSETS.data.waves);
    assert.equal(text(files['palettes/day.gpl']), ASSETS.palettes.day);
    /* the pictures: the same indices, in the cart's first palette, 0 clear */
    assert.deepEqual(decodeIndexedPng(files['sheet.png'], inflate).px, picture(128, 128, 1));
    assert.deepEqual(decodeIndexedPng(files['logo.png'], inflate).px, picture(44, 44, 4));
    const hud = decodeIndexedPng(files['images/hud.png'], inflate);
    assert.deepEqual([hud.width, hud.height], [5, 300]);
    assert.deepEqual(hud.px, picture(5, 300, 3));
    assert.ok(Buffer.from(files['sheet.png']).includes(Buffer.from([0, 0, 0, 255, 255, 255])));
    assert.ok(Buffer.from(files['sheet.png']).includes(Buffer.from('tRNS')));
    /* and packed, the same bytes */
    const input = compileInput(files);
    assert.ok(Buffer.from(build(input.files, input.assets)).equals(Buffer.from(cart)));
});

test('a cart without palettes unpacks in the engine\'s sixteen; one without SRC_ is refused', () => {
    const cart = build({ 'cart.json': CODE['cart.json'], 'main.js': 'function init() {} function update() {}' },
                       { sheet: ASSETS.sheet });
    const files = unpackCart(cart, inflateRaw, deflate);
    /* entry 1, dark blue */
    assert.ok(Buffer.from(files['sheet.png']).includes(Buffer.from([0, 0, 0, 0x1a, 0x22, 0x44])));
    const { chunks, order } = readCart(cart);
    const stripped = writeCart(order.filter((t) => t !== 'SRC_').map((t) => ({ tag: t, data: chunks[t] })));
    assert.throws(() => unpackCart(stripped, inflateRaw, deflate), /holds no sources/);
});

test('a cart built before its assets were kept says to build it again', () => {
    const cart = build(CODE, ASSETS);
    const { chunks, order } = readCart(cart);
    /* SRC_ as it was: the code alone */
    const w = [], names = ['cart.json', 'enemy.js', 'main.js'];
    const u16 = (v) => w.push(v & 255, v >> 8), u32 = (v) => { u16(v & 0xffff); u16(v >>> 16); };
    u16(names.length);
    for (const n of names) {
        const b = Buffer.from(n), t = Buffer.from(CODE[n]);
        u16(b.length); w.push(...b); u32(t.length); w.push(...t);
    }
    const src = new Uint8Array(zlib.deflateRawSync(Buffer.from(w)));
    const old = writeCart(order.map((t) => ({ tag: t, data: t === 'SRC_' ? src : chunks[t] })));
    assert.throws(() => unpackCart(old, inflateRaw, deflate), /build it again from its folder/);
});

test('a source named outside the folder\'s shapes is refused, never written', () => {
    const { chunks, order } = readCart(build(CODE, {}));
    const withName = (name) => {
        const w = [], u16 = (v) => w.push(v & 255, v >> 8);
        const b = Buffer.from(name);
        u16(1); u16(b.length); w.push(...b); u16(1); u16(0); w.push(120);
        const src = new Uint8Array(zlib.deflateRawSync(Buffer.from(w)));
        return writeCart(order.map((t) => ({ tag: t, data: t === 'SRC_' ? src : chunks[t] })));
    };
    for (const name of ['../escaped.txt', '..\\escaped.js', 'a/../../x.js', '/etc/x.js',
                        'C:x.js', '.js', 'maps/../../x.json', 'maps/a-b.json', 'data/x.png',
                        'images/sub/x.png', 'notes.txt'])
        assert.throws(() => unpackCart(withName(name), inflateRaw, deflate),
                      /names a file no cart folder holds/, name);
    assert.deepEqual(Object.keys(unpackCart(withName('my-lib.js'), inflateRaw, deflate)),
                     ['my-lib.js']);
});

test('a logo is 44x44, and comes out in the cart\'s first palette', () => {
    const wrong = compileFiles(CODE, { logo: encodeIndexedPng(40, 44, picture(40, 44, 1), [], deflate) });
    assert.ok(wrong.errors.some((e) => e.file === 'logo.png' && /44x44, not 40x44/.test(e.message)));
    const logo = cartLogo(build(CODE, { logo: ASSETS.logo, palettes: ASSETS.palettes }));
    assert.deepEqual([logo.width, logo.height], [44, 44]);
    /* palettes/day, the first by name: index 1 is white */
    assert.equal(logo.rgb[picture(44, 44, 4).indexOf(1)], 0xffffff);
    assert.equal(cartLogo(build(CODE, {})), null);
});

test('a new game made from a cart: a new id, the name given, the rest kept', () => {
    const was = JSON.parse(CODE['cart.json']);
    const made = JSON.parse(asNewGame(CODE['cart.json'], 'Every File 2'));
    assert.match(made.id, /^[0-9a-f]{16}$/);
    assert.notEqual(made.id, was.id);
    assert.equal(made.name, 'Every File 2');
    assert.equal(JSON.parse(asNewGame(CODE['cart.json'])).name, 'Every File');
    assert.deepEqual(Object.keys(made), Object.keys(was));
});
