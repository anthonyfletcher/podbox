/* A question in a dialog of the IDE's own, in place of the browser's
 * prompt and confirm boxes.
 *
 *   ask(title, fields, ok) -> Promise of the answers by name, or null
 *
 * fields: { name, label, value } for text; with options ([[value, label],
 * ...]) a choice; with type 'number' a number; with note, text under it;
 * with when(answers), shown only while that holds; { text } is a paragraph
 * to read. No fields asks only to confirm: the answer is {} for ok, null
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
            if (f.text) {
                const p = document.createElement('p');
                p.className = 'ask-text';
                p.textContent = f.text;
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
            if (f.note) {
                const n = document.createElement('small');
                n.textContent = f.note;
                row.append(n);
            }
            form.append(row);
            rows.push([row, f]);
        }
        const values = () => Object.fromEntries(Object.entries(inputs).map(([k, i]) => [k, i.value]));
        const refresh = () => { for (const [row, f] of rows) row.hidden = !!f.when && !f.when(values()); };
        form.addEventListener('input', refresh);
        refresh();
        const buttons = document.createElement('div');
        buttons.className = 'ask-buttons';
        const cancel = document.createElement('button');
        cancel.type = 'button';
        cancel.textContent = 'Cancel';
        const yes = document.createElement('button');
        yes.className = 'primary';
        yes.textContent = ok || 'OK';
        buttons.append(cancel, yes);
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
