/* The panels beside the editor and the player: Files, Outline, Inspector,
 * State, Problems, the tag reference and Output. Each render*(ide) draws its
 * panel again from the IDE's state; main.js calls them when that changes.
 *
 * ide is main.js's: { theme, current, view, model(), selected, fw,
 * open(path, offset), select(index), edit(from, to, text), player, goto(),
 * run() }. */

import { icon } from './icons.js';
import { fileInfo, isSkin, viewportInfo, tagInfo, allTags } from './names.js';
import { LIBRARY, albumDir } from './demo.js';

/* global parseOptions */

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const el = (tag, cls, html) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (html !== undefined) e.innerHTML = html;
    return e;
};

/* 0-based line of an offset in text */
export function lineOf(text, off) {
    let n = 0;
    for (let i = 0; i < off && i < text.length; i++) if (text.charCodeAt(i) === 10) n++;
    return n;
}

/* ---- Files ---- */

export function renderFiles(ide) {
    const tree = $('tree');
    tree.replaceChildren();
    if (!ide.theme) {
        const e = el('div', 'empty', 'No theme is open. The player is showing PodBox\'s own theme, Scrim.');
        const b = el('button', 'primary', 'Open a theme folder…');
        b.onclick = () => ide.openFolder();
        e.append(el('br'), b);
        tree.append(e);
        return;
    }
    const groups = new Map();
    const paths = [...ide.theme.files.keys()].sort();
    const seen = new Map();
    for (const p of paths) { const n = fileInfo(p).name; seen.set(n, (seen.get(n) || 0) + 1); }
    for (const p of paths) {
        const f = fileInfo(p);
        /* two Now Playing skins are told apart by their file names */
        if (seen.get(f.name) > 1 && f.skin) f.name += ' (' + p.split('/').pop().replace(/\.[^.]+$/, '') + ')';
        if (!groups.has(f.group)) groups.set(f.group, { order: f.order, items: [] });
        groups.get(f.group).items.push([p, f]);
    }
    const problems = ide.problemCounts();
    for (const [name, g] of [...groups].sort((a, b) => a[1].order - b[1].order || a[0].localeCompare(b[0]))) {
        tree.append(el('div', 'group', esc(name)));
        for (const [p, f] of g.items) {
            const file = ide.theme.get(p);
            const row = el('div', 'item');
            if (p === ide.current) row.classList.add('open');
            if (file.text !== null && file.text !== file.saved) row.classList.add('dirty');
            const n = problems.get(p) || 0;
            if (n) row.classList.add('bad');
            row.title = p;
            row.innerHTML = icon(f.icon) + '<span class="name">' + esc(f.name) + '</span><span class="what">' +
                            esc(n ? n + (n === 1 ? ' problem' : ' problems') : f.what) + '</span>';
            row.onclick = () => ide.open(p);
            tree.append(row);
        }
    }
}

/* ---- Outline ---- */

/* the firmware's record of a viewport, matched by the line it is declared on */
export function firmwareVp(ide, v) {
    if (!ide.fw || !ide.fw.viewports) return null;
    const line = lineOf(ide.model().text, v.off) + 1;
    return ide.fw.viewports.find((f) => f.line === line) || null;
}

export function renderOutline(ide) {
    const box = $('outline');
    box.replaceChildren();
    if (!ide.current || !isSkin(ide.current)) {
        box.append(el('div', 'empty', 'Open a screen from Files &mdash; Now Playing or Menus and status bar &mdash; ' +
                                      'to see what it is made of.'));
        return;
    }
    const m = ide.model();
    const screen = fileInfo(ide.current).name;
    const head = el('div', 'head');
    head.textContent = ide.onScreen()
        ? screen + ' is on the player: what is showing is marked.'
        : screen + ' is not on the player now. Player › Go to shows it.';
    box.append(head);
    m.vps.forEach((v, i) => {
        const info = viewportInfo(v);
        const fw = ide.onScreen() ? firmwareVp(ide, v) : null;
        const row = el('div', 'row');
        if (i === ide.selected) row.classList.add('on');
        let state = '';
        if (fw) {
            if (fw.ui) state = 'list';
            else if (fw.never) state = '';
            else if (fw.hidden) { state = 'hidden now'; row.classList.add('hidden'); }
            else { state = 'showing'; row.classList.add('shown'); }
        }
        const geo = v.implicit ? '' : v.x + ',' + v.y + '  ' + v.w + '×' + v.h;
        row.innerHTML = icon(info.icon) + '<span class="name">' + esc(info.name) +
            (info.detail ? ' <span class="geo">' + esc(info.detail) + '</span>' : '') +
            '</span><span class="geo">' + esc(geo) + '</span><span class="state">' + esc(state) + '</span>';
        row.title = 'Line ' + (lineOf(m.text, v.off) + 1);
        row.onclick = () => ide.select(i, true);
        box.append(row);
    });
}

/* ---- Inspector ---- */

/* a viewport tag's arguments, by name */
function vpArgs(v) {
    const names = v.tagName === 'V' ? ['x', 'y', 'width', 'height', 'font']
                                    : ['label', 'x', 'y', 'width', 'height', 'font'];
    const raw = (v.node ? v.node.args : []).map((a) => a.raw.trim());
    return { names, raw };
}

/* the viewport tag written again with arguments changed: { x: "10", ... } */
function vpText(v, changes) {
    const { names, raw } = vpArgs(v);
    const vals = names.map((n, i) => raw[i] !== undefined ? raw[i] : '-');
    for (const [name, value] of Object.entries(changes)) vals[names.indexOf(name)] = value === '' ? '-' : value;
    while (vals.length > (v.tagName === 'V' ? 4 : 5) && vals[vals.length - 1] === '-') vals.pop();
    return '%' + v.tagName + '(' + vals.join(',') + ')';
}

export function setVpArgs(ide, v, changes) {
    ide.edit(v.node.s, v.node.e, vpText(v, changes));
}
const setVpArg = (ide, v, name, value) => setVpArgs(ide, v, { [name]: value });

/* a viewport's own x or y as written, when it is a plain number of pixels */
export function plainArg(v, name) {
    const { names, raw } = vpArgs(v);
    const t = raw[names.indexOf(name)];
    return t !== undefined && /^\d+$/.test(t) ? parseInt(t, 10) : null;
}

/* the node of the first %Vf or %Vb in a viewport */
function colourTag(ide, v, name) {
    const off = v.tags && v.tags.get(name);
    if (off === undefined) return null;
    const find = (list) => {
        for (const n of list || []) {
            if (n.type === 'tag' && n.s === off) return n;
            if (n.type === 'cond') {
                for (const b of n.branches) { const r = find(b.nodes); if (r) return r; }
            }
            if (n.type === 'tag') for (const a of n.args) { const r = find(a.nodes); if (r) return r; }
        }
        return null;
    };
    return find(ide.model().nodes);
}

function colourField(ide, v, name, label) {
    const wrap = el('label');
    wrap.append(label);
    const row = el('div', 'colour');
    const tag = colourTag(ide, v, name);
    const raw = tag && tag.args[0] ? tag.args[0].raw.trim() : '';
    const picker = el('input');
    picker.type = 'color';
    const text = el('input');
    text.type = 'text';
    text.value = raw;
    text.placeholder = tag ? '' : 'the theme\'s, from the .cfg';
    const hex = raw.replace(/^!/, '');
    picker.value = /^[0-9a-fA-F]{6}$/.test(hex) ? '#' + hex : '#000000';
    const set = (value) => {
        if (tag && tag.args[0]) ide.edit(tag.args[0].s, tag.args[0].e, value);
        else if (tag) ide.edit(tag.e - 1, tag.e - 1, value);
        else ide.edit(v.node.e, v.node.e, '%' + name + '(' + value + ')');
    };
    picker.oninput = () => { text.value = picker.value.slice(1); set(text.value); };
    text.onchange = () => set(text.value.trim());
    row.append(picker, text);
    wrap.append(row);
    return wrap;
}

export function renderInspector(ide) {
    const box = $('inspector');
    box.replaceChildren();
    const m = ide.current && isSkin(ide.current) ? ide.model() : null;
    const v = m && ide.selected >= 0 ? m.vps[ide.selected] : null;
    if (!v) {
        box.append(el('div', 'empty', 'Pick a viewport in the Outline, or on the player with Outlines on, ' +
                                      'and change where it sits, its font and its colours here.'));
        return;
    }
    const info = viewportInfo(v);
    const f = el('div', 'form');
    f.append(el('h3', '', esc(info.name)));
    f.append(el('div', 'sub', esc((info.detail || 'the part of the file before any viewport') +
                                  ' · line ' + (lineOf(m.text, v.off) + 1))));
    if (!v.node) {
        f.append(el('div', 'note', 'This has no viewport tag of its own: it is the whole screen.'));
        box.append(f);
        return;
    }
    const { names, raw } = vpArgs(v);
    const num = (name, label, hint) => {
        const l = el('label');
        l.append(label);
        const i = el('input');
        i.value = raw[names.indexOf(name)] || '';
        i.placeholder = hint;
        i.onchange = () => setVpArg(ide, v, name, i.value.trim());
        l.append(i);
        return l;
    };
    f.append(el('div', 'sec', 'Where it sits'));
    const g = el('div', 'grid');
    g.append(num('x', 'Left', '0'), num('y', 'Top', '0'), num('width', 'Width', 'to the edge'),
             num('height', 'Height', 'to the edge'));
    f.append(g);
    f.append(el('div', 'note', 'Pixels, or a percentage of the screen. A minus number counts from the right ' +
                               'or bottom. Drag it on the player to move it.'));
    if (v.tagName !== 'V') {
        const l = el('label');
        l.append(v.tagName === 'Vi' ? 'Name (%VI shows it)' : 'Name (%Vd shows it)');
        const i = el('input');
        i.value = raw[0] || '';
        i.onchange = () => setVpArg(ide, v, 'label', i.value.trim());
        l.append(i);
        f.append(l);
    }
    f.append(el('div', 'sec', 'Text'));
    const fl = el('label');
    fl.append('Font');
    const sel = el('select');
    const cur = raw[names.indexOf('font')] || '';
    const opts = [['', 'The theme\'s font (from the .cfg)'], ['0', 'The built-in font'], ['1', 'The theme\'s font']];
    for (const [id, file] of Object.entries(m.fonts || {})) opts.push([id, id + ': ' + file]);
    for (const [id, text] of opts) {
        const o = el('option');
        o.value = id;
        o.textContent = text;
        if (id === cur) o.selected = true;
        sel.append(o);
    }
    sel.onchange = () => setVpArg(ide, v, 'font', sel.value);
    fl.append(sel);
    f.append(fl);
    const cols = el('div', 'row2');
    cols.append(colourField(ide, v, 'Vf', 'Text colour'), colourField(ide, v, 'Vb', 'Background'));
    f.append(cols);
    f.append(el('div', 'note', 'A colour can also be a word that follows the album art: accent, dominant ' +
                               'or vivid, with the theme\'s Dynamic Colors on.'));
    const used = [...(v.tags || new Map()).keys()].filter((t) => !['V', 'Vl', 'Vi', 'Vf', 'Vb'].includes(t));
    if (used.length) {
        f.append(el('div', 'sec', 'What it shows'));
        const c = el('div', 'contents');
        for (const t of used) {
            const ti = tagInfo(t);
            c.append(el('div', '', '<code>%' + esc(t) + '</code><span>' + esc(ti ? ti.desc : 'not a known tag') + '</span>'));
        }
        f.append(c);
    }
    box.append(f);
}

/* ---- State ---- */

const REPEAT = [['off', 'Off'], ['all', 'All'], ['one', 'One'], ['shuffle', 'Shuffle'], ['ab', 'A-B']];
const stateKept = { shuffle: 'off', repeat: 'off', volume: -25 };

export function renderState(ide) {
    const box = $('state');
    box.replaceChildren();
    const f = el('div', 'form');

    f.append(el('div', 'sec', 'Playing'));
    const t = el('div', 'buttons');
    for (const [btn, label, title] of [['back', icon('prev'), 'Previous track (on Now Playing)'],
                                       ['play', icon('play') + icon('pause'), 'Play or pause'],
                                       ['forward', icon('next'), 'Next track (on Now Playing)']]) {
        const b = el('button', '', label);
        b.title = title;
        b.onclick = () => ide.player.press(btn);
        t.append(b);
    }
    f.append(t);
    const al = el('label');
    al.append('Play an album from the demo library');
    const pick = el('select');
    pick.append(el('option', '', 'Choose an album…'));
    for (const a of LIBRARY) {
        const o = el('option');
        o.value = a.id;
        o.textContent = a.title + ' — ' + a.artist;
        pick.append(o);
    }
    pick.onchange = () => {
        const a = LIBRARY.find((x) => x.id === pick.value);
        if (!a) return;
        ide.player.play(albumDir(a));
        setTimeout(() => ide.goto('wps'), 900);
    };
    al.append(pick);
    f.append(al);

    const settings = () => ide.player.settings(['shuffle: ' + stateKept.shuffle, 'repeat: ' + stateKept.repeat,
                                                'volume: ' + stateKept.volume]);
    const row = el('div', 'row2');
    const sh = el('label');
    sh.append('Shuffle');
    const ss = el('select');
    for (const v of ['off', 'on']) { const o = el('option', '', v === 'on' ? 'On' : 'Off'); o.value = v; ss.append(o); }
    ss.value = stateKept.shuffle;
    ss.onchange = () => { stateKept.shuffle = ss.value; settings(); };
    sh.append(ss);
    const rp = el('label');
    rp.append('Repeat');
    const rs = el('select');
    for (const [v, l] of REPEAT) { const o = el('option', '', l); o.value = v; rs.append(o); }
    rs.value = stateKept.repeat;
    rs.onchange = () => { stateKept.repeat = rs.value; settings(); };
    rp.append(rs);
    row.append(sh, rp);
    f.append(row);
    const vl = el('label');
    vl.append('Volume');
    const vr = el('input');
    vr.type = 'range';
    vr.min = -74;
    vr.max = 6;
    vr.value = stateKept.volume;
    vr.onchange = () => { stateKept.volume = +vr.value; settings(); };
    vl.append(vr);
    f.append(vl);
    f.append(el('div', 'note', 'These are the player\'s own settings, so a theme\'s shuffle and repeat icons, ' +
                               'volume bar and the rest show them as they would on an iPod. The clock reads ' +
                               '9:00 and the battery full.'));

    /* the theme's own Theme Options, from its .options files */
    const files = ide.theme ? ide.theme.find(/\.options$/i) : [];
    for (const path of files) {
        const parsed = parseOptions(ide.theme.get(path).text || '');
        if (!parsed.ok || !parsed.options.length) continue;
        f.append(el('div', 'sec', esc(parsed.title || 'Theme Options')));
        const chosen = ide.optionValues(path, parsed);
        for (const o of parsed.options) {
            const l = el('label');
            l.append(o.name);
            l.title = o.help || '';
            let input;
            if (o.type === 'choice') {
                input = el('select');
                for (const c of o.choices) { const opt = el('option', '', esc(c.label)); opt.value = c.value; input.append(opt); }
            } else {
                input = el('input');
                input.maxLength = o.max;
            }
            input.value = chosen[o.key];
            input.onchange = () => { chosen[o.key] = input.value; ide.setOptions(path, parsed, chosen); };
            l.append(input);
            f.append(l);
        }
        f.append(el('div', 'note', 'Choices made here are tried on the player and not saved to the theme.'));
    }
    box.append(f);
}

/* ---- Problems ---- */

export function renderProblems(ide) {
    const box = $('problems');
    box.replaceChildren();
    const all = ide.allProblems();
    if (!all.length) {
        box.append(el('div', 'empty', ide.theme ? 'No problems found in the theme\'s screens.' : 'No theme is open.'));
        return 0;
    }
    for (const p of all) {
        const row = el('div', 'row');
        row.innerHTML = '<span class="sev ' + (p.sev === 'err' ? 'err' : 'warn') + '"></span><span class="where">' +
            esc(fileInfo(p.path).name) + ':' + (p.line + 1) + '</span><span>' + esc(p.msg) + '</span>';
        row.onclick = () => ide.open(p.path, p.off);
        box.append(row);
    }
    return all.length;
}

/* ---- the tag reference ---- */

export function renderReference(filter) {
    const box = $('reference');
    box.replaceChildren();
    const want = (filter || '').trim().toLowerCase().replace(/^%/, '');
    let n = 0;
    for (const t of allTags()) {
        if (want && !t.name.toLowerCase().startsWith(want) && !t.desc.toLowerCase().includes(want)) continue;
        if (++n > 400) break;
        const row = el('div', 'row');
        row.innerHTML = '<code>%' + esc(t.name) + (t.args.length ? '(' + esc(t.args.join(', ')) + ')' : '') + '</code>' +
                        '<span>' + esc(t.desc) + (t.custom ? ' <span class="args">(PodBox)</span>' : '') + '</span>';
        box.append(row);
    }
}

/* ---- Output ---- */

export function appendOutput(line, cls) {
    const c = $('console');
    const d = el('div', cls || '');
    d.textContent = line;
    const atEnd = c.scrollTop + c.clientHeight >= c.scrollHeight - 4;
    c.append(d);
    while (c.childElementCount > 1500) c.firstChild.remove();
    if (atEnd) c.scrollTop = c.scrollHeight;
}

