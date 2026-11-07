/* The editors' shared chrome: a side panel that folds to a rail, and a
 * toolbar of icon buttons. */

import { iconButton } from './icons.js';

/* a panel on an editor's left: its head, a hide button and a rail to
 * show it again; shown(open) is told when either is clicked. The panel's
 * content goes in .body. */
export function sidePanel(title, shown) {
    const el = document.createElement('div');
    el.className = 'ed-side';
    const rail = document.createElement('button');
    rail.className = 'rail';
    rail.textContent = title;
    rail.title = 'Show the ' + title.toLowerCase();
    rail.onclick = () => shown(true);
    const head = document.createElement('div');
    head.className = 'panel-head';
    head.innerHTML = '<span class="grow"></span>';
    head.firstChild.textContent = title;
    head.append(iconButton('left', 'Hide the ' + title.toLowerCase(), () => shown(false)));
    const body = document.createElement('div');
    body.className = 'ed-side-body';
    el.append(rail, head, body);
    return { el, head, body, setOpen: (open) => el.classList.toggle('closed', !open) };
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
