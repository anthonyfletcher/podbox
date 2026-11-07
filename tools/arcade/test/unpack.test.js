/* pbgc unpack and pack: a cart into its folder and back, exactly. */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import zlib from 'node:zlib';
import { compileFiles } from '../src/node.js';
import { unpackCart } from '../src/unpack.js';
import { encodeIndexedPng, decodeIndexedPng } from '../src/png.js';
import { readCart, writeCart } from '../src/cart.js';

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
    flags: '{ "1": 3, "9": 128 }\n',
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

/* The folder unpack wrote, as compileFiles takes it */
function asFolder(files) {
    const code = {}, assets = { palettes: {}, maps: {}, images: {}, sounds: {}, data: {},
                                texts: {}, fonts: {} };
    const dirs = { palettes: ['palettes', '.gpl'], maps: ['maps', '.json'], images: ['images', '.png'],
                   sounds: ['sounds', '.json'], data: ['data', '.json'], texts: ['data', '.txt'],
                   fonts: ['fonts', '.bdf'] };
    for (const [name, bytes] of Object.entries(files)) {
        if (!name.includes('/')) {
            if (name === 'sheet.png') assets.sheet = bytes;
            else if (name === 'flags.json') assets.flags = text(bytes);
            else code[name] = text(bytes);
            continue;
        }
        for (const [kind, [dir, ext]] of Object.entries(dirs))
            if (name.startsWith(dir + '/') && name.endsWith(ext))
                assets[kind][name.slice(dir.length + 1, -ext.length)] =
                    kind === 'images' ? bytes : text(bytes);
    }
    return [code, assets];
}

test('unpack gives back every file as written, and pack the same cart', () => {
    const cart = build(CODE, ASSETS);
    const files = unpackCart(cart, inflateRaw, deflate);
    assert.deepEqual(Object.keys(files).sort(), [
        'cart.json', 'data/story.txt', 'data/waves.json', 'enemy.js', 'flags.json',
        'fonts/tiny.bdf', 'images/hud.png', 'images/title.png', 'main.js', 'maps/level1.json',
        'palettes/day.gpl', 'palettes/night.gpl', 'sheet.png', 'sounds/coin.json']);
    for (const [name, want] of Object.entries(CODE)) assert.equal(text(files[name]), want, name);
    assert.equal(text(files['flags.json']), ASSETS.flags);
    assert.equal(text(files['maps/level1.json']), ASSETS.maps.level1);
    assert.equal(text(files['data/waves.json']), ASSETS.data.waves);
    assert.equal(text(files['palettes/day.gpl']), ASSETS.palettes.day);
    /* the pictures: the same indices, in the cart's first palette, 0 clear */
    assert.deepEqual(decodeIndexedPng(files['sheet.png'], inflate).px, picture(128, 128, 1));
    const hud = decodeIndexedPng(files['images/hud.png'], inflate);
    assert.deepEqual([hud.width, hud.height], [5, 300]);
    assert.deepEqual(hud.px, picture(5, 300, 3));
    assert.ok(Buffer.from(files['sheet.png']).includes(Buffer.from([0, 0, 0, 255, 255, 255])));
    assert.ok(Buffer.from(files['sheet.png']).includes(Buffer.from('tRNS')));
    /* and packed, the same bytes */
    const [code, assets] = asFolder(files);
    assert.ok(Buffer.from(build(code, assets)).equals(Buffer.from(cart)));
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
