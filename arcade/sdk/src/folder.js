/* A cart's folder as a map of its files ({ "maps/level1.json": text or
 * bytes }) and as the compiler takes it: the code, and the assets by kind
 * as assets.js describes them. The IDE keeps a cart this way. */

const KINDS = [
    ['palettes', 'palettes/', '.gpl'], ['maps', 'maps/', '.json'],
    ['images', 'images/', '.png'], ['sounds', 'sounds/', '.json'],
    ['data', 'data/', '.json'], ['texts', 'data/', '.txt'], ['fonts', 'fonts/', '.bdf'],
    ['editors', 'editors/', '.html']
];

const asText = (v) => (typeof v === 'string' ? v : new TextDecoder().decode(v));
const asBytes = (v) => (typeof v === 'string' ? new TextEncoder().encode(v) : v);

/* A cart's id: 16 hex digits, at random. Saves and high scores are kept
 * under it, so a game made from another takes a new one. */
export function newCartId() {
    return Array.from(globalThis.crypto.getRandomValues(new Uint8Array(8)),
                      (b) => b.toString(16).padStart(2, '0')).join('');
}

/* cart.json's text for a new game made from this one: a new id, and the
 * name given, if any */
export function asNewGame(cartJson, name) {
    const c = JSON.parse(cartJson);
    c.id = newCartId();
    if (name) c.name = name;
    return JSON.stringify(c, null, 2) + '\n';
}

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
        if (name === 'logo.png') { assets.logo = asBytes(v); continue; }
        if (name === 'flags.json') { assets.flags = asText(v); continue; }
        if (name === 'objects.json') { assets.objects = asText(v); continue; }
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
