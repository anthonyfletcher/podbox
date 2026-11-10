#!/usr/bin/env node
/* pbgc: the Arcade cart compiler.
 *
 *   pbgc new <folder> <name>        a new cart folder, with its id
 *   pbgc build <folder> [-o file]   compile; the cart defaults to <folder>.pbg
 *   pbgc pack <folder> [-o file]    the same: build is unpack's inverse
 *   pbgc unpack <file.pbg> <folder> the cart's folder again, from its sources
 *   pbgc dis <file.pbg>             verify a cart and list its bytecode
 *   pbgc html <folder|file.pbg> [-o file.html] [--wheel touch|always|never]
 *             [--scale fit|whole|1|2|3|4] [--font font.bin]
 *                                   the cart as a web page to play it in
 */

import fs from 'node:fs';
import path from 'node:path';
import { newCartId, asNewGame } from './src/folder.js';
import zlib from 'node:zlib';
import { compileFiles, readFolder, readAssets, loadDefs } from './src/node.js';
import { unpackCart } from './src/unpack.js';
import { load, verify, disassemble } from './src/bytecode.js';
import { webPage, musicCalls, musicRefusal, needsWorld, WHEELS, SCALES } from './src/webpage.js';

function usage() {
    console.error('usage: pbgc new <folder> <name>\n' +
                  '       pbgc build <folder> [-o file.pbg]\n' +
                  '       pbgc pack <folder> [-o file.pbg]\n' +
                  '       pbgc unpack <file.pbg> <folder>\n' +
                  '       pbgc copy <folder|file.pbg> <folder> [name]\n' +
                  '       pbgc dis <file.pbg>\n' +
                  '       pbgc html <folder|file.pbg> [-o file.html] [--wheel touch|always|never]\n' +
                  '                 [--scale fit|whole|1|2|3|4] [--font font.bin]');
    process.exit(2);
}

const MAIN = `function init() {
}

function update() {
}

function draw() {
    cls(1);
    print("hello", 44, 56, 7);
}
`;

function cmdNew(dir, name) {
    if (!dir || !name) usage();
    if (fs.existsSync(dir)) {
        console.error(dir + ' already exists');
        process.exit(1);
    }
    fs.mkdirSync(dir, { recursive: true });
    const cart = { id: newCartId(), name: name,
                   author: '', version: '1', rate: 33 };
    fs.writeFileSync(path.join(dir, 'cart.json'), JSON.stringify(cart, null, 2) + '\n');
    fs.writeFileSync(path.join(dir, 'main.js'), MAIN);
    console.log('made ' + dir);
}

function cmdBuild(dir, out) {
    if (!dir) usage();
    const res = compileFiles(readFolder(dir), readAssets(dir));
    if (res.errors.length) {
        for (const e of res.errors)
            console.error(path.join(dir, e.file) + ':' + e.line + ':' + e.column +
                          ': ' + e.message);
        process.exit(1);
    }
    const file = out || dir.replace(/[\\/]+$/, '') + '.pbg';
    const d = loadDefs();
    verify(d.isa, d.api, load(res.cart));
    fs.writeFileSync(file, res.cart);
    const s = res.stats;
    console.log(file + ': ' + s.size + ' bytes, ' + s.functions + ' functions (' +
                s.dropped + ' unused dropped), ' + s.code + ' bytes of code, ' +
                s.globals + ' globals, API ' + s.api);
}

/* dir, which must be empty or not there */
function freshDir(dir) {
    if (fs.existsSync(dir) && fs.readdirSync(dir).length) {
        console.error(dir + ' is not empty');
        process.exit(1);
    }
}

function cmdUnpack(file, dir) {
    if (!file || !dir) usage();
    freshDir(dir);
    const files = unpackCart(new Uint8Array(fs.readFileSync(file)),
                             (b) => new Uint8Array(zlib.inflateRawSync(b)),
                             (b) => new Uint8Array(zlib.deflateSync(b, { level: 9 })));
    /* every path checked before any is written */
    const root = path.resolve(dir);
    const names = Object.keys(files).sort();
    const paths = names.map((name) => {
        const to = path.resolve(root, ...name.split('/'));
        if (!to.startsWith(root + path.sep))
            throw new Error('the cart names a file outside ' + dir + ': ' + name);
        return to;
    });
    names.forEach((name, i) => {
        fs.mkdirSync(path.dirname(paths[i]), { recursive: true });
        fs.writeFileSync(paths[i], files[name]);
    });
    console.log(dir + ': ' + Object.keys(files).length + ' files');
}

/* A new game made from a cart: its folder, or a .pbg unpacked, with a new
 * id, so its saves and high scores start empty, and the name given */
function cmdCopy(src, dir, name) {
    if (!src || !dir) usage();
    freshDir(dir);
    if (fs.statSync(src).isDirectory()) {
        if (!fs.existsSync(path.join(src, 'cart.json'))) {
            console.error(src + ' has no cart.json');
            process.exit(1);
        }
        fs.cpSync(src, dir, { recursive: true });
    } else
        cmdUnpack(src, dir);
    const at = path.join(dir, 'cart.json');
    const text = asNewGame(fs.readFileSync(at, 'utf8'), name);
    fs.writeFileSync(at, text);
    const c = JSON.parse(text);
    console.log(dir + ': ' + c.name + ', a new game, id ' + c.id);
}

function cmdDis(file) {
    if (!file) usage();
    const d = loadDefs();
    const prog = load(new Uint8Array(fs.readFileSync(file)));
    verify(d.isa, d.api, prog);
    console.log(disassemble(d.isa, d.api, prog));
}

/* A cart, from its folder or its .pbg, as one HTML page that plays it.
 * A cart that uses the owner's music has none on the web, so it is refused. */
async function cmdHtml(src, out, wheel, scale, fontFile) {
    if (!src) usage();
    let cart, files;
    if (/\.pbg$/i.test(src)) {
        cart = new Uint8Array(fs.readFileSync(src));
        files = unpackCart(cart, (b) => new Uint8Array(zlib.inflateRawSync(b)),
                           (b) => new Uint8Array(zlib.deflateSync(b, { level: 9 })));
    } else {
        files = readFolder(src);
        const res = compileFiles(files, readAssets(src));
        if (res.errors.length) {
            for (const e of res.errors)
                console.error(path.join(src, e.file) + ':' + e.line + ':' + e.column + ': ' + e.message);
            process.exit(1);
        }
        cart = res.cart;
    }
    const d = loadDefs();
    verify(d.isa, d.api, load(cart));
    const music = musicCalls(cart, d.isa, d.api);
    if (music.length) {
        console.error('pbgc: ' + musicRefusal(src, music));
        process.exit(1);
    }
    if (!WHEELS.includes(wheel)) usage();
    if (!SCALES.includes(scale)) usage();
    const meta = JSON.parse(typeof files['cart.json'] === 'string'
        ? files['cart.json'] : new TextDecoder().decode(files['cart.json']));
    if (!fontFile && needsWorld(files))
        console.error('pbgc: warning: the cart has kana, Hangul or ideographs, which draw as ? ' +
                      'without --font font.bin');
    const { playerParts } = await import('./web/bundle.js');
    const parts = await playerParts();
    const wasm = fs.readFileSync(new URL('./wasm/arcade.wasm', import.meta.url));
    const page = webPage({
        template: parts.template, script: parts.script, cart: cart,
        wasmGz: zlib.gzipSync(wasm, { level: 9 }),
        fontGz: fontFile ? zlib.gzipSync(fs.readFileSync(fontFile), { level: 9 }) : null,
        meta: meta, wheel: wheel, scale: scale
    });
    const file = out || src.replace(/[\\/]+$/, '').replace(/\.pbg$/i, '') + '.html';
    fs.writeFileSync(file, page);
    console.log(file + ': ' + Math.round(page.length / 1024) + ' KB');
}

const argv = process.argv.slice(2);
try {
    switch (argv[0]) {
    case 'new': cmdNew(argv[1], argv[2]); break;
    case 'build':
    case 'pack': {
        const o = argv.indexOf('-o');
        cmdBuild(argv[1], o > 0 ? argv[o + 1] : null);
        break;
    }
    case 'unpack': cmdUnpack(argv[1], argv[2]); break;
    case 'copy': cmdCopy(argv[1], argv[2], argv[3]); break;
    case 'dis': cmdDis(argv[1]); break;
    case 'html': {
        const opt = (k, v) => { const i = argv.indexOf(k); return i > 0 ? argv[i + 1] : v; };
        await cmdHtml(argv[1], opt('-o', null), opt('--wheel', 'touch'), opt('--scale', 'fit'),
                      opt('--font', null));
        break;
    }
    default: usage();
    }
} catch (e) {
    console.error('pbgc: ' + e.message);
    process.exit(1);
}
