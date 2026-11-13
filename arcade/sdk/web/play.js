/* The runner inside an exported web page (src/webpage.js): the cart played
 * on the engine at its rate, each tick taking what it takes on an iPod
 * Classic, so a game too slow for the player drops frames here too.
 *
 * Parts: unpacking what the page carries; what the cart keeps, in the
 * browser under its id; the run; the pause menu; the input (keys and the
 * drawn wheel); the screen's size. */

import { Engine, fontGlyphs } from '../src/runner.js';
import { cartLogo } from '../src/unpack.js';

const $ = (id) => document.getElementById(id);
const data = JSON.parse($('cart-data').textContent);

/* Button bits, as the engine takes them */
const PREV = 1, NEXT = 2, MENU = 4, PLAY = 8, SELECT = 16;

/* An iPod Classic, in ms, as the IDE measures it: a unit of script, a
 * whole screen's rows changed and sent, and a whole screen's rows drawn
 * but unchanged. CEILING is the units one call may run before the player
 * stops the cart as not responding. */
const UNIT = 0.00016, SCREEN = 2.2, SAME = 0.5;
const CEILING = 2000000;

const sleep = (ms) => new Promise((done) => setTimeout(done, Math.max(0, ms)));
const bytes = (b64) => Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
const b64 = (b) => { let s = ''; for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]); return btoa(s); };

/* ---- unpacking ---------------------------------------------------------- */

async function gunzip(text) {
    const s = new Blob([bytes(text)]).stream().pipeThrough(new DecompressionStream('gzip'));
    return new Uint8Array(await new Response(s).arrayBuffer());
}

/* ---- keeping ------------------------------------------------------------ */

const KEY = 'arcade/' + data.id + '/';
const STORE = KEY + 'store/';

function read(k) {
    try { const v = localStorage.getItem(KEY + k); return v ? bytes(v) : null; } catch (e) { return null; }
}

function write(k, b) {
    try { localStorage.setItem(KEY + k, b64(b)); } catch (e) { /* not kept */ }
}

function storeNames() {
    try { return Object.keys(localStorage).filter((k) => k.startsWith(STORE)); } catch (e) { return []; }
}

const store = {
    read: (name) => {
        try { const v = localStorage.getItem(STORE + name); return v === null ? undefined : bytes(v); } catch (e) { return undefined; }
    },
    /* the player's bounds: 64 names and 256 KB a cart */
    write: (name, b) => {
        const names = storeNames(), fresh = !names.includes(STORE + name);
        let total = b.length;
        for (const k of names) if (k !== STORE + name) total += atob(localStorage.getItem(k)).length;
        if ((fresh && names.length >= 64) || total > 256 * 1024) return false;
        try { localStorage.setItem(STORE + name, b64(b)); return true; } catch (e) { return false; }
    },
    remove: (name) => {
        const had = storeNames().includes(STORE + name);
        try { localStorage.removeItem(STORE + name); } catch (e) { /* nothing kept */ }
        return had;
    },
    list: () => storeNames().map((k) => k.slice(STORE.length)).sort()
};

/* ---- the run ------------------------------------------------------------ */

let wasm, cart, glyph, audio = null, audioAt = 0, soundAt = 0;
let engine = null, generation = 0, running = false;
/* paused: the pause menu open; choosing: one of its items running, whose
 * dialogs go on while the game waits; inDialog: a cart's dialog open */
let paused = false, choosing = 0, inDialog = false, dialogShown = false;
const screen = $('screen').getContext('2d');
const image = screen.createImageData(160, 120);

function present(e) {
    image.data.set(e.rgba());
    screen.putImageData(image, 0, 0);
}

/* The player's clock, in ms of its own time: each tick takes what its
 * units and rows take on the player, and when a tick runs late the
 * player's rule applies -- behind by a whole tick, one update without
 * drawing; further behind, the time is given up. */
let clock, seen = null;
const HELD_BACK = 250;              /* ms the page may run late, caught up */

function rebase(e) {
    seen = null;
    clock.emu = performance.now() - clock.real;
    clock.start = clock.emu - clock.frame * (1000 / e.rate());
}

async function until(ms, e, live) {
    await sleep(clock.real + ms - performance.now());
    /* the page held back -- a tab out of sight runs its timers late --
     * is time the player never had: given up, not caught up */
    const over = performance.now() - (clock.real + ms);
    if (over > HELD_BACK) clock.real += over;
    if (!paused && !choosing) return;
    while ((paused || choosing) && live()) await sleep(50);
    rebase(e);
}

function cost(e, drawn) {
    let ms = e.units() * UNIT;
    if (!drawn) return ms;
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
        ms += (SCREEN * changed + SAME * (hi - lo + 1 - changed)) / 120;
    }
    /* and the four rows the player sends every frame regardless */
    return ms + (SCREEN * 4) / 120;
}

/* the engine's calls on the page, for the run gen: a run that has been
 * replaced (Restart) is answered as if nothing were there, so a dialog it
 * opens backs out and it keeps nothing */
const hostFor = (gen) => ({
    print: (s) => console.log(s),
    date: () => {
        const d = new Date();
        return [d.getFullYear(), d.getMonth() + 1, d.getDate(), d.getHours(), d.getMinutes(), d.getSeconds(), d.getDay()];
    },
    click: (beep) => click(beep),
    glyph: (cp) => (glyph ? glyph(cp) : undefined),
    storeRead: store.read, storeList: store.list,
    storeWrite: (name, b) => gen === generation && store.write(name, b),
    storeRemove: (name) => gen === generation && store.remove(name),
    /* a dialog's frame: shown, then the next tick's input a tick later */
    frame: (open) => {
        if (gen !== generation) return null;
        inDialog = open;
        if (!open) return null;
        dialogShown = true;
        const e = engine;
        return (async () => {
            present(e);
            await sleep(1000 / e.rate());
            while (paused && !choosing && gen === generation) await sleep(50);
            if (gen !== generation) return null;
            const i = input(true);
            return { held: i.held, wheel: i.clicks };
        })();
    }
});

async function run() {
    const gen = ++generation, live = () => generation === gen;
    paused = false;
    choosing = 0;
    inDialog = false;
    closePause();
    cover(null);
    pauseDue = false;
    const e = engine = new Engine(wasm, hostFor(gen));
    e.music({ playingFn: false, beat: false, playing: false, bpm: 0 });
    let seed = 0n;
    for (const v of crypto.getRandomValues(new Uint32Array(2))) seed = (seed << 32n) | BigInt(v);
    if (!e.load(cart, seed)) return fault(e);
    const saves = read('saves'), scores = read('scores'), options = read('options');
    if (saves) e.setSaves(saves);
    if (scores) e.setScores(scores);
    if (options) e.setOptions(options);
    e.ceiling(CEILING);
    running = true;
    clock = { real: performance.now(), emu: 0, start: 0, frame: 0 };
    seen = null;
    if (!(await e.start())) return live() ? fault(e) : undefined;
    if (!live()) return;
    present(e);
    rebase(e);
    let rate = e.rate();
    while (live()) {
        const p = 1000 / rate;
        clock.emu = Math.max(clock.emu, clock.start + clock.frame * p);
        await until(clock.emu, e, live);
        if (!live()) return;
        clock.frame++;
        const late = clock.emu - (clock.start + clock.frame * p);
        dialogShown = false;
        const skip = late >= p ? Math.floor(late / p) : 0;
        if (skip) {
            e.skipped(skip);
            const i = input(false);
            if (!(await e.tick(i.held, i.clicks, i.pos, false))) return live() ? fault(e) : undefined;
            if (!live()) return;
            clock.emu += cost(e, false);
            if (after(e)) return quit();
            pauseNow();
            clock.frame++;
            if (late >= 2 * p) clock.start = clock.emu - clock.frame * p;
        }
        const i = input(false);
        const ok = await e.tick(i.held, i.clicks, i.pos, true, Math.round(1000 / rate));
        if (!live()) return;
        if (!ok) return fault(e);
        if (dialogShown) rebase(e);
        else clock.emu += cost(e, true);
        if (after(e)) return quit();
        pauseNow();
        if (e.rate() !== rate) {
            rate = e.rate();
            clock.start = clock.emu - clock.frame * (1000 / rate);
        }
        await until(clock.emu, e, live);
        if (!live()) return;
        present(e);
        sound(e);
        e.idle();
    }
}

/* after each tick: what changed kept, a board the cart asked for shown,
 * an achievement told; true when the cart asked to quit */
function after(e) {
    const sv = e.saves();
    if (sv.dirty) write('saves', sv.bytes);
    const sc = e.scores();
    if (sc.dirty) write('scores', sc.bytes);
    let a;
    while ((a = e.achieved()) >= 0) {
        const t = e.achievement(a);
        if (t) toast('Achievement: ' + t.title);
    }
    const shown = e.scoreShown();
    if (shown) { openPause(); scoresPanel(shown.board, shown.rank); }
    return e.quitAsked();
}

function stop() {
    generation++;
    running = false;
    engine = null;
}

function quit() {
    stop();
    closePause();
    cover('Play again', '');
}

function fault(e) {
    const msg = e.error();
    stop();
    closePause();
    cover('Play again', 'The game stopped: ' + msg, true);
}

/* the sound since the last: what the time since then holds */
function sound(e) {
    const now = performance.now();
    const frames = Math.min(4096, Math.round(((now - (soundAt || now - 1000 / e.rate())) * 44100) / 1000));
    soundAt = now;
    if (frames <= 0 || !audio) return;
    const pcm = e.audio(frames, 44100);
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

/* click(): the wheel's tick, a sliver of noise; with beep, a short tone */
function click(beep) {
    if (!audio) return;
    const t = audio.currentTime, g = audio.createGain();
    g.connect(audio.destination);
    if (beep) {
        const osc = audio.createOscillator();
        osc.frequency.value = 2000;
        g.gain.setValueAtTime(0.15, t);
        g.gain.exponentialRampToValueAtTime(0.001, t + 0.05);
        osc.connect(g);
        osc.start(t);
        osc.stop(t + 0.05);
    } else {
        const n = Math.floor(audio.sampleRate * 0.003);
        const buf = audio.createBuffer(1, n, audio.sampleRate);
        const d = buf.getChannelData(0);
        for (let i = 0; i < n; i++) d[i] = (Math.random() * 2 - 1) * (1 - i / n);
        const src = audio.createBufferSource();
        src.buffer = buf;
        g.gain.value = 0.3;
        src.connect(g);
        src.start(t);
    }
}

/* the cover over the screen: what starts the game (the browser plays
 * sound only after a click or key), or null to take it away */
let coverAction = null;

function cover(title, text, bad) {
    $('cover').hidden = title === null;
    if (title === null) { coverAction = null; return; }
    $('cover-title').textContent = title;
    $('cover-text').textContent = text;
    $('cover-text').hidden = !text;
    $('cover-text').classList.toggle('bad', !!bad);
    coverAction = () => {
        wake();
        run();
    };
}

/* The sound let start: the browser allows it only from a click, a tap
 * let go or a key -- not every browser counts a finger put down -- so it
 * is asked again at each until it runs */
function wake() {
    if (!audio) audio = new AudioContext({ sampleRate: 44100 });
    if (audio.state !== 'running') audio.resume().catch(() => {});
}

for (const type of ['click', 'pointerup', 'keydown'])
    window.addEventListener(type, () => { if (audio && audio.state !== 'running') wake(); }, true);

$('cover').addEventListener('pointerdown', (e) => { e.preventDefault(); if (coverAction) coverAction(); });

let toastTimer = 0;
function toast(text) {
    const t = $('toast');
    t.textContent = text;
    t.classList.add('on');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => t.classList.remove('on'), 3000);
}

/* ---- the pause menu ----------------------------------------------------- */

const panel = $('panel');

function openPause() {
    if (!running) return;
    paused = true;
    keys = 0;
    wheelHeld = 0;
    mainPanel();
}

function closePause() {
    $('pause').hidden = true;
    paused = false;
}

function heading(text) {
    panel.innerHTML = '';
    const h = document.createElement('h2');
    h.textContent = text;
    panel.append(h);
}

function row(label, fn) {
    const b = document.createElement('button');
    b.textContent = label;
    b.onclick = (ev) => { ev.stopPropagation(); fn(); };
    panel.append(b);
    return b;
}

function show() {
    $('pause').hidden = false;
    const first = panel.querySelector('button, select, input');
    if (first) first.focus();
}

/* The menu's rows. A cart's dialog left open (the page hidden while it
 * showed) holds the engine inside a call, which a menu item or the
 * options must not run beside: they wait for it to close. */
function mainPanel() {
    const e = engine;
    heading(data.name);
    row('Resume', closePause);
    let items = 0;
    for (let i = 1; i <= 5 && !inDialog; i++) {
        const label = e.menuItem(i);
        if (!label) continue;
        if (!items++) panel.append(document.createElement('hr'));
        row(label, () => { if (!inDialog) choose(i); });
    }
    panel.append(document.createElement('hr'));
    if (data.options.length && !inDialog) row('Options', optionsPanel);
    if (e.board(0) || e.achievement(0)) row('High scores', () => scoresPanel(-1, 0));
    if (data.help || data.options.some((o) => o.help)) row('Help', helpPanel);
    row('Restart', run);
    show();
}

/* a menu item's function, run with the game held and the menu closed, its
 * dialogs answered by the input as the game's are */
async function choose(i) {
    const e = engine;
    $('pause').hidden = true;
    choosing++;
    try {
        const r = await e.menuChoose(i);
        if (r < 0 && engine === e) return fault(e);
    } finally { choosing--; }
    if (engine === e) paused = false;
}

function back() {
    const b = row('Back', mainPanel);
    b.style.marginTop = '6px';
}

function optionsPanel() {
    const e = engine;
    heading('Options');
    const fields = data.options.map((op, i) => {
        const label = document.createElement('label');
        label.textContent = op.title || op.name;
        let input;
        if (op.type === 'number') {
            input = document.createElement('input');
            input.type = 'number';
            input.min = op.min;
            input.max = op.max;
            input.step = op.step || 1;
        } else {
            input = document.createElement('select');
            const choices = op.type === 'choice' ? op.choices : ['Off', 'On'];
            choices.forEach((c, k) => {
                const o = document.createElement('option');
                o.value = k;
                o.textContent = c;
                input.append(o);
            });
        }
        input.value = String(e.option(i));
        label.append(input);
        panel.append(label);
        return [input, op];
    });
    row('Set', () => {
        fields.forEach(([input, op], i) => {
            let v = Math.round(+input.value);
            if (op.type === 'number') v = Math.max(op.min, Math.min(op.max, v));
            if (Number.isFinite(v)) e.setOption(i, v);
        });
        write('options', e.options());
        mainPanel();
    });
    back();
    show();
}

const day = (d) => (d > 0 ? String(d).replace(/^(\d{4})(\d\d)(\d\d)$/, '$1-$2-$3') : '');

/* the boards, or the one asked for with its rank marked, and the
 * achievements */
function scoresPanel(board, rank) {
    const e = engine;
    heading('High scores');
    const pre = (text) => { const p = document.createElement('pre'); p.textContent = text; panel.append(p); };
    for (let b = 0; ; b++) {
        const t = e.board(b);
        if (!t) break;
        if (board >= 0 && b !== board) continue;
        const p = document.createElement('p');
        p.textContent = t.title;
        panel.append(p);
        pre(t.rows.length ? t.rows.map(([v, name, date], k) =>
            (b === board && k + 1 === rank ? '*' : ' ') + String(k + 1).padStart(2) + '  ' +
            v.padStart(10) + '  ' + (name || '').padEnd(12) + '  ' + day(+date)).join('\n') : '  No scores yet');
    }
    if (board < 0) {
        const got = [];
        for (let i = 0; ; i++) {
            const t = e.achievement(i);
            if (!t) break;
            got.push((t.date ? '✓ ' : '  ') + t.title + (t.text ? ' -- ' + t.text : ''));
        }
        if (got.length) {
            const p = document.createElement('p');
            p.textContent = 'Achievements';
            panel.append(p);
            pre(got.join('\n'));
        }
    }
    if (board >= 0) row('Resume', closePause); else back();
    show();
}

function helpPanel() {
    heading(data.name + ': help');
    const text = [data.help, ...data.options.filter((o) => o.help).map((o) => (o.title || o.name) + ': ' + o.help)]
        .filter(Boolean).join('\n\n');
    const p = document.createElement('p');
    p.textContent = text;
    panel.append(p);
    back();
    show();
}

/* ---- the input ---------------------------------------------------------- */

let keys = 0, wheelHeld = 0, clicksOwed = 0, wheelPos = -1;
/* a cart that takes MENU as a button gets it as it is let go, unless it
 * was held a second, which opens the pause menu, as on the player */
let menuSince = 0, menuUsed = false;
/* a pause asked for while working out a tick's input, opened once the
 * tick is over: the engine is not to be called into during one */
let pauseDue = false;

function pauseNow() {
    if (!pauseDue) return;
    pauseDue = false;
    openPause();
}

function input(dialog) {
    const down = keys | wheelHeld;
    let held = down & ~MENU;
    if (dialog) held = down;
    else if (engine && engine.menuIsGame()) {
        if (down & MENU) {
            if (!menuSince) { menuSince = performance.now(); menuUsed = false; }
            else if (!menuUsed && performance.now() - menuSince >= 1000) { menuUsed = true; pauseDue = true; }
        } else if (menuSince) {
            if (!menuUsed) held |= MENU;
            menuSince = 0;
        }
    }
    const clicks = Math.trunc(clicksOwed);
    clicksOwed -= clicks;
    return { held, clicks, pos: wheelPos };
}

/* a button pressed: MENU opens the pause menu unless the cart or its
 * dialog takes it */
function press(b) {
    if (b === MENU && running && !paused && !inDialog && engine && !engine.menuIsGame()) {
        openPause();
        return false;
    }
    return true;
}

/* the key held since it started the game, until it is let go */
let starter = null;

const KEYS = { ArrowLeft: PREV, ArrowRight: NEXT, Escape: MENU, ' ': PLAY, Enter: SELECT };

window.addEventListener('keydown', (e) => {
    /* the key that started the game is not also a press in it */
    if (e.key === starter) { e.preventDefault(); return; }
    if (coverAction && (e.key === 'Enter' || e.key === ' ')) {
        e.preventDefault();
        starter = e.key;
        coverAction();
        return;
    }
    if (paused && !choosing) {
        if (e.key === 'Escape') { e.preventDefault(); if (!e.repeat) closePause(); }
        else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
            const all = [...panel.querySelectorAll('button, select, input')];
            const at = all.indexOf(document.activeElement);
            const to = all[(at + (e.key === 'ArrowDown' ? 1 : all.length - 1)) % all.length];
            if (to && !(document.activeElement && document.activeElement.tagName === 'SELECT')) { e.preventDefault(); to.focus(); }
        }
        return;
    }
    if (!running) return;
    if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
        clicksOwed += e.key === 'ArrowDown' ? 4 : -4;
        e.preventDefault();
        return;
    }
    const b = KEYS[e.key];
    if (!b) return;
    e.preventDefault();
    if (!e.repeat && !press(b)) return;
    keys |= b;
});
window.addEventListener('keyup', (e) => {
    if (e.key === starter) starter = null;
    const b = KEYS[e.key];
    if (b) keys &= ~b;
});
window.addEventListener('blur', () => { keys = 0; starter = null; });
document.addEventListener('visibilitychange', () => { if (document.hidden && running && !paused) openPause(); });

/* The wheel: dragging round the ring turns it, 96 clicks a turn, and its
 * place is where the finger is. A tap on the ring that does not turn it
 * is MENU, PREV, NEXT or PLAY by where it falls, and held half a second
 * without turning, that button is held. The middle is SELECT. */
(function wheel() {
    const svg = $('wheel');
    let lastTurn = null, moved = 0, button = 0, holding = 0;
    const at = (e) => {
        const r = svg.getBoundingClientRect();
        const x = e.clientX - r.left - r.width / 2, y = e.clientY - r.top - r.height / 2;
        return { d: Math.hypot(x, y) / (r.width / 2), turn: (Math.atan2(x, -y) / (2 * Math.PI) + 1) % 1 };
    };
    const tap = (b) => {
        if (!press(b)) return;
        wheelHeld = b;
        setTimeout(() => { if (wheelHeld === b) wheelHeld = 0; }, 120);
    };
    svg.addEventListener('pointerdown', (e) => {
        if (!running || (paused && !choosing)) return;
        const p = at(e);
        try { svg.setPointerCapture(e.pointerId); } catch (x) { /* a pointer already gone */ }
        e.preventDefault();
        if (p.d < 0.38) { wheelHeld = SELECT; return; }
        lastTurn = p.turn;
        moved = 0;
        wheelPos = Math.floor(p.turn * 96) % 96;
        button = [MENU, NEXT, PLAY, PREV][Math.round(p.turn * 4) % 4];
        holding = setTimeout(() => { holding = 0; if (press(button)) wheelHeld = button; }, 500);
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

/* ---- the screen's size -------------------------------------------------- */

const touch = matchMedia('(pointer: coarse)');

function layout() {
    /* an SVG element has no hidden property: the attribute itself */
    const wheel = $('wheel');
    const shown = data.wheel === 'always' || (data.wheel === 'touch' && touch.matches);
    wheel.toggleAttribute('hidden', !shown);
    const w = innerWidth - 32;
    let h = innerHeight - 32 - 16 - $('source').offsetHeight;
    if (shown) {
        const size = Math.round(Math.max(140, Math.min(240, w, innerHeight * 0.36)));
        wheel.style.width = wheel.style.height = size + 'px';
        h -= size + 16;
    }
    const fit = Math.max(0.5, Math.min(w / 160, h / 120));
    /* a fixed size is the most it is drawn at: smaller where the window is */
    const s = data.scale === 'fit' ? fit : data.scale === 'whole' ? Math.max(1, Math.floor(fit))
        : Math.min(+data.scale, fit);
    $('stage').style.width = $('screen').style.width = 160 * s + 'px';
    $('stage').style.height = $('screen').style.height = 120 * s + 'px';
}

addEventListener('resize', layout);
touch.addEventListener('change', layout);

/* ---- starting ----------------------------------------------------------- */

/* the cart's logo, if it has one, on the cover and as the page's icon */
function showLogo() {
    const logo = cartLogo(cart);
    if (!logo) return;
    const c = document.createElement('canvas');
    c.width = logo.width;
    c.height = logo.height;
    const g = c.getContext('2d'), im = g.createImageData(logo.width, logo.height);
    logo.rgb.forEach((v, i) => im.data.set([v >> 16, v >> 8 & 255, v & 255, 255], i * 4));
    g.putImageData(im, 0, 0);
    const url = c.toDataURL('image/png');
    $('cover-logo').src = url;
    $('cover-logo').hidden = false;
    const icon = document.createElement('link');
    icon.rel = 'icon';
    icon.href = url;
    document.head.append(icon);
}

(async function start() {
    layout();
    $('cover-title').textContent = data.name;
    try {
        wasm = await WebAssembly.compile(await gunzip(data.wasm));
        cart = bytes(data.cart);
        try { showLogo(); } catch (e) { /* the game plays without it */ }
        if (data.font) glyph = fontGlyphs(await gunzip(data.font));
    } catch (e) {
        cover(data.name, 'This browser cannot run the game: ' + (e && e.message || e), true);
        coverAction = null;
        return;
    }
    cover(data.name, (touch.matches ? 'Tap' : 'Click or press Enter') + ' to play');
})();
