/* The Arcade IDE: a cart's files in an editor beside the running game.
 *
 * Parts: the cart (its files, kept in the browser between visits); the
 * editor, with errors from the compiler and the API's names to complete;
 * running it on the engine as one of the players would, a tick at the
 * cart's rate, a dialog's frames included; the input (keys and the drawn
 * click wheel); the output; opening and saving; the panels and the
 * menus. */

import { EditorView, basicSetup } from 'codemirror';
import { undo, redo } from '@codemirror/commands';
import { openSearchPanel } from '@codemirror/search';
import { EditorState } from '@codemirror/state';
import { javascript } from '@codemirror/lang-javascript';
import { json } from '@codemirror/lang-json';
import { oneDark } from '@codemirror/theme-one-dark';
import { setDiagnostics, lintGutter } from '@codemirror/lint';
import { autocompletion } from '@codemirror/autocomplete';
import { Engine, fontGlyphs } from '../src/runner.js';
import { isBinary } from '../src/folder.js';
import { build, open, names, NEW_CART, palette, readPicture, writePicture } from './cart.js';
import { PixelEditor, SheetEditor, toPalette } from './pixels.js';
import { PaletteEditor } from './palette.js';
import { FontEditor } from './font.js';
import { MapEditor } from './map.js';
import { SoundEditor } from './sound.js';
import { CartSettings } from './settings.js';
import { missing, dither, medianCut } from './colours.js';
import { parseGpl } from '../src/assets.js';
import { hints, about } from './hints.js';
import { ask } from './ask.js';
import { icon, iconButton } from './icons.js';
import capsBdf from '../fonts/caps.bdf';
import wasmBytes from '../wasm/arcade.wasm';

const $ = (id) => document.getElementById(id);
window.addEventListener('error', (e) => say('error', 'The IDE: ' + e.message));
window.addEventListener('unhandledrejection', (e) => say('error', 'The IDE: ' + (e.reason && e.reason.message || e.reason)));
const wasm = new WebAssembly.Module(wasmBytes);
/* FONT.WORLD's glyphs beyond the engine's, when the IDE was built with them */
const glyph = window.ARCADE_FONT
    ? fontGlyphs(Uint8Array.from(atob(window.ARCADE_FONT), (c) => c.charCodeAt(0))) : undefined;

/* Button bits, as the engine takes them */
const PREV = 1, NEXT = 2, MENU = 4, PLAY = 8, SELECT = 16;

/* ---- the cart ------------------------------------------------------------ */

let folder = {};              /* name: text, or bytes for a picture */
let current = 'main.js';
let lastBuild = null;
let saveTo = null;            /* a file or folder handle Save writes to */
const states = {};            /* each text file's editor state, its undo */

const DB = 'podbox-arcade-ide';

function db() {
    return new Promise((done, fail) => {
        const r = indexedDB.open(DB, 1);
        r.onupgradeneeded = () => r.result.createObjectStore('carts');
        r.onsuccess = () => done(r.result);
        r.onerror = () => fail(r.error);
    });
}

async function keep() {
    try {
        const d = await db();
        d.transaction('carts', 'readwrite').objectStore('carts').put({ folder, current }, 'open');
    } catch (e) { /* a private window: nothing kept */ }
}

async function kept() {
    try {
        const d = await db();
        return await new Promise((done) => {
            const r = d.transaction('carts').objectStore('carts').get('open');
            r.onsuccess = () => done(r.result);
            r.onerror = () => done(null);
        });
    } catch (e) { return null; }
}

let keepTimer = 0;
function changed() {
    runError = null;
    clearTimeout(keepTimer);
    keepTimer = setTimeout(() => { keep(); check(); }, 400);
}

function cartName() {
    try { return JSON.parse(folder['cart.json']).name || 'Untitled'; } catch (e) { return 'Untitled'; }
}

function load(files, file) {
    stop();
    if (view) { view.destroy(); view = null; }
    folder = files;
    for (const k of Object.keys(states)) delete states[k];
    current = file && folder[file] !== undefined ? file : 'main.js';
    showFiles();
    show(current);
    check();
    keep();
}

/* ---- the editor ---------------------------------------------------------- */

let view;
const dark = window.matchMedia('(prefers-color-scheme: dark)').matches ? [oneDark] : [];

function completions(ctx) {
    const word = ctx.matchBefore(/[A-Za-z_][\w]*\.[\w]*|[A-Za-z_][\w]*/);
    if (!word || (word.from === word.to && !ctx.explicit)) return null;
    const n = names(lastBuild);
    const dot = word.text.indexOf('.');
    if (dot > 0) {
        const ns = word.text.slice(0, dot);
        const own = Object.keys(folder).filter((f) => f === ns + '.js').length
            ? fileFunctions(ns + '.js') : null;
        const list = own || n.members[ns];
        if (!list) return null;
        return { from: word.from + dot + 1, options: list.map((l) => option(ns + '.' + l, l)) };
    }
    const files = Object.keys(folder).filter((f) => /^[A-Za-z_]\w*\.js$/.test(f) && f !== 'main.js')
        .map((f) => f.slice(0, -3));
    return { from: word.from, options: [...n.top, ...files].map((l) => option(l, l)) };
}

/* a completion, with its signature and line where it is a function */
function option(name, label) {
    const a = about(name, folder);
    return a ? { label: label, type: 'function', detail: a.sig.slice(a.sig.indexOf('(')), info: a.doc || undefined }
             : { label: label };
}

/* the functions a namespace file declares */
function fileFunctions(file) {
    const out = [];
    for (const m of String(folder[file]).matchAll(/^function\s+([A-Za-z_]\w*)/gm)) out.push(m[1]);
    return out;
}

function stateFor(file) {
    if (states[file]) return states[file];
    const lang = /\.js$/.test(file) ? [javascript()] : /\.json$/.test(file) ? [json()] : [];
    states[file] = EditorState.create({
        doc: String(folder[file]),
        extensions: [basicSetup, ...lang, ...dark, lintGutter(), ...(/\.js$/.test(file) ? hints(() => folder) : []), autocompletion({ override: [completions] }),
                     EditorView.updateListener.of((u) => {
                         if (!u.docChanged) return;
                         folder[current] = u.state.doc.toString();
                         changed();
                     })]
    });
    return states[file];
}

let pixels = null;

function show(file) {
    if (view) states[current] = view.state;
    if (pixels) { pixels.destroy(); pixels = null; }
    current = file;
    const pane = $('editor');
    const gplFile = /^palettes\/\w+\.gpl$/.test(file), bdfFile = /^fonts\/\w+\.bdf$/.test(file);
    const mapFile = /^maps\/\w+\.json$/.test(file), sfxFile = /^sounds\/\w+\.json$/.test(file);
    const cfgFile = file === 'cart.json';
    const visual = gplFile || bdfFile || mapFile || sfxFile || cfgFile;
    if (isBinary(file) || (visual && !asText[file])) {
        if (view) { view.destroy(); view = null; }
        pane.innerHTML = '';
        if (gplFile) showPalette(pane, file);
        else if (bdfFile) showFont(pane, file);
        else if (mapFile) showMap(pane, file);
        else if (sfxFile) showSound(pane, file);
        else if (cfgFile) showSettings(pane, file);
        else showPicture(pane, file);
    } else {
        /* a file with an editor of its own has a bar over its text to go back */
        if (view && (visual || view.dom.parentElement !== pane)) { view.destroy(); view = null; }
        if (!view) {
            pane.innerHTML = '';
            let host = pane;
            if (visual) {
                const bar = document.createElement('div');
                bar.className = 'tb';
                const [ic, label] = gplFile ? ['swatches', 'Edit as swatches'] : bdfFile ? ['glyphs', 'Edit as glyphs']
                    : mapFile ? ['grid', 'Edit as a map'] : sfxFile ? ['waveSine', 'Edit as a sound']
                    : ['settings', 'Edit as a form'];
                bar.append(iconButton(ic, label,
                    () => { delete asText[file]; check(); show(file); }));
                host = document.createElement('div');
                host.className = 'text-host';
                pane.append(bar, host);
            }
            view = new EditorView({ state: stateFor(file), parent: host });
        } else view.setState(stateFor(file));
    }
    for (const li of $('files').querySelectorAll('li'))
        li.classList.toggle('open', li.dataset.file === file);
    mark();
}

/* the sheet's animation, as the sprite editor last had it */
let sheetAnim = null;
/* each image's animation, and the images edited as sprites, by file */
const imageAnims = {};
let asSprites = {};
try { asSprites = JSON.parse(localStorage.getItem('arcade-as-sprites') || '{}'); } catch (e) { /* none kept */ }

function setAsSprites(file, on) {
    if (on) asSprites[file] = true; else delete asSprites[file];
    try { localStorage.setItem('arcade-as-sprites', JSON.stringify(asSprites)); } catch (e) { /* not kept */ }
}

/* files with editors of their own, shown as text */
const asText = {};

/* a font in the font editor */
function showFont(pane, file) {
    try {
        pixels = new FontEditor(pane, {
            text: String(folder[file]), ask: ask,
            picker: layout.picker,
            pickerShown: (open) => { layout.picker = open; applyLayout(); },
            say: (text, bad) => say(bad ? 'error' : 'print', text),
            asText: () => { asText[file] = true; show(file); },
            changed: (text) => { folder[file] = text; delete states[file]; changed(); }
        });
    } catch (e) {
        asText[file] = true;
        say('error', file + ': ' + e.message + '. Shown as text to put right.');
        show(file);
    }
}

/* a map in the map editor, painted with the sheet */
function showMap(pane, file) {
    let sheet = null;
    try { if (folder['sheet.png']) sheet = readPicture(folder['sheet.png']).px; } catch (e) { /* the build says */ }
    try {
        pixels = new MapEditor(pane, {
            text: String(folder[file]), sheet: sheet, rgb: palette(folder), flags: readFlags(),
            flagNames: flagNames, ask: ask,
            picker: layout.picker,
            pickerShown: (open) => { layout.picker = open; applyLayout(); },
            say: (text, bad) => say(bad ? 'error' : 'print', text),
            asText: () => { asText[file] = true; show(file); },
            changed: (text) => { folder[file] = text; delete states[file]; changed(); }
        });
        pixels.focus();
    } catch (e) {
        asText[file] = true;
        say('error', file + ': ' + e.message + '. Shown as text to put right.');
        show(file);
    }
}

/* cart.json as a form */
function showSettings(pane, file) {
    try {
        pixels = new CartSettings(pane, {
            text: String(folder[file]),
            asText: () => { asText[file] = true; show(file); },
            changed: (text) => { folder[file] = text; delete states[file]; changed(); }
        });
        pixels.focus();
    } catch (e) {
        asText[file] = true;
        say('error', file + ': ' + e.message + '. Shown as text to put right.');
        show(file);
    }
}

/* a sound effect in the sound editor */
function showSound(pane, file) {
    try {
        pixels = new SoundEditor(pane, {
            text: String(folder[file]), name: file.slice(7, -5),
            picker: layout.picker,
            pickerShown: (open) => { layout.picker = open; applyLayout(); },
            say: (text, bad) => say(bad ? 'error' : 'print', text),
            asText: () => { asText[file] = true; show(file); },
            changed: (text) => { folder[file] = text; delete states[file]; changed(); }
        });
        pixels.focus();
    } catch (e) {
        asText[file] = true;
        say('error', file + ': ' + e.message + '. Shown as text to put right.');
        show(file);
    }
}

/* the sheet's 256 flags, from flags.json */
function readFlags() {
    const flags = new Uint8Array(256);
    if (folder['flags.json']) {
        try {
            const f = JSON.parse(String(folder['flags.json']));
            if (Array.isArray(f)) f.forEach((v, i) => { flags[i] = v; });
            else for (const [k, v] of Object.entries(f)) flags[+k] = v;
        } catch (e) { /* the build reports it */ }
    }
    return flags;
}

/* the eight flags' names from cart.json, null where none */
function flagNames() {
    try {
        const n = JSON.parse(String(folder['cart.json'])).flags || [];
        return Array.from({ length: 8 }, (_, b) => n[b] || null);
    } catch (e) { return new Array(8).fill(null); }
}

/* every picture of the cart: its file, size and palette numbers */
function pictures() {
    const out = [];
    for (const f of Object.keys(folder).filter((n) => isBinary(n))) {
        try { const p = readPicture(folder[f]); out.push({ name: f, w: p.width, h: p.height, px: p.px }); } catch (e) { /* not ours */ }
    }
    return out;
}

function paletteFiles() {
    return Object.keys(folder).filter((f) => /^palettes\/\w+\.gpl$/.test(f)).sort();
}

/* a palette in its editor. Entries moved in the cart's own palette move
 * in every picture and in the other palettes too, so nothing looks
 * different. */
function showPalette(pane, file) {
    let rgb;
    try { rgb = parseGpl(String(folder[file])).map(([r, g, b]) => r << 16 | g << 8 | b); } catch (e) {
        asText[file] = true;
        say('error', file + ': ' + e.message + '. Shown as text to put right.');
        show(file);
        return;
    }
    const first = paletteFiles()[0] === file;
    const name = file.slice(9, -4);
    pixels = new PaletteEditor(pane, {
        rgb: rgb, name: name, first: first, pictures: () => pictures(),
        say: (text, bad) => say(bad ? 'error' : 'print', text),
        asText: () => { asText[file] = true; show(file); },
        sure: async () => await ask('Move colours?', [{
            text: 'The pictures are rewritten so they look the same. Code that names a ' +
                  'colour by its number -- cls(12), print(s, x, y, 7) -- is not: it will ' +
                  'draw in whichever colour now has that number.' }], 'Move them') !== null,
        changed: (next, order) => {
            folder[file] = gpl(name, next);
            delete states[file];
            if (order && first) {
                /* new number k was old order[k] */
                const now = new Uint8Array(256);
                order.forEach((old, k) => { now[old] = k; });
                for (const p of pictures()) {
                    for (let i = 0; i < p.px.length; i++) p.px[i] = now[p.px[i]];
                    folder[p.name] = writePicture(p.w, p.h, p.px, next);
                }
                for (const other of paletteFiles().slice(1)) {
                    try {
                        const o = parseGpl(String(folder[other])).map(([r, g, b]) => r << 16 | g << 8 | b);
                        folder[other] = gpl(other.slice(9, -4), order.map((k) => o[k] === undefined ? 0 : o[k]));
                        delete states[other];
                    } catch (e) { /* a broken palette: the build says so */ }
                }
            } else if (first) {
                /* the pictures carry the palette too, for other programs */
                for (const p of pictures()) folder[p.name] = writePicture(p.w, p.h, p.px, next);
            }
            changed();
        }
    });
}

/* The cart's own palette replaced, as when colours are added to it: its
 * first palette file rewritten, or palettes/main.gpl made from the
 * engine's sixteen; the pictures keep their numbers and carry the new
 * colours for other programs. */
function setCartPalette(rgb) {
    const file = paletteFiles()[0] || 'palettes/main.gpl';
    folder[file] = gpl(file.slice(9, -4), rgb);
    delete states[file];
    for (const p of pictures()) folder[p.name] = writePicture(p.w, p.h, p.px, rgb);
    showFiles();
    if (pixels && pixels.setPalette) pixels.setPalette(rgb);
    changed();
}

/* A picture from elsewhere as palette numbers. Where its colours are not
 * all in the palette, the owner chooses: the nearest, the nearest
 * dithered, or new colours added -- reduced to the room the palette has.
 * null when not wanted. */
async function fitPicture(rgba, w, h) {
    let rgb = palette(folder);
    const miss = missing(rgba, rgb);
    if (!miss.n) return toPalette(rgba, rgb).px;
    const room = 256 - rgb.length;
    const options = [['near', 'The nearest colours of the palette'],
                     ['dither', 'The nearest, dithered: mixes for the shades between']];
    if (room > 0)
        options.push(['add', miss.n <= room ? 'Its ' + miss.n + ' colours, added to the palette'
            : room + ' colours added to the palette, standing for its ' + miss.n]);
    const a = await ask('Colours not in the palette', [
        { text: miss.n + (miss.n === 1 ? ' colour' : ' colours') + ' of this picture (' + miss.pixels +
                ' pixels) are not in the palette' + (room ? '' : ', which is full') + '.' },
        { name: 'how', label: 'Use', options: options }], 'Use them');
    if (!a) return null;
    if (a.how === 'add') {
        rgb = rgb.concat(medianCut(miss.colours, Math.min(room, miss.n)));
        setCartPalette(rgb);
        say('print', 'Added ' + Math.min(room, miss.n) + ' colours to the palette.');
    }
    return a.how === 'dither' ? dither(rgba, w, h, rgb) : toPalette(rgba, rgb).px;
}

/* the open picture saved to disk, an indexed PNG in the cart's palette */
function exportPicture() {
    if (!isBinary(current)) { say('error', 'Open a picture to export it.'); return; }
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([folder[current]], { type: 'image/png' }));
    a.download = current.split('/').pop();
    a.click();
}

/* the sheet or an image in the pixel editor; a PNG that is not indexed is
 * offered for converting to the palette */
function showPicture(pane, file) {
    const rgb = palette(folder);
    let pic;
    try { pic = readPicture(folder[file]); } catch (e) {
        const p = document.createElement('p');
        p.textContent = file + ' is not a picture of palette numbers (' + e.message + '). ';
        const b = document.createElement('button');
        b.textContent = 'Convert it to the palette';
        b.onclick = async () => {
            const bmp = await createImageBitmap(new Blob([folder[file]], { type: 'image/png' }));
            const c = document.createElement('canvas');
            c.width = bmp.width;
            c.height = bmp.height;
            c.getContext('2d').drawImage(bmp, 0, 0);
            const px = await fitPicture(c.getContext('2d').getImageData(0, 0, c.width, c.height).data, c.width, c.height);
            if (!px) return;
            folder[file] = writePicture(c.width, c.height, px, palette(folder));
            say('print', 'Converted ' + file + '.');
            changed();
            show(file);
        };
        p.append(b);
        pane.append(p);
        return;
    }
    const sheet = file === 'sheet.png';
    const flags = readFlags();
    const write = (px) => {
        folder[file] = writePicture(pic.width, pic.height, px, rgb);
        changed();
    };
    const say_ = (text, bad) => say(bad ? 'error' : 'print', text);
    /* an image a cell or more each way can be edited as sprites, for sheet() */
    const cells = pic.width >= 8 && pic.height >= 8;
    if (!sheet && !(cells && asSprites[file])) {
        pixels = new PixelEditor(pane, { width: pic.width, height: pic.height, px: pic.px, rgb: rgb,
                                         changed: write, fitColours: fitPicture, say: say_ });
        if (cells) {
            const sep = document.createElement('span');
            sep.className = 'sep';
            pixels.bar.append(sep, iconButton('sprites', 'Edit as sprites: 8x8 cells, drawn after sheet(IMG.' +
                file.slice(7, -4) + ')', () => { setAsSprites(file, true); show(file); }));
        }
        pixels.root.focus();
        return;
    }
    if (!sheet) {
        pixels = new SheetEditor(pane, {
            px: pic.px, width: pic.width, height: pic.height, image: file.slice(7, -4),
            rgb: rgb, fitColours: fitPicture, picker: layout.picker,
            anim: imageAnims[file] || null,
            animChanged: (a) => { imageAnims[file] = a; },
            pickerShown: (open) => { layout.picker = open; applyLayout(); },
            tools: [iconButton('picture', 'Edit as one picture', () => { setAsSprites(file, false); show(file); })],
            changed: write, say: say_
        });
        return;
    }
    pixels = new SheetEditor(pane, {
        px: pic.px, rgb: rgb, flags: flags, fitColours: fitPicture,
        picker: layout.picker,
        anim: sheetAnim,
        animChanged: (a) => { sheetAnim = a; },
        pickerShown: (open) => { layout.picker = open; applyLayout(); },
        flagNames: flagNames,
        nameFlag: async (b) => {
            let cart;
            try { cart = JSON.parse(String(folder['cart.json'])); } catch (e) {
                say('error', 'cart.json has an error to put right first.');
                return;
            }
            const names = (cart.flags || []).slice();
            const a = await ask('Name flag ' + b, [
                { name: 'name', label: 'Name', value: names[b] || '',
                  note: 'Code then has FLAG.' + (names[b] || 'name') + '; empty for no name' }], 'Name it');
            if (!a) return;
            const name = a.name.trim();
            if (name && !NAME.test(name)) { say('error', 'A name is letters, digits and _, not starting with a digit.'); return; }
            while (names.length <= b) names.push(null);
            names[b] = name || null;
            while (names.length && !names[names.length - 1]) names.pop();
            if (names.length) cart.flags = names; else delete cart.flags;
            folder['cart.json'] = JSON.stringify(cart, null, 2) + '\n';
            delete states['cart.json'];
            changed();
            pixels.drawFlags();
        },
        changed: write,
        flagsChanged: (f) => {
            const out = {};
            f.forEach((v, i) => { if (v) out[i] = v; });
            folder['flags.json'] = JSON.stringify(out, null, 1) + '\n';
            delete states['flags.json'];
            showFiles();
            changed();
        },
        say: say_
    });
}

function showFiles() {
    const ul = $('files');
    ul.innerHTML = '';
    const order = (f) => (f === 'main.js' ? 0 : f === 'cart.json' ? 1 : /\.js$/.test(f) && !f.includes('/') ? 2 : 3);
    for (const f of Object.keys(folder).sort((a, b) => order(a) - order(b) || (a < b ? -1 : 1))) {
        const li = document.createElement('li');
        li.textContent = f;
        li.dataset.file = f;
        li.onclick = () => show(f);
        ul.append(li);
    }
    $('cart-name').textContent = cartName();
    document.title = cartName() + ' - Arcade';
}

/* the compiler's errors: in the editor for this file, listed in the
 * console for all */
function check() {
    lastBuild = build(folder);
    $('cart-name').textContent = cartName();
    mark();
    const errs = lastBuild.errors;
    for (const li of $('files').querySelectorAll('li'))
        li.classList.toggle('bad', errs.some((e) => e.file === li.dataset.file));
    $('status').textContent = errs.length
        ? '\u2716 ' + errs.length + (errs.length === 1 ? ' error' : ' errors')
        : '\u2714 Built, ' + lastBuild.stats.size.toLocaleString() + ' bytes';
    $('status').classList.toggle('bad', errs.length > 0);
}

/* the last run's error, marked until the code next changes */
let runError = null;

function mark() {
    if (!view || !lastBuild) return;
    const doc = view.state.doc;
    const errs = [...lastBuild.errors];
    if (runError) errs.push(runError);
    const diags = errs.filter((e) => e.file === current).map((e) => {
        const line = doc.line(Math.min(Math.max(1, e.line), doc.lines));
        if (!e.column)
            return { from: line.from, to: line.to, severity: 'error', message: e.message };
        const from = Math.min(line.from + e.column - 1, line.to);
        /* the word the error is at, or the rest of the line */
        const word = /^[\w$.]+/.exec(doc.sliceString(from, line.to));
        const to = word ? from + word[0].length : line.to;
        return { from: from, to: Math.max(to, from), severity: 'error', message: e.message };
    });
    view.dispatch(setDiagnostics(view.state, diags));
}

function goTo(file, line) {
    if (folder[file] === undefined) return;
    show(file);
    if (!view) return;
    const l = view.state.doc.line(Math.min(Math.max(1, line), view.state.doc.lines));
    view.dispatch({ selection: { anchor: l.from }, scrollIntoView: true });
    view.focus();
}

/* ---- running it ---------------------------------------------------------- */

let engine = null, running = false, paused = false, generation = 0, dialogShown = false;
let audio = null, audioAt = 0;
const screen = $('screen').getContext('2d');
const image = screen.createImageData(160, 120);

function present() {
    if (!engine) return;
    image.data.set(engine.rgba());
    screen.putImageData(image, 0, 0);
}

const sleep = (ms) => new Promise((done) => setTimeout(done, Math.max(0, ms)));

/* The players a cart can be run as: a unit of script and a whole screen's
 * update, in ms on each. The iPod Video's are measured; the Classic's are
 * from its speed against the Video. */
const DEVICES = {
    '6g': { name: 'iPod Classic', unit: 0.00016, screen: 2.0 },
    '5g': { name: 'iPod Video', unit: 0.00055, screen: 6.2 },
    '5g-music': { name: 'iPod Video, music playing', unit: 0.0011, screen: 6.6 },
    'free': { name: 'this computer, no limit', unit: 0, screen: 0 }
};
let device = '5g-music';
try { if (DEVICES[localStorage.getItem('arcade-device')]) device = localStorage.getItem('arcade-device'); } catch (e) { /* none kept */ }

/* The units one call may run before the player stops the cart as not
 * responding: the shell's CEILING */
const CEILING = 2000000;

/* The player's clock, in ms of its own time: each tick takes what its
 * units and the rows it drew take on the player, and when a tick runs
 * late the player's rule applies -- behind by a whole tick, one update
 * without drawing; further behind, the time is given up. */
let clock;

function clockStart() {
    clock = { real: performance.now(), emu: 0, start: 0, frame: 0, drawn: [] };
}

/* after time the player does not count (a pause, a dialog), from now on */
function rebase(e) {
    clock.emu = performance.now() - clock.real;
    clock.start = clock.emu - clock.frame * (1000 / e.rate());
}

async function until(ms, e, live) {
    await sleep(clock.real + ms - performance.now());
    if (!paused) return;
    while (paused && live()) await sleep(50);
    rebase(e);
}

function cost(e, drawn) {
    const d = DEVICES[device];
    let ms = e.units() * d.unit;
    if (drawn) {
        const [lo, hi] = e.dirty();
        if (hi >= lo) ms += (d.screen * (hi - lo + 1)) / 120;
    }
    return ms;
}

async function run() {
    stop();
    check();
    if (lastBuild.errors.length) {
        say('error', lastBuild.errors.map((e) => e.file + ':' + e.line + ': ' + e.message).join('\n'),
            lastBuild.errors[0]);
        return;
    }
    $('console').textContent = '';
    quietRun = false;
    if (!layout.player) { layout.player = true; applyLayout(); }
    /* a run ends when another starts: each keeps its own engine */
    const gen = ++generation, live = () => running && generation === gen;
    const e = engine = new Engine(wasm, hostFor(() => e, live));
    e.music({ playingFn: true, beat: true, playing: musicOn, bpm: 0 });
    if (!e.load(lastBuild.cart, BigInt(Date.now()))) return fault();
    metronomeAt = -1;
    e.ceiling(CEILING);
    if (!audio) audio = new AudioContext({ sampleRate: 44100 });
    audio.resume();
    audioAt = 0;
    running = true;
    paused = false;
    $('screen').focus();
    document.body.classList.add('running');
    clockStart();
    if (!(await e.start())) return live() ? fault() : undefined;
    present();
    rebase(e);
    let rate = e.rate();
    while (live()) {
        const p = 1000 / rate;
        clock.emu = Math.max(clock.emu, clock.start + clock.frame * p);
        await until(clock.emu, e, live);
        if (!live()) break;
        clock.frame++;
        const late = clock.emu - (clock.start + clock.frame * p);
        if (late >= p) {
            e.skipped(Math.floor(late / p));
            const i = input();
            if (!(await e.tick(i.held, i.clicks, i.pos, false))) return live() ? fault() : undefined;
            clock.emu += cost(e, false);
            clock.frame++;
            if (late >= 2 * p) clock.start = clock.emu - clock.frame * p;
        }
        const i = input();
        dialogShown = false;
        const ok = await e.tick(i.held, i.clicks, i.pos, true, Math.round(1000 / rate));
        if (!live()) break;
        if (!ok) return fault();
        if (dialogShown) rebase(e);
        else clock.emu += cost(e, true);
        if (e.rate() !== rate) {
            rate = e.rate();
            clock.start = clock.emu - clock.frame * (1000 / rate);
        }
        /* shown when the player would have it on its screen */
        await until(clock.emu, e, live);
        if (!live()) break;
        present();
        shown(rate);
        sound();
        music(e);
        e.idle();
    }
}

/* the cart stopped; the screen cleared, but for an error, whose last
 * frame stays to be looked at */
function stop(keep) {
    running = false;
    paused = false;
    document.body.classList.remove('running', 'paused');
    if (keep !== true) screen.clearRect(0, 0, 160, 120);
    showDevice();
}

function fault() {
    const msg = engine.error();
    stop(true);
    const m = /^([^:]+):(\d+): (.*)$/.exec(msg);
    say('error', msg, m ? { file: m[1], line: +m[2] } : null);
    if (m) {
        runError = { file: m[1], line: +m[2], column: 0, message: m[3] };
        goTo(m[1], +m[2]);
    }
}

/* the sound since the last: what the time since then holds, so a cart
 * running slow on its player still sounds whole, as the player's sound
 * is drawn by the audio, not the ticks */
let soundAt = 0;

function sound() {
    const now = performance.now();
    const frames = Math.min(4096, Math.round(((now - (soundAt || now - 1000 / engine.rate())) * 44100) / 1000));
    soundAt = now;
    if (frames <= 0) return;
    const pcm = engine.audio(frames, 44100);
    if (!pcm.some((v) => v)) return;
    const buf = audio.createBuffer(2, frames, 44100);
    const l = buf.getChannelData(0), r = buf.getChannelData(1);
    for (let i = 0; i < frames; i++) { l[i] = pcm[i * 2] / 32768; r[i] = pcm[i * 2 + 1] / 32768; }
    const src = audio.createBufferSource();
    src.buffer = buf;
    src.connect(audio.destination);
    audioAt = Math.max(audioAt, audio.currentTime + 0.05);
    src.start(audioAt);
    audioAt += frames / 44100;
}

/* the engine's calls on the page, for the run whose engine is e() */
const hostFor = (e, live) => ({
    print: (s) => say('print', s),
    musicSay: (line) => say('music', 'music: ' + line),
    click: () => {},
    say: (text, queue) => say('say', (queue ? 'then says: ' : 'says: ') + text),
    speechOn: () => $('speech').classList.contains('on'),
    glyph: glyph,
    storeRead: (name) => {
        const v = localStorage.getItem(storeKey(name));
        return v === null ? undefined : new TextEncoder().encode(v);
    },
    storeWrite: (name, bytes) => {
        try { localStorage.setItem(storeKey(name), new TextDecoder().decode(bytes)); return true; } catch (e) { return false; }
    },
    storeRemove: (name) => {
        const had = localStorage.getItem(storeKey(name)) !== null;
        localStorage.removeItem(storeKey(name));
        return had;
    },
    storeList: () => Object.keys(localStorage).filter((k) => k.startsWith(storeKey(''))).
        map((k) => k.slice(storeKey('').length)).sort(),
    /* a dialog's frame: shown, then the next tick's input, a tick of the
     * cart's rate later; null backs out. The player's clock starts again
     * after it. */
    frame: (open) => {
        if (!open) return null;
        dialogShown = true;
        return Promise.resolve().then(async () => {
            if (live()) present();
            await sleep(1000 / e().rate());
            while (paused && live()) await sleep(50);
            if (!live()) return null;
            const i = input(true);
            return { held: i.held, wheel: i.clicks };
        });
    }
});

function storeKey(name) {
    let id = '';
    try { id = JSON.parse(folder['cart.json']).id; } catch (e) { /* no id */ }
    return 'arcade-store/' + id + '/' + name;
}

/* ---- the input ----------------------------------------------------------- */

let keys = 0, wheelHeld = 0, clicksOwed = 0, wheelPos = -1;

/* this tick's buttons and wheel; MENU goes to a dialog, or to a cart that
 * takes it as a button, else it pauses */
function input(dialog) {
    let held = keys | wheelHeld;
    if (!dialog && !engine.menuIsGame()) held &= ~MENU;
    const clicks = Math.trunc(clicksOwed);
    clicksOwed -= clicks;
    return { held, clicks, pos: wheelPos };
}

const KEYS = { ArrowLeft: PREV, ArrowRight: NEXT, Escape: MENU, ' ': PLAY, Enter: SELECT };

$('screen').addEventListener('keydown', (e) => {
    if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
        clicksOwed += e.key === 'ArrowDown' ? 8 : -8;
        e.preventDefault();
        return;
    }
    const b = KEYS[e.key];
    if (!b) return;
    e.preventDefault();
    if (b === MENU && engine && !engine.menuIsGame() && !e.repeat) {
        paused = !paused;
        document.body.classList.toggle('paused', paused);
    }
    keys |= b;
});
$('screen').addEventListener('keyup', (e) => { const b = KEYS[e.key]; if (b) keys &= ~b; });
$('screen').addEventListener('blur', () => { keys = 0; });

/* The wheel: dragging round the ring turns it, 96 clicks a turn, and its
 * place is where the finger is. Touching it presses nothing, as on the
 * iPod: a click on the ring that does not turn it is MENU, PREV, NEXT or
 * PLAY by where it falls, and held half a second without turning, that
 * button is held. The middle is SELECT. */
(function wheel() {
    const svg = $('wheel');
    let lastTurn = null, moved = 0, button = 0, holding = 0;
    const at = (e) => {
        const r = svg.getBoundingClientRect();
        const x = e.clientX - r.left - r.width / 2, y = e.clientY - r.top - r.height / 2;
        return { d: Math.hypot(x, y) / (r.width / 2), turn: (Math.atan2(x, -y) / (2 * Math.PI) + 1) % 1 };
    };
    /* a click: the button down long enough for a tick to see it */
    const tap = (b) => {
        wheelHeld = b;
        setTimeout(() => { if (wheelHeld === b) wheelHeld = 0; }, 120);
    };
    svg.addEventListener('pointerdown', (e) => {
        const p = at(e);
        try { svg.setPointerCapture(e.pointerId); } catch (x) { /* a pointer already gone */ }
        $('screen').focus();
        e.preventDefault();
        if (p.d < 0.38) { wheelHeld = SELECT; return; }
        lastTurn = p.turn;
        moved = 0;
        wheelPos = Math.floor(p.turn * 96) % 96;
        button = [MENU, NEXT, PLAY, PREV][Math.round(p.turn * 4) % 4];
        holding = setTimeout(() => { holding = 0; wheelHeld = button; }, 500);
    });
    svg.addEventListener('pointermove', (e) => {
        if (lastTurn === null) return;
        const p = at(e);
        let d = p.turn - lastTurn;
        if (d > 0.5) d -= 1;
        if (d < -0.5) d += 1;
        lastTurn = p.turn;
        wheelPos = Math.floor(p.turn * 96) % 96;
        clicksOwed += d * 96;
        moved += Math.abs(d * 96);
        if (moved > 3) { clearTimeout(holding); holding = 0; button = 0; }
    });
    const up = () => {
        if (lastTurn === null) { wheelHeld = 0; return; }
        lastTurn = null;
        wheelPos = -1;
        if (holding) { clearTimeout(holding); holding = 0; if (button) tap(button); }
        else wheelHeld = 0;
    };
    svg.addEventListener('pointerup', up);
    svg.addEventListener('pointercancel', up);
})();

/* ---- the made-up music ---------------------------------------------------- */

let musicOn = true, metronomeAt = -1;

const clockText = (ms) => Math.floor(ms / 60000) + ':' + String(Math.floor(ms / 1000) % 60).padStart(2, '0');

/* the panel, and the metronome's click on each beat it crossed */
function music(e) {
    const n = e.nowPlaying();
    musicOn = n.playing;
    $('m-play').innerHTML = icon(n.playing ? 'pause' : 'play');
    $('m-now').firstChild.textContent = n.id < 0 ? 'Nothing playing' : n.title;
    $('m-now').lastChild.textContent = n.id < 0 ? '' : n.artist + ' \u2013 ' + n.album;
    $('m-time').textContent = n.id < 0 ? '' : clockText(n.ms) + ' / ' + clockText(n.length) +
        (n.bpm ? ', ' + n.bpm + ' bpm' : ', spoken');
    if (!n.bpm || !n.playing) { metronomeAt = -1; return; }
    const beat = Math.floor(n.ms / (60000 / n.bpm));
    if (metronomeAt >= 0 && beat !== metronomeAt && $('m-click').checked) tick(beat % 4 === 0);
    metronomeAt = beat;
}

function tick(accent) {
    if (!audio) return;
    const o = audio.createOscillator(), g = audio.createGain(), t = audio.currentTime + 0.01;
    o.frequency.value = accent ? 1760 : 1320;
    g.gain.setValueAtTime(0.25, t);
    g.gain.exponentialRampToValueAtTime(0.001, t + 0.05);
    o.connect(g).connect(audio.destination);
    o.start(t);
    o.stop(t + 0.06);
}

$('m-play').onclick = () => {
    if (engine && running) { engine.musicControl(0); music(engine); } else {
        musicOn = !musicOn;
        $('m-play').innerHTML = icon(musicOn ? 'pause' : 'play');
    }
};
$('m-next').onclick = () => { if (engine && running) { engine.musicControl(1); music(engine); } };
$('m-prev').onclick = () => { if (engine && running) { engine.musicControl(2); music(engine); } };

/* ---- the console, and what the player would show -------------------------- */

/* the ticks drawn in the last second of the player's time, against the
 * cart's rate, on the player it runs as */
function shown(rate) {
    const t = clock.drawn;
    t.push(clock.emu);
    while (t.length && t[0] < clock.emu - 1000) t.shift();
    $('rate').textContent = (t.length >= rate ? rate : t.length + ' of ' + rate) +
        ' frames a second as ' + (device === 'free' ? 'fast as this computer' : 'an ' + DEVICES[device].name);
}

function showDevice() {
    for (const b of document.querySelectorAll('[data-device]'))
        b.classList.toggle('on', b.dataset.device === device);
    $('rate').textContent = device === 'free' ? 'Runs as fast as this computer.'
        : 'Runs as an ' + DEVICES[device].name + '.';
}

/* a line in the output, which opens to show it -- but for the game's own
 * lines once the output was closed during this run */
let quietRun = false;

function say(kind, text, where) {
    if (!layout.output && (kind === 'error' || !(running && quietRun))) {
        layout.output = true;
        applyLayout();
    }
    const c = $('console');
    const line = document.createElement('div');
    line.className = kind;
    line.textContent = text;
    if (where && where.file) {
        line.classList.add('link');
        line.onclick = () => goTo(where.file, where.line);
    }
    c.append(line);
    while (c.childNodes.length > 500) c.firstChild.remove();
    c.scrollTop = c.scrollHeight;
}

/* ---- opening and saving -------------------------------------------------- */

const canPick = 'showOpenFilePicker' in window;

/* the browser refused a picker for a reason other than being closed */
function refused(e) {
    if (e && e.name === 'AbortError') return false;
    say('print', 'The browser would not open its own file picker here (' +
        (e && e.message || e) + '), so the plain one is used instead.');
    return true;
}

async function openCart() {
    let bytes, handle = null;
    if (canPick) {
        try {
            [handle] = await window.showOpenFilePicker({ types: [{ description: 'Arcade cart',
                accept: { 'application/octet-stream': ['.pbg'] } }] });
            bytes = new Uint8Array(await (await handle.getFile()).arrayBuffer());
        } catch (e) {
            if (!refused(e)) return;
            handle = null;
        }
    }
    if (!bytes) {
        const f = await pickFile();
        if (!f) return;
        bytes = new Uint8Array(await f.arrayBuffer());
    }
    openBytes(bytes, handle);
}

function openBytes(bytes, handle) {
    try {
        load(open(bytes));
        saveTo = handle ? { file: handle } : null;
    } catch (e) {
        say('error', 'Could not open that cart: ' + e.message);
    }
}

function pickFile() {
    return new Promise((done) => {
        const i = document.createElement('input');
        i.type = 'file';
        i.accept = '.pbg';
        i.onchange = () => done(i.files[0]);
        i.click();
    });
}

/* a cart folder: the files a cart is made of, each in its place. With
 * the browser's folder access Save writes them back; without it they are
 * read from the plain picker, and Save writes a .pbg. */
async function openFolder() {
    let dir;
    if ('showDirectoryPicker' in window) {
        try { dir = await window.showDirectoryPicker({ mode: 'readwrite' }); } catch (e) {
            if (!refused(e)) return;
        }
    }
    if (!dir) return openFolderPlain();
    const files = {};
    async function walk(d, prefix, depth) {
        for await (const [name, h] of d.entries()) {
            if (h.kind === 'directory' && depth === 0 &&
                ['palettes', 'maps', 'images', 'sounds', 'data', 'fonts'].includes(name))
                await walk(h, name + '/', 1);
            else if (h.kind === 'file' && /\.(js|json|png|gpl|txt|bdf)$/.test(name)) {
                const f = await h.getFile();
                files[prefix + name] = isBinary(name) ? new Uint8Array(await f.arrayBuffer()) : await f.text();
            }
        }
    }
    try { await walk(dir, '', 0); } catch (e) { say('error', 'Could not read that folder: ' + e.message); return; }
    if (!files['cart.json']) { say('error', 'That folder has no cart.json: it is not a cart.'); return; }
    load(files);
    saveTo = { dir };
    say('print', 'Opened the folder ' + dir.name + ': Save writes its files back.');
}

const CART_FILE = /^((palettes|maps|images|sounds|data|fonts)\/)?[^/]+\.(js|json|png|gpl|txt|bdf)$/;

function openFolderPlain() {
    const i = document.createElement('input');
    i.type = 'file';
    i.webkitdirectory = true;
    i.onchange = async () => {
        const files = {};
        for (const f of i.files) {
            /* the path below the folder chosen */
            const rel = f.webkitRelativePath.split('/').slice(1).join('/');
            if (CART_FILE.test(rel))
                files[rel] = isBinary(rel) ? new Uint8Array(await f.arrayBuffer()) : await f.text();
        }
        if (!files['cart.json']) { say('error', 'That folder has no cart.json: it is not a cart.'); return; }
        load(files);
        saveTo = null;
        say('print', 'Opened the folder. This browser cannot write it back: Save writes a .pbg.');
    };
    i.click();
}

/* Whether the IDE may write where the cart came from. A cart opened
 * with Open comes with leave to read only; the browser asks the owner for
 * leave to write, and only while the click or key that asked to save is
 * fresh -- so this is asked before anything slow. */
async function canWrite(handle) {
    try {
        if (await handle.queryPermission({ mode: 'readwrite' }) === 'granted') return true;
        return await handle.requestPermission({ mode: 'readwrite' }) === 'granted';
    } catch (e) { return false; }
}

/* Save: back to the folder or the .pbg the cart came from, or else as a
 * new .pbg. Where the browser will not write there, a new .pbg instead. */
async function save() {
    const target = saveTo && (saveTo.dir || saveTo.file);
    const allowed = target ? await canWrite(target) : false;
    check();
    if (lastBuild.errors.length) { say('error', 'Fix the errors before saving: the cart would not build.'); return; }
    if (target && !allowed) {
        say('print', 'The browser would not let the IDE write to ' + target.name + ', so it is saved as a new .pbg.');
        saveTo = null;
        return saveAs();
    }
    try {
        if (saveTo && saveTo.dir) {
            for (const [name, v] of Object.entries(folder)) {
                let d = saveTo.dir;
                const parts = name.split('/');
                for (const p of parts.slice(0, -1)) d = await d.getDirectoryHandle(p, { create: true });
                const w = await (await d.getFileHandle(parts[parts.length - 1], { create: true })).createWritable();
                await w.write(v);
                await w.close();
            }
            say('print', 'Saved the folder ' + saveTo.dir.name + '.');
            return;
        }
        if (saveTo && saveTo.file) {
            const w = await saveTo.file.createWritable();
            await w.write(lastBuild.cart);
            await w.close();
            say('print', 'Saved ' + saveTo.file.name + '.');
            return;
        }
    } catch (e) {
        say('error', 'Could not save to ' + target.name + ' (' + e.message + '), so it is saved as a new .pbg.');
        saveTo = null;
    }
    return saveAs();
}

/* Save as .pbg: asks where, and Save writes there after; where the
 * browser has no such picker, or refuses it, the .pbg is downloaded */
async function saveAs() {
    check();
    if (lastBuild.errors.length) { say('error', 'Fix the errors before saving: the cart would not build.'); return; }
    const file = cartName().replace(/[^\w -]/g, '').trim().replace(/ +/g, '_').toLowerCase() + '.pbg';
    if (window.showSaveFilePicker) {
        let h;
        try {
            h = await window.showSaveFilePicker({ suggestedName: file,
                types: [{ description: 'Arcade cart', accept: { 'application/octet-stream': ['.pbg'] } }] });
        } catch (e) {
            if (e && e.name === 'AbortError') return;
        }
        if (h) {
            try {
                const w = await h.createWritable();
                await w.write(lastBuild.cart);
                await w.close();
                saveTo = { file: h };
                say('print', 'Saved ' + h.name + '.');
                return;
            } catch (e) { /* downloaded below */ }
        }
    }
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([lastBuild.cart]));
    a.download = file;
    a.click();
    say('print', 'Downloaded ' + file + '.');
}

/* ---- the cart's files: made, added from disk, renamed, deleted ----------- */

const NAME = /^[A-Za-z_]\w*$/;

/* where a file of each kind lives, by its name */
const KINDS = {
    code: { label: 'Code', path: (n) => n + '.js' },
    sheet: { label: 'The sprite sheet', path: () => 'sheet.png' },
    image: { label: 'An image', path: (n) => 'images/' + n + '.png' },
    palette: { label: 'A palette', path: (n) => 'palettes/' + n + '.gpl' },
    map: { label: 'A map', path: (n) => 'maps/' + n + '.json' },
    sound: { label: 'A sound', path: (n) => 'sounds/' + n + '.json' },
    data: { label: 'Data (JSON)', path: (n) => 'data/' + n + '.json' },
    text: { label: 'Text', path: (n) => 'data/' + n + '.txt' },
    font: { label: 'A font, starting from CAPS', path: (n) => 'fonts/' + n + '.bdf' }
};

/* the cart's palette as a GIMP palette */
function gpl(name, rgb) {
    return 'GIMP Palette\nName: ' + name + '\n#\n' +
        rgb.map((c) => [c >> 16, c >> 8 & 255, c & 255].map((v) => String(v).padStart(3)).join(' ')).join('\n') + '\n';
}

/* what a new file of each kind holds */
function fresh(kind, name, a) {
    const rgb = palette(folder);
    switch (kind) {
    case 'code': return '// ' + name + '.js: its functions are ' + name + '.name() elsewhere\n';
    case 'sheet': return writePicture(128, 128, new Uint8Array(128 * 128), rgb);
    case 'image': return writePicture(+a.w, +a.h, new Uint8Array(a.w * a.h), rgb);
    case 'palette': return gpl(name, rgb);
    case 'map': return JSON.stringify({ width: +a.mw, height: +a.mh,
        rows: Array.from({ length: +a.mh }, () => '00'.repeat(+a.mw)), objects: [] }, null, 1) + '\n';
    case 'sound': return JSON.stringify({ wave_type: 0, p_env_attack: 0, p_env_sustain: 0.08,
        p_env_decay: 0.12, p_base_freq: 0.45, sound_vol: 0.5 }) + '\n';
    case 'data': return '{\n}\n';
    case 'text': return '';
    case 'font': return capsBdf;
    }
    return '';
}

async function newFile() {
    const kinds = Object.entries(KINDS).filter(([k]) => k !== 'sheet' || folder['sheet.png'] === undefined);
    const a = await ask('New file', [
        { name: 'kind', label: 'What', options: kinds.map(([k, v]) => [k, v.label]) },
        { name: 'name', label: 'Name', value: '', when: (v) => v.kind !== 'sheet',
          note: 'Letters, digits and _: the name the code calls it by' },
        { name: 'w', label: 'Width', type: 'number', value: 64, when: (v) => v.kind === 'image' },
        { name: 'h', label: 'Height', type: 'number', value: 64, when: (v) => v.kind === 'image',
          note: 'Pixels, up to 1024 each way' },
        { name: 'mw', label: 'Width', type: 'number', value: 32, when: (v) => v.kind === 'map' },
        { name: 'mh', label: 'Height', type: 'number', value: 16, when: (v) => v.kind === 'map',
          note: 'Cells, up to 256 each way' }
    ], 'Make it');
    if (!a) return;
    const name = a.kind === 'sheet' ? 'sheet' : a.name.trim();
    if (!NAME.test(name)) { say('error', 'A name is letters, digits and _, not starting with a digit.'); return; }
    if (a.kind === 'code' && name === 'main') { say('error', 'main.js is there already.'); return; }
    if (a.kind === 'image' && !(a.w >= 1 && a.w <= 1024 && a.h >= 1 && a.h <= 1024)) {
        say('error', 'An image is 1 to 1024 pixels each way.');
        return;
    }
    if (a.kind === 'map' && !(a.mw >= 1 && a.mw <= 256 && a.mh >= 1 && a.mh <= 256)) {
        say('error', 'A map is 1 to 256 cells each way.');
        return;
    }
    const path = KINDS[a.kind].path(name);
    if (folder[path] !== undefined) { say('error', path + ' is there already.'); show(path); return; }
    folder[path] = fresh(a.kind, name, a);
    showFiles();
    show(path);
    changed();
}

/* where a file from disk goes, by its kind */
function placeFor(file, text) {
    const base = file.name.replace(/\.[^.]*$/, '').replace(/[^\w]/g, '_').replace(/^(\d)/, '_$1');
    const ext = (/\.([^.]*)$/.exec(file.name) || [])[1] || '';
    switch (ext.toLowerCase()) {
    case 'js': return base + '.js';
    case 'gpl': return 'palettes/' + base + '.gpl';
    case 'bdf': return 'fonts/' + base + '.bdf';
    case 'txt': return 'data/' + base + '.txt';
    case 'png': case 'gif': case 'jpg': case 'jpeg':
        return base === 'sheet' ? 'sheet.png' : 'images/' + base + '.png';
    case 'json': {
        let v = null;
        try { v = JSON.parse(text); } catch (e) { /* data, to show its error */ }
        if (base === 'flags') return 'flags.json';
        if (v && v.width && v.height && Array.isArray(v.rows)) return 'maps/' + base + '.json';
        if (v && ('wave_type' in v || 'p_base_freq' in v)) return 'sounds/' + base + '.json';
        return 'data/' + base + '.json';
    }
    }
    return null;
}

/* files from disk, each put where its kind belongs; a picture that is not
 * of palette numbers is matched to the palette */
function addFiles() {
    const i = document.createElement('input');
    i.type = 'file';
    i.multiple = true;
    i.accept = '.js,.json,.png,.gif,.jpg,.jpeg,.gpl,.bdf,.txt';
    i.onchange = async () => {
        for (const f of i.files) {
            const picture = /^image\//.test(f.type);
            const text = picture ? '' : await f.text();
            const to = placeFor(f, text);
            if (!to) { say('error', f.name + ': not a kind of file a cart holds.'); continue; }
            if (folder[to] !== undefined &&
                await ask('Replace ' + to + '?', [], 'Replace it') === null) continue;
            if (!picture) folder[to] = text;
            else {
                const bytes = new Uint8Array(await f.arrayBuffer());
                let pic = null;
                try { pic = /\.png$/i.test(f.name) ? readPicture(bytes) : null; } catch (e) { /* matched below */ }
                if (pic) folder[to] = bytes;
                else {
                    const bmp = await createImageBitmap(f);
                    const c = document.createElement('canvas');
                    c.width = bmp.width;
                    c.height = bmp.height;
                    c.getContext('2d').drawImage(bmp, 0, 0);
                    const px = await fitPicture(c.getContext('2d').getImageData(0, 0, c.width, c.height).data,
                                                c.width, c.height);
                    if (!px) continue;
                    folder[to] = writePicture(c.width, c.height, px, palette(folder));
                }
            }
            say('print', 'Added ' + to + '.');
            delete states[to];
            showFiles();
            show(to);
            changed();
        }
    };
    i.click();
}

const KEPT = ['main.js', 'cart.json'];

async function renameFile() {
    const f = current;
    if (KEPT.includes(f) || f === 'sheet.png' || f === 'flags.json') {
        say('error', f + ' keeps its name: the engine looks for it by it.');
        return;
    }
    const m = /^(.*\/)?([^/]+)(\.[^.]+)$/.exec(f);
    const a = await ask('Rename ' + f, [{ name: 'name', label: 'Name', value: m[2],
        note: 'The code then calls it by the new name' }], 'Rename');
    if (!a) return;
    const name = a.name.trim();
    if (!NAME.test(name)) { say('error', 'A name is letters, digits and _, not starting with a digit.'); return; }
    const to = (m[1] || '') + name + m[3];
    if (to === f) return;
    if (folder[to] !== undefined) { say('error', to + ' is there already.'); return; }
    folder[to] = folder[f];
    delete folder[f];
    if (states[f]) { states[to] = states[f]; delete states[f]; }
    current = to;
    showFiles();
    show(to);
    changed();
}

async function removeFile() {
    const f = current;
    if (KEPT.includes(f)) { say('error', 'Every cart has ' + f + '.'); return; }
    if (await ask('Delete ' + f + '?', [], 'Delete') === null) return;
    delete folder[f];
    delete states[f];
    current = 'main.js';
    showFiles();
    show('main.js');
    changed();
}

/* ---- the page ------------------------------------------------------------ */

const ACTIONS = {
    new: async () => {
        if (await ask('Start a new cart?', [], 'New cart') === null) return;
        load(NEW_CART());
        saveTo = null;
    },
    open: openCart,
    'open-folder': openFolder,
    save: save,
    export: saveAs,
    undo: () => view ? undo(view) : pixels && pixels.undoEdit && pixels.undoEdit(),
    redo: () => view ? redo(view) : pixels && pixels.redoEdit && pixels.redoEdit(),
    find: () => view && openSearchPanel(view),
    add: newFile,
    'view-files': () => toggle('files'),
    'view-picker': () => toggle('picker'),
    'view-player': () => toggle('player'),
    'view-output': () => toggle('output'),
    import: addFiles,
    'export-picture': exportPicture,
    settings: () => { if (folder['cart.json'] !== undefined) { delete asText['cart.json']; show('cart.json'); } },
    rename: renameFile,
    remove: removeFile,
    run: run,
    stop: stop,
    speech: () => $('speech').classList.toggle('on'),
    keys: () => $('help-keys').showModal(),
};

/* the menus: a click opens one; while one is open, hovering moves along
 * the bar; choosing an item or clicking elsewhere closes it */
(function menus() {
    const all = [...document.querySelectorAll('.menu')];
    const close = () => all.forEach((m) => m.classList.remove('open'));
    for (const m of all) {
        const title = m.querySelector('.title');
        title.onclick = (e) => {
            e.stopPropagation();
            const was = m.classList.contains('open');
            close();
            if (!was) m.classList.add('open');
        };
        title.onmouseenter = () => {
            if (all.some((o) => o.classList.contains('open'))) { close(); m.classList.add('open'); }
        };
    }
    document.addEventListener('click', close);
    document.addEventListener('keydown', (e) => { if (e.key === 'Escape') close(); });
    for (const b of document.querySelectorAll('.menu .items button, .panel-head [data-do]'))
        b.onclick = (e) => {
            e.stopPropagation();
            close();
            if (b.dataset.device) {
                device = b.dataset.device;
                try { localStorage.setItem('arcade-device', device); } catch (x) { /* not kept */ }
                showDevice();
            } else ACTIONS[b.dataset.do]();
        };
})();
showDevice();
$('run').onclick = run;
$('stop').onclick = stop;


document.addEventListener('keydown', (e) => {
    if (!(e.ctrlKey || e.metaKey)) return;
    if (e.key === 'Enter') { e.preventDefault(); if (e.shiftKey) stop(); else run(); }
    if (e.key === 's') { e.preventDefault(); save(); }
    if (e.key === 'o') { e.preventDefault(); openCart(); }
    if (e.key.toLowerCase() === 'b') { e.preventDefault(); toggle(e.shiftKey ? 'player' : 'files'); }
    if (e.key === 'j') { e.preventDefault(); toggle('output'); }
});

/* ---- the panels ---------------------------------------------------------- */

/* which panels are open, and how big, kept between visits */
const layout = { files: true, player: true, output: false, picker: true, filesW: 200, outH: 180 };
try { Object.assign(layout, JSON.parse(localStorage.getItem('arcade-layout') || '{}')); } catch (e) { /* none kept */ }
layout.output = false;

function applyLayout() {
    const b = document.body.classList;
    b.toggle('files-closed', !layout.files);
    b.toggle('player-closed', !layout.player);
    b.toggle('output-closed', !layout.output);
    document.documentElement.style.setProperty('--files-w', layout.filesW + 'px');
    document.documentElement.style.setProperty('--out-h', layout.outH + 'px');
    for (const k of ['files', 'picker', 'player', 'output']) $('v-' + k).classList.toggle('on', !!layout[k]);
    if (pixels && pixels.setPicker) pixels.setPicker(layout.picker);
    try { localStorage.setItem('arcade-layout', JSON.stringify(layout)); } catch (e) { /* not kept */ }
}

function toggle(k) {
    layout[k] = !layout[k];
    if (k === 'output' && !layout.output && running) quietRun = true;
    applyLayout();
}

for (const b of document.querySelectorAll('[data-icon]')) b.innerHTML = icon(b.dataset.icon);
for (const b of document.querySelectorAll('[data-toggle]')) b.onclick = () => toggle(b.dataset.toggle);
$('clear-output').onclick = () => { $('console').textContent = ''; };
$('status').onclick = () => { layout.output = true; applyLayout(); };
$('rate').onclick = (e) => {
    e.stopPropagation();
    const run = [...document.querySelectorAll('.menu')].find((m) => m.querySelector('.title').textContent === 'Run');
    run.classList.add('open');
};

/* the dividers: the files' width and the output's height, dragged */
function splitter(el, set) {
    el.addEventListener('pointerdown', (e) => {
        try { el.setPointerCapture(e.pointerId); } catch (x) { /* gone */ }
        el.classList.add('drag');
        const move = (ev) => { set(ev); applyLayout(); };
        const up = () => {
            el.classList.remove('drag');
            el.removeEventListener('pointermove', move);
            el.removeEventListener('pointerup', up);
        };
        el.addEventListener('pointermove', move);
        el.addEventListener('pointerup', up);
    });
}
splitter($('split-files'), (e) => { layout.filesW = Math.max(120, Math.min(420, e.clientX)); });
splitter($('split-output'), (e) => {
    const box = $('centre').getBoundingClientRect();
    layout.outH = Math.max(60, Math.min(box.height - 120, box.bottom - e.clientY));
});
applyLayout();

document.addEventListener('dragover', (e) => e.preventDefault());
document.addEventListener('drop', async (e) => {
    e.preventDefault();
    const f = e.dataTransfer.files[0];
    if (f && /\.pbg$/i.test(f.name)) openBytes(new Uint8Array(await f.arrayBuffer()), null);
    /* a picture dropped on the pixel editor goes in as a paste does */
    else if (f && pixels && /^image\//.test(f.type) && (pixels.editor || pixels).pasteImage)
        (pixels.editor || pixels).pasteImage(f);
});

(async function start() {
    /* the cart kept from last time, unless the browser's storage is slow
     * to answer or does not */
    const was = await Promise.race([kept(), sleep(1000).then(() => null)]);
    load(was && was.folder && was.folder['cart.json'] ? was.folder : NEW_CART(), was && was.current);
})();

/* for the tests, which drive the page */
window.arcadeIde = { load, run, stop, openBytes, folder: () => folder, show, build: () => lastBuild,
                    view: () => view };
