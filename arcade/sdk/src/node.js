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

/* the engine's .def files: from the checkout, or built into the one-file
 * compiler the build action carries (arcade/builder), which sets
 * globalThis.arcadeDefs before this is first asked */
export function loadDefs() {
    if (!defs) {
        const built = globalThis.arcadeDefs;
        const text = (name) => built ? built[name]
            : fs.readFileSync(path.join(engine, name), 'utf8');
        defs = {
            isa: parseOpcodes(text('opcodes.def')),
            api: parseApi(text('arcade_api.def'))
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

/* files: { name: text }; assets as readAssets() returns them. */
export function compileFiles(files, assets) {
    const d = loadDefs();
    return compile({ files: files, assets: assets, isa: d.isa, api: d.api,
                     deflate: deflate,
                     inflate: (z) => new Uint8Array(zlib.inflateSync(z)) });
}

/* A cart folder: cart.json and its .js files. */
export function readFolder(dir) {
    const files = {};
    for (const name of fs.readdirSync(dir)) {
        if (/\.js$/.test(name) || name === 'cart.json')
            files[name] = fs.readFileSync(path.join(dir, name), 'utf8');
    }
    return files;
}

/* Its assets, as assets.js describes them: PNGs as bytes, the rest as
 * text, each kind keyed by name. */
export function readAssets(dir) {
    const at = (...p) => path.join(dir, ...p);
    const some = (sub, ext, binary) => {
        const out = {};
        if (!fs.existsSync(at(sub))) return out;
        for (const name of fs.readdirSync(at(sub)))
            if (name.endsWith(ext))
                out[name.slice(0, -ext.length)] = binary
                    ? new Uint8Array(fs.readFileSync(at(sub, name)))
                    : fs.readFileSync(at(sub, name), 'utf8');
        return out;
    };
    return {
        sheet: fs.existsSync(at('sheet.png'))
            ? new Uint8Array(fs.readFileSync(at('sheet.png'))) : undefined,
        logo: fs.existsSync(at('logo.png'))
            ? new Uint8Array(fs.readFileSync(at('logo.png'))) : undefined,
        flags: fs.existsSync(at('flags.json'))
            ? fs.readFileSync(at('flags.json'), 'utf8') : undefined,
        objects: fs.existsSync(at('objects.json'))
            ? fs.readFileSync(at('objects.json'), 'utf8') : undefined,
        palettes: some('palettes', '.gpl', false),
        maps: some('maps', '.json', false),
        images: some('images', '.png', true),
        sounds: some('sounds', '.json', false),
        data: some('data', '.json', false),
        texts: some('data', '.txt', false),
        fonts: some('fonts', '.bdf', false),
        editors: some('editors', '.html', false)
    };
}
