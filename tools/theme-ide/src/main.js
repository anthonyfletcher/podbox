/* The Theme IDE: a theme's files in an editor beside PodBox's own firmware
 * running them.
 *
 * Parts: the dock and its panels; the editor; the theme -- opening, saving,
 * changes on disk; the player -- starting it, keeping it up to date, its
 * outlines, wheel and Go to; the menus and keys; the status bar; ctl.py's
 * commands. */

import { EditorView, basicSetup } from 'codemirror';
import { EditorState, Compartment, Prec } from '@codemirror/state';
import { keymap } from '@codemirror/view';
import { indentWithTab, undo, redo, toggleComment } from '@codemirror/commands';
import { openSearchPanel, gotoLine } from '@codemirror/search';
import { oneDark } from '@codemirror/theme-one-dark';
import { createDock } from './dock.js';
import { Theme, ServerSource, HandleSource, FileListSource } from './theme.js';
import { Player } from './player.js';
import { LIBRARY, albumDir } from './demo.js';
import { skinLanguage, model as modelOf } from './skinlang.js';
import { fileInfo, isSkin, viewportInfo, adoptFirmwareTags } from './names.js';
import { iconButton } from './icons.js';
import * as P from './panels.js';

/* global build */

adoptFirmwareTags();

const $ = (id) => document.getElementById(id);
const wait = (ms) => new Promise((ok) => setTimeout(ok, ms));
const dark = window.matchMedia('(prefers-color-scheme: dark)').matches;

const ide = {
    theme: null,
    current: null,       /* the open file's path */
    selected: -1,        /* index into the open skin's viewports */
    fw: null,            /* the firmware's viewports for the open skin */
    live: true,          /* the player follows the editor as it is typed in */
    outlines: true,
    player: Player
};

/* ---- the dock ---- */

const panes = {};
for (const id of ['files', 'editor', 'player', 'outline', 'inspector', 'state', 'problems', 'reference', 'output'])
    panes[id] = $('pane-' + id);
/* the dock reports changes while it is first laid out, before it is here */
let dock = null;
dock = createDock($('dock'), panes, () => { if (dock) paintMenus(); }, $('tray'));
const setLive = (v) => { ide.live = v; try { localStorage.setItem('theme-ide-live', v ? '1' : '0'); } catch (e) { /* */ } };
try { ide.live = localStorage.getItem('theme-ide-live') !== '0'; ide.outlines = localStorage.getItem('theme-ide-outlines') !== '0'; }
catch (e) { /* the defaults */ }

/* ---- the editor ---- */

const language = new Compartment();
const states = new Map();          /* path -> EditorState, so each file keeps its undo and place */
const comments = EditorState.languageData.of(() => [{ commentTokens: { line: '#' } }]);
/* ahead of the editor's own: Mod-Enter would insert a line, Mod-G find the
   next match */
const ownKeys = Prec.highest(keymap.of([
    { key: 'Mod-Enter', run: () => { run(); return true; } },
    { key: 'Mod-g', run: gotoLine }
]));

function newState(path, text) {
    return EditorState.create({
        doc: text,
        extensions: [
            ownKeys, basicSetup, keymap.of([indentWithTab]), comments,
            language.of(isSkin(path) ? skinLanguage() : []),
            dark ? oneDark : [],
            EditorView.updateListener.of((u) => {
                if (u.docChanged) changed();
                if (u.selectionSet || u.docChanged) caret();
            })
        ]
    });
}

const view = new EditorView({ state: EditorState.create({ doc: '' }), parent: $('editor-host') });
view.dom.hidden = true;
const blank = document.createElement('div');
blank.className = 'empty';
blank.textContent = 'Open a file from Files.';
$('editor-host').append(blank);

ide.model = () => modelOf(view.state.doc);
ide.view = view;

ide.open = (path, offset) => {
    const f = ide.theme && ide.theme.get(path);
    if (!f) return;
    if (f.text === null) { flash(fileInfo(path).name + ' is not a text file.'); return; }
    if (ide.current) states.set(ide.current, view.state);
    ide.current = path;
    ide.selected = -1;
    view.setState(states.get(path) || newState(path, f.text));
    view.dom.hidden = false;
    blank.hidden = true;
    dock.setTitle('editor', path.split('/').pop());
    if (isSkin(path)) useCfg(ide.theme.cfgFor(path));
    else if (/\.cfg$/i.test(path) && !/(^|\/)config\.cfg$/i.test(path)) useCfg(path);
    if (offset !== undefined) {
        view.dispatch({ selection: { anchor: offset }, scrollIntoView: true });
        view.focus();
    }
    refresh();
    showScreenOf(path);
};

/* an edit made by a panel: in the open file, through the editor, so it can
   be undone */
ide.edit = (from, to, text) => {
    view.dispatch({ changes: { from, to, insert: text } });
};

let refreshTimer = 0, runTimer = 0;
function changed() {
    ide.theme.setText(ide.current, view.state.doc.toString());
    clearTimeout(refreshTimer);
    refreshTimer = setTimeout(refresh, 150);
    if (ide.live) { clearTimeout(runTimer); runTimer = setTimeout(run, 600); }
    paintLive();
}

/* the viewport the caret is in follows into the Outline and the Inspector */
function caret() {
    const pos = view.state.selection.main.head;
    const line = view.state.doc.lineAt(pos);
    $('cursor').textContent = 'Line ' + line.number + ', column ' + (pos - line.from + 1);
    if (!ide.current || !isSkin(ide.current)) return;
    const vps = ide.model().vps;
    let i = -1;
    for (let k = 0; k < vps.length; k++) if (vps[k].off <= pos) i = k;
    if (i !== ide.selected) { ide.selected = i; P.renderOutline(ide); P.renderInspector(ide); drawOverlay(); }
}

ide.select = (i, jump) => {
    ide.selected = i;
    const v = ide.model().vps[i];
    if (jump && v) {
        view.dispatch({ selection: { anchor: v.off }, scrollIntoView: true });
        view.focus();
    }
    P.renderOutline(ide);
    P.renderInspector(ide);
    drawOverlay();
};

/* ---- the theme ---- */

/* every skin's problems, for the Problems panel and the Files badges */
const problemCache = new Map();
ide.allProblems = () => {
    const out = [];
    if (!ide.theme) return out;
    for (const p of ide.theme.find(/\.(wps|sbs|fms)$/i)) {
        const text = ide.theme.get(p).text || '';
        let c = problemCache.get(p);
        if (!c || c.text !== text) {
            const m = build(text);
            c = { text, issues: m.issues.map((i) => ({ path: p, off: i.off || 0, line: P.lineOf(text, i.off || 0),
                                                      sev: i.sev, msg: i.msg })) };
            problemCache.set(p, c);
        }
        out.push(...c.issues);
    }
    return out.sort((a, b) => (a.sev === 'err' ? 0 : 1) - (b.sev === 'err' ? 0 : 1) || a.path.localeCompare(b.path) || a.off - b.off);
};
ide.problemCounts = () => {
    const n = new Map();
    for (const p of ide.allProblems()) n.set(p.path, (n.get(p.path) || 0) + 1);
    return n;
};

function refresh() {
    P.renderFiles(ide);
    P.renderOutline(ide);
    P.renderInspector(ide);
    const n = P.renderProblems(ide);
    dock.setTitle('problems', n ? 'Problems (' + n + ')' : 'Problems');
    paintStatus();
    drawOverlay();
}

async function adopt(theme) {
    ide.theme = theme;
    ide.current = null;
    states.clear();
    problemCache.clear();
    $('theme-name').textContent = theme.name;
    document.title = theme.name + ' — Theme IDE';
    ide.cfg = null;
    /* Now Playing first, and the theme's own before a variant's: scrim.wps
       before scrim_p.wps */
    const skins = theme.find(/\.(wps|sbs)$/i).sort((a, b) =>
        (/\.wps$/i.test(a) ? 0 : 1) - (/\.wps$/i.test(b) ? 0 : 1) || a.length - b.length || a.localeCompare(b));
    if (skins.length) ide.open(skins[0]);
    else ide.cfg = theme.cfgFor(null);
    /* a player still starting takes the theme as it starts */
    if (Player.running()) await Player.update(playerFiles(), cfgPath());
    else startPlayer();
    refresh();
    P.renderState(ide);
    paintLive();
}

/* the theme's files as the player's disk wants them */
function playerFiles() {
    if (!ide.theme) return [];
    const out = [];
    for (const f of ide.theme.files.values()) {
        const rb = Player.rockboxPath(f.path);
        if (rb) out.push({ path: rb, data: f.data });
    }
    return out;
}
/* the theme .cfg the player is showing: the one that names the open skin, or
   the .cfg last opened */
function cfgPath() {
    return ide.cfg ? Player.rockboxPath(ide.cfg) : null;
}
function useCfg(path) {
    if (!path || path === ide.cfg) return;
    ide.cfg = path;
    Player.apply(cfgPath());
}

ide.openFolder = async () => {
    if (window.showDirectoryPicker) {
        try {
            const dir = await window.showDirectoryPicker({ mode: 'readwrite' });
            const t = new Theme(new HandleSource(dir));
            await t.load();
            await adopt(t);
        } catch (e) { if (e.name !== 'AbortError') flash('Could not open the folder: ' + e.message, true); }
    } else $('folder-input').click();
};
$('folder-input').onchange = async (e) => {
    const files = [...e.target.files];
    if (!files.length) return;
    const t = new Theme(new FileListSource(files));
    await t.load();
    await adopt(t);
    flash('This browser can only read the folder: save will not work. Chrome or Edge can write it.', true);
};

/* true when saved; a failure is shown, and thrown again for ctl.py */
async function save(path, rethrow) {
    try {
        await ide.theme.save(path);
        flash('Saved ' + path.split('/').pop());
        if (!ide.live) await run();
    } catch (e) {
        flash(e.message, true);
        if (rethrow) throw e;
    } finally {
        refresh();
        paintLive();
    }
}
const saveAll = async () => { for (const f of ide.theme ? ide.theme.dirty() : []) await save(f.path); };

/* A file changed on disk, in another editor or by ctl.py. One change is
   taken at a time, in the order they came. */
let diskQueue = Promise.resolve();
function onDisk(changedPaths, removed, force) {
    diskQueue = diskQueue.then(() => takeDisk(changedPaths, removed, force)).catch((e) => flash(e.message, true));
    return diskQueue;
}
async function takeDisk(changedPaths, removed, force) {
    if (!ide.theme) return;
    let any = false;
    if (removed.length || changedPaths.some((p) => !ide.theme.get(p))) {
        any = await ide.theme.sync();
        for (const p of removed) states.delete(p);
        if (ide.current && !ide.theme.get(ide.current)) {
            ide.current = null;
            view.dom.hidden = true;
            blank.hidden = false;
        }
    }
    for (const p of changedPaths) {
        if (!ide.theme.get(p)) continue;
        const r = await ide.theme.reread(p, force);
        if (r === 'same') continue;
        if (r === 'kept') { flash(p.split('/').pop() + ' changed on disk while you were editing it: File › Take the copy on disk takes it.', true); continue; }
        any = true;
        states.delete(p);
        if (p === ide.current) {
            const sel = view.state.selection.main.head, text = ide.theme.get(p).text;
            view.setState(newState(p, text));
            view.dispatch({ selection: { anchor: Math.min(sel, text.length) } });
        }
    }
    if (any) { refresh(); paintLive(); await reload(playerFiles()); }
}

/* ---- the player ---- */

const screen = $('screen'), overlay = $('overlay'), stack = $('stack');

/* ?noplayer leaves it off, and the stream of changes from serve.py: a
   headless browser photographs a page only once it falls quiet */
const noPlayer = new URLSearchParams(location.search).has('noplayer');

function startPlayer() {
    if (noPlayer) return;
    Player.onState = () => paintStatus();
    Player.onOutput = (line) => P.appendOutput(line, /error|panic|fail/i.test(line) ? 'error' : '');
    Player.boot(screen, () => ({ key: ide.theme, files: playerFiles(), cfg: cfgPath() }));
}

/* Files to the player's disk and the theme loaded again. Now Playing closes
   while a theme loads, as it would on an iPod; the player is taken back to
   it, the track playing on where it was. */
async function reload(files) {
    const before = Player.viewports('sbs');
    await Player.update(files, cfgPath());
    if (!before || before.activity !== 2) return;
    for (let i = 0; i < 10; i++) {
        await wait(300);
        const now = Player.viewports('sbs');
        if (!now || now.activity === 2) return;
        if (now.activity === 1 && (now.audio & 1)) { await Player.press('play'); return; }
    }
}

/* the editor's text, on the player, saved or not */
async function run() {
    if (!ide.theme || !ide.current) return;
    const rb = Player.rockboxPath(ide.current);
    if (!rb) return;
    await reload([{ path: rb, data: ide.theme.get(ide.current).data }]);
}

/* which of the open skin's screens the player is on */
const skinOf = (path) => (path && /\.wps$/i.test(path) ? 'wps' : path && /\.sbs$/i.test(path) ? 'sbs' : null);
ide.onScreen = () => {
    const s = skinOf(ide.current);
    if (!s || !ide.fw) return false;
    return s === 'wps' ? ide.fw.activity === 2 : ide.fw.activity !== 2;
};

let booted = false;
setInterval(() => {
    if (!Player.running()) return;
    const s = skinOf(ide.current);
    const fw = s ? Player.viewports(s) : null;
    /* the open skin's screen, once the player has reached its main menu */
    if (!booted && fw && fw.activity === 1) { booted = true; showScreenOf(ide.current); }
    const was = JSON.stringify(ide.fw);
    ide.fw = fw;
    if (JSON.stringify(fw) !== was) { P.renderOutline(ide); drawOverlay(); }
    paintStatus();
}, 400);

/* ---- go to a screen, by the buttons an owner would press ---- */
const SCREENS = ['menu', 'music', 'albums', 'wps', 'quick'];
ide.goto = async (where) => {
    if (!SCREENS.includes(where)) throw new Error('the screens are ' + SCREENS.join(', '));
    if (!Player.running()) return;
    if (where === 'menu') await Player.sequence(['menu']);
    else if (where === 'music') await Player.sequence(['menu', 'select']);
    else if (where === 'albums') await Player.sequence(['menu', 'select', 'select']);
    else if (where === 'quick') await Player.sequence([['menu', 1300]]);
    else if (where === 'wps') {
        if (((Player.viewports('sbs') || {}).audio & 1) === 0) { await Player.play(albumDir(LIBRARY[6])); await wait(900); }
        if ((Player.viewports('sbs') || {}).activity !== 2) await Player.sequence(['play']);
    }
};

/* opening a screen's skin shows that screen */
async function showScreenOf(path) {
    const s = skinOf(path);
    const fw = Player.viewports('sbs');
    if (!s || !fw) return;
    if (s === 'wps' && fw.activity !== 2) await ide.goto('wps');
    if (s === 'sbs' && fw.activity === 2) await ide.goto('menu');
}

/* zoom: the screen as large as the panel has room for, in half steps */
function fitScreen() {
    const body = $('player-body');
    const z = Math.max(1, Math.min(4, Math.floor(Math.min((body.clientWidth - 24) / 320,
                                                          (body.clientHeight - 210) / 240) * 2) / 2));
    for (const c of [screen, overlay]) { c.style.width = 320 * z + 'px'; c.style.height = 240 * z + 'px'; }
    overlay.width = 320 * z * devicePixelRatio;
    overlay.height = 240 * z * devicePixelRatio;
    ide.zoom = z;
    drawOverlay();
}
new ResizeObserver(fitScreen).observe($('player-body'));

/* the outlines: the open skin's viewports where the firmware has them, the
   selected one filled; drag it to move it */
let drag = null;
function rects() {
    const out = [];
    if (!ide.outlines || !ide.onScreen() || !ide.current) return out;
    ide.model().vps.forEach((v, i) => {
        const fw = P.firmwareVp(ide, v);
        if (!fw || fw.never) return;
        if (fw.hidden && i !== ide.selected) return;
        out.push({ i, x: fw.x, y: fw.y, w: fw.w, h: fw.h, hidden: fw.hidden, v });
    });
    return out;
}
function drawOverlay() {
    const g = overlay.getContext('2d');
    const k = (ide.zoom || 1) * devicePixelRatio;
    g.setTransform(1, 0, 0, 1, 0, 0);
    g.clearRect(0, 0, overlay.width, overlay.height);
    stack.classList.toggle('picking', ide.outlines && ide.onScreen());
    g.setTransform(k, 0, 0, k, 0, 0);
    g.lineWidth = 1 / k;
    for (const r of rects()) {
        const sel = r.i === ide.selected;
        let { x, y } = r;
        if (sel && drag && drag.moved) { x += drag.dx; y += drag.dy; }
        g.setLineDash(r.hidden ? [3 / k * 2, 3 / k * 2] : []);
        g.strokeStyle = sel ? '#ff5bd2' : 'rgba(80, 200, 255, 0.85)';
        if (sel) { g.fillStyle = 'rgba(255, 91, 210, 0.18)'; g.fillRect(x, y, r.w, r.h); g.lineWidth = 2 / k; }
        g.strokeRect(x + 0.5 / k, y + 0.5 / k, r.w - 1 / k, r.h - 1 / k);
        g.lineWidth = 1 / k;
    }
}
const lcdPoint = (e) => {
    const b = overlay.getBoundingClientRect();
    return [(e.clientX - b.left) * 320 / b.width, (e.clientY - b.top) * 240 / b.height];
};
const hit = (x, y) => rects().filter((r) => !r.hidden && x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h)
                             .sort((a, b) => a.w * a.h - b.w * b.h)[0];
overlay.addEventListener('pointerdown', (e) => {
    const [x, y] = lcdPoint(e);
    const r = hit(x, y);
    if (!r) return;
    if (r.i !== ide.selected) { ide.select(r.i, true); return; }
    drag = { x0: x, y0: y, dx: 0, dy: 0, moved: false, r };
    overlay.setPointerCapture(e.pointerId);
});
overlay.addEventListener('pointermove', (e) => {
    const [x, y] = lcdPoint(e);
    if (!drag) { const r = hit(x, y); overlay.title = r ? viewportInfo(r.v).name : ''; return; }
    drag.dx = Math.round(x - drag.x0);
    drag.dy = Math.round(y - drag.y0);
    drag.moved = drag.moved || Math.abs(drag.dx) + Math.abs(drag.dy) > 1;
    drawOverlay();
});
overlay.addEventListener('pointerup', () => {
    const d = drag;
    drag = null;
    if (!d || !d.moved) { drawOverlay(); return; }
    const v = ide.model().vps[d.r.i], x = P.plainArg(v, 'x'), y = P.plainArg(v, 'y');
    if (x === null || y === null) { flash('Only a viewport placed in plain pixels can be dragged: change it in the Inspector.', true); drawOverlay(); return; }
    P.setVpArgs(ide, v, { x: String(Math.max(0, x + d.dx)), y: String(Math.max(0, y + d.dy)) });
});

/* the player panel's toolbar */
(function playerBar() {
    const tb = document.createElement('div');
    tb.className = 'tb';
    const go = document.createElement('select');
    go.title = 'Show a screen on the player';
    for (const [v, t] of [['', 'Go to…'], ['menu', 'Main menu'], ['music', 'Music'], ['albums', 'Albums'],
                          ['wps', 'Now Playing'], ['quick', 'Quick Settings']]) {
        const o = document.createElement('option');
        o.value = v;
        o.textContent = t;
        go.append(o);
    }
    go.onchange = () => { const w = go.value; go.value = ''; if (w) ide.goto(w); screen.focus(); };
    const out = iconButton('outlines', 'Show the open screen\'s viewports on the player', () => toggleOutlines());
    out.id = 'outlines-btn';
    const shot = iconButton('shot', 'Save a screenshot of the player', () => screenshot());
    const grow = document.createElement('span');
    grow.className = 'grow';
    tb.append(go, out, grow, shot);
    $('pane-player').prepend(tb);
})();

function toggleOutlines() {
    ide.outlines = !ide.outlines;
    try { localStorage.setItem('theme-ide-outlines', ide.outlines ? '1' : '0'); } catch (e) { /* */ }
    paintMenus();
    drawOverlay();
}

function screenshot() {
    const url = Player.shot();
    if (!url) return;
    const a = document.createElement('a');
    a.href = url;
    a.download = (ide.theme ? ide.theme.name : 'podbox') + '.png';
    a.click();
}

/* the wheel: drag round the ring to turn it; a click on the ring is the
   button under it, held for as long as it is held; the middle is Select */
(function wheel() {
    const svg = $('wheel');
    let turning = null, holding = null;
    const at = (e) => {
        const b = svg.getBoundingClientRect();
        return [(e.clientX - b.left) / b.width * 200 - 100, (e.clientY - b.top) / b.height * 200 - 100];
    };
    svg.addEventListener('pointerdown', (e) => {
        e.preventDefault();
        svg.setPointerCapture(e.pointerId);
        const [x, y] = at(e);
        if (Math.hypot(x, y) < 36) { holding = 'select'; Player.key('keydown', 'select'); return; }
        turning = { a: Math.atan2(y, x), moved: false, x, y, t0: Date.now() };
    });
    svg.addEventListener('pointermove', (e) => {
        if (!turning) return;
        const [x, y] = at(e);
        let d = Math.atan2(y, x) - turning.a;
        if (d > Math.PI) d -= 2 * Math.PI;
        if (d < -Math.PI) d += 2 * Math.PI;
        if (Math.abs(d) > Math.PI / 8) {
            Player.press(d > 0 ? 'down' : 'up');
            turning.a = Math.atan2(y, x);
            turning.moved = true;
        }
    });
    const end = () => {
        if (holding) { Player.key('keyup', holding); holding = null; }
        if (turning && !turning.moved) {
            const { x, y } = turning;
            const b = Math.abs(x) > Math.abs(y) ? (x > 0 ? 'forward' : 'back') : (y < 0 ? 'menu' : 'play');
            Player.press(b, Date.now() - turning.t0 > 600 ? 1200 : 80);
        }
        turning = null;
    };
    svg.addEventListener('pointerup', end);
    svg.addEventListener('pointercancel', end);
})();

/* ---- Theme Options, tried on the player ---- */

const optionChoices = new Map();     /* .options path -> { key: value } */
ide.optionValues = (path, parsed) => {
    if (!optionChoices.has(path)) {
        const v = {};
        for (const o of parsed.options) v[o.key] = o.def;
        const pref = ide.theme.get(path.replace(/\.options$/i, '.preferences'));
        for (const line of ((pref && pref.text) || '').split('\n')) {
            const m = /^\s*([^:#]+?)\s*:\s*(.*?)\s*$/.exec(line);
            if (m && m[1] in v) v[m[1]] = m[2];
        }
        optionChoices.set(path, v);
    }
    return optionChoices.get(path);
};
ide.setOptions = (path, parsed, chosen) => {
    const rb = Player.rockboxPath(path.replace(/\.options$/i, '.preferences'));
    if (!rb) return;
    const text = parsed.options.map((o) => o.key + ': ' + chosen[o.key]).join('\n') + '\n';
    reload([{ path: rb, data: new TextEncoder().encode(text) }]);
};

/* ---- the menus and keys ---- */

const ACTIONS = {
    'open-folder': () => ide.openFolder(),
    save: () => ide.current && save(ide.current),
    'save-all': () => saveAll(),
    revert: () => ide.current && onDisk([ide.current], [], true),
    undo: () => undo(view), redo: () => redo(view),
    find: () => openSearchPanel(view), 'goto-line': () => gotoLine(view), comment: () => toggleComment(view),
    run: () => run(),
    live: () => { setLive(!ide.live); paintMenus(); if (ide.live) run(); },
    outlines: () => toggleOutlines(),
    shot: () => screenshot(),
    restart: () => {
        if (ide.theme && ide.theme.dirty().length && !confirm('Restarting reloads the page, and the unsaved changes go. Restart anyway?')) return;
        location.reload();
    },
    'view-reset': () => dock.reset(),
    'help-start': () => $('help-start').showModal(),
    'help-keys': () => $('help-keys').showModal(),
    about: () => $('about').showModal()
};
for (const id of dock.panels) ACTIONS['view-' + id] = () => dock.toggle(id);
ACTIONS['view-reference'] = () => dock.show('reference');

(function menus() {
    const all = [...document.querySelectorAll('.menu')];
    const close = () => all.forEach((m) => m.classList.remove('open'));
    for (const m of all) {
        const title = m.querySelector('.title');
        title.onclick = (e) => {
            e.stopPropagation();
            const was = m.classList.contains('open');
            close();
            if (!was) { paintMenus(); m.classList.add('open'); }
        };
        title.onmouseenter = () => { if (all.some((o) => o.classList.contains('open'))) { close(); paintMenus(); m.classList.add('open'); } };
    }
    document.addEventListener('click', close);
    document.addEventListener('keydown', (e) => { if (e.key === 'Escape') close(); });
    for (const b of document.querySelectorAll('.menu .items button'))
        b.onclick = (e) => {
            e.stopPropagation();
            close();
            if (b.dataset.goto) ide.goto(b.dataset.goto);
            else ACTIONS[b.dataset.do]();
        };
})();

function paintMenus() {
    for (const b of document.querySelectorAll('[data-panel]')) b.classList.toggle('on', dock.shown(b.dataset.panel));
    $('m-live').classList.toggle('on', ide.live);
    $('m-outlines').classList.toggle('on', ide.outlines);
    const ob = $('outlines-btn');
    if (ob) ob.classList.toggle('on', ide.outlines);
    const writable = ide.theme && ide.theme.source.writable;
    for (const d of ['save', 'save-all', 'revert'])
        document.querySelector('[data-do="' + d + '"]').disabled = !writable;
}

document.addEventListener('keydown', (e) => {
    if (!(e.ctrlKey || e.metaKey) || e.altKey || e.defaultPrevented) return;
    const k = e.key.toLowerCase();
    const act = { s: 'save', o: 'open-folder', b: 'view-files', j: 'view-output', enter: 'run' }[k];
    if (act) { e.preventDefault(); ACTIONS[act](); }
});
window.addEventListener('beforeunload', (e) => {
    if (ide.theme && ide.theme.dirty().length) { e.preventDefault(); e.returnValue = ''; }
});

/* ---- the status bar and the header ---- */

let flashMsg = null, flashTimer = 0;
function flash(msg, bad) {
    flashMsg = { msg, bad };
    clearTimeout(flashTimer);
    flashTimer = setTimeout(() => { flashMsg = null; paintStatus(); }, bad ? 6000 : 2200);
    paintStatus();
}

function paintStatus() {
    const s = $('status'), bar = $('statusbar'), note = $('player-note');
    let text, bad = false;
    if (flashMsg) { text = flashMsg.msg; bad = flashMsg.bad; }
    else if (Player.state === 'failed') { text = Player.note; bad = true; }
    else if (Player.state !== 'running') text = Player.note || 'Starting the player';
    else if (Player.busy) text = Player.busy;
    else {
        const n = ide.allProblems().length;
        text = n ? n + (n === 1 ? ' problem' : ' problems') : (ide.theme ? 'No problems' : 'Ready');
    }
    s.textContent = text;
    s.onclick = () => dock.show(Player.state === 'failed' ? 'output' : 'problems');
    bar.classList.toggle('bad', bad);
    note.classList.toggle('warn', Player.soundWaiting());
    note.textContent = Player.state !== 'running' ? (Player.note || '')
        : Player.busy ? Player.busy
        : Player.soundWaiting() ? 'Click the player to hear it: the browser holds its sound back until then.'
        : ide.current && skinOf(ide.current) && !ide.onScreen()
            ? fileInfo(ide.current).name + ' is not on the player: Go to shows it.' : '';
}

function paintLive() {
    const l = $('live');
    if (!ide.theme) { l.textContent = ''; return; }
    const n = ide.theme.dirty().length;
    l.textContent = !ide.theme.source.writable ? 'read only' : n ? n + ' unsaved' : 'all saved';
}

/* ---- ctl.py: commands from a script, through serve.py ---- */

async function command(cmd) {
    switch (cmd.op) {
    case 'ping': return 'pong';
    case 'status':
        return { theme: ide.theme && ide.theme.name, file: ide.current, player: Player.state,
                 note: Player.note, screen: ide.fw && ide.fw.activity, onScreen: ide.onScreen(),
                 selected: ide.selected, problems: ide.allProblems().length,
                 dirty: ide.theme ? ide.theme.dirty().map((f) => f.path) : [] };
    case 'skins': return ide.theme ? ide.theme.find(/\.(wps|sbs|fms)$/i) : [];
    case 'open': {
        const want = String(cmd.path).toLowerCase();
        const hit = [...ide.theme.files.keys()].find((p) => p.toLowerCase() === want || p.toLowerCase().endsWith('/' + want));
        if (!hit) throw new Error('no file matches ' + cmd.path);
        ide.open(hit);
        return hit;
    }
    case 'save':
        if (!ide.current) throw new Error('no file is open');
        await save(ide.current, true);
        return ide.current;
    case 'text': return view.state.doc.toString();
    case 'player': return { state: Player.state, note: Player.note };
    case 'press':
        if (!Player.KEYS[cmd.button]) throw new Error('button is one of ' + Object.keys(Player.KEYS).join(', '));
        await Player.press(cmd.button, cmd.ms);
        await wait(250);
        return { state: Player.state };
    case 'goto': await ide.goto(cmd.screen); await wait(400); return ide.fw && ide.fw.activity;
    case 'shot': {
        const url = Player.shot();
        if (!url) throw new Error('the player is not running');
        return { width: 320, height: 240, png: url.slice(url.indexOf(',') + 1) };
    }
    case 'select': ide.select(cmd.index, true); return ide.selected;
    case 'issues': return ide.allProblems();
    case 'viewports': return Player.viewports(cmd.skin || skinOf(ide.current) || 'sbs');
    case 'outline': return ide.current ? ide.model().vps.map((v) => viewportInfo(v).name) : [];
    case 'output': return Player.log.slice(-(cmd.lines || 60)).join('\n');
    case 'reloadPage': setTimeout(() => location.reload(), 50); return 'reloading';
    case 'eval': {
        const r = await (new Function('ide', 'Player', 'return (' + cmd.js + ')'))(ide, Player);
        return r === undefined ? null : JSON.parse(JSON.stringify(r));
    }
    default: throw new Error('unknown command: ' + cmd.op);
    }
}
async function remote(id, cmd) {
    let result;
    try { result = { ok: true, value: await command(cmd || {}) }; }
    catch (e) { result = { ok: false, error: String((e && e.message) || e) }; }
    try {
        await fetch('/api/result', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                                     body: JSON.stringify({ id, result }) });
    } catch (e) { /* the script gave up */ }
}

/* ---- the start ---- */

$('ref-find').oninput = (e) => P.renderReference(e.target.value);
P.renderReference('');
paintMenus();
fitScreen();
refresh();
P.renderState(ide);

(async () => {
    const server = await ServerSource.connect();
    if (server) {
        if (!noPlayer) server.listen(onDisk, remote);
        const t = new Theme(server);
        await t.load();
        await adopt(t);
    } else startPlayer();
})();
