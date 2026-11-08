/* The palette editor: a palette's entries as swatches, each set by hex or
 * by red, green and blue, and shown as the iPod shows it (its screen has
 * 65,536 colours: RGB565). A swatch dragged onto another moves there; +
 * adds one, and Delete removes the chosen one. The
 * cart's first palette is the one its pictures' numbers mean; the others
 * recolour the same numbers. Moving or deleting entries rewrites the
 * numbers in every picture, and in the other palettes, so nothing changes
 * how it looks; code that names a colour by number is the cart's to
 * follow.
 *
 * Parts: the palette and its pictures; the swatches, dragged; the chosen
 * entry; rearranging. */

import { shown, hex } from './pixels.js';
import { icon } from './icons.js';

export class PaletteEditor {
    /* opts: rgb (this palette), first (whether it is the cart's own),
     * pictures() ([{ name, px }] the cart's pictures, as palette numbers),
     * changed(rgb, order) (order: the old entry each new one was, when
     * entries moved or went), say(text, error), sure(question)
     * (a Promise: whether to go on) */
    constructor(parent, opts) {
        this.o = opts;
        this.rgb = opts.rgb.slice();
        this.at = Math.min(7, this.rgb.length - 1);
        const root = this.root = document.createElement('div');
        root.className = 'pal';
        root.tabIndex = 0;
        const body = document.createElement('div');
        body.className = 'pal-body';
        this.grid = document.createElement('div');
        this.grid.className = 'pal-grid';
        this.panel = document.createElement('div');
        this.panel.className = 'pal-panel';
        body.append(this.grid, this.panel);
        root.append(body);
        root.addEventListener('keydown', (e) => {
            if (e.target.tagName === 'INPUT') return;
            if (e.key === 'Delete' || e.key === 'Backspace') { e.preventDefault(); this.remove(); }
            if (e.key === 'ArrowLeft' && this.at > 0) { this.at--; this.draw(); }
            if (e.key === 'ArrowRight' && this.at < this.rgb.length - 1) { this.at++; this.draw(); }
        });
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
            s.onpointerdown = (e) => this.drag(e, i);
            this.grid.append(s);
            const k = shown(c);
            seen.set(k, (seen.get(k) || []).concat(i));
        });
        if (this.rgb.length < 256) {
            const plus = document.createElement('button');
            plus.className = 'pal-swatch pal-add';
            plus.innerHTML = icon('plus');
            plus.title = 'A new colour, the chosen one to start from';
            plus.onclick = () => this.add();
            this.grid.append(plus);
        }
        this.same = [...seen.values()].filter((v) => v.length > 1);
        this.drawPanel();
        this.root.focus();
    }

    /* a press chooses a swatch; dragged onto another, it moves there and
     * those between shift along */
    drag(e, i) {
        e.preventDefault();
        this.at = i;
        const swatches = [...this.grid.querySelectorAll('.pal-swatch:not(.pal-add)')];
        swatches.forEach((s, k) => s.classList.toggle('on', k === i));
        this.drawPanel();
        this.root.focus();
        let over = i;
        const under = (ev) => {
            const el = document.elementFromPoint(ev.clientX, ev.clientY);
            const k = swatches.indexOf(el);
            return k < 0 ? over : k;
        };
        const move = (ev) => {
            over = under(ev);
            swatches.forEach((s, k) => s.classList.toggle('drop', k === over && k !== i));
        };
        const up = async (ev) => {
            document.removeEventListener('pointermove', move);
            document.removeEventListener('pointerup', up);
            swatches.forEach((s) => s.classList.remove('drop'));
            const to = under(ev);
            if (to !== i) await this.moveTo(i, to);
        };
        document.addEventListener('pointermove', move);
        document.addEventListener('pointerup', up);
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
        hexIn.className = 'pal-hex';
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
            const n = document.createElement('input');
            n.type = 'number';
            n.min = 0;
            n.max = 255;
            n.value = c >> shift & 255;
            r.oninput = () => {
                n.value = r.value;
                this.set(this.rgb[this.at] & ~(255 << shift) | +r.value << shift, true);
            };
            r.onchange = () => this.draw();
            n.onchange = () => {
                const v = Math.max(0, Math.min(255, Math.round(+n.value) || 0));
                this.set(this.rgb[this.at] & ~(255 << shift) | v << shift);
            };
            row.append(name, r, n);
            fields.append(row);
        });
        const note = document.createElement('div');
        note.className = 'pal-note';
        const twins = this.same.find((v) => v.includes(i));
        const others = twins ? twins.filter((k) => k !== i) : [];
        note.textContent = (this.uses[i] ? this.uses[i] + ' pixels use it.' : 'No picture uses it.') +
            (others.length ? ' On the iPod it looks the same as colour ' + others.join(' and ') + '.' : '');
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

    /* entry i to place j, those between shifting along */
    async moveTo(i, j) {
        const order = this.rgb.map((_, k) => k);
        order.splice(j, 0, order.splice(i, 1)[0]);
        if (await this.reorder(order, 'Colour ' + i + ' is now colour ' + j + '.')) {
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
}
