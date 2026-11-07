/* The palette editor: a palette's entries as swatches, each set by hex or
 * by red, green and blue, and shown as the player shows it (RGB565). The
 * cart's first palette is the one its pictures' numbers mean; the others
 * recolour the same numbers. Moving or sorting entries rewrites the
 * numbers in every picture, and in the other palettes, so nothing changes
 * how it looks; code that names a colour by number is the cart's to
 * follow.
 *
 * Parts: the palette and its pictures; the swatches; the chosen entry;
 * rearranging. */

import { shown } from './pixels.js';

const hex = (c) => '#' + c.toString(16).padStart(6, '0');

export class PaletteEditor {
    /* opts: rgb (this palette), first (whether it is the cart's own),
     * pictures() ([{ name, px }] the cart's pictures, as palette numbers),
     * changed(rgb, order) (order: the old entry each new one was, when
     * entries moved or went), say(text, error), asText(), sure(question)
     * (a Promise: whether to go on) */
    constructor(parent, opts) {
        this.o = opts;
        this.rgb = opts.rgb.slice();
        this.at = Math.min(7, this.rgb.length - 1);
        const root = this.root = document.createElement('div');
        root.className = 'pal';
        root.tabIndex = 0;
        const head = document.createElement('div');
        head.className = 'pal-head';
        head.textContent = opts.first
            ? 'The cart\'s palette: the colours its pictures are drawn in.'
            : 'Another palette: loadpal() shows the same pictures in these colours.';
        const text = document.createElement('button');
        text.textContent = 'Edit as text';
        text.onclick = () => opts.asText();
        head.append(' ', text);
        this.grid = document.createElement('div');
        this.grid.className = 'pal-grid';
        this.panel = document.createElement('div');
        this.panel.className = 'pal-panel';
        const tools = document.createElement('div');
        tools.className = 'pal-tools';
        const act = (label, title, fn) => {
            const b = document.createElement('button');
            b.textContent = label;
            b.title = title;
            b.onclick = fn;
            tools.append(b);
        };
        act('← Move', 'Swap with the one before', () => this.move(-1));
        act('Move →', 'Swap with the one after', () => this.move(1));
        act('Add', 'A new colour at the end', () => this.add());
        act('Delete', 'Remove this colour, if no picture uses it', () => this.remove());
        act('Sort by brightness', 'Darkest first, colour 0 kept first', () => this.sort('light'));
        act('Sort by hue', 'Round the colour wheel, greys first, colour 0 kept first', () => this.sort('hue'));
        root.append(head, this.grid, this.panel, tools);
        parent.append(root);
        this.count();
        this.draw();
    }

    destroy() { this.root.remove(); }

    /* how many pixels of the pictures use each entry */
    count() {
        this.uses = new Array(256).fill(0);
        for (const p of this.o.pictures()) for (const v of p.px) this.uses[v]++;
    }

    draw() {
        this.grid.innerHTML = '';
        const seen = new Map();
        this.rgb.forEach((c, i) => {
            const s = document.createElement('button');
            s.className = 'pal-swatch';
            s.style.background = hex(shown(c));
            s.classList.toggle('on', i === this.at);
            s.classList.toggle('unused', !this.uses[i]);
            s.title = i + ': ' + hex(c) + (this.uses[i] ? ', ' + this.uses[i] + ' pixels' : ', unused');
            s.textContent = i;
            /* light text on dark colours */
            const sc = shown(c), lum = (sc >> 16) * 0.3 + (sc >> 8 & 255) * 0.59 + (sc & 255) * 0.11;
            s.style.color = lum < 120 ? '#ffffffcc' : '#000000aa';
            s.onclick = () => { this.at = i; this.draw(); };
            this.grid.append(s);
            const k = shown(c);
            seen.set(k, (seen.get(k) || []).concat(i));
        });
        this.same = [...seen.values()].filter((v) => v.length > 1);
        this.drawPanel();
    }

    drawPanel() {
        const i = this.at, c = this.rgb[i];
        const p = this.panel;
        p.innerHTML = '';
        const big = document.createElement('div');
        big.className = 'pal-big';
        big.style.background = hex(shown(c));
        const fields = document.createElement('div');
        fields.className = 'pal-fields';
        const title = document.createElement('div');
        title.innerHTML = '<b>Colour ' + i + '</b>' + (i === 0 ? ' (clear in sprites, the map and images)' : '');
        const hexIn = document.createElement('input');
        hexIn.value = hex(c);
        hexIn.spellcheck = false;
        hexIn.onchange = () => {
            const m = /^#?([0-9a-f]{6})$/i.exec(hexIn.value.trim());
            if (m) this.set(parseInt(m[1], 16));
            else hexIn.value = hex(this.rgb[this.at]);
        };
        fields.append(title, hexIn);
        ['Red', 'Green', 'Blue'].forEach((name, k) => {
            const shift = 16 - 8 * k;
            const row = document.createElement('label');
            row.className = 'pal-slider';
            const r = document.createElement('input');
            r.type = 'range';
            r.min = 0;
            r.max = 255;
            r.value = c >> shift & 255;
            const n = document.createElement('span');
            n.textContent = c >> shift & 255;
            r.oninput = () => {
                n.textContent = r.value;
                this.set(this.rgb[this.at] & ~(255 << shift) | +r.value << shift, true);
            };
            r.onchange = () => this.draw();
            row.append(name, r, n);
            fields.append(row);
        });
        const note = document.createElement('div');
        note.className = 'pal-note';
        const twins = this.same.find((v) => v.includes(i));
        note.textContent = 'On the player: ' + hex(shown(c)) + '. ' +
            (this.uses[i] ? this.uses[i] + ' pixels use it.' : 'No picture uses it.') +
            (twins ? ' Colours ' + twins.join(' and ') + ' look the same on the player.' : '');
        if (twins) note.classList.add('warn');
        fields.append(note);
        p.append(big, fields);
    }

    /* the chosen entry's colour; while a slider moves, only it is redrawn */
    set(c, moving) {
        this.rgb[this.at] = c;
        this.o.changed(this.rgb.slice(), null);
        if (moving) {
            this.grid.children[this.at].style.background = hex(shown(c));
            this.panel.querySelector('.pal-big').style.background = hex(shown(c));
        } else this.draw();
    }

    /* entries in a new order: order[k] the old entry that becomes k. The
     * first time, asked: code naming colours by number follows the numbers,
     * not the colours. */
    async reorder(order, why) {
        if (this.o.first && !this.agreed) {
            this.agreed = await this.o.sure();
            if (!this.agreed) return false;
        }
        this.rgb = order.map((k) => this.rgb[k]);
        this.o.changed(this.rgb.slice(), order);
        this.count();
        this.draw();
        if (why) this.o.say(why + ' The pictures keep their colours.');
        return true;
    }

    async move(d) {
        const i = this.at, j = i + d;
        if (j < 0 || j >= this.rgb.length) return;
        const order = this.rgb.map((_, k) => k);
        [order[i], order[j]] = [order[j], order[i]];
        if (await this.reorder(order, 'Colours ' + i + ' and ' + j + ' swapped.')) {
            this.at = j;
            this.draw();
        }
    }

    add() {
        if (this.rgb.length >= 256) { this.o.say('A palette has at most 256 colours.', true); return; }
        this.rgb.push(this.rgb[this.at]);
        this.at = this.rgb.length - 1;
        this.o.changed(this.rgb.slice(), null);
        this.count();
        this.draw();
    }

    async remove() {
        const i = this.at;
        if (this.uses[i]) { this.o.say('Colour ' + i + ' is used by ' + this.uses[i] + ' pixels; it stays.', true); return; }
        if (this.rgb.length <= 1) return;
        const order = this.rgb.map((_, k) => k).filter((k) => k !== i);
        const last = i === order.length;
        if (last ? this.reorderQuietly(order) : await this.reorder(order, 'Colour ' + i + ' removed, those after it moved down one.')) {
            this.at = Math.min(i, order.length - 1);
            this.draw();
        }
    }

    /* the last entry gone: nothing else moves, so nothing to ask */
    reorderQuietly(order) {
        this.rgb = order.map((k) => this.rgb[k]);
        this.o.changed(this.rgb.slice(), order);
        this.count();
        return true;
    }

    async sort(by) {
        const key = (c) => {
            const r = (c >> 16) / 255, g = (c >> 8 & 255) / 255, b = (c & 255) / 255;
            const max = Math.max(r, g, b), min = Math.min(r, g, b), l = (max + min) / 2;
            if (by === 'light') return 0.3 * r + 0.59 * g + 0.11 * b;
            if (max - min < 0.08) return -1 + l;           /* greys first, dark to light */
            const d = max - min;
            const h = max === r ? ((g - b) / d + 6) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
            return h + l / 10;
        };
        const rest = this.rgb.map((_, k) => k).slice(1).sort((a, b) => key(this.rgb[a]) - key(this.rgb[b]));
        const order = [0, ...rest];
        if (await this.reorder(order, by === 'light' ? 'Sorted by brightness.' : 'Sorted by hue.')) {
            this.at = order.indexOf(this.at);
            this.draw();
        }
    }
}
