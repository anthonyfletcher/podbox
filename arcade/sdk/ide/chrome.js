/* The editors' shared chrome: their panels in the IDE's dock, sections in
 * them that fold away, a toolbar of icon buttons and a dropdown. */

import { icon, iconButton } from './icons.js';

/* where an editor's panels go: the IDE's dock, which sets it */
let host = { mount: () => {} };
export function setPanelHost(h) { host = h; }

/* an editor's own panel in the dock, titled: 'picker' for what it draws
 * with, 'objects' for a map's objects. Its content goes in .body. */
export function sidePanel(title, id) {
    const el = document.createElement('div');
    el.className = 'ed-side';
    const body = document.createElement('div');
    body.className = 'ed-side-body';
    el.append(body);
    host.mount(id || 'picker', title, el);
    return { el, body };
}

/* a section's heading in a side panel that folds what is under it away:
 * a click on it shows or hides the body, and whether it is folded is kept
 * between visits; .head is the heading, for buttons of its own */
export function foldSection(title, body, open) {
    const key = 'arcade-fold-' + title.toLowerCase();
    try { const kept = localStorage.getItem(key); if (kept !== null) open = kept === '1'; } catch (e) { /* none kept */ }
    const head = document.createElement('div');
    head.className = 'ed-sub fold';
    const twist = document.createElement('span');
    twist.className = 'twist';
    const name = document.createElement('span');
    name.className = 'grow';
    name.textContent = title;
    head.append(twist, name);
    const set = (on) => {
        twist.innerHTML = icon(on ? 'down' : 'right');
        body.hidden = !on;
    };
    head.onclick = (e) => {
        if (e.target.closest('.ib')) return;
        set(body.hidden);
        try { localStorage.setItem(key, body.hidden ? '0' : '1'); } catch (x) { /* not kept */ }
    };
    set(open !== false);
    return { head, body };
}

/* a toolbar; add(name, title, onclick) appends an icon button and
 * returns it, sep() a divider, put(...nodes) anything else */
export function toolbar() {
    const el = document.createElement('div');
    el.className = 'tb';
    return {
        el,
        add(name, title, onclick) { const b = iconButton(name, title, onclick); el.append(b); return b; },
        sep() { const s = document.createElement('span'); s.className = 'sep'; el.append(s); },
        put(...nodes) { el.append(...nodes); }
    };
}

/* a small labelled number box for a toolbar */
export function numberBox(label, value, min, max, title, set) {
    const l = document.createElement('label');
    l.title = title;
    const i = document.createElement('input');
    i.type = 'number';
    i.min = min;
    i.max = max;
    i.value = value;
    i.onchange = () => set(Math.max(min, Math.min(max, Math.round(+i.value) || min)), i);
    l.append(label, i);
    return l;
}

/* a toolbar button that drops a list down under it: fill(list) puts the
 * list's rows in each time it opens; it closes on a click elsewhere or
 * Escape. setLabel(text) changes what the button says. */
export function dropdown(label, title, fill) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'tb-drop';
    b.title = title;
    const text = document.createElement('span');
    b.append(text);
    b.insertAdjacentHTML('beforeend', icon('down'));
    const setLabel = (t) => { text.textContent = t; };
    setLabel(label);
    let list = null;
    const close = () => {
        if (!list) return;
        list.remove();
        list = null;
        document.removeEventListener('pointerdown', outside, true);
        document.removeEventListener('keydown', key, true);
    };
    const outside = (e) => { if (list && !list.contains(e.target) && !b.contains(e.target)) close(); };
    const key = (e) => { if (e.key === 'Escape') { e.stopPropagation(); close(); } };
    const open = () => {
        list = document.createElement('div');
        list.className = 'drop-list';
        fill(list);
        const r = b.getBoundingClientRect();
        list.style.left = r.left + 'px';
        list.style.top = r.bottom + 2 + 'px';
        document.body.append(list);
        document.addEventListener('pointerdown', outside, true);
        document.addEventListener('keydown', key, true);
    };
    b.onclick = () => { if (list) close(); else open(); };
    return { el: b, setLabel, close, refill: () => { if (list) { list.innerHTML = ''; fill(list); } } };
}
