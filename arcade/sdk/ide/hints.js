/* What a function takes, shown as you write the call: its signature, the
 * argument the cursor is at in bold, and its line from api.md -- or for
 * the cart's own functions, their declaration and the comment above it.
 * Hovering over a function's name shows the same. */

import { StateField } from '@codemirror/state';
import { EditorView, showTooltip, hoverTooltip } from '@codemirror/view';
import { syntaxTree } from '@codemirror/language';
import { parseReference } from '../src/reference.js';
import apiText from '../api.md';

const reference = parseReference(apiText);

/* the cart's own functions, from its files: main.js's by name, another
 * file's as file.name */
export function ownFunctions(folder) {
    const out = {};
    for (const [file, text] of Object.entries(folder)) {
        if (!/^[A-Za-z_]\w*\.js$/.test(file) || typeof text !== 'string') continue;
        const ns = file === 'main.js' ? '' : file.slice(0, -3) + '.';
        const lines = text.split(/\r?\n/);
        lines.forEach((line, i) => {
            const m = /^function\s+([A-Za-z_]\w*)\s*\(([^)]*)\)/.exec(line);
            if (!m) return;
            /* the comment lines just above it */
            const doc = [];
            for (let k = i - 1; k >= 0 && /^\s*(\/\/|\/?\*)/.test(lines[k]); k--)
                doc.unshift(lines[k].replace(/^\s*(\/\/+|\/\*+|\*+\/?)\s?/, '').replace(/\*\/\s*$/, '').trim());
            const args = m[2].split(',').map((a) => a.trim()).filter(Boolean)
                .map((a) => ({ name: a, optional: false, rest: false }));
            out[ns + m[1]] = { sig: ns + m[1] + '(' + m[2].trim() + ')', args: args,
                               doc: doc.filter(Boolean).join(' ') };
        });
    }
    return out;
}

/* the call the cursor is in: its name, and which argument */
function callAt(state, pos) {
    for (let node = syntaxTree(state).resolveInner(pos, -1); node; node = node.parent) {
        if (node.name !== 'ArgList' || pos <= node.from) continue;
        const closed = state.sliceDoc(node.to - 1, node.to) === ')';
        if (closed && pos >= node.to) continue;
        const call = node.parent;
        if (!call || call.name !== 'CallExpression' || !call.firstChild) continue;
        const name = state.sliceDoc(call.firstChild.from, call.firstChild.to).replace(/\s+/g, '');
        let index = 0;
        for (let c = node.firstChild; c; c = c.nextSibling)
            if (c.name === ',' && c.to <= pos) index++;
        return { name: name, index: index, from: node.from };
    }
    return null;
}

function card(entry, index) {
    const dom = document.createElement('div');
    dom.className = 'cm-hint';
    const sig = document.createElement('div');
    sig.className = 'cm-hint-sig';
    const name = entry.sig.slice(0, entry.sig.indexOf('('));
    sig.append(name + '(');
    /* the arguments, as the signature writes them, the current one bold */
    const inner = entry.sig.slice(entry.sig.indexOf('(') + 1, -1);
    let k = 0, word = '';
    const flush = (text) => {
        if (!text) return;
        const named = text.replace(/[[\]\s.]/g, '');
        if (named && entry.args[k]) {
            const b = document.createElement(k === index || (entry.args[k].rest && index >= k) ? 'b' : 'span');
            b.textContent = text;
            sig.append(b);
            k++;
        } else sig.append(text);
    };
    for (const ch of inner) {
        if (ch === ',' || ch === '[' || ch === ']') { flush(word); word = ''; sig.append(ch); } else word += ch;
    }
    flush(word);
    sig.append(')');
    dom.append(sig);
    if (entry.doc) {
        const doc = document.createElement('div');
        doc.className = 'cm-hint-doc';
        /* `code` as code, the rest as text */
        entry.doc.split('`').forEach((part, i) => {
            if (i % 2 === 0) { doc.append(part); return; }
            const code = document.createElement('code');
            code.textContent = part;
            doc.append(code);
        });
        dom.append(doc);
    }
    return dom;
}

/* the hints, given a way to read the cart's files */
export function hints(folder) {
    const find = (name) => ownFunctions(folder())[name] || reference[name];
    const field = StateField.define({
        create: () => null,
        update(value, tr) {
            if (!tr.docChanged && !tr.selection) return value;
            const pos = tr.state.selection.main.head;
            const c = callAt(tr.state, pos);
            const e = c && find(c.name);
            if (!e) return null;
            return { pos: c.from, above: true, strictSide: false, arrow: false,
                     create: () => ({ dom: card(e, c.index) }) };
        },
        provide: (f) => showTooltip.from(f)
    });
    const hover = hoverTooltip((view, pos) => {
        const word = view.state.wordAt(pos);
        if (!word) return null;
        /* the name with any namespace before it: ui.choose, not choose */
        let from = word.from;
        while (from > 0 && /[\w.]/.test(view.state.sliceDoc(from - 1, from))) from--;
        const name = view.state.sliceDoc(from, word.to);
        if (view.state.sliceDoc(word.to, word.to + 1) !== '(') return null;
        const e = find(name);
        if (!e) return null;
        return { pos: from, end: word.to, above: true, create: () => ({ dom: card(e, -1) }) };
    });
    const theme = EditorView.baseTheme({
        '.cm-hint': { padding: '6px 10px', maxWidth: '520px', font: '13px system-ui, sans-serif' },
        '.cm-hint-sig': { fontFamily: 'ui-monospace, Consolas, monospace' },
        '.cm-hint-sig b': { color: 'var(--accent, #2f6fd6)' },
        '.cm-hint-doc': { marginTop: '4px', opacity: 0.85, lineHeight: 1.4 },
        '.cm-hint-doc code': { fontFamily: 'ui-monospace, Consolas, monospace', fontSize: '12px' }
    });
    return [field, hover, theme];
}

/* a completion's signature and line */
export function about(name, folder) {
    return ownFunctions(folder)[name] || reference[name] || null;
}
