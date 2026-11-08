/* Find and replace in code, as a toolbar of the IDE's own over the text:
 * the find box with its three ways of matching, back and on, and the
 * replace box with replacing one or all. Enter finds the next, Shift+Enter
 * the one before, Escape closes it. */

import {
    search, SearchQuery, getSearchQuery, setSearchQuery, findNext, findPrevious,
    replaceNext, replaceAll, closeSearchPanel
} from '@codemirror/search';
import { iconButton } from './icons.js';

function panel(view) {
    const el = document.createElement('div');
    el.className = 'tb find';
    const box = (name, placeholder) => {
        const i = document.createElement('input');
        i.type = 'text';
        i.name = name;
        i.placeholder = placeholder;
        i.spellcheck = false;
        i.setAttribute('main-field', name === 'search' ? 'true' : 'false');
        return i;
    };
    const find = box('search', 'Find'), repl = box('replace', 'Replace');
    const flags = { caseSensitive: false, regexp: false, wholeWord: false };
    const toggles = {};
    const query = () => new SearchQuery({ search: find.value, replace: repl.value, ...flags });
    const update = () => view.dispatch({ effects: setSearchQuery.of(query()) });
    const toggle = (key, ic, title) => {
        const b = iconButton(ic, title, () => {
            flags[key] = !flags[key];
            b.classList.toggle('on', flags[key]);
            update();
        });
        toggles[key] = b;
        return b;
    };
    const sep = () => { const s = document.createElement('span'); s.className = 'sep'; return s; };
    find.addEventListener('input', update);
    repl.addEventListener('input', update);
    find.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') { e.preventDefault(); (e.shiftKey ? findPrevious : findNext)(view); }
    });
    repl.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') { e.preventDefault(); replaceNext(view); }
    });
    el.addEventListener('keydown', (e) => {
        if (e.key === 'Escape') { e.preventDefault(); closeSearchPanel(view); view.focus(); }
    });
    el.append(find,
        toggle('caseSensitive', 'matchCase', 'Match case'),
        toggle('wholeWord', 'wholeWord', 'Whole words'),
        toggle('regexp', 'regex', 'Regular expression'),
        iconButton('up', 'Previous (Shift+Enter)', () => findPrevious(view)),
        iconButton('down', 'Next (Enter)', () => findNext(view)),
        sep(), repl,
        iconButton('replace', 'Replace (Enter)', () => replaceNext(view)),
        iconButton('replaceAll', 'Replace all', () => replaceAll(view)));
    const grow = document.createElement('span');
    grow.className = 'grow';
    el.append(grow, iconButton('close', 'Close (Escape)', () => { closeSearchPanel(view); view.focus(); }));
    /* what the editor was last asked to find, as the panel opens */
    const show = (q) => {
        find.value = q.search;
        repl.value = q.replace;
        for (const k of Object.keys(flags)) {
            flags[k] = q[k];
            toggles[k].classList.toggle('on', q[k]);
        }
    };
    show(getSearchQuery(view.state));
    return {
        dom: el,
        top: true,
        mount: () => { find.focus(); find.select(); },
        update: (u) => {
            for (const tr of u.transactions)
                for (const e of tr.effects)
                    if (e.is(setSearchQuery) && !e.value.eq(query())) show(e.value);
        }
    };
}

export const findPanel = search({ top: true, createPanel: panel });
