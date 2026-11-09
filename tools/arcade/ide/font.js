/* The font editor: a BDF font's characters as a grid, the one chosen
 * drawn on in the pixel editor -- its cell the font's line tall and its
 * advance wide, the baseline marked -- and a line of text shown in the
 * font as it changes. Characters are added and deleted; the ascent,
 * descent and each advance are set by number. The file is written back
 * as BDF, its header kept, each glyph trimmed to its ink.
 *
 * Parts: reading and writing BDF; the editor. */

import { PixelEditor } from './pixels.js';
import { icon, iconButton } from './icons.js';
import { sidePanel, toolbar, numberBox } from './chrome.js';

/* ---- reading and writing BDF ---- */

/* A code point as U+XXXX */
const uplus = (cp) => 'U+' + cp.toString(16).toUpperCase().padStart(4, '0');

/* { head: the lines before CHARS, ascent, descent, glyphs: [{ cp, name,
 * adv, cell: { w, px } }] }: each glyph as a cell of 0 and 1, the line
 * tall, its baseline ascent rows down */
export function readFont(text) {
    const lines = text.split(/\r?\n/);
    const head = [];
    let ascent = 0, descent = 0, i = 0;
    for (; i < lines.length && !/^CHARS\b/.test(lines[i]); i++) {
        head.push(lines[i]);
        const p = lines[i].trim().split(/\s+/);
        if (p[0] === 'FONT_ASCENT') ascent = +p[1];
        if (p[0] === 'FONT_DESCENT') descent = +p[1];
    }
    const glyphs = [];
    let g = null, rows = null;
    for (; i < lines.length; i++) {
        const p = lines[i].trim().split(/\s+/);
        if (rows) {
            if (p[0] === 'ENDCHAR') {
                if (g.cp >= 0) glyphs.push(toCell(g, rows, ascent, descent));
                g = rows = null;
            } else rows.push(p[0]);
            continue;
        }
        if (p[0] === 'STARTCHAR') g = { name: p.slice(1).join(' '), cp: -1, adv: 0, bbx: [0, 0, 0, 0] };
        else if (g && p[0] === 'ENCODING') g.cp = +p[1];
        else if (g && p[0] === 'DWIDTH') g.adv = +p[1];
        else if (g && p[0] === 'BBX') g.bbx = p.slice(1, 5).map(Number);
        else if (g && p[0] === 'BITMAP') rows = [];
    }
    return { head, ascent, descent, glyphs: glyphs.sort((a, b) => a.cp - b.cp) };
}

/* a glyph's bitmap placed in its cell */
function toCell(g, rows, ascent, descent) {
    const [w, h, xo, yo] = g.bbx;
    const cw = Math.max(1, g.adv, xo + w), line = ascent + descent;
    const px = new Uint8Array(cw * line);
    const top = ascent - (yo + h);
    rows.slice(0, h).forEach((hex, r) => {
        const bits = BigInt('0x' + (hex || '0'));
        const nb = hex.length * 4;
        for (let c = 0; c < w; c++) {
            if (!((bits >> BigInt(nb - 1 - c)) & 1n)) continue;
            const x = xo + c, y = top + r;
            if (x >= 0 && x < cw && y >= 0 && y < line) px[y * cw + x] = 1;
        }
    });
    return { cp: g.cp, name: g.name, adv: g.adv, cell: { w: cw, px } };
}

export function writeFont(f) {
    const line = f.ascent + f.descent;
    const out = [];
    const boxes = f.glyphs.map((g) => trim(g, f.ascent));
    let bw = 0, bh = 0, bx = 0, by = 0;
    for (const b of boxes) {
        bw = Math.max(bw, b.w); bh = Math.max(bh, b.h); bx = Math.min(bx, b.xo); by = Math.min(by, b.yo);
    }
    for (const l of f.head) {
        const k = l.trim().split(/\s+/)[0];
        if (k === 'FONT_ASCENT') out.push('FONT_ASCENT ' + f.ascent);
        else if (k === 'FONT_DESCENT') out.push('FONT_DESCENT ' + f.descent);
        else if (k === 'FONTBOUNDINGBOX') out.push('FONTBOUNDINGBOX ' + [bw, bh, bx, by].join(' '));
        else out.push(l);
    }
    out.push('CHARS ' + f.glyphs.length);
    f.glyphs.forEach((g, i) => {
        const b = boxes[i];
        out.push('STARTCHAR ' + (g.name || 'uni' + g.cp.toString(16).toUpperCase().padStart(4, '0')),
                 'ENCODING ' + g.cp, 'SWIDTH ' + Math.round(g.adv * 1000 / Math.max(1, line)) + ' 0',
                 'DWIDTH ' + g.adv + ' 0', 'BBX ' + [b.w, b.h, b.xo, b.yo].join(' '), 'BITMAP');
        for (const r of b.rows) out.push(r);
        out.push('ENDCHAR');
    });
    out.push('ENDFONT', '');
    return out.join('\n');
}

/* a glyph's ink: its box and its rows in hex */
function trim(g, ascent) {
    const { w, px } = g.cell, h = px.length / w;
    let x0 = w, x1 = -1, y0 = h, y1 = -1;
    for (let y = 0; y < h; y++)
        for (let x = 0; x < w; x++)
            if (px[y * w + x]) { x0 = Math.min(x0, x); x1 = Math.max(x1, x); y0 = Math.min(y0, y); y1 = Math.max(y1, y); }
    if (x1 < 0) return { w: 0, h: 0, xo: 0, yo: 0, rows: [] };
    const bw = x1 - x0 + 1, bh = y1 - y0 + 1, nb = Math.ceil(bw / 8);
    const rows = [];
    for (let y = y0; y <= y1; y++) {
        let v = 0n;
        for (let x = x0; x <= x1; x++) if (px[y * w + x]) v |= 1n << BigInt(nb * 8 - 1 - (x - x0));
        rows.push(v.toString(16).toUpperCase().padStart(nb * 2, '0'));
    }
    return { w: bw, h: bh, xo: x0, yo: ascent - (y1 + 1), rows };
}

/* ---- the editor ---- */

const INK = [0x16161c, 0xf2efe6];
const label = (cp) => cp < 32 ? uplus(cp) : String.fromCodePoint(cp);

export class FontEditor {
    /* opts: text (the BDF), changed(text), ask (the IDE's dialog),
     * say(text, error) */
    constructor(parent, opts) {
        this.o = opts;
        this.f = readFont(opts.text);
        this.at = this.f.glyphs.findIndex((g) => g.cp === 65);
        if (this.at < 0) this.at = 0;
        this.sample = 'The quick brown fox, 0123';
        const root = this.root = document.createElement('div');
        root.className = 'font';
        this.side = sidePanel('Glyphs');
        const bar = toolbar();
        bar.put(numberBox('Ascent', this.f.ascent, 0, 32, 'Rows above the baseline',
                          (v) => this.lines(v, this.f.descent)),
                numberBox('Descent', this.f.descent, 0, 32, 'Rows below it',
                          (v) => this.lines(this.f.ascent, v)));
        const body = document.createElement('div');
        body.className = 'font-body';
        this.grid = document.createElement('div');
        this.grid.className = 'font-grid';
        this.preview = document.createElement('canvas');
        this.preview.className = 'font-preview';
        const sample = document.createElement('input');
        sample.className = 'font-sample';
        sample.value = this.sample;
        sample.title = 'Text shown in the font';
        sample.oninput = () => { this.sample = sample.value; this.drawPreview(); };
        body.append(this.grid, sample, this.preview);
        this.side.body.append(bar.el, body);
        this.edit = document.createElement('div');
        this.edit.className = 'font-edit';
        root.append(this.edit);
        parent.append(root);
        this.drawGrid();
        this.open();
    }

    destroy() {
        if (this.editor) this.editor.destroy();
        this.root.remove();
    }

    focus() { if (this.editor) this.editor.root.focus(); }

    save() {
        this.o.changed(writeFont(this.f));
        this.drawPreview();
    }

    /* the line changed: every cell taller or shorter, the baseline kept */
    lines(ascent, descent) {
        if (ascent + descent < 1) return;
        const da = ascent - this.f.ascent;
        for (const g of this.f.glyphs) {
            const { w, px } = g.cell, h = px.length / w, nh = ascent + descent;
            const out = new Uint8Array(w * nh);
            for (let y = 0; y < h; y++) {
                const ny = y + da;
                if (ny >= 0 && ny < nh) out.set(px.subarray(y * w, (y + 1) * w), ny * w);
            }
            g.cell = { w, px: out };
        }
        this.f.ascent = ascent;
        this.f.descent = descent;
        this.save();
        this.drawGrid();
        this.open();
    }

    drawGrid() {
        this.grid.innerHTML = '';
        this.f.glyphs.forEach((g, i) => {
            const b = document.createElement('button');
            b.className = 'font-glyph';
            b.classList.toggle('on', i === this.at);
            b.title = label(g.cp) + ' (' + uplus(g.cp) + ')';
            const c = document.createElement('canvas');
            this.paint(c, [g], 2);
            const s = document.createElement('span');
            s.textContent = label(g.cp);
            b.append(c, s);
            b.onclick = () => { this.at = i; this.drawGrid(); this.open(); };
            this.grid.append(b);
        });
        const add = document.createElement('button');
        add.className = 'font-glyph font-add';
        add.innerHTML = icon('plus');
        add.title = 'Add a character';
        add.onclick = () => this.add();
        this.grid.append(add);
        this.drawPreview();
    }

    /* glyphs side by side on a canvas, k pixels to a dot */
    paint(canvas, glyphs, k) {
        const line = this.f.ascent + this.f.descent;
        const width = Math.max(1, glyphs.reduce((s, g) => s + g.adv, 0));
        canvas.width = width * k;
        canvas.height = line * k;
        const g2 = canvas.getContext('2d');
        g2.fillStyle = '#16161c';
        g2.fillRect(0, 0, canvas.width, canvas.height);
        g2.fillStyle = '#f2efe6';
        let x0 = 0;
        for (const g of glyphs) {
            const { w, px } = g.cell;
            for (let i = 0; i < px.length; i++)
                if (px[i]) g2.fillRect((x0 + i % w) * k, Math.floor(i / w) * k, k, k);
            x0 += g.adv;
        }
    }

    drawPreview() {
        const by = new Map(this.f.glyphs.map((g) => [g.cp, g]));
        const q = by.get(63);
        const glyphs = [...this.sample].map((ch) => by.get(ch.codePointAt(0)) || q).filter(Boolean);
        this.paint(this.preview, glyphs, 2);
    }

    open() {
        const g = this.f.glyphs[this.at];
        if (this.editor) { this.editor.destroy(); this.editor = null; }
        this.edit.innerHTML = '';
        if (!g) return;
        const host = document.createElement('div');
        host.className = 'font-host';
        this.edit.append(host);
        this.editor = new PixelEditor(host, {
            width: g.cell.w, height: this.f.ascent + this.f.descent, px: g.cell.px.slice(), rgb: INK,
            baseline: this.f.ascent, was: this.was,
            changed: (px) => {
                g.cell.px = px.slice();
                this.save();
                const tile = this.grid.children[this.at];
                if (tile) this.paint(tile.querySelector('canvas'), [g], 2);
            },
            say: this.o.say
        });
        this.was = this.editor.state;
        /* the character's own controls on the end of the drawing toolbar */
        const bar = this.editor.bar, sep = document.createElement('span');
        sep.className = 'sep';
        const name = document.createElement('span');
        name.className = 'note';
        name.textContent = label(g.cp) + '  ' + uplus(g.cp);
        bar.append(sep, numberBox('Advance', g.adv, 0, 32, 'How far the text moves on after it',
                                  (v) => this.advance(g, v)),
                   iconButton('trash', 'Remove this character from the font', () => this.remove()), name);
    }

    /* the advance set: the cell widened to it, or narrowed to it where no
     * ink is lost */
    advance(g, adv) {
        const { w, px } = g.cell, h = px.length / w;
        let ink = 0;
        for (let i = 0; i < px.length; i++) if (px[i]) ink = Math.max(ink, i % w + 1);
        const nw = Math.max(1, adv, ink), out = new Uint8Array(nw * h);
        for (let y = 0; y < h; y++)
            for (let x = 0; x < Math.min(w, nw); x++) out[y * nw + x] = px[y * w + x];
        g.adv = adv;
        g.cell = { w: nw, px: out };
        this.save();
        this.drawGrid();
        this.open();
    }

    async add() {
        const a = await this.o.ask('Add a character', [{ name: 'ch', label: 'Character',
            note: 'Type it, or its code as U+00E9' }], 'Add it');
        if (!a || !a.ch) return;
        const m = /^U\+([0-9a-f]{1,6})$/i.exec(a.ch.trim());
        const cp = m ? parseInt(m[1], 16) : a.ch.codePointAt(0);
        const had = this.f.glyphs.findIndex((g) => g.cp === cp);
        if (had >= 0) { this.at = had; this.drawGrid(); this.open(); return; }
        const line = this.f.ascent + this.f.descent;
        /* as wide as the font's own capital A, or half the line */
        const like = this.f.glyphs.find((g) => g.cp === 65);
        const adv = like ? like.adv : Math.max(1, Math.round(line / 2));
        this.f.glyphs.push({ cp, name: '', adv, cell: { w: adv, px: new Uint8Array(adv * line) } });
        this.f.glyphs.sort((p, q) => p.cp - q.cp);
        this.at = this.f.glyphs.findIndex((g) => g.cp === cp);
        this.save();
        this.drawGrid();
        this.open();
    }

    async remove() {
        const g = this.f.glyphs[this.at];
        if (!g || await this.o.ask('Delete ' + label(g.cp) + '?', [], 'Delete') === null) return;
        this.f.glyphs.splice(this.at, 1);
        this.at = Math.min(this.at, this.f.glyphs.length - 1);
        this.save();
        this.drawGrid();
        this.open();
    }
}
