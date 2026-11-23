/* The IDE's panels in a dock (dockview, MIT), as in the Arcade's IDE: each
 * can be dragged beside, above or below another, or onto it as a tab, or
 * minimised to a button in the status bar, and the arrangement is kept
 * between visits.
 *
 *   createDock(root, panes, changed, tray) -> the dock
 *
 * panes maps each panel's id to its content element; changed() is told
 * whenever what is shown changes. */

import { createDockview, themeDark } from 'dockview-core/dist/dockview-core.js';
import { iconButton } from './icons.js';

const KEY = 'theme-ide-dock';

/* each panel's title, and where it goes when it has no place yet: the files
 * and the outline on the left, the editor in the middle over problems and
 * output, the player on the right over the inspector and the state */
/* placed by panel ref in direction, or at fallback without it */
const beside = (ref, direction, fallback) => (d) =>
    d.getPanel(ref) ? { referencePanel: ref, direction } : { direction: fallback };
const PANELS = {
    files: { title: 'Files', place: () => ({ direction: 'left' }), size: { width: 250 } },
    editor: { title: 'Editor', place: beside('files', 'right', 'right') },
    player: { title: 'Player', place: () => ({ direction: 'right' }), size: { width: 420 }, min: 340 },
    outline: { title: 'Outline', place: beside('files', 'below', 'left'), size: { height: 360 } },
    problems: { title: 'Problems', place: beside('editor', 'below', 'below'), size: { height: 170 } },
    output: { title: 'Output', place: beside('problems', 'within', 'below') },
    reference: { title: 'Tag reference', place: beside('problems', 'within', 'below') },
    inspector: { title: 'Inspector', place: beside('player', 'below', 'right'), size: { height: 330 } },
    state: { title: 'State', place: beside('inspector', 'within', 'right') }
};
const ORDER = ['files', 'editor', 'player', 'outline', 'problems', 'output', 'reference', 'inspector', 'state'];

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

export function createDock(root, panes, changed, tray) {
    const api = createDockview(root, {
        theme: themeDark,
        createComponent: (o) => ({ element: panes[o.name], init: () => {} }),
        defaultTabComponent: 'tab',
        createTabComponent: (o) => tab(o.id, minimise)
    });
    const titles = {};
    const minimised = new Map();
    let restoring = false;

    function add(id, position) {
        const p = PANELS[id];
        return api.addPanel({
            id, component: id, title: titles[id] || p.title, position: position || p.place(api),
            inactive: id !== 'editor' && id !== 'problems',
            ...(p.min ? { minimumWidth: p.min } : {})
        });
    }

    function shown(id) {
        const p = api.getPanel(id);
        return !!p && p.group.api.isVisible;
    }

    function hide(id) {
        const p = api.getPanel(id);
        if (!p) return;
        restoring = true;
        if (p.group.panels.length === 1) p.group.api.setVisible(false);
        else api.removePanel(p);
        restoring = false;
        changed();
    }

    function show(id, focus) {
        let p = api.getPanel(id);
        const beside = minimised.get(id);
        if (minimised.delete(id)) fillTray();
        if (!p) p = add(id, beside && shown(beside) ? { referencePanel: beside, direction: 'within' } : null);
        if (!p.group.api.isVisible) p.group.api.setVisible(true);
        if (focus !== false) p.api.setActive();
        changed();
    }

    function toggle(id) { if (shown(id)) hide(id); else show(id); }

    function setTitle(id, title) {
        titles[id] = title;
        const p = api.getPanel(id);
        if (p) p.api.setTitle(title);
    }

    function minimise(id) {
        const p = api.getPanel(id);
        if (!p || !p.group.api.isVisible) return;
        const other = p.group.panels.find((q) => q.id !== id);
        minimised.set(id, other ? other.id : null);
        hide(id);
        fillTray();
    }

    function fillTray() {
        if (!tray) return;
        tray.replaceChildren(...[...minimised.keys()].map((id) => {
            const b = document.createElement('button');
            b.textContent = titles[id] || PANELS[id].title;
            b.title = 'Show ' + b.textContent;
            b.onclick = () => show(id);
            return b;
        }));
    }

    function keep() {
        if (restoring) return;
        try { localStorage.setItem(KEY, JSON.stringify({ layout: api.toJSON(), minimised: [...minimised] })); }
        catch (e) { /* not kept */ }
    }

    function sizePanels() {
        for (const id of ORDER) if (PANELS[id].size && api.getPanel(id)) api.getPanel(id).api.setSize(PANELS[id].size);
    }

    function reset() {
        minimised.clear();
        fillTray();
        restoring = true;
        api.clear();
        for (const id of ORDER) add(id);
        sizePanels();
        restoring = false;
        keep();
        changed();
    }

    let kept = null;
    try { kept = JSON.parse(localStorage.getItem(KEY)); } catch (e) { /* none kept */ }
    restoring = true;
    try { if (kept && kept.layout) api.fromJSON(kept.layout); } catch (e) { api.clear(); }
    try { for (const [id, beside] of (kept && kept.minimised) || []) if (PANELS[id]) minimised.set(id, beside); }
    catch (e) { minimised.clear(); }
    let fresh = false;
    for (const id of ORDER) if (!api.getPanel(id) && !minimised.has(id)) { add(id); fresh = true; }
    restoring = false;
    fillTray();
    if (fresh && !(kept && kept.layout)) requestAnimationFrame(sizePanels);
    api.onDidLayoutChange(keep);
    api.onDidActivePanelChange(() => changed());
    api.onDidRemovePanel(() => { if (!restoring) changed(); });

    return { api, shown, show, hide, toggle, setTitle, reset, panels: ORDER };
}
