/* The cart's settings: cart.json as a form -- its name and text, how it
 * runs, the sprite flags' names, its high-score boards, the options its
 * pause menu shows and its achievements.
 *
 * Parts: writing cart.json tidily; the form, a section at a time; undo.
 * The compiler's own checks still run on every change and report to the
 * output; the form marks a name code could not use as it is typed. */

import { iconButton } from './icons.js';

const NAME = /^[A-Za-z_][A-Za-z0-9_]*$/;
const ORDER = ['id', 'name', 'author', 'version', 'help', 'rate', 'menu_button', 'music', 'suspend',
               'flags', 'lazy_images', 'boards', 'options', 'achievements'];

/* cart.json as text: settings in a fixed order, empty texts and lists left
 * out, flag names trimmed of unnamed ones at the end */
export function writeSettings(cart) {
    const c = Object.assign({}, cart);
    for (const k of ['author', 'version', 'help']) if (c[k] === '') delete c[k];
    if (Array.isArray(c.flags)) {
        const f = c.flags.map((n) => n || null);
        while (f.length && !f[f.length - 1]) f.pop();
        if (f.length) c.flags = f; else delete c.flags;
    }
    for (const k of ['lazy_images', 'boards', 'options', 'achievements'])
        if (Array.isArray(c[k]) && !c[k].length) delete c[k];
    const out = {};
    for (const k of ORDER) if (c[k] !== undefined) out[k] = c[k];
    for (const k in c) if (!(k in out)) out[k] = c[k];
    return JSON.stringify(out, null, 2) + '\n';
}

/* ---- the form ------------------------------------------------------------ */

const field = (tag, cls) => { const e = document.createElement(tag); if (cls) e.className = cls; return e; };

export class CartSettings {
    /* opts: text (cart.json; JSON.parse's error is thrown), changed(text) */
    constructor(parent, opts) {
        this.o = opts;
        this.cart = JSON.parse(opts.text);
        if (!this.cart || typeof this.cart !== 'object' || Array.isArray(this.cart))
            throw new Error('cart.json is a record of settings');
        this.undos = [];
        this.redos = [];
        const root = this.root = field('div', 'cfg');
        root.tabIndex = 0;
        this.body = field('div', 'cfg-body');
        root.append(this.body);
        parent.append(root);
        root.addEventListener('keydown', (e) => {
            if (!(e.ctrlKey || e.metaKey) || e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA') return;
            const k = e.key.toLowerCase();
            if (k === 'z' && !e.shiftKey) { e.preventDefault(); this.undoEdit(); }
            else if (k === 'y' || (k === 'z' && e.shiftKey)) { e.preventDefault(); this.redoEdit(); }
        });
        this.draw();
    }

    destroy() { this.root.remove(); }

    focus() { this.root.focus(); }

    /* a change: kept for undo, written, and the form redrawn when its
     * shape changed (a row added or removed, a type changed) */
    edit(fn, redraw) {
        this.undos.push(JSON.stringify(this.cart));
        if (this.undos.length > 100) this.undos.shift();
        this.redos = [];
        fn(this.cart);
        this.o.changed(writeSettings(this.cart));
        if (redraw) this.draw();
    }

    undoEdit() { this.step(this.undos, this.redos); }

    redoEdit() { this.step(this.redos, this.undos); }

    step(from, to) {
        if (!from.length) return;
        to.push(JSON.stringify(this.cart));
        this.cart = JSON.parse(from.pop());
        this.o.changed(writeSettings(this.cart));
        this.draw();
    }

    /* ---- the parts of a section ---- */

    section(title, add, addTitle) {
        const sec = field('section', 'cfg-sec');
        const h = field('div', 'ed-sub');
        h.innerHTML = '<span class="grow"></span>';
        h.firstChild.textContent = title;
        if (add) { this.addButton = iconButton('plus', addTitle, add); h.append(this.addButton); }
        sec.append(h);
        this.body.append(sec);
        return sec;
    }

    /* a labelled field in a grid of them */
    row(sec, label, input, tip) {
        const l = field('label', 'cfg-row');
        const s = field('span');
        s.textContent = label;
        l.append(s, input);
        if (tip) l.title = tip;
        sec.append(l);
        return input;
    }

    /* a text box writing to obj[key]; empty leaves it out where it may be */
    text(obj, key, opts) {
        opts = opts || {};
        const i = field(opts.long ? 'textarea' : 'input');
        if (!opts.long) i.type = 'text';
        i.value = obj[key] === undefined || obj[key] === null ? '' : obj[key];
        if (opts.placeholder) i.placeholder = opts.placeholder;
        if (opts.code) i.className = 'code';
        const check = () => i.classList.toggle('bad', !!(opts.name && i.value !== '' && !NAME.test(i.value)) ||
                                                     !!(opts.required && i.value.trim() === ''));
        i.oninput = check;
        i.onchange = () => this.edit(() => {
            const v = opts.long ? i.value : i.value.trim();
            if (v === '' && !opts.required) delete obj[key]; else obj[key] = v;
        }, opts.redraw);
        check();
        return i;
    }

    choose(obj, key, choices, fallback, redraw) {
        const s = field('select');
        for (const [v, label] of choices) {
            const o = field('option');
            o.value = JSON.stringify(v);
            o.textContent = label;
            s.append(o);
        }
        s.value = JSON.stringify(obj[key] === undefined ? fallback : obj[key]);
        s.onchange = () => this.edit(() => { obj[key] = JSON.parse(s.value); }, redraw);
        return s;
    }

    number(obj, key, fallback) {
        const i = field('input');
        i.type = 'number';
        i.step = 1;
        i.value = obj[key] === undefined ? fallback : obj[key];
        i.onchange = () => this.edit(() => {
            const v = Math.round(+i.value);
            if (Number.isFinite(v)) obj[key] = v;
        });
        return i;
    }

    remover(list, i, what) {
        return iconButton('trash', 'Remove this ' + what, () => this.edit(() => list.splice(i, 1), true));
    }

    /* ---- the sections ---- */

    draw() {
        const c = this.cart, scroll = this.body.scrollTop;
        this.body.innerHTML = '';

        let sec = this.section('The cart');
        this.row(sec, 'Name', this.text(c, 'name', { required: true }), 'Shown in the Games menu');
        this.row(sec, 'Author', this.text(c, 'author'));
        this.row(sec, 'Version', this.text(c, 'version'));
        this.row(sec, 'Help', this.text(c, 'help', { long: true }), 'Shown by the pause menu\'s Help');
        const id = field('input', 'code');
        id.value = c.id || '';
        id.readOnly = true;
        this.row(sec, 'ID', id, 'Saves and scores are kept under it, so it never changes');

        sec = this.section('Running');
        this.row(sec, 'Rate', this.choose(c, 'rate',
            [[20, '20 ticks a second'], [25, '25 ticks a second'], [33, '33 ticks a second'], [50, '50 ticks a second']], 33),
            'How often update() runs');
        this.row(sec, 'Menu button', this.choose(c, 'menu_button',
            [['pause', 'Pauses the game'], ['game', 'Is button 2, for the game']], 'pause'));
        this.row(sec, 'Music', this.choose(c, 'music',
            [['listen', 'Listen: what is playing'], ['control', 'Control: pause, skip, volume'],
             ['own', 'Own: plays tracks itself']], 'listen'), 'What the cart may do with the owner\'s music');
        this.row(sec, 'On Quit', this.choose(c, 'suspend',
            [[true, 'Keeps the game, to carry on next time'], [false, 'Ends the game']], true),
            'Whether the player offers the game as it was left');

        sec = this.section('Sprite flags');
        const flags = Array.isArray(c.flags) ? c.flags : (c.flags = []);
        const grid = field('div', 'cfg-flags');
        for (let b = 0; b < 8; b++) {
            const l = field('label');
            l.textContent = b;
            if (flags[b] === undefined) flags[b] = null;
            const i = this.text(flags, b, { name: true, code: true, placeholder: 'unnamed', required: false });
            i.onchange = () => this.edit(() => { flags[b] = i.value.trim() || null; });
            l.title = 'Code has FLAG.<name> for flag ' + b;
            l.append(i);
            grid.append(l);
        }
        sec.append(grid);

        this.boards(c);
        this.options(c);
        this.achievements(c);
        this.body.scrollTop = scroll;
    }

    /* a table of records: its head row, then a row each */
    table(sec, cls, heads) {
        const t = field('div', 'cfg-table ' + cls);
        for (const h of heads) {
            const s = field('span', 'cfg-th');
            s.textContent = h;
            t.append(s);
        }
        sec.append(t);
        return t;
    }

    empty(sec, text) {
        const p = field('div', 'cfg-none');
        p.textContent = text;
        sec.append(p);
    }

    boards(c) {
        const list = c.boards || [];
        const sec = this.section('High-score boards', () => this.edit((cart) => {
            cart.boards = list;
            let k = list.length + 1;
            while (list.some((b) => b.name === 'board' + k)) k++;
            list.push({ name: 'board' + k });
        }, true), 'Add a board');
        this.addButton.disabled = list.length >= 8;
        if (!list.length) { this.empty(sec, 'None.'); return; }
        const t = this.table(sec, 'cfg-boards', ['Name', 'Title', 'Wins', 'Shown as', 'Version', '']);
        list.forEach((b, i) => {
            t.append(this.text(b, 'name', { name: true, code: true, required: true }),
                     this.text(b, 'title', { placeholder: b.name }),
                     this.choose(b, 'wins', [['high', 'Highest'], ['low', 'Lowest']], 'high'),
                     this.choose(b, 'format', [['number', 'A number'], ['time', 'A time (ms)'], ['ticks', 'A time (ticks)']], 'number'),
                     this.number(b, 'version', 1),
                     this.remover(list, i, 'board'));
        });
    }

    options(c) {
        const list = c.options || [];
        const sec = this.section('Pause-menu options', () => this.edit((cart) => {
            cart.options = list;
            let k = list.length + 1;
            while (list.some((o) => o.name === 'option' + k)) k++;
            list.push({ name: 'option' + k, type: 'onoff' });
        }, true), 'Add an option');
        this.addButton.disabled = list.length >= 8;
        if (!list.length) { this.empty(sec, 'None.'); return; }
        list.forEach((o, i) => {
            const card = field('div', 'cfg-card');
            const top = field('div', 'cfg-line');
            const type = this.choose(o, 'type', [['onoff', 'On or off'], ['choice', 'A choice'], ['number', 'A number']], 'onoff', true);
            type.onchange = () => this.edit(() => {
                o.type = JSON.parse(type.value);
                for (const k of ['choices', 'min', 'max', 'step', 'default']) delete o[k];
                if (o.type === 'choice') o.choices = ['One', 'Two'];
                if (o.type === 'number') { o.min = 0; o.max = 10; }
            }, true);
            top.append(this.text(o, 'name', { name: true, code: true, required: true }),
                       this.text(o, 'title', { placeholder: o.name }), type, this.remover(list, i, 'option'));
            const more = field('div', 'cfg-line');
            const label = (text, input) => { const l = field('label'); l.append(text, input); return l; };
            if (o.type === 'choice') {
                const ch = field('input');
                ch.type = 'text';
                ch.value = (o.choices || []).join(', ');
                ch.title = 'The choices, with commas between';
                ch.onchange = () => this.edit(() => {
                    o.choices = ch.value.split(',').map((s) => s.trim()).filter(Boolean);
                    if (o.default >= o.choices.length) delete o.default;
                }, true);
                more.append(label('Choices', ch), label('Default',
                    this.choose(o, 'default', (o.choices || []).map((s, k) => [k, s]), 0)));
            } else if (o.type === 'number') {
                more.append(label('From', this.number(o, 'min', 0)), label('to', this.number(o, 'max', 10)),
                            label('in steps of', this.number(o, 'step', 1)),
                            label('Default', this.number(o, 'default', o.min === undefined ? 0 : o.min)));
            } else {
                more.append(label('Default', this.choose(o, 'default', [[false, 'Off'], [true, 'On']], false)));
            }
            const help = this.text(o, 'help', { placeholder: 'Help, shown under it by the pause menu\'s Help' });
            card.append(top, more, help);
            sec.append(card);
        });
    }

    achievements(c) {
        const list = c.achievements || [];
        const sec = this.section('Achievements', () => this.edit((cart) => {
            cart.achievements = list;
            let k = list.length + 1;
            while (list.some((a) => a.name === 'achievement' + k)) k++;
            list.push({ name: 'achievement' + k });
        }, true), 'Add an achievement');
        this.addButton.disabled = list.length >= 32;
        if (!list.length) { this.empty(sec, 'None.'); return; }
        const t = this.table(sec, 'cfg-ach', ['Name', 'Title', 'How it is earned', '']);
        list.forEach((a, i) => {
            t.append(this.text(a, 'name', { name: true, code: true, required: true }),
                     this.text(a, 'title', { placeholder: a.name }),
                     this.text(a, 'text'),
                     this.remover(list, i, 'achievement'));
        });
    }
}
