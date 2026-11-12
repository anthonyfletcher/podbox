/* api.md read as data: each function's signature and line, for the IDE's
 * hints and for the test that holds api.md to arcade_api.def.
 *
 * An entry is "### name(args)" and the paragraph under it. Arguments in
 * [brackets] are optional, a bracket holding several (as [x, y]) taking
 * them together; "...name" is any number. */

/* { name: { sig, args: [{ name, optional, rest }], min, max, doc, section } } */
export function parseReference(text) {
    const out = {};
    let section = '', cur = null;
    for (const line of text.split(/\r?\n/)) {
        const h2 = /^## (.+)$/.exec(line);
        if (h2) { section = h2[1]; cur = null; continue; }
        const h3 = /^### ([A-Za-z_][\w.]*)\((.*)\)\s*$/.exec(line);
        if (h3) {
            cur = { sig: h3[1] + '(' + h3[2] + ')', args: args(h3[2]), doc: '', section: section };
            cur.min = cur.args.filter((a) => !a.optional && !a.rest).length;
            cur.max = cur.args.some((a) => a.rest) ? Infinity : cur.args.length;
            out[h3[1]] = cur;
            continue;
        }
        if (cur && line.trim()) cur.doc += (cur.doc ? ' ' : '') + line.trim();
        else if (cur && cur.doc) cur = null;
    }
    return out;
}

function args(list) {
    const out = [];
    let depth = 0, word = '';
    const push = () => {
        const w = word.trim();
        word = '';
        if (!w) return;
        const rest = w.startsWith('...');
        out.push({ name: rest ? w.slice(3) : w, optional: depth > 0, rest: rest });
    };
    for (const ch of list) {
        if (ch === '[') { push(); depth++; } else if (ch === ']') { push(); depth--; } else if (ch === ',') push();
        else word += ch;
    }
    push();
    return out;
}
