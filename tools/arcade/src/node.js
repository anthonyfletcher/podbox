/* The Node side of the SDK: the engine's .def files from this checkout,
 * and compiling a cart folder. */

import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { fileURLToPath } from 'node:url';
import { parseOpcodes, parseApi } from './defs.js';
import { compile } from './compiler.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const engine = path.resolve(here, '../../../apps-ipod/games/arcade');

let defs = null;

export function loadDefs() {
    if (!defs) {
        defs = {
            isa: parseOpcodes(fs.readFileSync(path.join(engine, 'opcodes.def'), 'utf8')),
            api: parseApi(fs.readFileSync(path.join(engine, 'arcade_api.def'), 'utf8'))
        };
    }
    return defs;
}

export function deflate(bytes) {
    return new Uint8Array(zlib.deflateRawSync(bytes, { level: 9 }));
}

export function inflate(bytes) {
    return new Uint8Array(zlib.inflateRawSync(bytes));
}

/* files: { name: text }. */
export function compileFiles(files) {
    const d = loadDefs();
    return compile({ files: files, isa: d.isa, api: d.api, deflate: deflate });
}

/* A cart folder: cart.json and its .js files, no subfolders. */
export function readFolder(dir) {
    const files = {};
    for (const name of fs.readdirSync(dir)) {
        if (/\.js$/.test(name) || name === 'cart.json')
            files[name] = fs.readFileSync(path.join(dir, name), 'utf8');
    }
    return files;
}
