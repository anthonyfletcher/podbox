/* The skin language in the editor: colouring, hover help, completion and
 * problems, all from skin-core.js -- the scanner and tag tables the rest of
 * the IDE reads skins with -- so the editor knows exactly the tags this
 * firmware has. One parse per edit serves all four. */

import { ViewPlugin, Decoration, WidgetType, hoverTooltip, EditorView } from '@codemirror/view';
import { RangeSetBuilder } from '@codemirror/state';
import { linter } from '@codemirror/lint';
import { autocompletion } from '@codemirror/autocomplete';
import { tagInfo, allTags } from './names.js';

/* global scan, build, ARG_ENUMS */

const cache = new WeakMap();
/* build() of a document's text, kept until the text changes */
export function model(doc) {
    let m = cache.get(doc);
    if (!m) { m = build(doc.toString()); cache.set(doc, m); }
    return m;
}

const marks = {};
const mark = (cls) => marks[cls] || (marks[cls] = Decoration.mark({ class: cls }));

class Swatch extends WidgetType {
    constructor(colour) { super(); this.colour = colour; }
    eq(o) { return o.colour === this.colour; }
    toDOM() {
        const s = document.createElement('span');
        s.className = 'swatch-chip';
        s.style.background = '#' + this.colour;
        return s;
    }
}

function decorate(view) {
    const m = model(view.state.doc);
    const b = new RangeSetBuilder();
    const ranges = m.ranges.slice().sort((a, c) => a.s - c.s || a.e - c.e);
    let last = -1;
    for (const r of ranges) {
        if (r.s < last) continue;
        if (r.cls === 't-str' || r.cls === 't-num') {
            const t = view.state.doc.sliceString(r.s, r.e).trim().replace(/^!/, '');
            if (/^[0-9a-fA-F]{6}$/.test(t))
                b.add(r.s, r.s, Decoration.widget({ widget: new Swatch(t), side: -1 }));
        }
        b.add(r.s, r.e, mark(r.cls));
        last = r.e;
    }
    return b.finish();
}

const colouring = ViewPlugin.fromClass(class {
    constructor(view) { this.decorations = decorate(view); }
    update(u) { if (u.docChanged || u.viewportChanged) this.decorations = decorate(u.view); }
}, { decorations: (v) => v.decorations });

/* the innermost tag under pos, and the argument it is in */
export function tagAt(nodes, pos) {
    let hit = null;
    const visit = (list) => {
        for (const n of list || []) {
            if (pos < n.s || pos > n.e) continue;
            if (n.type === 'tag') {
                hit = { node: n, arg: -1 };
                n.args.forEach((a, i) => {
                    if (pos >= a.s && pos <= a.e) { hit.arg = i; if (a.nodes) visit(a.nodes); }
                });
            } else if (n.type === 'cond') {
                if (n.subject && pos >= n.subject.s && pos <= n.subject.e) visit([n.subject]);
                for (const br of n.branches) visit(br.nodes);
            }
        }
    };
    visit(nodes);
    return hit;
}

const esc = (s) => String(s).replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));

/* what an argument's value means, where the tables say */
function valueNote(tag, i, raw) {
    const e = ARG_ENUMS[tag + ':' + i];
    const t = raw.trim();
    if (t === '' || t === '-') return 'the default';
    if (/^!?[0-9a-fA-F]{6}$/.test(t)) return t[0] === '!' ? 'colour #' + t.slice(1) + ', kept as written'
                                                            : 'colour #' + t;
    if (e && e.vals) {
        const key = e.firstChar ? t[0].toLowerCase() : t;
        if (e.vals[key]) return e.vals[key];
    }
    if (/^-?\d+%$/.test(t)) return t + ' of the space';
    return null;
}

export function helpHtml(n, argIndex) {
    const info = tagInfo(n.name);
    if (!info) return '<h4>%' + esc(n.name) + '</h4>Not a tag this firmware has.';
    let h = '<h4><code>%' + esc(n.name) + '</code> &nbsp;' + esc(info.desc) + '</h4>';
    if (n.args.length) {
        h += '<div class="arg">';
        n.args.forEach((a, i) => {
            const k = info.args[i] || 'argument ' + (i + 1);
            const note = valueNote(n.name, i, a.raw);
            const val = a.raw.trim() === '' ? '(empty)' : a.raw.trim();
            h += '<span class="k">' + (i === argIndex ? '<b>' + esc(k) + '</b>' : esc(k)) + '</span><span><code>' +
                 esc(val.length > 40 ? val.slice(0, 40) + '…' : val) + '</code>' + (note ? ' — ' + esc(note) : '') + '</span>';
        });
        h += '</div>';
    } else if (info.args.length) {
        h += '<div class="arg"><span class="k">takes</span><span>' + esc(info.args.join(', ')) + '</span></div>';
    }
    if (info.custom) h += '<div class="fork">A PodBox tag: other Rockbox builds do not have it.</div>';
    return h;
}

const hover = hoverTooltip((view, pos) => {
    const hit = tagAt(model(view.state.doc).nodes, pos);
    if (!hit) return null;
    return {
        pos: hit.node.s, end: hit.node.e, above: true,
        create: () => {
            const dom = document.createElement('div');
            dom.className = 'skin-help';
            dom.innerHTML = helpHtml(hit.node, hit.arg);
            return { dom };
        }
    };
}, { hoverTime: 350 });

let TAGS = null;
function complete(ctx) {
    const m = ctx.matchBefore(/%\??[A-Za-z]*/);
    if (!m || (m.from === m.to && !ctx.explicit)) return null;
    if (!TAGS) TAGS = allTags().map((t) => ({
        label: t.name, detail: t.desc,
        info: t.args.length ? 'takes ' + t.args.join(', ') : undefined,
        apply: t.args.length && !/^\|/.test(t.spec) && !/^\?/.test(t.spec) ? t.name + '(' : t.name,
        boost: t.custom ? -1 : 0
    }));
    const lead = m.text.startsWith('%?') ? 2 : 1;
    return { from: m.from + lead, options: TAGS, validFor: /^[A-Za-z]*$/ };
}

function problems(view) {
    const m = model(view.state.doc);
    const len = view.state.doc.length;
    return m.issues.map((i) => {
        const from = Math.max(0, Math.min(len, i.off || 0));
        const line = view.state.doc.lineAt(from);
        return { from, to: Math.min(line.to, from + 3) || from, severity: i.sev === 'err' ? 'error' : 'warning',
                 message: i.msg };
    });
}

export function skinLanguage() {
    return [
        colouring, hover,
        autocompletion({ override: [complete], icons: false }),
        linter(problems, { delay: 400 }),
        EditorView.theme({ '.cm-tooltip-autocomplete ul li': { fontFamily: 'ui-monospace, Consolas, monospace' } })
    ];
}
