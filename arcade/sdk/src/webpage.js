/* A cart as a web page: one HTML file holding the engine, the cart and the
 * page's own runner (web/play.js), which plays it in a browser as an iPod
 * Classic would. pbgc's html command and the IDE's export both make it.
 *
 *   musicCalls(cart, isa, api)  the owner's-music calls a cart makes, by
 *                               name: a cart with any is not exported
 *   musicRefusal(name, calls)   the message saying so
 *   needsWorld(files)           whether a cart's text has characters only
 *                               font.bin draws (kana, Hangul, ideographs)
 *   webPage(o)                  the page, as text
 *
 * The engine and font go in gzipped, as base64, and the page unpacks them
 * with the browser's DecompressionStream. */

import { load, decode } from './bytecode.js';

/* the engine's source and licences, linked from every page */
export const SOURCE = 'https://github.com/anthonyfletcher/podbox#credits-and-licence';

/* when the drawn wheel shows, and how the screen is sized */
export const WHEELS = ['touch', 'always', 'never'];
export const SCALES = ['fit', 'whole', '1', '2', '3', '4'];

/* playback.colors() is not one: a page gets the theme's */
const MUSIC = /^(playback|beat|tracks)\./;
const NOT_MUSIC = new Set(['playback.colors']);

export function musicCalls(cart, isa, api) {
    const prog = load(cart);
    const found = new Set();
    for (const f of prog.fns) {
        for (let pc = 0; pc < f.code.length;) {
            const d = decode(isa, f.code, pc);
            if (d.op.name === 'NCALL') {
                const n = api.bySlot[d.args[0]];
                if (n && MUSIC.test(n.label || '') && !NOT_MUSIC.has(n.label)) found.add(n.label + '()');
            }
            pc = d.next;
        }
    }
    return [...found].sort();
}

/* why a cart with these music calls is not exported, for pbgc and the IDE */
export function musicRefusal(name, calls) {
    return name + ' cannot be exported as a web page: it uses the owner\'s music (' +
        calls.slice(0, 3).join(', ') + (calls.length > 3 ? ' and ' + (calls.length - 3) + ' more' : '') +
        '), and a web page has none';
}

/* files: { name: text or bytes }; only the text files are looked at */
export function needsWorld(files) {
    for (const [name, v] of Object.entries(files)) {
        if (!/\.(js|json|txt)$/.test(name)) continue;
        const s = typeof v === 'string' ? v : new TextDecoder().decode(v);
        for (const ch of s) if (ch.codePointAt(0) >= 0x3000) return true;
    }
    return false;
}

function base64(bytes) {
    let s = '';
    for (let i = 0; i < bytes.length; i += 0x8000)
        s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    return btoa(s);
}

const html = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

/* o: template and script (web/play.html and web/play.js bundled), cart
 * (the .pbg's bytes), wasmGz and fontGz (gzipped; fontGz may be null),
 * meta (cart.json's name, id, help and options), wheel and scale */
export function webPage(o) {
    if (!WHEELS.includes(o.wheel)) throw new Error('wheel must be one of ' + WHEELS.join(', '));
    if (!SCALES.includes(String(o.scale))) throw new Error('scale must be one of ' + SCALES.join(', '));
    const meta = o.meta || {};
    const data = {
        name: meta.name || 'Game', id: meta.id || '', help: meta.help || '',
        options: meta.options || [], wheel: o.wheel, scale: String(o.scale), source: SOURCE,
        cart: base64(o.cart), wasm: base64(o.wasmGz), font: o.fontGz ? base64(o.fontGz) : null
    };
    const json = JSON.stringify(data).replace(/</g, '\\u003c');
    const script = o.script.replace(/<\/script/gi, '<\\/script');
    return o.template
        .replace('<title></title>', () => '<title>' + html(data.name) + '</title>')
        .replace('<a id="source"', () => '<a id="source" href="' + SOURCE + '"')
        .replace('<script id="cart-data" type="application/json"></script>',
                 () => '<script id="cart-data" type="application/json">' + json + '</script>')
        .replace('<script>/*SCRIPT*/</script>', () => '<script>' + script + '</script>');
}
