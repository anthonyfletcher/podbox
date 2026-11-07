/* A cart's folder as a map of its files ({ "maps/level1.json": text or
 * bytes }) and as the compiler takes it: the code, and the assets by kind
 * as assets.js describes them. The IDE keeps a cart this way. */

const KINDS = [
    ['palettes', 'palettes/', '.gpl'], ['maps', 'maps/', '.json'],
    ['images', 'images/', '.png'], ['sounds', 'sounds/', '.json'],
    ['data', 'data/', '.json'], ['texts', 'data/', '.txt'], ['fonts', 'fonts/', '.bdf']
];

const asText = (v) => (typeof v === 'string' ? v : new TextDecoder().decode(v));
const asBytes = (v) => (typeof v === 'string' ? new TextEncoder().encode(v) : v);

/* Whether a file of the folder is kept as bytes rather than text */
export function isBinary(name) {
    return /\.png$/.test(name);
}

/* { files, assets } for compile() */
export function compileInput(folder) {
    const files = {}, assets = {};
    for (const [kind] of KINDS) assets[kind] = {};
    for (const [name, v] of Object.entries(folder)) {
        if (name === 'sheet.png') { assets.sheet = asBytes(v); continue; }
        if (name === 'flags.json') { assets.flags = asText(v); continue; }
        if (!name.includes('/')) {
            if (/\.js$/.test(name) || name === 'cart.json') files[name] = asText(v);
            continue;
        }
        for (const [kind, dir, ext] of KINDS)
            if (name.startsWith(dir) && name.endsWith(ext) &&
                !name.slice(dir.length).includes('/'))
                assets[kind][name.slice(dir.length, -ext.length)] =
                    kind === 'images' ? asBytes(v) : asText(v);
    }
    return { files, assets };
}
