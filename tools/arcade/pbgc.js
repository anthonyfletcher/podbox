#!/usr/bin/env node
/* pbgc: the Arcade cart compiler.
 *
 *   pbgc new <folder> <name>        a new cart folder, with its id
 *   pbgc build <folder> [-o file]   compile; the cart defaults to <folder>.pbg
 *   pbgc dis <file.pbg>             verify a cart and list its bytecode
 */

import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { compileFiles, readFolder, readAssets, loadDefs } from './src/node.js';
import { load, verify, disassemble } from './src/bytecode.js';

function usage() {
    console.error('usage: pbgc new <folder> <name>\n' +
                  '       pbgc build <folder> [-o file.pbg]\n' +
                  '       pbgc dis <file.pbg>');
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
    const cart = { id: crypto.randomBytes(8).toString('hex'), name: name,
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

function cmdDis(file) {
    if (!file) usage();
    const d = loadDefs();
    const prog = load(new Uint8Array(fs.readFileSync(file)));
    verify(d.isa, d.api, prog);
    console.log(disassemble(d.isa, d.api, prog));
}

const argv = process.argv.slice(2);
try {
    switch (argv[0]) {
    case 'new': cmdNew(argv[1], argv[2]); break;
    case 'build': {
        const o = argv.indexOf('-o');
        cmdBuild(argv[1], o > 0 ? argv[o + 1] : null);
        break;
    }
    case 'dis': cmdDis(argv[1]); break;
    default: usage();
    }
} catch (e) {
    console.error('pbgc: ' + e.message);
    process.exit(1);
}
