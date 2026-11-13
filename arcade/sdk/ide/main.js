/* The Arcade IDE: a cart's files in an editor beside the running game.
 *
 * Parts: the cart (its files, kept in the browser between visits); the
 * editor, with errors from the compiler and the API's names to complete;
 * running it on the engine as one of the players would, a tick at the
 * cart's rate, a dialog's frames included; the input (keys and the drawn
 * click wheel); the output; opening and saving; the panels and the
 * menus. */

import { EditorView, basicSetup } from 'codemirror';
import { undo, redo, indentWithTab } from '@codemirror/commands';
import { keymap } from '@codemirror/view';
import { indentUnit } from '@codemirror/language';
import { openSearchPanel } from '@codemirror/search';
import { EditorState } from '@codemirror/state';
import { javascript } from '@codemirror/lang-javascript';
import { json } from '@codemirror/lang-json';
import { oneDark } from '@codemirror/theme-one-dark';
import { setDiagnostics, lintGutter } from '@codemirror/lint';
import { autocompletion } from '@codemirror/autocomplete';
import { findPanel } from './search.js';
import { Engine, fontGlyphs } from '../src/runner.js';
import { isBinary, asNewGame } from '../src/folder.js';
import { build, open, names, NEW_CART, palette, readPicture, writePicture, functions, apiName, musicUsed } from './cart.js';
import { webPage, musicRefusal, needsWorld } from '../src/webpage.js';
import { gzipSync } from 'fflate';
import player from 'arcade:player';
import { PixelEditor, SheetEditor, toPalette } from './pixels.js';
import { PaletteEditor } from './palette.js';
import { FontEditor } from './font.js';
import { MapEditor } from './map.js';
import { SoundEditor } from './sound.js';
import { CartSettings } from './settings.js';
import { createDock } from './dock.js';
import { Profiler } from './profiler.js';
import { MemoryView } from './memory.js';
import { createGame } from './game.js';
import { VERSION, hashScreen, hashCart, memoryStore, readTape } from './replay.js';
import { setPanelHost } from './chrome.js';
import { missing, dither, medianCut } from './colours.js';
import { parseGpl, objectKinds } from '../src/assets.js';
import { hints, about } from './hints.js';
import { ask } from './ask.js';
import { CartEditor, CART_FILE, editorOf, isEditor, approved, approve, starterEditor } from './editors.js';
import { icon, iconButton } from './icons.js';
import capsBdf from '../fonts/caps.bdf';
import wasmBytes from '../wasm/arcade.wasm';

/* the page's elements by id, found once: the dock takes a panel that is
 * closed, or a tab behind another, out of the page, where getElementById
 * cannot see it */
const byId = new Map([...document.querySelectorAll('[id]')].map((el) => [el.id, el]));
const $ = (id) => byId.get(id) || document.getElementById(id);
window.addEventListener('error', (e) => say('error', 'The IDE: ' + e.message));
window.addEventListener('unhandledrejection', (e) => say('error', 'The IDE: ' + (e.reason && e.reason.message || e.reason)));
const wasm = new WebAssembly.Module(wasmBytes);
/* FONT.WORLD's glyphs beyond the engine's, when the IDE was built with them */
const fontBytes = window.ARCADE_FONT
    ? Uint8Array.from(atob(window.ARCADE_FONT), (c) => c.charCodeAt(0)) : null;
const glyph = fontBytes ? fontGlyphs(fontBytes) : undefined;

/* Button bits, as the engine takes them */
const PREV = 1, NEXT = 2, MENU = 4, PLAY = 8, SELECT = 16;

/* ---- the cart ------------------------------------------------------------ */

let folder = {};              /* name: text, or bytes for a picture */
let current = 'main.js';
let lastBuild = null;
let saveTo = null;            /* a file or folder handle Save writes to */
let dirty = false;            /* changed since it was opened or saved */
const gone = new Set();       /* renamed or deleted since, for Save to delete */
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
        d.transaction('carts', 'readwrite').objectStore('carts').put({ folder, current, dirty }, 'open');
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
    dirty = true;
    clearTimeout(keepTimer);
    keepTimer = setTimeout(() => { keep(); check(); }, 400);
}

/* the cart written out as it is: nothing to lose by replacing it */
function saved() {
    dirty = false;
    keep();
}

function cartName() {
    try { return JSON.parse(folder['cart.json']).name || 'Untitled'; } catch (e) { return 'Untitled'; }
}

/* The cart's name as a file name, without its extension */
function cartStem() {
    return cartName().replace(/[^\w -]/g, '').trim().replace(/ +/g, '_').toLowerCase();
}

/* blob downloaded as a file called name */
function download(blob, name) {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = name;
    a.click();
}

function cartId() {
    try { return JSON.parse(folder['cart.json']).id || ''; } catch (e) { return ''; }
}

/* unsaved: the cart has changes not yet saved, as one kept from last time
 * can */
function load(files, file, unsaved) {
    stop();
    if (view) { view.destroy(); view = null; }
    folder = files;
    for (const k of Object.keys(states)) delete states[k];
    gone.clear();
    current = file && folder[file] !== undefined ? file : 'main.js';
    showFiles();
    show(current);
    check();
    clearTimeout(keepTimer);
    dirty = !!unsaved;
    keep();
}

/* whether the open cart may be replaced, asked when it has changes unsaved */
async function mayReplace() {
    return !dirty || await ask('Discard the unsaved changes?', [], 'Discard them') !== null;
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
    const code = /\.js$/.test(file);
    const lang = code ? [javascript()] : /\.json$/.test(file) ? [json()] : [];
    /* the API's hints and completions are for code only */
    const api = code ? [...hints(() => folder), autocompletion({ override: [completions] })] : [];
    /* Tab indents, by the carts' four spaces, rather than leaving the
     * editor; Escape then Tab still leaves it */
    states[file] = EditorState.create({
        doc: String(folder[file]),
        extensions: [basicSetup, keymap.of([indentWithTab]), indentUnit.of('    '),
                     findPanel, ...lang, ...dark, lintGutter(), ...api,
                     EditorView.updateListener.of((u) => {
                         if (!u.docChanged) return;
                         folder[current] = u.state.doc.toString();
                         if (isEditor(current)) authored.add(current);
                         changed();
                     })]
    });
    return states[file];
}

let pixels = null;
/* the switch between an editor and the text, in the editor's tab bar; the
 * opener of an editor with a switch of its own sets it */
let editorMode = null, showing = 0;

function show(file) {
    const at = ++showing;
    editorMode = null;
    if (view) states[current] = view.state;
    if (pixels) { pixels.destroy(); pixels = null; }
    dock.clear();
    current = file;
    dock.setTitle('editor', file);
    if (!dock.shown('editor')) dock.show('editor');
    document.querySelector('[data-do="export-picture"]').hidden = !isBinary(file);
    const pane = $('editor');
    const gplFile = /^palettes\/\w+\.gpl$/.test(file), bdfFile = /^fonts\/\w+\.bdf$/.test(file);
    const mapFile = /^maps\/\w+\.json$/.test(file), sfxFile = /^sounds\/\w+\.json$/.test(file);
    const cfgFile = file === 'cart.json';
    const custom = editorOf(folder, file);
    const visual = gplFile || bdfFile || mapFile || sfxFile || cfgFile || !!custom;
    const runEditor = isEditor(file) && runEditors[file];
    if (isBinary(file) || (visual && !asText[file]) || runEditor) {
        if (view) { view.destroy(); view = null; }
        pane.innerHTML = '';
        if (runEditor) {
            showCartEditor(pane, null, file, at);
            editorMode = iconButton('text', 'Edit its HTML', () => { delete runEditors[file]; show(file); });
        } else if (custom) showCartEditor(pane, file, custom, at);
        else if (gplFile) showPalette(pane, file);
        else if (bdfFile) showFont(pane, file);
        else if (mapFile) showMap(pane, file);
        else if (sfxFile) showSound(pane, file);
        else if (cfgFile) showSettings(pane, file);
        else showPicture(pane, file);
        if (visual && !runEditor)
            editorMode = iconButton('text', 'Edit as text', () => { asText[file] = true; show(file); });
    } else {
        if (view && view.dom.parentElement !== pane) { view.destroy(); view = null; }
        if (!view) {
            pane.innerHTML = '';
            view = new EditorView({ state: stateFor(file), parent: pane });
        } else view.setState(stateFor(file));
        /* a file with an editor of its own goes back to it */
        if (visual) {
            const [ic, label] = gplFile ? ['swatches', 'Edit as swatches'] : bdfFile ? ['glyphs', 'Edit as glyphs']
                : mapFile ? ['grid', 'Edit as a map'] : sfxFile ? ['waveSine', 'Edit as a sound']
                : custom ? ['settings', 'Edit with ' + custom.slice(8)] : ['settings', 'Edit as a form'];
            editorMode = iconButton(ic, label, () => { delete asText[file]; check(); show(file); });
        }
        if (isEditor(file))
            editorMode = iconButton('play', 'Run this editor', () => { runEditors[file] = true; show(file); });
    }
    /* an editor that could not open shows its text instead, from within */
    if (at !== showing) return;
    dock.setActions('editor', editorMode ? [editorMode] : []);
    for (const li of $('files').querySelectorAll('li'))
        li.classList.toggle('open', li.dataset.file === file);
    dock.done();
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

/* the cart's editors (editors/*.html) shown running rather than as
 * their HTML, and those the owner wrote in the IDE, which run unasked */
const runEditors = {};
const authored = new Set();

/* An editor of the cart's own, in its frame, for file (null when it is
 * run as itself) -- once the owner has said it may run */
async function showCartEditor(pane, file, editor, at) {
    const html = String(folder[editor]);
    if (!authored.has(editor) && !(await approved(html))) {
        if (at !== showing) return;
        const p = document.createElement('div');
        p.className = 'cart-editor-ask';
        const say1 = document.createElement('p');
        say1.textContent = 'This cart has an editor of its own, ' + editor + '. It runs inside the IDE, ' +
            'walled off from the IDE and the network, and can change this cart\'s files. Run it?';
        const go = document.createElement('button');
        go.textContent = 'Run it';
        go.onclick = async () => { await approve(html); show(current); };
        const text = document.createElement('button');
        text.textContent = 'Show it as text';
        text.onclick = () => {
            if (file) asText[file] = true; else delete runEditors[editor];
            show(current);
        };
        p.append(say1, go, text);
        pane.append(p);
        return;
    }
    if (at !== showing) return;
    pixels = new CartEditor(pane, {
        folder: folder, file: file, html: html,
        written: (name, isNew) => {
            delete states[name];
            if (isNew) showFiles();
            changed();
        },
        run: runUnseen
    });
}

/* The units one call of a run nobody watches may take: an editor's check
 * works through all of a cart's data at once, not a frame of it */
const UNSEEN_CEILING = 400000000;

/* A cart's editor's cart.run(): the cart built with the .js files named and
 * no others (true for the cart's own), any other file named in place of
 * the cart's, and run unseen for up to ticks ticks or until it prints
 * "done". Every line printed, and the error that stopped it, if one did. */
async function runUnseen(files, ticks) {
    const f = {};
    for (const [n, v] of Object.entries(folder))
        if (!/\.js$/.test(n)) f[n] = v;
    for (const [n, v] of Object.entries(files))
        f[n] = v === true ? folder[n] : v;
    const b = build(f);
    if (b.errors.length)
        return { lines: [], error: b.errors.map((x) => x.file + ':' + x.line + ': ' + x.message).join('\n') };
    const lines = [];
    const e = new Engine(wasm, { print: (s) => lines.push(s) });
    if (!e.load(b.cart, 1n)) return { lines, error: e.error() };
    e.ceiling(UNSEEN_CEILING);
    if (!(await e.start())) return { lines, error: e.error() };
    for (let i = 0; i < Math.min(ticks, 100000) && lines[lines.length - 1] !== 'done'; i++) {
        if (!(await e.tick(0, 0, 0, false))) return { lines, error: e.error() };
        if (i % 20 === 19) await sleep(0);
    }
    return { lines };
}

/* a font in the font editor */
function showFont(pane, file) {
    try {
        pixels = new FontEditor(pane, {
            text: String(folder[file]), ask: ask,
            say: (text, bad) => say(bad ? 'error' : 'print', text),
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
            text: String(folder[file]), file: file, cart: cartId(), sheet: sheet, rgb: palette(folder), flags: readFlags(),
            kinds: readKinds(),
            flagNames: flagNames, ask: ask,
            say: (text, bad) => say(bad ? 'error' : 'print', text),
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
            say: (text, bad) => say(bad ? 'error' : 'print', text),
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

/* objects.json's kinds, or null for any: none, or a file the build
 * reports as wrong */
function readKinds() {
    if (folder['objects.json'] === undefined) return null;
    try { return objectKinds(String(folder['objects.json'])); } catch (e) { return null; }
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
        rgb: rgb, first: first, pictures: () => pictures(),
        say: (text, bad) => say(bad ? 'error' : 'print', text),
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
    download(new Blob([folder[current]], { type: 'image/png' }), current.split('/').pop());
}

/* A picture's new canvas: its size, and where the picture sits in it.
 * Promise of { w, h, ax, ay }, or null */
const ANCHORS = [['0,0', 'Top left'], ['1,0', 'Top'], ['2,0', 'Top right'],
                 ['0,1', 'Left'], ['1,1', 'Middle'], ['2,1', 'Right'],
                 ['0,2', 'Bottom left'], ['1,2', 'Bottom'], ['2,2', 'Bottom right']];

async function askCanvas(w, h) {
    const side = (v) => /^\d+$/.test(String(v).trim()) && +v >= 1 && +v <= 1024 ? null : '1 to 1,024 pixels';
    const a = await ask('Canvas size', [
        { text: 'Now ' + w + 'x' + h + '. Made smaller, what falls outside is cut off; made bigger, the ' +
                'new room is colour 0, which is see-through.' },
        { name: 'w', label: 'Width', value: String(w), type: 'number', check: side },
        { name: 'h', label: 'Height', value: String(h), type: 'number', check: side },
        { name: 'at', label: 'The picture at the', options: ANCHORS, value: '0,0' }], 'Resize');
    if (!a) return null;
    const [ax, ay] = String(a.at).split(',').map(Number);
    return { w: +a.w, h: +a.h, ax, ay };
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
    /* the palette as it is now: adding colours rewrites it under the editor */
    const write = (px) => {
        folder[file] = writePicture(pic.width, pic.height, px, palette(folder));
        changed();
    };
    const say_ = (text, bad) => say(bad ? 'error' : 'print', text);
    /* an image a cell or more each way can be edited as sprites, for sheet() */
    const cells = pic.width >= 8 && pic.height >= 8;
    if (!sheet && !(cells && asSprites[file])) {
        /* an image can change size; the logo is the size lists of games show */
        const sized = file !== 'logo.png' ? {
            resize: (w, h, px) => {
                folder[file] = writePicture(w, h, px, palette(folder));
                changed();
                show(file);
                say('print', file + ' is now ' + w + 'x' + h + '.');
            },
            askSize: askCanvas
        } : {};
        pixels = new PixelEditor(pane, Object.assign({ width: pic.width, height: pic.height, px: pic.px, rgb: rgb,
                                                       changed: write, fitColours: fitPicture, say: say_ }, sized));
        if (cells) editorMode = iconButton('sprites', 'Edit as sprites: 8x8 cells, drawn after sheet(IMG.' +
            file.slice(7, -4) + ')', () => { setAsSprites(file, true); show(file); });
        pixels.root.focus();
        return;
    }
    if (!sheet) {
        pixels = new SheetEditor(pane, {
            px: pic.px, width: pic.width, height: pic.height, image: file.slice(7, -4),
            rgb: rgb, fitColours: fitPicture,
            anim: imageAnims[file] || null,
            animChanged: (a) => { imageAnims[file] = a; },
            changed: write, say: say_
        });
        editorMode = iconButton('picture', 'Edit as one picture', () => { setAsSprites(file, false); show(file); });
        return;
    }
    pixels = new SheetEditor(pane, {
        px: pic.px, rgb: rgb, flags: flags, fitColours: fitPicture,
        anim: sheetAnim,
        animChanged: (a) => { sheetAnim = a; },
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
    document.title = cartName() + ' - ArcadeIDE';
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

/* The players a cart can be run as, in ms on each, all measured: a unit of
 * script, a whole screen's rows changed and sent, and a whole screen's rows
 * drawn but found unchanged against the last frame. Music means FLAC, the
 * heaviest to decode. */
const DEVICES = {
    '6g': { name: 'iPod Classic', unit: 0.00016, screen: 2.2, same: 0.5 },
    '5g': { name: 'iPod Video', unit: 0.00055, screen: 5.0, same: 1.8 },
    '5g-music': { name: 'iPod Video, playing FLAC', unit: 0.0019, screen: 6.6, same: 2.0 },
    'free': { name: 'this computer, no limit', unit: 0, screen: 0, same: 0 }
};
let device = '5g-music';
try { if (DEVICES[localStorage.getItem('arcade-device')]) device = localStorage.getItem('arcade-device'); } catch (e) { /* none kept */ }

/* The units one call may run before the player stops the cart as not
 * responding: the shell's CEILING */
const CEILING = 2000000;

/* the manual, on the repository's pages */
const MANUAL = 'https://github.com/anthonyfletcher/podbox/blob/master/arcade/manual/';

/* The player's clock, in ms of its own time: each tick takes what its
 * units and the rows it drew take on the player, and when a tick runs
 * late the player's rule applies -- behind by a whole tick, one update
 * without drawing; further behind, the time is given up. */
let clock;
const HELD_BACK = 250;              /* ms the page may run late, caught up */

function clockStart() {
    clock = { real: performance.now(), emu: 0, start: 0, frame: 0, drawn: [] };
    seen = null;
}

/* after time the player does not count (a pause, a dialog), from now on */
function rebase(e) {
    seen = null;                    /* the player redraws it all after */
    clock.emu = performance.now() - clock.real;
    clock.start = clock.emu - clock.frame * (1000 / e.rate());
}

async function until(ms, e, live) {
    await sleep(clock.real + ms - performance.now());
    /* the page held back -- a tab out of sight runs its timers late --
     * is time the player never had: given up, not caught up */
    const over = performance.now() - (clock.real + ms);
    if (over > HELD_BACK) clock.real += over;
    if (!paused && !held()) return;
    while ((paused || held()) && live()) await sleep(50);
    rebase(e);
}

/* What holds the run between ticks, as the player's pause menu holds the
 * game: a Game-menu dialog open, or a menu item's function running. A
 * cart's own dialog waits for the first, not the second, which may open
 * one. busy: in a call into the engine, which a menu item's must not
 * overlap. */
let dialogs = 0, choosing = 0, busy = 0;
const held = () => dialogs > 0 || choosing > 0;

async function call(p) {
    busy++;
    try { return await p; } finally { busy--; }
}

/* fn() run with the run held, the clock started again after */
async function hold(fn) {
    dialogs++;
    if (engine) engine.paused();
    try { return await fn(); } finally {
        dialogs--;
        if (running && engine) rebase(engine);
    }
}

/* whether a Game-menu choice can be made now: a run between ticks, and
 * not a replay, whose choices are its tape's */
const free = () => running && !busy && !held() && !playing;

/* A Game-menu choice: { i } a menu item, its dialog frames recorded in
 * rec.d, or answered from c.d on a replay; { o: [[option, value]] } the
 * options set. */
async function makeChoice(e, c, rec) {
    if (c.o) { for (const [k, v] of c.o) e.setOption(k, v); return; }
    const was = step;
    step = rec;
    dialogTape = rec === c ? null : (c.d || []).slice();
    choosing++;
    try {
        const r = await call(e.menuChoose(c.i));
        if (r < 0 && engine === e && running) say('error', e.error());
    } finally { choosing--; step = was; }
}

/* a choice from the Game menu, recorded for the run's next step */
async function choose(c) {
    if (!free()) return;
    const e = engine;
    await makeChoice(e, c, c);
    if (engine !== e || !running) return;
    choices.push(c);
    rebase(e);
}

/* The last frame's pixels, as the player keeps them to compare each drawn
 * row against: a row unchanged is not sent. Null until a frame is shown,
 * and after a palette change, when every row is sent. */
let seen = null;

function cost(e, drawn) {
    const d = DEVICES[device];
    let ms = e.units() * d.unit;
    if (drawn) {
        const [lo, hi] = e.dirty();
        if (hi >= lo) {
            const now = e.pixels();
            let changed = 0;
            if (!seen || e.paletteChanged()) {
                changed = hi - lo + 1;
                seen = now;
            } else {
                for (let y = lo; y <= hi; y++) {
                    const a = y * 160;
                    for (let x = a; x < a + 160; x++)
                        if (now[x] !== seen[x]) { changed++; seen.set(now.subarray(a, a + 160), a); break; }
                }
            }
            ms += (d.screen * changed + d.same * (hi - lo + 1 - changed)) / 120;
        }
        /* and the four rows the player sends every frame regardless */
        ms += (d.screen * 4) / 120;
    }
    return ms;
}

/* the run being recorded, the last one recorded, and the tape being
 * played back with how far it has got */
let tape = null, lastTape = null, playing = null, played = 0, diverged = false;
/* this turn of the loop's record, and music buttons pressed and Game-menu
 * choices made since the last */
let step = null, musicPressed = [], choices = [];
/* where the run's store lives: the browser's, or a replay's own */
let runStore = null;

/* the cart's store as it is now, to start a tape with */
function storeSnapshot() {
    const out = {}, pre = storeKey('');
    for (const k of Object.keys(localStorage)) if (k.startsWith(pre)) out[k.slice(pre.length)] = localStorage.getItem(k);
    return out;
}

const browserStore = {
    read: (name) => {
        const v = localStorage.getItem(storeKey(name));
        return v === null ? undefined : new TextEncoder().encode(v);
    },
    write: (name, bytes) => {
        /* the player's bounds: 64 names and 256 KB a cart */
        const pre = storeKey(''), names = Object.keys(localStorage).filter((k) => k.startsWith(pre));
        const fresh = localStorage.getItem(storeKey(name)) === null;
        let total = bytes.length;
        for (const k of names) if (k !== storeKey(name)) total += new TextEncoder().encode(localStorage.getItem(k)).length;
        if ((fresh && names.length >= 64) || total > 256 * 1024) return false;
        try { localStorage.setItem(storeKey(name), new TextDecoder().decode(bytes)); return true; } catch (e) { return false; }
    },
    remove: (name) => {
        const had = localStorage.getItem(storeKey(name)) !== null;
        localStorage.removeItem(storeKey(name));
        return had;
    },
    list: () => Object.keys(localStorage).filter((k) => k.startsWith(storeKey(''))).
        map((k) => k.slice(storeKey('').length)).sort()
};

/* this tick's input, from the player or from the tape */
const live3 = () => { const i = input(); return [i.held, i.clicks, i.pos]; };

/* a run, and a fault of the IDE's own in one -- a tape it cannot follow --
 * ending it and said, as the cart's own errors are */
async function run(replay) {
    const gen = generation;
    try { await runCart(replay); } catch (x) {
        if (generation === gen + 1 && running) stop(true);
        say('error', 'The run stopped: ' + (x && x.message || x));
    }
}

async function runCart(replay) {
    stop();
    check();
    if (lastBuild.errors.length) {
        say('error', lastBuild.errors.map((e) => e.file + ':' + e.line + ': ' + e.message).join('\n'),
            lastBuild.errors[0]);
        return;
    }
    $('console').textContent = '';
    quietRun = false;
    if (!dock.shown('player')) dock.show('player', false);
    /* a run ends when another starts: each keeps its own engine */
    const gen = ++generation, live = () => running && generation === gen;
    const e = engine = new Engine(wasm, hostFor(() => e, live));
    playing = replay || null;
    played = 0;
    diverged = false;
    musicPressed = [];
    choices = [];
    const seed = replay ? replay.seed : String(Date.now());
    let id = '';
    try { id = JSON.parse(folder['cart.json']).id; } catch (x) { /* no id */ }
    tape = { version: VERSION, cart: { id, name: cartName(), code: hashCart(lastBuild.cart) }, seed,
             music: replay ? replay.music : musicOn, store: replay ? replay.store : storeSnapshot(), steps: [] };
    runStore = replay ? memoryStore(replay.store) : browserStore;
    if (replay) {
        say('print', 'Replaying ' + replay.steps.length + ' frames of ' + replay.cart.name +
            (replay.cart.code !== tape.cart.code ? '. The cart has changed since they were recorded, so it may go its own way.' : '.'));
    }
    e.music({ playingFn: true, beat: true, playing: tape.music, bpm: 0 });
    if (!e.load(lastBuild.cart, BigInt(seed))) return fault();
    tape.kept = game.begin(e, replay ? replay.kept || {} : null);
    tape.start = replay && replay.start ? replay.start : Date.now();
    ticksRun = 0;
    profSections = new Map();
    metronomeAt = -1;
    e.ceiling(CEILING);
    profiler.begin(functions(lastBuild.cart));
    memoryView.begin(lastBuild.globals || []);
    memoryAt = 0;
    e.profile(profiler.on);
    if (!audio) audio = new AudioContext({ sampleRate: 44100 });
    audio.resume();
    audioAt = 0;
    running = true;
    paused = false;
    $('screen').focus();
    document.body.classList.add('running');
    $('power').title = 'Turn off: stop the cart (Shift+Ctrl+Enter)';
    clockStart();
    if (!(await call(e.start()))) return live() ? fault() : undefined;
    if (!live()) return;
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
        const t = playing && played < playing.steps.length ? playing.steps[played++] : null;
        if (playing && !t) {
            playing = null;
            say('print', 'The replay is over: the cart carries on with your input.');
        }
        step = {};
        tape.steps.push(step);
        dialogShown = false;
        if (t && t.m) for (const w of t.m) { e.musicControl(w); music(e); }
        const pressed = t ? t.m || [] : musicPressed.splice(0);
        if (pressed.length) step.m = pressed;
        if (t && t.g) for (const c of t.g) {
            await makeChoice(e, c, {});
            if (!live()) return;
        }
        const made = t ? t.g || [] : choices.splice(0);
        if (made.length) step.g = made;
        dialogTape = t && t.d ? t.d.slice() : null;
        const skip = t ? t.s || 0 : late >= p ? Math.floor(late / p) : 0;
        if (skip) {
            e.skipped(skip);
            const i = t ? t.a : live3();
            step.s = skip;
            step.a = i;
            if (!(await call(e.tick(i[0], i[1], i[2], false)))) return live() ? fault() : undefined;
            if (!live()) return;
            ticksRun++;
            const c = cost(e, false);
            clock.emu += c;
            profiler.frame(e.samples(), c, p, c - e.units() * DEVICES[device].unit, takeSections(), e.units());
            if (game.tick(e)) return quitRun();
            clock.frame++;
            if (late >= 2 * p) clock.start = clock.emu - clock.frame * p;
        }
        const i = t ? t.b : live3();
        step.b = i;
        const ok = await call(e.tick(i[0], i[1], i[2], true, Math.round(1000 / rate)));
        if (!live()) break;
        if (!ok) return fault();
        step.h = hashScreen(e.pixels());
        if (t && t.h !== step.h && !diverged) {
            diverged = true;
            say('print', 'The replay stops matching the recording at frame ' + tape.steps.length +
                ', about ' + (tape.steps.length / rate).toFixed(1) + ' s in.');
        }
        ticksRun++;
        if (dialogShown) rebase(e);
        else {
            const c = cost(e, true);
            clock.emu += c;
            profiler.frame(e.samples(), c, 1000 / rate, c - e.units() * DEVICES[device].unit, takeSections(), e.units());
            /* twice a second while the panel shows: the walk is the whole heap */
            if (dock.shown('memory') && performance.now() - memoryAt > 500) {
                memoryAt = performance.now();
                memoryView.show(e.memory());
            }
        }
        if (game.tick(e)) return quitRun();
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

/* quit(): the cart is over, as on the player, which goes back to its menu */
function quitRun() {
    stop(true);
    say('print', 'The cart quit.');
}

/* the ticks run, for the date the cart is told: the run's start and the
 * player's time since, so a replay is told the same */
let ticksRun = 0;

function runDate() {
    const d = new Date((tape ? tape.start : Date.now()) + ticksRun * 1000 / (engine ? engine.rate() : 33));
    return [d.getFullYear(), d.getMonth() + 1, d.getDate(), d.getHours(), d.getMinutes(), d.getSeconds(), d.getDay()];
}

/* prof() sections: begun and ended within a tick, their units summed by
 * name for the profiler */
let profSections = new Map(), profOpen = [];

function profHook(name) {
    if (!engine) return;
    const now = engine.unitsNow();
    if (name !== null) { profOpen.push([name, now]); return; }
    const top = profOpen.pop();
    if (top) profSections.set(top[0], (profSections.get(top[0]) || 0) + now - top[1]);
}

function takeSections() {
    const s = profSections;
    profSections = new Map();
    profOpen = [];
    return s;
}

/* the cart stopped; the screen cleared, but for an error, whose last
 * frame stays to be looked at */
function stop(keep) {
    /* the cart closing runs its ending(), as the player's does */
    if (running && engine && !engine.error() && !engine.end())
        say('error', engine.error());
    if (tape && tape.steps.length) lastTape = tape;
    tape = null;
    playing = null;
    running = false;
    paused = false;
    document.body.classList.remove('running', 'paused');
    $('power').title = 'Turn on: run the cart (Ctrl+Enter)';
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
    date: runDate,
    watch: (name, value) => game.watch(name, value),
    prof: profHook,
    musicSay: (line) => say('music', 'music: ' + line),
    click: (beep) => game.click(beep, audio),
    say: (text, queue) => say('say', (queue ? 'then says: ' : 'says: ') + text),
    speechOn: () => $('speech').classList.contains('on'),
    glyph: glyph,
    storeRead: (name) => runStore.read(name),
    storeWrite: (name, bytes) => runStore.write(name, bytes),
    storeRemove: (name) => runStore.remove(name),
    storeList: () => runStore.list(),
    /* a dialog's frame: shown, then the next tick's input, a tick of the
     * cart's rate later; null backs out. The player's clock starts again
     * after it. */
    frame: (open) => {
        if (!open) return null;
        dialogShown = true;
        return Promise.resolve().then(async () => {
            if (live()) present();
            await sleep(1000 / e().rate());
            while (((paused && !choosing) || dialogs) && live()) await sleep(50);
            if (!live()) return null;
            /* from the tape while it has them */
            const r = dialogTape && dialogTape.length ? dialogTape.shift() : null;
            const i = r ? { held: r[0], clicks: r[1] } : input(true);
            if (step) (step.d || (step.d = [])).push([i.held, i.clicks]);
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
/* a replayed step's dialog frames, still to answer */
let dialogTape = null;

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
        clicksOwed += e.key === 'ArrowDown' ? 4 : -4;
        e.preventDefault();
        return;
    }
    const b = KEYS[e.key];
    if (!b) return;
    e.preventDefault();
    if (b === MENU && engine && !engine.menuIsGame() && !e.repeat) {
        paused = !paused;
        if (paused) engine.paused();
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

/* a music button while a cart runs: recorded with the run, and left to
 * the tape while one plays */
function musicButton(what) {
    if (playing) return;
    engine.musicControl(what);
    musicPressed.push(what);
    music(engine);
}
$('m-play').onclick = () => {
    if (engine && running) musicButton(0); else {
        musicOn = !musicOn;
        $('m-play').innerHTML = icon(musicOn ? 'pause' : 'play');
    }
};
$('m-next').onclick = () => { if (engine && running) musicButton(1); };
$('m-prev').onclick = () => { if (engine && running) musicButton(2); };

/* ---- replays ------------------------------------------------------------ */

/* the last run, or the one running, as a file to play again */
function saveReplay() {
    const t = tape && tape.steps.length ? tape : lastTape;
    if (!t) { say('error', 'Run the cart first: each run is recorded to save.'); return; }
    download(new Blob([JSON.stringify(t)], { type: 'application/json' }), cartStem() + '.replay.json');
    say('print', 'Saved a replay of ' + t.steps.length + ' frames.');
}

/* a replay from a file, played on the cart open, which must be the one
 * it was made from */
function openReplay() {
    const i = document.createElement('input');
    i.type = 'file';
    i.accept = '.json';
    i.onchange = async () => {
        const f = i.files[0];
        if (!f) return;
        const t = readTape(await f.text());
        if (t instanceof Error) { say('error', f.name + ': ' + t.message + '.'); return; }
        let id = '';
        try { id = JSON.parse(folder['cart.json']).id; } catch (x) { /* no id */ }
        if (t.cart.id !== id) { say('error', f.name + ' is a replay of ' + t.cart.name + ', not of this cart.'); return; }
        run(t);
    };
    i.click();
}

/* ---- the console, and what the player would show -------------------------- */

/* the ticks drawn in the last second of the player's time, against the
 * cart's rate, on the player it runs as */
function shown(rate) {
    const t = clock.drawn;
    t.push(clock.emu);
    while (t.length && t[0] < clock.emu - 1000) t.shift();
    $('rate').textContent = (playing ? 'Replaying ' + played + ' of ' + playing.steps.length + '; ' : '') +
        (t.length >= rate ? rate : t.length + ' of ' + rate) +
        ' frames a second as ' + (device === 'free' ? 'fast as this computer' : 'an ' + DEVICES[device].name);
}

function showDevice() {
    for (const b of document.querySelectorAll('[data-device]'))
        b.classList.toggle('on', b.dataset.device === device);
    $('rate').textContent = device === 'free' ? 'Runs as fast as this computer.'
        : 'Runs as an ' + DEVICES[device].name + '.';
}

/* a line in the output, which opens to show it -- but for the game's own
 * lines once the output was closed during this run, and for all but an
 * error while it is minimised */
let quietRun = false;

function say(kind, text, where) {
    if (dock && !dock.shown('output') &&
        (kind === 'error' || !((running && quietRun) || dock.isMinimised('output'))))
        dock.show('output', false);
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

async function openBytes(bytes, handle) {
    if (!(await mayReplace())) return;
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
                ['palettes', 'maps', 'images', 'sounds', 'data', 'fonts', 'editors'].includes(name))
                await walk(h, name + '/', 1);
            else if (h.kind === 'file' && (CART_FILE.test(prefix + name) || isEditor(prefix + name))) {
                const f = await h.getFile();
                files[prefix + name] = isBinary(name) ? new Uint8Array(await f.arrayBuffer()) : await f.text();
            }
        }
    }
    try { await walk(dir, '', 0); } catch (e) { say('error', 'Could not read that folder: ' + e.message); return; }
    if (!files['cart.json']) { say('error', 'That folder has no cart.json: it is not a cart.'); return; }
    if (!(await mayReplace())) return;
    load(files);
    saveTo = { dir };
    say('print', 'Opened the folder ' + dir.name + ': Save writes its files back.');
}

function openFolderPlain() {
    const i = document.createElement('input');
    i.type = 'file';
    i.webkitdirectory = true;
    i.onchange = async () => {
        const files = {};
        for (const f of i.files) {
            /* the path below the folder chosen */
            const rel = f.webkitRelativePath.split('/').slice(1).join('/');
            if (CART_FILE.test(rel) || isEditor(rel))
                files[rel] = isBinary(rel) ? new Uint8Array(await f.arrayBuffer()) : await f.text();
        }
        if (!files['cart.json']) { say('error', 'That folder has no cart.json: it is not a cart.'); return; }
        if (!(await mayReplace())) return;
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

/* ---- served by pbgide ---------------------------------------------------- *
 * pbgide serves this page from the computer it runs on, with a cart folder
 * it reads and writes on the page's behalf: the page's address carries the
 * token every request about the folder has to send back.
 *
 * The folder is looked at again every SYNC_MS, and a file changed there --
 * by another editor, or another program -- is taken in where it has not
 * been changed here as well since it was last read or saved; where it has,
 * the owner is asked which to keep. disk holds each file as last read or
 * saved, and diskHash the server's fingerprint of it. */

const SERVED = new URLSearchParams(location.search).get('token');
const SYNC_MS = 1500;

let disk = {}, diskHash = {}, syncing = false;

function fromServer(path, init) {
    const o = Object.assign({}, init);
    o.headers = Object.assign({ 'X-Arcade-Token': SERVED }, o.headers);
    return fetch(path, o).then((r) => {
        if (!r.ok) throw new Error(r.status + ' ' + r.statusText);
        return r;
    });
}

async function readServed(name) {
    const r = await fromServer('/cart/file?path=' + encodeURIComponent(name));
    return isBinary(name) ? new Uint8Array(await r.arrayBuffer()) : await r.text();
}

function same(a, b) {
    if (typeof a === 'string' || typeof b === 'string') return a === b;
    if (!a || !b || a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}

const copyOf = (v) => typeof v === 'string' ? v : new Uint8Array(v);

/* The folder pbgide was started with, opened; false where there is none */
async function openServed() {
    if (!SERVED || !/^http/.test(location.protocol)) return false;
    let list;
    try { list = await (await fromServer('/cart/list')).json(); } catch (e) {
        say('error', /^403/.test(e.message)
            ? 'pbgide no longer knows this page: it has been started again since. Open the address it ' +
              'printed this time. This is the cart the browser kept from before, not the folder.'
            : 'pbgide is not answering (' + e.message + '): start it again, and open the address it prints.');
        return false;
    }
    const files = {};
    disk = {};
    diskHash = {};
    try {
        for (const { path, hash } of list.files) {
            if (!CART_FILE.test(path) && !isEditor(path)) continue;
            files[path] = await readServed(path);
            disk[path] = copyOf(files[path]);
            diskHash[path] = hash;
        }
    } catch (e) {
        say('error', 'Could not read the folder pbgide serves: ' + e.message);
        return false;
    }
    if (!files['cart.json']) return false;
    load(files);
    saveTo = { served: list.name };
    say('print', 'Opened the folder ' + list.name + ' through pbgide: Save writes its files back, and ' +
        'files changed there are taken in as they change.');
    setInterval(syncServed, SYNC_MS);
    return true;
}

/* The folder looked at again: what changed there taken in */
async function syncServed() {
    if (syncing || !saveTo || !saveTo.served) return;
    syncing = true;
    try {
        const list = await (await fromServer('/cart/list')).json();
        const now = {}, took = [];
        for (const { path, hash } of list.files)
            if (CART_FILE.test(path) || isEditor(path)) now[path] = hash;
        for (const name of Object.keys(now)) {
            if (diskHash[name] === now[name]) continue;
            const v = await readServed(name);
            const mine = folder[name];
            /* unchanged here since it was last read: the disk's, as it is */
            if (mine === undefined && !gone.has(name) || mine !== undefined && same(mine, disk[name]) ||
                mine !== undefined && same(mine, v) ||
                await ask(name + ' changed on the disk', [{ text: name + ' has been changed in the folder, and ' +
                    'here too since it was last saved. Take the folder\'s, losing the changes made here?' }],
                    'Take the folder\'s') !== null) {
                folder[name] = copyOf(v);
                gone.delete(name);
                took.push(name);
            }
            disk[name] = copyOf(v);
            diskHash[name] = now[name];
        }
        /* gone from the disk: gone here too, unless changed here since */
        for (const name of Object.keys(diskHash)) {
            if (now[name] !== undefined) continue;
            if (folder[name] !== undefined && same(folder[name], disk[name])) {
                delete folder[name];
                took.push(name);
            }
            delete disk[name];
            delete diskHash[name];
        }
        if (took.length) {
            const wasDirty = dirty;
            /* the open editor goes too, or show() keeps its old text */
            if (view && took.includes(current)) { view.destroy(); view = null; }
            for (const name of took) delete states[name];
            showFiles();
            if (took.includes(current)) show(folder[current] !== undefined ? current : 'main.js');
            changed();
            if (!wasDirty) saved();
            say('print', 'Taken in from the folder: ' + took.join(', ') + '.');
        }
    } catch (e) {
        /* pbgide stopped, or busy: looked at again next time */
    }
    syncing = false;
}

/* Save through pbgide: each file written, and the ones renamed or deleted
 * removed; what was written is then the disk's as far as the next look is
 * concerned */
async function saveServed() {
    try {
        for (const [name, v] of Object.entries(folder))
            await fromServer('/cart/file?path=' + encodeURIComponent(name), { method: 'PUT', body: v });
        for (const name of gone)
            if (folder[name] === undefined)
                await fromServer('/cart/file?path=' + encodeURIComponent(name), { method: 'DELETE' });
        const list = await (await fromServer('/cart/list')).json();
        disk = {};
        diskHash = {};
        for (const { path, hash } of list.files) {
            if (folder[path] === undefined) continue;
            disk[path] = copyOf(folder[path]);
            diskHash[path] = hash;
        }
    } catch (e) {
        say('error', 'Could not save through pbgide (' + e.message + '): is it still running? ' +
            'The cart is kept here; Save as .pbg writes it out instead.');
        return;
    }
    gone.clear();
    saved();
    say('print', 'Saved the folder ' + saveTo.served + '.');
}

/* Save: back to the folder or the .pbg the cart came from, or else as a
 * new .pbg. Where the browser will not write there, a new .pbg instead. */
async function save() {
    if (saveTo && saveTo.served) {
        check();
        if (lastBuild.errors.length) { say('error', 'Fix the errors before saving: the cart would not build.'); return; }
        return saveServed();
    }
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
            /* the files renamed or deleted since, deleted there too */
            for (const name of gone) {
                if (folder[name] !== undefined) continue;
                const parts = name.split('/');
                try {
                    let d = saveTo.dir;
                    for (const p of parts.slice(0, -1)) d = await d.getDirectoryHandle(p);
                    await d.removeEntry(parts[parts.length - 1]);
                } catch (e) {
                    if (e.name !== 'NotFoundError') throw e;
                }
            }
            gone.clear();
            saved();
            say('print', 'Saved the folder ' + saveTo.dir.name + '.');
            return;
        }
        if (saveTo && saveTo.file) {
            const w = await saveTo.file.createWritable();
            await w.write(lastBuild.cart);
            await w.close();
            saved();
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
    const file = cartStem() + '.pbg';
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
                saved();
                say('print', 'Saved ' + h.name + '.');
                return;
            } catch (e) { /* downloaded below */ }
        }
    }
    download(new Blob([lastBuild.cart]), file);
    saved();
    say('print', 'Downloaded ' + file + '.');
}

/* Save as a new game: the cart made a game of its own -- a new id, so its
 * saves and high scores start empty, and the name asked for -- and saved
 * as a new .pbg, the one it came from left as it was */
async function saveNewGame() {
    const a = await ask('Save as a new game', [{ name: 'name', label: 'Name', value: cartName() + ' 2',
        note: 'The new game starts with none of this one\'s saves or high scores',
        check: (n) => n.trim() ? null : 'A name to show in the Games menu' }], 'Make it');
    if (!a) return;
    try {
        folder['cart.json'] = asNewGame(folder['cart.json'], a.name.trim());
    } catch (e) {
        say('error', 'cart.json is not readable, so the cart cannot be made a new game: ' + e.message);
        return;
    }
    delete states['cart.json'];
    saveTo = null;
    gone.clear();
    showFiles();
    if (current === 'cart.json') show('cart.json');
    changed();
    say('print', 'This is now a new game, ' + cartName() + '.');
    return saveAs();
}

/* Export as web page: the cart in one HTML file that plays it, as an iPod
 * Classic runs it, with the wheel and size asked for. A cart that uses
 * the owner's music has none on the web, so it is refused. */
const EXPORT_KEY = 'arcade-web-export';

async function exportWeb() {
    check();
    if (lastBuild.errors.length) { say('error', 'Fix the errors before exporting: the cart would not build.'); return; }
    const music = musicUsed(lastBuild.cart);
    if (music.length) {
        say('error', musicRefusal(cartName(), music) + '.');
        return;
    }
    let last = {};
    try { last = JSON.parse(localStorage.getItem(EXPORT_KEY)) || {}; } catch (e) { /* none kept */ }
    const world = needsWorld(folder);
    const fields = [
        { name: 'wheel', label: 'Wheel', value: last.wheel || 'touch',
          options: [['touch', 'On touch screens'], ['always', 'Always'], ['never', 'Never: keys only']],
          note: 'The click wheel drawn under the screen. The keys work either way.' },
        { name: 'scale', label: 'Size', value: last.scale || 'fit',
          options: [['fit', 'Fit the window'], ['whole', 'Fit the window, in whole pixels'],
                    ['1', '1×: 160 by 120'], ['2', '2×: 320 by 240'], ['3', '3×: 480 by 360'],
                    ['4', '4×: 640 by 480']] }
    ];
    if (fontBytes)
        fields.push({ name: 'font', label: 'Text font', value: world ? 'yes' : 'no',
                      options: [['yes', 'Kana, Hangul and ideographs too'], ['no', 'Leave them out']],
                      note: world ? 'The cart has them in its text.' : 'About 170 KB more.' });
    else if (world)
        fields.push({ text: 'The cart has kana, Hangul or ideographs, which this IDE was built without: ' +
                            'they will show as ?.' });
    const a = await ask('Export as a web page', fields, 'Export');
    if (!a) return;
    try { localStorage.setItem(EXPORT_KEY, JSON.stringify({ wheel: a.wheel, scale: a.scale })); } catch (e) { /* not kept */ }
    let meta = {};
    try { meta = JSON.parse(folder['cart.json']); } catch (e) { /* the build checked it */ }
    const page = webPage({
        template: player.template, script: player.script, cart: lastBuild.cart,
        wasmGz: gzipSync(wasmBytes, { level: 9 }),
        fontGz: a.font === 'yes' ? gzipSync(fontBytes, { level: 9 }) : null,
        meta: meta, wheel: a.wheel, scale: a.scale
    });
    const file = cartStem() + '.html';
    download(new Blob([page], { type: 'text/html' }), file);
    say('print', 'Downloaded ' + file + ', ' + Math.round(page.length / 1024) + ' KB: open it in a browser to play.');
}

/* ---- the cart's files: made, added from disk, renamed, deleted ----------- */

const NAME = /^[A-Za-z_]\w*$/;

/* where a file of each kind lives, by its name */
const KINDS = {
    code: { label: 'Code', path: (n) => n + '.js' },
    sheet: { label: 'The sprite sheet', path: () => 'sheet.png' },
    logo: { label: 'The logo, 44x44, for lists of games', path: () => 'logo.png' },
    objects: { label: 'The kinds of map object', path: () => 'objects.json' },
    image: { label: 'An image', path: (n) => 'images/' + n + '.png' },
    palette: { label: 'A palette', path: (n) => 'palettes/' + n + '.gpl' },
    map: { label: 'A map', path: (n) => 'maps/' + n + '.json' },
    sound: { label: 'A sound', path: (n) => 'sounds/' + n + '.json' },
    data: { label: 'Data', path: (n) => 'data/' + n + '.json' },
    text: { label: 'Text', path: (n) => 'data/' + n + '.txt' },
    font: { label: 'A font', path: (n) => 'fonts/' + n + '.bdf' },
    caps: { label: 'A font, a copy of CAPS', path: (n) => 'fonts/' + n + '.bdf' },
    editor: { label: 'An editor for data/<name>.json', path: (n) => 'editors/' + n + '.html' }
};

/* a font with no characters but the space, 7 pixels over the baseline and
 * 1 under, to draw from nothing */
const EMPTY_BDF = 'STARTFONT 2.1\nFONT -PodBox-New-Medium-R-Normal--8-80-75-75-P-50-ISO10646-1\n' +
    'SIZE 8 75 75\nFONTBOUNDINGBOX 5 7 0 0\nSTARTPROPERTIES 2\nFONT_ASCENT 7\nFONT_DESCENT 1\n' +
    'ENDPROPERTIES\nCHARS 1\nSTARTCHAR space\nENCODING 32\nSWIDTH 500 0\nDWIDTH 4 0\n' +
    'BBX 1 1 0 0\nBITMAP\n00\nENDCHAR\nENDFONT\n';

/* the cart's palette as a GIMP palette */
function gpl(name, rgb) {
    return 'GIMP Palette\nName: ' + name + '\n#\n' +
        rgb.map((c) => [c >> 16, c >> 8 & 255, c & 255].map((v) => String(v).padStart(3)).join(' ')).join('\n') + '\n';
}

/* the kinds with one file each, by a name of their own */
const SINGLE = ['sheet', 'logo', 'objects'];

/* a start for objects.json: one kind, with one property */
const OBJECTS_JSON = '{\n  "coin": { "help": "A coin to collect",\n' +
    '            "props": { "value": { "default": 10, "help": "Points it is worth" } } }\n}\n';

/* what a new file of each kind holds */
function fresh(kind, name, a) {
    const rgb = palette(folder);
    switch (kind) {
    case 'code': return '// ' + name + '.js: its functions are ' + name + '.name() elsewhere\n';
    case 'sheet': return writePicture(128, 128, new Uint8Array(128 * 128), rgb);
    case 'logo': return writePicture(44, 44, new Uint8Array(44 * 44), rgb);
    case 'objects': return OBJECTS_JSON;
    case 'image': return writePicture(+a.w, +a.h, new Uint8Array(a.w * a.h), rgb);
    case 'palette': return gpl(name, rgb);
    case 'map': return JSON.stringify({ width: +a.mw, height: +a.mh,
        rows: Array.from({ length: +a.mh }, () => '00'.repeat(+a.mw)), objects: [] }, null, 1) + '\n';
    case 'sound': return JSON.stringify({ wave_type: 0, p_env_attack: 0, p_env_sustain: 0.08,
        p_env_decay: 0.12, p_base_freq: 0.45, sound_vol: 0.5 }) + '\n';
    case 'data': return '{\n}\n';
    case 'text': return '';
    case 'font': return EMPTY_BDF;
    case 'caps': return capsBdf;
    case 'editor': return starterEditor(name);
    }
    return '';
}

async function newFile() {
    const kinds = Object.entries(KINDS).filter(([k, v]) =>
        !SINGLE.includes(k) || folder[v.path()] === undefined);
    const a = await ask('New file', [
        { name: 'kind', label: 'What', options: kinds.map(([k, v]) => [k, v.label]) },
        { name: 'name', label: 'Name', value: '', when: (v) => !SINGLE.includes(v.kind),
          note: 'Letters, digits and _: the name the code calls it by',
          check: (n, v) => !NAME.test(n.trim()) ? 'Letters, digits and _, not starting with a digit'
              : folder[KINDS[v.kind].path(n.trim())] !== undefined ? KINDS[v.kind].path(n.trim()) + ' is there already'
              : null },
        { name: 'w', label: 'Width', type: 'number', value: 64, when: (v) => v.kind === 'image',
          check: (n) => n >= 1 && n <= 1024 && Number.isInteger(+n) ? null : '1 to 1024' },
        { name: 'h', label: 'Height', type: 'number', value: 64, when: (v) => v.kind === 'image',
          note: 'Pixels, up to 1024 each way',
          check: (n) => n >= 1 && n <= 1024 && Number.isInteger(+n) ? null : '1 to 1024' },
        { name: 'mw', label: 'Width', type: 'number', value: 32, when: (v) => v.kind === 'map',
          check: (n) => n >= 1 && n <= 256 && Number.isInteger(+n) ? null : '1 to 256' },
        { name: 'mh', label: 'Height', type: 'number', value: 16, when: (v) => v.kind === 'map',
          note: 'Cells, up to 256 each way',
          check: (n) => n >= 1 && n <= 256 && Number.isInteger(+n) ? null : '1 to 256' }
    ], 'Make it');
    if (!a) return;
    const name = SINGLE.includes(a.kind) ? a.kind : a.name.trim();
    const path = KINDS[a.kind].path(name);
    folder[path] = fresh(a.kind, name, a);
    if (a.kind === 'editor') authored.add(path);
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
    case 'html': return 'editors/' + base + '.html';
    case 'png': case 'gif': case 'jpg': case 'jpeg':
        return base === 'sheet' || base === 'logo' ? base + '.png' : 'images/' + base + '.png';
    case 'json': {
        let v = null;
        try { v = JSON.parse(text); } catch (e) { /* data, to show its error */ }
        if (base === 'flags' || base === 'objects') return base + '.json';
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
    i.accept = '.js,.json,.png,.gif,.jpg,.jpeg,.gpl,.bdf,.txt,.html';
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
    if (KEPT.includes(f) || ['sheet.png', 'logo.png', 'flags.json', 'objects.json'].includes(f)) {
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
    gone.add(f);
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
    gone.add(f);
    delete states[f];
    current = 'main.js';
    showFiles();
    show('main.js');
    changed();
}

/* ---- the page ------------------------------------------------------------ */

const ACTIONS = {
    new: async () => {
        if (!(await mayReplace())) return;
        load(NEW_CART());
        saveTo = null;
    },
    open: openCart,
    'open-folder': openFolder,
    save: save,
    export: saveAs,
    'save-new': saveNewGame,
    'export-web': exportWeb,
    replay: () => {
        const t = tape && tape.steps.length ? tape : lastTape;
        if (!t) { say('error', 'Run the cart first: each run is recorded to play again.'); return; }
        run(t);
    },
    'save-replay': saveReplay,
    'open-replay': openReplay,
    undo: () => view ? undo(view) : pixels && pixels.undoEdit && pixels.undoEdit(),
    redo: () => view ? redo(view) : pixels && pixels.redoEdit && pixels.redoEdit(),
    find: () => view && openSearchPanel(view),
    add: newFile,
    'view-files': () => toggle('files'),
    'view-editor': () => toggle('editor'),
    'view-picker': () => toggle('picker'),
    'view-objects': () => toggle('objects'),
    'view-player': () => toggle('player'),
    'view-output': () => toggle('output'),
    'view-profiler': () => toggle('profiler'),
    'view-memory': () => toggle('memory'),
    'view-watch': () => toggle('watch'),
    profile: () => { dock.show('profiler'); profiler.record(true); if (!running) run(); },
    'view-reset': () => dock.reset(),
    import: addFiles,
    'export-picture': exportPicture,
    settings: () => { if (folder['cart.json'] !== undefined) { delete asText['cart.json']; show('cart.json'); } },
    rename: renameFile,
    remove: removeFile,
    run: run,
    stop: stop,
    speech: () => $('speech').classList.toggle('on'),
    keys: () => $('help-keys').showModal(),
    about: () => $('about').showModal(),
    manual: () => window.open(MANUAL + 'README.md', '_blank', 'noopener'),
    tutorial: () => window.open(MANUAL + 'tutorial-1-ball-and-bat.md', '_blank', 'noopener'),
    'manual-fast': () => window.open(MANUAL + 'reference-fast.md', '_blank', 'noopener'),
};

/* the menus: a click opens one; while one is open, hovering moves along
 * the bar; choosing an item or clicking elsewhere closes it */
(function menus() {
    const all = [...document.querySelectorAll('.menu')];
    const close = () => all.forEach((m) => m.classList.remove('open'));
    const open = (m) => {
        if (m.id === 'game-menu') game.fill(m.querySelector('.items'), close);
        m.classList.add('open');
    };
    for (const m of all) {
        const title = m.querySelector('.title');
        title.onclick = (e) => {
            e.stopPropagation();
            const was = m.classList.contains('open');
            close();
            if (!was) open(m);
        };
        title.onmouseenter = () => {
            if (all.some((o) => o.classList.contains('open'))) { close(); open(m); }
        };
    }
    document.addEventListener('click', close);
    document.addEventListener('keydown', (e) => { if (e.key === 'Escape') close(); });
    for (const b of document.querySelectorAll('.menu .items button'))
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
$('power').onclick = () => { if (running) stop(); else run(); };


document.addEventListener('keydown', (e) => {
    if (!(e.ctrlKey || e.metaKey)) return;
    if (e.key === 'Enter') { e.preventDefault(); if (e.shiftKey) stop(); else run(); }
    if (e.key === 's') { e.preventDefault(); save(); }
    if (e.key === 'o') { e.preventDefault(); openCart(); }
    if (e.key.toLowerCase() === 'b') { e.preventDefault(); toggle(e.shiftKey ? 'player' : 'files'); }
    if (e.key === 'j') { e.preventDefault(); toggle('output'); }
});

/* ---- the panels ---------------------------------------------------------- */

let dock = null;

/* before the dock, while every panel is in the page */
for (const b of document.querySelectorAll('[data-icon]')) b.innerHTML = icon(b.dataset.icon);

/* the panels in the dock, the editor's own among them; each group's header
 * shows its open panel's buttons */
dock = createDock($('dock'), {
    files: $('pane-files'), editor: $('editor'), output: $('output'), player: $('pane-player'),
    picker: $('pane-picker'), objects: $('pane-objects'), profiler: $('pane-profiler'),
    memory: $('pane-memory'), watch: $('pane-watch')
}, { files: [$('new-file')], output: [$('clear-output')], player: [$('power')] }, refreshView, $('tray'));
setPanelHost(dock);

/* the pause menu's contents as the Game menu, and what the player keeps */
const game = createGame({
    say: say, ask: ask, dock: dock, watchPane: $('pane-watch'),
    cart: () => { try { return JSON.parse(folder['cart.json']); } catch (e) { return null; } },
    engine: () => (running ? engine : null),
    free: free, choose: choose, hold: hold,
    restart: () => run()
});

/* the profiler, sampling the running engine while it records */
const profiler = new Profiler($('pane-profiler'), {
    goTo: goTo, apiName: apiName, device: () => DEVICES[device],
    recording: (on) => { if (engine && running) engine.profile(on); }
});
/* the memory view, refreshed from the running engine while it shows */
const memoryView = new MemoryView($('pane-memory'));
let memoryAt = 0;

/* the output opens when there is something to read */
dock.hide('output');

/* the View menu: each panel ticked while shown, the editor's own named
 * for what the open editor puts there, and offered only when it does */
function refreshView() {
    if (!dock) return;
    for (const k of ['files', 'editor', 'picker', 'objects', 'player', 'output', 'profiler', 'memory', 'watch']) {
        const b = $('v-' + k);
        b.classList.toggle('on', dock.shown(k));
        b.disabled = !dock.has(k);
        b.hidden = !dock.has(k);
    }
    $('v-picker').firstChild.textContent = dock.title('picker');
}

function toggle(k) {
    if (k === 'output' && dock.shown('output') && running) quietRun = true;
    dock.toggle(k);
}

$('new-file').onclick = () => ACTIONS.add();
$('clear-output').onclick = () => { $('console').textContent = ''; };
$('status').onclick = () => dock.show('output', false);
$('rate').onclick = (e) => {
    e.stopPropagation();
    const run = [...document.querySelectorAll('.menu')].find((m) => m.querySelector('.title').textContent === 'Run');
    run.classList.add('open');
};
refreshView();

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
    /* the folder pbgide serves, when it serves one */
    if (await openServed()) return;
    /* the cart kept from last time, unless the browser's storage is slow
     * to answer or does not */
    const was = await Promise.race([kept(), sleep(1000).then(() => null)]);
    const ok = was && was.folder && was.folder['cart.json'];
    load(ok ? was.folder : NEW_CART(), was && was.current, ok && was.dirty);
})();

/* for the tests, which drive the page */
window.arcadeIde = { load, run, stop, openBytes, folder: () => folder, show, build: () => lastBuild, runUnseen,
                    view: () => view };
