/* A question in a dialog of the IDE's own, in place of the browser's
 * prompt and confirm boxes.
 *
 *   ask(title, fields, ok) -> Promise of the answers by name, or null
 *
 * fields: { name, label, value } for text; with options ([[value, label],
 * ...]) a choice; with type 'number' a number; with note, text under it;
 * with when(answers), shown only while that holds; with check(value,
 * answers), the problem with an answer, or nothing when it will do: OK
 * waits until every field shown will do. { text } is a paragraph to read,
 * { pre } text set out as written, in columns. No fields asks only to confirm: the answer is {} for ok, null
 * otherwise. */

export function ask(title, fields, ok) {
    return new Promise((done) => {
        const d = document.createElement('dialog');
        d.className = 'ask';
        const form = document.createElement('form');
        form.method = 'dialog';
        const h = document.createElement('h2');
        h.textContent = title;
        form.append(h);
        const inputs = {};
        const rows = [];
        for (const f of fields || []) {
            if (f.text || f.pre !== undefined) {
                const p = document.createElement(f.pre !== undefined ? 'pre' : 'p');
                p.className = f.pre !== undefined ? 'ask-pre' : 'ask-text';
                p.textContent = f.pre !== undefined ? f.pre : f.text;
                form.append(p);
                continue;
            }
            const row = document.createElement('label');
            row.className = 'ask-row';
            const label = document.createElement('span');
            label.textContent = f.label;
            let input;
            if (f.options) {
                input = document.createElement('select');
                for (const [v, text] of f.options) {
                    const o = document.createElement('option');
                    o.value = v;
                    o.textContent = text;
                    input.append(o);
                }
            } else {
                input = document.createElement('input');
                input.type = f.type || 'text';
                input.spellcheck = false;
            }
            if (f.value !== undefined) input.value = f.value;
            input.name = f.name;
            inputs[f.name] = input;
            row.append(label, input);
            const n = document.createElement('small');
            n.textContent = f.note || '';
            row.append(n);
            form.append(row);
            rows.push([row, f, input, n]);
        }
        const values = () => Object.fromEntries(Object.entries(inputs).map(([k, i]) => [k, i.value]));
        /* each field shown or not, and its problem in place of its note once
         * something is typed; OK only when there is none */
        const refresh = () => {
            const v = values();
            let ready = true;
            for (const [row, f, input, n] of rows) {
                row.hidden = !!f.when && !f.when(v);
                const problem = !row.hidden && f.check ? f.check(input.value, v) : null;
                if (problem) ready = false;
                const say = problem && input.value !== '';
                n.textContent = say ? problem : f.note || '';
                n.classList.toggle('bad', !!say);
                n.hidden = !n.textContent;
            }
            yes.disabled = !ready;
        };
        const buttons = document.createElement('div');
        buttons.className = 'ask-buttons';
        const cancel = document.createElement('button');
        cancel.type = 'button';
        cancel.textContent = 'Cancel';
        const yes = document.createElement('button');
        yes.className = 'primary';
        yes.textContent = ok || 'OK';
        /* nothing to answer: the one button closes it */
        if (rows.length || !fields || !fields.length) buttons.append(cancel);
        buttons.append(yes);
        form.addEventListener('input', refresh);
        form.addEventListener('change', refresh);
        refresh();
        form.append(buttons);
        d.append(form);
        document.body.append(d);
        let answer = null;
        cancel.onclick = () => d.close();
        form.onsubmit = () => { answer = values(); };
        d.addEventListener('close', () => { d.remove(); done(answer); });
        d.showModal();
        const first = form.querySelector('input, select');
        if (first) first.focus(); else yes.focus();
    });
}
