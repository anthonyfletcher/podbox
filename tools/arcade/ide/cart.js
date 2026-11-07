/* The IDE's side of a cart: building it in the browser, opening a .pbg,
 * and the names the editor completes. The engine's definitions come in as
 * text when the IDE is built, so the compiler here is pbgc's exactly. */

import { deflateSync, inflateSync, unzlibSync, zlibSync } from 'fflate';
import { compile } from '../src/compiler.js';
import { parseOpcodes, parseApi } from '../src/defs.js';
import { unpackCart } from '../src/unpack.js';
import { compileInput, isBinary } from '../src/folder.js';
import opcodesText from '../../../apps-ipod/games/arcade/opcodes.def';
import apiText from '../../../apps-ipod/games/arcade/arcade_api.def';

const isa = parseOpcodes(opcodesText);
const api = parseApi(apiText);

/* { cart, errors, stats } from the folder's files */
export function build(folder) {
    const { files, assets } = compileInput(folder);
    return compile({ files: files, assets: assets, isa: isa, api: api,
                     deflate: (b) => deflateSync(b, { level: 9 }),
                     inflate: (b) => unzlibSync(b) });
}

/* the folder a .pbg was built from: text as text, pictures as bytes */
export function open(bytes) {
    const files = unpackCart(bytes, (b) => inflateSync(b), (b) => zlibSync(b, { level: 9 }));
    for (const name of Object.keys(files))
        if (!isBinary(name)) files[name] = new TextDecoder().decode(files[name]);
    return files;
}

/* What the editor offers: the natives, constants and groups at the top,
 * and after a dot each system's functions or a group's constants. The
 * cart's own groups (MAP., FONT.<name>) come from its last build. */
export function names(built) {
    const members = {};
    for (const [ns, fns] of Object.entries(api.namespaces)) members[ns] = Object.keys(fns);
    for (const [g, m] of Object.entries(api.groups)) members[g] = Object.keys(m);
    for (const [g, m] of Object.entries((built && built.groups) || {}))
        members[g] = [...new Set([...(members[g] || []), ...Object.keys(m)])];
    const top = [...Object.keys(api.natives), ...Object.keys(api.consts), ...Object.keys(members)];
    return { top: [...new Set(top)].sort(), members: members };
}

export const NEW_CART = () => {
    const id = Array.from(crypto.getRandomValues(new Uint8Array(8)),
                          (b) => b.toString(16).padStart(2, '0')).join('');
    return {
        'cart.json': JSON.stringify({ id: id, name: 'New Game', author: '', version: '1', rate: 33 },
                                    null, 2) + '\n',
        'main.js': 'function init() {\n}\n\nfunction update() {\n}\n\nfunction draw() {\n' +
                   '    cls(1);\n    print("hello", 66, 56, 7);\n}\n'
    };
};
