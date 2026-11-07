/* The Arcade IDE: a cart's files in an editor beside the running game.
 *
 * Parts: the cart (its files, kept in the browser between visits); the
 * editor, with errors from the compiler and the API's names to complete;
 * running it on the engine, a tick at the cart's rate, a dialog's frames
 * included; the input (keys and the drawn click wheel); the meter and the
 * console; opening and saving. */

import { EditorView, basicSetup } from 'codemirror';
import { EditorState } from '@codemirror/state';
import { javascript } from '@codemirror/lang-javascript';
import { json } from '@codemirror/lang-json';
import { oneDark } from '@codemirror/theme-one-dark';
import { setDiagnostics, lintGutter } from '@codemirror/lint';
import { autocompletion } from '@codemirror/autocomplete';
import { Engine, budget, fontGlyphs } from '../src/runner.js';
import { isBinary } from '../src/folder.js';
import { build, open, names, NEW_CART } from './cart.js';
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
        return { from: word.from + dot + 1, options: list.map((l) => ({ label: l, type: 'function' })) };
    }
    const files = Object.keys(folder).filter((f) => /^[A-Za-z_]\w*\.js$/.test(f) && f !== 'main.js')
        .map((f) => f.slice(0, -3));
    return { from: word.from, options: [...n.top, ...files].map((l) => ({ label: l })) };
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
        extensions: [basicSetup, ...lang, ...dark, lintGutter(), autocompletion({ override: [completions] }),
                     EditorView.updateListener.of((u) => {
                         if (!u.docChanged) return;
                         folder[current] = u.state.doc.toString();
                         changed();
                     })]
    });
    return states[file];
}

function show(file) {
    if (view) states[current] = view.state;
    current = file;
    const pane = $('editor');
    if (isBinary(file)) {
        if (view) { view.destroy(); view = null; }
        pane.innerHTML = '';
        const img = document.createElement('img');
        img.src = URL.createObjectURL(new Blob([folder[file]], { type: 'image/png' }));
        img.className = 'picture';
        const note = document.createElement('p');
        note.textContent = file + ': the picture editor comes in a later step.';
        pane.append(img, note);
    } else if (!view) {
        pane.innerHTML = '';
        view = new EditorView({ state: stateFor(file), parent: pane });
    } else view.setState(stateFor(file));
    for (const li of $('files').querySelectorAll('li'))
        li.classList.toggle('open', li.dataset.file === file);
    mark();
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
        ? errs.length + (errs.length === 1 ? ' error' : ' errors')
        : 'built: ' + lastBuild.stats.size + ' bytes';
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

let engine = null, running = false, paused = false, generation = 0;
let audio = null, audioAt = 0;
const screen = $('screen').getContext('2d');
const image = screen.createImageData(160, 120);

function present() {
    if (!engine) return;
    image.data.set(engine.rgba());
    screen.putImageData(image, 0, 0);
}

const sleep = (ms) => new Promise((done) => setTimeout(done, Math.max(0, ms)));

let nextAt = 0;
/* until e's next tick is due, keeping time even when a tick runs late */
async function nextTick(e, live) {
    const step = 1000 / e.rate();
    nextAt += step;
    const now = performance.now();
    if (nextAt < now - 5 * step) {
        e.skipped(Math.floor((now - nextAt) / step));
        nextAt = now;
    }
    await sleep(nextAt - performance.now());
    while (paused && live()) await sleep(50);
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
    /* a run ends when another starts: each keeps its own engine */
    const gen = ++generation, live = () => running && generation === gen;
    const e = engine = new Engine(wasm, hostFor(() => e, live));
    if (!e.load(lastBuild.cart, BigInt(Date.now()))) return fault();
    if (!audio) audio = new AudioContext({ sampleRate: 44100 });
    audio.resume();
    audioAt = 0;
    running = true;
    paused = false;
    $('screen').focus();
    document.body.classList.add('running');
    meterReset();
    nextAt = performance.now();
    if (!(await e.start())) return live() ? fault() : undefined;
    present();
    while (live()) {
        await nextTick(e, live);
        if (!live()) break;
        const i = input();
        const ok = await e.tick(i.held, i.clicks, i.pos);
        if (!live()) break;
        if (!ok) return fault();
        present();
        meter(e.units());
        sound();
        e.idle();
    }
}

function stop() {
    running = false;
    paused = false;
    document.body.classList.remove('running', 'paused');
}

function fault() {
    const msg = engine.error();
    stop();
    const m = /^([^:]+):(\d+): (.*)$/.exec(msg);
    say('error', msg, m ? { file: m[1], line: +m[2] } : null);
    if (m) {
        runError = { file: m[1], line: +m[2], column: 0, message: m[3] };
        goTo(m[1], +m[2]);
    }
}

function sound() {
    const frames = Math.round(44100 / engine.rate());
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
    click: () => {},
    say: (text, queue) => say('say', (queue ? 'then says: ' : 'says: ') + text),
    speechOn: () => $('speech').checked,
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
    /* a dialog's frame: shown, then the next tick's input; null backs out */
    frame: (open) => {
        if (!open) return null;
        return Promise.resolve().then(async () => {
            if (live()) present();
            await nextTick(e(), live);
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

/* ---- the meter and the console ------------------------------------------- */

let ticks = [];

function meterReset() { ticks = []; }

function meter(units) {
    ticks.push(units);
    const rate = engine.rate();
    if (ticks.length > rate) ticks.shift();
    const most = budget(rate);
    const mean = ticks.reduce((a, b) => a + b, 0) / ticks.length;
    const worst = Math.max(...ticks);
    $('meter-bar').style.width = Math.min(100, (100 * mean) / most) + '%';
    $('meter-worst').style.left = Math.min(100, (100 * worst) / most) + '%';
    $('meter').classList.toggle('over', worst > most);
    $('meter-text').textContent = Math.round(mean).toLocaleString() + ' units a tick, ' +
        Math.round((100 * mean) / most) + '% of the 5G with music; worst ' +
        Math.round((100 * worst) / most) + '%';
}

function say(kind, text, where) {
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

async function openCart() {
    let bytes, handle = null;
    if (canPick) {
        try {
            [handle] = await window.showOpenFilePicker({ types: [{ description: 'Arcade cart',
                accept: { 'application/octet-stream': ['.pbg'] } }] });
        } catch (e) { return; }
        bytes = new Uint8Array(await (await handle.getFile()).arrayBuffer());
    } else {
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

/* a cart folder: the files a cart is made of, each in its place */
async function openFolder() {
    let dir;
    try { dir = await window.showDirectoryPicker({ mode: 'readwrite' }); } catch (e) { return; }
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
    await walk(dir, '', 0);
    if (!files['cart.json']) { say('error', 'That folder has no cart.json: it is not a cart.'); return; }
    load(files);
    saveTo = { dir };
}

async function save() {
    check();
    if (lastBuild.errors.length) { say('error', 'Fix the errors before saving: the cart would not build.'); return; }
    if (saveTo && saveTo.dir) {
        for (const [name, v] of Object.entries(folder)) {
            let d = saveTo.dir;
            const parts = name.split('/');
            for (const p of parts.slice(0, -1)) d = await d.getDirectoryHandle(p, { create: true });
            const w = await (await d.getFileHandle(parts[parts.length - 1], { create: true })).createWritable();
            await w.write(v);
            await w.close();
        }
        say('print', 'Saved the folder.');
        return;
    }
    await exportCart();
}

async function exportCart() {
    check();
    if (lastBuild.errors.length) return;
    const file = cartName().replace(/[^\w -]/g, '').trim().replace(/ +/g, '_').toLowerCase() + '.pbg';
    if (saveTo && saveTo.file) {
        const w = await saveTo.file.createWritable();
        await w.write(lastBuild.cart);
        await w.close();
        say('print', 'Saved ' + saveTo.file.name + '.');
        return;
    }
    if (window.showSaveFilePicker) {
        try {
            const h = await window.showSaveFilePicker({ suggestedName: file,
                types: [{ description: 'Arcade cart', accept: { 'application/octet-stream': ['.pbg'] } }] });
            saveTo = { file: h };
            return exportCart();
        } catch (e) { return; }
    }
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([lastBuild.cart]));
    a.download = file;
    a.click();
}

function newFile() {
    const name = window.prompt('A new file: its name, such as enemy.js');
    if (!name) return;
    if (folder[name] !== undefined) { show(name); return; }
    if (!/^([A-Za-z_]\w*\.js|(palettes\/\w+\.gpl|maps\/\w+\.json|sounds\/\w+\.json|data\/\w+\.(json|txt)|fonts\/\w+\.bdf))$/.test(name)) {
        say('error', name + ': a code file is a name and .js; data goes in data/, as data/levels.json');
        return;
    }
    folder[name] = '';
    showFiles();
    show(name);
    changed();
}

/* ---- the page ------------------------------------------------------------ */

$('new').onclick = () => { if (confirmLeave()) load(NEW_CART()); saveTo = null; };
$('open').onclick = openCart;
$('open-folder').onclick = openFolder;
$('open-folder').hidden = !('showDirectoryPicker' in window);
$('save').onclick = save;
$('export').onclick = exportCart;
$('add').onclick = newFile;
$('run').onclick = run;
$('stop').onclick = stop;

function confirmLeave() {
    return window.confirm('Start a new cart? The one open now is kept only if you saved it.');
}

document.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') { e.preventDefault(); run(); }
    if ((e.ctrlKey || e.metaKey) && e.key === 's') { e.preventDefault(); save(); }
});

document.addEventListener('dragover', (e) => e.preventDefault());
document.addEventListener('drop', async (e) => {
    e.preventDefault();
    const f = e.dataTransfer.files[0];
    if (f && /\.pbg$/i.test(f.name)) openBytes(new Uint8Array(await f.arrayBuffer()), null);
});

(async function start() {
    /* the cart kept from last time, unless the browser's storage is slow
     * to answer or does not */
    const was = await Promise.race([kept(), sleep(1000).then(() => null)]);
    load(was && was.folder && was.folder['cart.json'] ? was.folder : NEW_CART(), was && was.current);
})();

/* for the tests, which drive the page */
window.arcadeIde = { load, run, stop, folder: () => folder, show, build: () => lastBuild };
