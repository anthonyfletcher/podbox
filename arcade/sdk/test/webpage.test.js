/* A cart as a web page: what goes in the page, and which carts are kept
 * out of it, through src/webpage.js and pbgc html. */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { compileFiles, loadDefs } from '../src/node.js';
import { webPage, musicCalls, needsWorld } from '../src/webpage.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const pbgc = path.join(here, '..', 'pbgc.js');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-web-'));

function files(update, name = 'Web <Test>') {
    return {
        'cart.json': JSON.stringify({ id: '00000000000eb7e5', name: name, help: 'Press Select.',
            options: [{ name: 'speed', type: 'number', min: 1, max: 9 }] }),
        'main.js': 'function init() {}\nfunction update() { ' + update + ' }\n' +
                   'function draw() { cls(1); }\n'
    };
}

function cart(update) {
    const res = compileFiles(files(update));
    assert.deepEqual(res.errors, []);
    return res.cart;
}

test('a cart that reads the owner\'s music is found out, by each call', () => {
    const d = loadDefs();
    assert.deepEqual(musicCalls(cart('var t = playback.title(); beat.listen(1);'), d.isa, d.api),
                     ['beat.listen()', 'playback.title()']);
    assert.deepEqual(musicCalls(cart('var n = tracks.count();'), d.isa, d.api), ['tracks.count()']);
    assert.deepEqual(musicCalls(cart('var x = rnd(4);'), d.isa, d.api), []);
});

test('text past U+3000 asks for font.bin; Latin and Greek do not', () => {
    assert.equal(needsWorld({ 'main.js': 'print("café αβ")' }), false);
    assert.equal(needsWorld({ 'data/lines.txt': 'こんにちは' }), true);
    assert.equal(needsWorld({ 'sheet.png': new Uint8Array([0xe3, 0x81, 0x93]) }), false);
});

test('the page carries the cart, the engine gzipped, and the choices made', () => {
    const c = cart('');
    const wasm = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]);
    const page = webPage({
        template: '<title></title><a id="source">x</a>' +
                  '<script id="cart-data" type="application/json"></script><script>/*SCRIPT*/</script>',
        script: 'go("</script>")', cart: c, wasmGz: zlib.gzipSync(wasm), fontGz: null,
        meta: JSON.parse(files('')['cart.json']), wheel: 'always', scale: '2'
    });
    assert.match(page, /<title>Web &lt;Test&gt;<\/title>/);
    assert.match(page, /<a id="source" href="https:\/\/github.com\/[^"]+"/);
    assert.match(page, /go\("<\\\/script>"\)/);
    const json = /<script id="cart-data" type="application\/json">(.*?)<\/script>/.exec(page)[1];
    assert.ok(!json.includes('<'), 'nothing in the data can end its script');
    const data = JSON.parse(json);
    assert.equal(data.name, 'Web <Test>');
    assert.equal(data.wheel, 'always');
    assert.equal(data.scale, '2');
    assert.equal(data.font, null);
    assert.equal(data.options[0].name, 'speed');
    assert.deepEqual(new Uint8Array(Buffer.from(data.cart, 'base64')), c);
    assert.deepEqual(new Uint8Array(zlib.gunzipSync(Buffer.from(data.wasm, 'base64'))), wasm);
    assert.throws(() => webPage({ template: '', script: '', cart: c, wasmGz: wasm, wheel: 'big', scale: 'fit' }));
    assert.throws(() => webPage({ template: '', script: '', cart: c, wasmGz: wasm, wheel: 'never', scale: '5' }));
});

function folder(name, update) {
    const dir = path.join(tmp, name);
    fs.mkdirSync(dir, { recursive: true });
    for (const [f, text] of Object.entries(files(update))) fs.writeFileSync(path.join(dir, f), text);
    return dir;
}

test('pbgc html writes a page from a folder or a .pbg, and refuses a music cart', () => {
    const plain = folder('plain', '');
    const out = path.join(tmp, 'plain.html');
    let r = spawnSync(process.execPath, [pbgc, 'html', plain, '-o', out, '--scale', 'whole'], { encoding: 'utf8' });
    assert.equal(r.status, 0, r.stderr);
    const page = fs.readFileSync(out, 'utf8');
    assert.match(page, /"scale":"whole"/);
    assert.match(page, /"wheel":"touch"/);
    assert.match(page, /DecompressionStream/);
    assert.match(page, /<script id="licences" type="text\/plain">[^<]*SIL OPEN FONT LICENSE[^<]*cloudlibc/);

    const pbg = path.join(tmp, 'plain.pbg');
    fs.writeFileSync(pbg, cart(''));
    r = spawnSync(process.execPath, [pbgc, 'html', pbg], { encoding: 'utf8' });
    assert.equal(r.status, 0, r.stderr);
    assert.ok(fs.existsSync(path.join(tmp, 'plain.html')));

    const music = folder('music', 'var a = playback.artist();');
    r = spawnSync(process.execPath, [pbgc, 'html', music, '-o', path.join(tmp, 'music.html')], { encoding: 'utf8' });
    assert.equal(r.status, 1);
    assert.match(r.stderr, /owner's music \(playback\.artist\(\)\)/);
    assert.ok(!fs.existsSync(path.join(tmp, 'music.html')));

    r = spawnSync(process.execPath, [pbgc, 'html', plain, '--wheel', 'sometimes'], { encoding: 'utf8' });
    assert.equal(r.status, 2);
});
