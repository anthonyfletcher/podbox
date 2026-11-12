/* The IDE's panels in a dock (dockview, MIT): each can be dragged beside,
 * above or below any other, or onto it as a tab, or minimised to a button
 * in tray, and the arrangement is kept between visits.
 *
 *   createDock(root, panes, actions, changed, tray) -> the dock
 *
 * panes maps each panel's id to its content element; actions maps an id
 * to the buttons its group's header shows while it is the open tab, which
 * setActions(id, buttons) changes;
 * changed() is told whenever what is shown changes.
 *
 * Two panels belong to the editor that is open: 'picker' (the sheet's
 * sprites, a map's tiles, a font's glyphs, a sound's generators) and
 * 'objects' (a map's). The editor fills them with mount(); clear() empties
 * them before another opens and done() hides any left empty, in place, so
 * they come back where they were.
 *
 * Parts: the panels and where each goes at first; their sizes; showing
 * and hiding; minimising; the editor's panels; keeping the arrangement. */

import { createDockview, themeDark } from 'dockview-core/dist/dockview-core.js';
import { iconButton } from './icons.js';

const KEY = 'arcade-dock';

/* each panel's title, and where it goes when it has no place yet: beside
 * another, in order, so the first arrangement is files and the picker on
 * the left, the editor over the output, and the player on the right */
const PANELS = {
    files: { title: 'Files', place: () => ({ direction: 'left' }), size: { width: 300, height: 180 } },
    editor: { title: 'Editor', place: (d) => d.getPanel('files') ? { referencePanel: 'files', direction: 'right' }
                                                                 : { direction: 'right' } },
    player: { title: 'Player', place: () => ({ direction: 'right' }), size: { width: 352 }, min: 340 },
    output: { title: 'Output', place: (d) => d.getPanel('editor') ? { referencePanel: 'editor', direction: 'below' }
                                                                  : { direction: 'below' }, size: { height: 180 } },
    picker: { title: 'Picker', place: (d) => d.getPanel('files') ? { referencePanel: 'files', direction: 'below' }
                                                                 : { direction: 'left' }, size: { height: 320 } },
    objects: { title: 'Objects', place: (d) => d.getPanel('picker') ? { referencePanel: 'picker', direction: 'below' }
                                                                    : { direction: 'left' } },
    profiler: { title: 'Profiler', place: (d) => d.getPanel('output') ? { referencePanel: 'output', direction: 'right' }
                                                                      : { direction: 'below' } },
    memory: { title: 'Memory', place: (d) => d.getPanel('profiler') ? { referencePanel: 'profiler', direction: 'right' }
                                  : d.getPanel('output') ? { referencePanel: 'output', direction: 'right' }
                                  : { direction: 'below' } },
    watch: { title: 'Watch', place: (d) => d.getPanel('player') ? { referencePanel: 'player', direction: 'below' }
                                                                : { direction: 'right' } }
};
const ORDER = ['files', 'editor', 'player', 'output', 'picker', 'objects', 'profiler', 'memory', 'watch'];
/* hidden until asked for */
const LATER = ['profiler', 'memory', 'watch'];
const OWN = ['picker', 'objects'];

/* a tab: the panel's title, kept as written for the editor's file and in
 * capitals for the rest, and buttons to minimise it (all but the editor)
 * and close it */
function tab(id, minimise) {
    const el = document.createElement('div');
    el.className = 'ide-tab' + (id === 'editor' ? ' file' : '');
    const title = document.createElement('span');
    const close = iconButton('close', 'Close');
    close.addEventListener('pointerdown', (e) => e.stopPropagation());
    el.append(title);
    if (id !== 'editor') {
        const min = iconButton('minimise', 'Minimise', (e) => { e.stopPropagation(); minimise(id); });
        min.addEventListener('pointerdown', (e) => e.stopPropagation());
        el.append(min);
    }
    el.append(close);
    let off = null;
    return {
        element: el,
        init: (p) => {
            title.textContent = p.api.title;
            off = p.api.onDidTitleChange((e) => { title.textContent = e.title; });
            close.onclick = (e) => { e.stopPropagation(); p.api.close(); };
        },
        dispose: () => { if (off) off.dispose(); }
    };
}

export function createDock(root, panes, actions, changed, tray) {
    /* the group header's buttons: the open tab's own, which setActions()
     * changes */
    const fills = new Set();
    const headers = (group) => {
        const el = document.createElement('div');
        el.className = 'dock-actions';
        let off = null;
        const fill = () => {
            const p = group.activePanel;
            el.replaceChildren(...(p && actions[p.id] ? actions[p.id] : []));
        };
        return {
            element: el,
            init: (params) => {
                fills.add(fill);
                fill();
                off = params.api.onDidActivePanelChange(fill);
            },
            dispose: () => { fills.delete(fill); if (off) off.dispose(); }
        };
    };
    function setActions(id, nodes) {
        actions[id] = nodes;
        for (const f of fills) f();
    }
    const api = createDockview(root, {
        theme: themeDark,
        createComponent: (o) => ({ element: panes[o.name], init: () => {} }),
        defaultTabComponent: 'tab',
        createTabComponent: (o) => tab(o.id, minimise),
        createRightHeaderActionComponent: headers
    });
    /* who the user closed, so the editor's panels stay closed for it */
    const closed = new Set();
    const titles = {};
    let restoring = false;

    function add(id, position) {
        const p = PANELS[id];
        return api.addPanel({
            id, component: id, title: titles[id] || p.title, position: position || p.place(api),
            inactive: id !== 'editor',
            ...(p.min ? { minimumWidth: p.min } : {})
        });
    }

    /* ---- the sizes ---- */

    /* each panel's size but the editor's, as the user left it: the first
     * arrangement's at first, then what dragging a divider makes them.
     * Whatever the dock changes itself -- a panel shown or hidden, the
     * window resized -- it lays out on a later frame, and these are put
     * back after that, so the editor takes the difference. */
    const sizes = {};
    const defaults = () => {
        for (const k of Object.keys(sizes)) delete sizes[k];
        for (const id of ORDER) if (PANELS[id].size) sizes[id] = Object.assign({}, PANELS[id].size);
    };
    defaults();
    let ours = 0, rootW = 0, rootH = 0;

    function settle() {
        ours++;
        requestAnimationFrame(() => requestAnimationFrame(() => { if (--ours === 0) keepSizes(); }));
    }

    function measure() {
        const ed = api.getPanel('editor');
        for (const p of api.panels)
            if (p.group.api.isVisible && (!ed || p.group !== ed.group))
                sizes[p.id] = { width: p.group.width, height: p.group.height };
    }

    /* a panel in the editor's column keeps its height only, one in its row
     * its width only, so the editor is left to fill */
    function keepSizes() {
        rootW = root.clientWidth;
        rootH = root.clientHeight;
        const ed = api.getPanel('editor');
        const er = ed && ed.group.api.isVisible ? ed.group.element.getBoundingClientRect() : null;
        restoring = true;
        for (let pass = 0; pass < 2; pass++)
            for (const [id, want] of Object.entries(sizes)) {
                const p = api.getPanel(id);
                if (!p || !p.group.api.isVisible || (ed && p.group === ed.group)) continue;
                const r = p.group.element.getBoundingClientRect(), s = {};
                if (want.width && (!er || r.left !== er.left || r.width !== er.width)) s.width = want.width;
                if (want.height && (!er || r.top !== er.top || r.height !== er.height)) s.height = want.height;
                p.api.setSize(s);
            }
        restoring = false;
        keep();
    }

    /* ---- showing and hiding ---- */

    function shown(id) {
        const p = api.getPanel(id);
        return !!p && p.group.api.isVisible;
    }

    /* a panel alone in its group hides with it, so it comes back in place;
     * one among tabs leaves them */
    function hide(id) {
        const p = api.getPanel(id);
        if (!p) return;
        if (p.group.panels.length === 1) p.group.api.setVisible(false);
        else {
            /* not the user's close, which onDidRemovePanel takes it for */
            restoring = true;
            api.removePanel(p);
            restoring = false;
        }
        settle();
        changed();
    }

    function show(id, focus) {
        let p = api.getPanel(id);
        const beside = minimised.get(id);
        if (minimised.delete(id)) fillTray();
        if (!p || !p.group.api.isVisible) {
            const ref = beside && api.getPanel(beside);
            if (!p && ref && !shown(beside) && minimised.has(beside)) {
                /* its partner minimised too, its group hidden: this one
                 * takes the partner's place, and the partner comes back
                 * beside it in turn */
                p = add(id, { referencePanel: beside, direction: 'within' });
                restoring = true;
                api.removePanel(ref);
                restoring = false;
                minimised.set(beside, id);
            } else if (!p)
                p = add(id, beside && shown(beside) ? { referencePanel: beside, direction: 'within' } : null);
            p.group.api.setVisible(true);
            settle();
        }
        if (focus !== false) p.api.setActive();
        changed();
    }

    function toggle(id) {
        if (shown(id)) { hide(id); closed.add(id); } else { closed.delete(id); show(id); }
    }

    function setTitle(id, title) {
        titles[id] = title;
        const p = api.getPanel(id);
        if (p) p.api.setTitle(title);
        fillTray();
    }

    /* ---- minimising ---- */

    /* each minimised panel, and a panel it was a tab beside, if any, so it
     * comes back as that tab; show() brings one back */
    const minimised = new Map();

    function minimise(id) {
        const p = api.getPanel(id);
        if (!p || !p.group.api.isVisible) return;
        const other = p.group.panels.find((q) => q.id !== id);
        minimised.set(id, other ? other.id : null);
        hide(id);
        fillTray();
    }

    /* the editor's own panels have a button only while it fills them */
    function fillTray() {
        if (!tray) return;
        tray.replaceChildren(...[...minimised.keys()].filter(has).map((id) => {
            const b = document.createElement('button');
            b.textContent = titles[id] || PANELS[id].title;
            b.title = 'Show ' + b.textContent;
            b.onclick = () => show(id);
            return b;
        }));
    }

    /* ---- the editor's panels ---- */

    const filled = {};

    function clear() {
        for (const id of OWN) {
            panes[id].replaceChildren();
            filled[id] = false;
        }
    }

    function mount(id, title, el) {
        panes[id].replaceChildren(el);
        filled[id] = true;
        setTitle(id, title);
    }

    /* those the open editor filled shown, unless closed by the user; the
     * rest hidden */
    function done() {
        for (const id of OWN) {
            if (filled[id] && !closed.has(id) && !minimised.has(id) && !shown(id)) show(id, false);
            else if (!filled[id] && shown(id)) hide(id);
        }
        fillTray();
        changed();
    }

    function has(id) { return !OWN.includes(id) || !!filled[id]; }

    /* ---- keeping the arrangement ---- */

    function keep() {
        try {
            localStorage.setItem(KEY, JSON.stringify({ layout: api.toJSON(), sizes, minimised: [...minimised] }));
        } catch (e) { /* not kept */ }
    }

    /* a change of the user's own: a divider dragged, a panel moved */
    function save() {
        if (restoring || ours || root.clientWidth !== rootW || root.clientHeight !== rootH) return;
        measure();
        keep();
    }

    function reset() {
        closed.clear();
        minimised.clear();
        fillTray();
        restoring = true;
        api.clear();
        for (const id of ORDER) add(id);
        restoring = false;
        defaults();
        for (const id of OWN) if (!filled[id]) hide(id);
        hide('output');
        for (const id of LATER) hide(id);
        settle();
        changed();
    }

    let kept = null;
    try { kept = JSON.parse(localStorage.getItem(KEY)); } catch (e) { /* none kept */ }
    restoring = true;
    try { if (kept && kept.layout) api.fromJSON(kept.layout); } catch (e) { api.clear(); }
    try {
        for (const [id, beside] of (kept && kept.minimised) || [])
            if (PANELS[id] && id !== 'editor') minimised.set(id, beside);
    } catch (e) { minimised.clear(); }
    /* one minimised from among tabs is out of the layout until shown */
    for (const id of ORDER) if (!api.getPanel(id) && !minimised.has(id)) {
        add(id);
        if (LATER.includes(id)) hide(id);
    }
    fillTray();
    restoring = false;
    if (kept && kept.sizes) Object.assign(sizes, kept.sizes);
    new ResizeObserver(() => {
        if (root.clientWidth !== rootW || root.clientHeight !== rootH) settle();
    }).observe(root);
    /* a panel closed by its tab is gone until shown again */
    api.onDidRemovePanel((p) => {
        if (!restoring && PANELS[p.id]) { closed.add(p.id); settle(); changed(); }
    });
    api.onDidLayoutChange(save);
    api.onDidActivePanelChange(() => changed());

    return { api, shown, isMinimised: (id) => minimised.has(id), show, hide, toggle, setTitle, setActions, mount, clear, done, has, reset, title: (id) => titles[id] || PANELS[id].title };
}
