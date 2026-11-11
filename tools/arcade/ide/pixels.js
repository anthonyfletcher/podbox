/* The pixel editor: a picture of palette numbers -- one of the cart's
 * images, or a block of the sheet's sprites -- drawn on, a pixel at a time
 * or by shape, with undo. The sheet, or an image edited as sprites, is a
 * picker beside it (SheetEditor): a block of cells chosen there opens here.
 *
 * Parts: the picture and its palette; drawing it, zoomed, with the grids,
 * the selection and anything floating; the tools; the selection, and
 * copying and pasting, within the IDE and with other programs; a new size,
 * by the canvas or by cropping; the pointer, scrolling the view while a
 * drag runs past its edge, and the keys; the sheet, its picker and its
 * sprites' flags.
 *
 * A picture pasted from another program has its colours matched to the
 * palette: exactly where a colour is in it, else to the nearest, and the
 * editor says how many were approximated. Copied out, the selection goes
 * as a PNG for other programs and as its palette numbers for the IDE, so
 * pasting back here is exact. */

import { iconButton, toolCursor } from './icons.js';
import { sidePanel, toolbar, numberBox, dropdown } from './chrome.js';

const MARK = 'arcade-pixels ';
const MAX_UNDO = 100;

/* The colour the player shows for an entry: RGB565, widened again */
export function shown(c) {
    const r = (c >> 16) & 0xf8, g = (c >> 8) & 0xfc, b = c & 0xf8;
    return (r | r >> 5) << 16 | (g | g >> 6) << 8 | (b | b >> 5);
}

/* A colour as #rrggbb */
export const hex = (c) => '#' + c.toString(16).padStart(6, '0');

/* the palette entry nearest a colour, weighting the channels as the eye
 * does (the "redmean" distance) */
function nearest(rgb, r, g, b) {
    let best = 0, bestD = Infinity;
    for (let i = 0; i < rgb.length; i++) {
        const c = rgb[i], cr = c >> 16 & 255, cg = c >> 8 & 255, cb = c & 255;
        const rm = (r + cr) / 2, dr = r - cr, dg = g - cg, db = b - cb;
        const d = (2 + rm / 256) * dr * dr + 4 * dg * dg + (2 + (255 - rm) / 256) * db * db;
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

/* RGBA pixels as palette numbers: exact colours as they are, transparent
 * ones as 0, the rest to the nearest; and how many of each */
export function toPalette(rgba, rgb) {
    const exact = new Map();
    rgb.forEach((c, i) => { if (!exact.has(c)) exact.set(c, i); });
    const shownExact = new Map();
    rgb.forEach((c, i) => { if (!shownExact.has(shown(c))) shownExact.set(shown(c), i); });
    const px = new Uint8Array(rgba.length / 4);
    let near = 0;
    for (let i = 0; i < px.length; i++) {
        const r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
        if (rgba[i * 4 + 3] < 128) { px[i] = 0; continue; }
        const c = r << 16 | g << 8 | b;
        if (exact.has(c)) px[i] = exact.get(c);
        else if (shownExact.has(c)) px[i] = shownExact.get(c);
        else { px[i] = nearest(rgb, r, g, b); near++; }
    }
    return { px, near };
}

export class PixelEditor {
    /* opts: width, height, px (Uint8Array), rgb (the palette, 0xRRGGBB),
     * grid (lines every so many pixels, 8 for sprites), baseline (a row
     * to mark the top of, a font's baseline under it), fit (the largest
     * the picture is shown, in pixels), was (another editor whose tool,
     * colours and mirror carry on), changed(px), tooBig(picture) (a
     * pasted picture wider or taller than this one: true when handled),
     * fitColours(rgba, w, h) (a picture from elsewhere as palette numbers, asking
     * how where its colours are not in the palette; a Promise, of null if
     * not wanted), say(text, error); for a picture that can change size,
     * resize(w, h, px) (the picture made again at that size) and askSize(w,
     * h) (a Promise of { w, h, ax, ay }: the new size, and where the picture
     * sits in it across and down, 0 the start, 1 the middle, 2 the end; or
     * null) */
    constructor(parent, opts) {
        this.o = opts;
        this.w = opts.width;
        this.h = opts.height;
        this.px = opts.px;
        this.rgb = opts.rgb;
        const was = opts.was;
        this.colour = was ? was.colour.slice() : [Math.min(7, this.rgb.length - 1), 0];
        this.tool = was ? was.tool : 'pencil';
        /* as large as the room it has, but whole pixels */
        const fit = opts.fit || Math.max(128, Math.min(parent.clientWidth - 48, parent.clientHeight - 150));
        this.zoom = Math.max(1, Math.min(32, Math.floor(fit / Math.max(this.w, this.h))));
        this.undo = [];
        this.redo = [];
        this.sel = null;          /* { x, y, w, h } */
        this.float = null;        /* { x, y, w, h, px } over the picture */
        this.mirror = was ? was.mirror : false;
        this.tile = was ? was.tile : false;
        this.onion = null;          /* another frame, shown faintly under */
        this.build(parent);
        this.render();
    }

    /* ---- the page ---------------------------------------------------- */

    build(parent) {
        const root = this.root = document.createElement('div');
        root.className = 'pix';
        root.tabIndex = 0;
        const tools = [['select', 'select', 'Select (S): drag to choose, drag inside to move'],
                       ['pencil', 'pencil', 'Pencil (P)'], ['line', 'line', 'Line (L)'],
                       ['rect', 'rect', 'Rectangle (R); Shift fills it'], ['oval', 'oval', 'Oval (O); Shift fills it'],
                       ['fill', 'fill', 'Fill (F)'],
                       ['replace', 'recolour', 'Replace (C): click a colour to make all of it, in the selection or ' +
                        'the whole picture, the drawing colour; right click for the second colour'],
                       ['pick', 'pick', 'Colour picker (I); Alt+click picks with any tool']];
        const bar = this.bar = document.createElement('div');
        bar.className = 'tb';
        this.toolButtons = {};
        for (const [id, ic, title] of tools) {
            const b = iconButton(ic, title, () => this.use(id));
            this.toolButtons[id] = b;
            bar.append(b);
        }
        const sep = () => { const s = document.createElement('span'); s.className = 'sep'; bar.append(s); };
        sep();
        const act = (ic, title, fn) => {
            const b = iconButton(ic, title, () => { fn(); this.root.focus(); });
            bar.append(b);
            return b;
        };
        this.mirrorButton = act('mirror', 'Mirror: draw both halves at once (M)', () => this.toggleMirror());
        act('flipH', 'Flip across (H)', () => this.flip(true));
        act('flipV', 'Flip down (V)', () => this.flip(false));
        act('turn', 'Turn a quarter clockwise (T)', () => this.turn());
        act('invert', 'Invert: each colour to the one nearest its opposite; 0 stays clear (N)', () => this.invert());
        sep();
        this.tileButton = act('tile', 'Tile: shown repeated round itself, drawing wrapping at the edges (W)',
                              () => this.toggleTile());
        sep();
        if (this.o.resize) {
            act('canvas', 'Canvas size: make the picture bigger or smaller, keeping what is drawn',
                () => this.canvasSize());
            act('crop', 'Crop to the selection', () => this.crop());
            sep();
        }
        act('zoomOut', 'Zoom out (-)', () => this.setZoom(this.zoom - 1));
        act('zoomIn', 'Zoom in (+)', () => this.setZoom(this.zoom + 1));
        root.append(bar);

        const area = this.area = document.createElement('div');
        area.className = 'pix-area';
        const stack = this.stack = document.createElement('div');
        stack.className = 'pix-stack';
        this.canvas = document.createElement('canvas');
        this.over = document.createElement('canvas');
        this.over.className = 'pix-over';
        stack.append(this.canvas, this.over);
        area.append(stack);
        root.append(area);

        this.status = document.createElement('div');
        this.status.className = 'pix-status';
        root.append(this.status);

        this.swatches = document.createElement('div');
        this.swatches.className = 'pix-palette';
        root.append(this.swatches);
        parent.append(root);

        this.image = new ImageData(this.w, this.h);
        this.listen();
        this.use(this.tool);
        this.mirrorButton.classList.toggle('on', this.mirror);
        this.tileButton.classList.toggle('on', this.tile);
        this.drawPalette();
        this.setZoom(this.zoom);
    }

    destroy() {
        document.removeEventListener('paste', this.onPaste);
        this.root.remove();
    }

    /* copies drawn each side of the picture: one in tile mode */
    get off() { return this.tile ? 1 : 0; }

    setZoom(z) {
        this.zoom = Math.max(1, Math.min(32, z));
        const n = 1 + 2 * this.off;
        for (const c of [this.canvas, this.over]) {
            c.width = this.w * this.zoom * n;
            c.height = this.h * this.zoom * n;
        }
        this.render();
    }

    toggleTile() {
        this.tile = !this.tile;
        this.tileButton.classList.toggle('on', this.tile);
        this.setZoom(this.tile ? Math.max(1, Math.floor(this.zoom / 2)) : this.zoom * 2);
    }

    /* another frame drawn faintly where this one is colour 0, or null */
    setOnion(px) {
        this.onion = px;
        this.overlay();
    }

    use(tool) {
        if (tool !== 'select') this.drop();
        this.tool = tool;
        for (const [id, b] of Object.entries(this.toolButtons)) b.classList.toggle('on', id === tool);
        this.cursor(null, false);
        this.root.focus();
    }

    /* the cursor says what a click there does: the tool's icon, the picker
     * while Alt is held, and moving over a selection */
    cursor(p, alt) {
        const box = this.float || this.sel;
        let c = toolCursor(alt ? 'pick' : this.tool);
        if (!alt && this.tool === 'select' && p && box &&
            p.x >= box.x && p.y >= box.y && p.x < box.x + box.w && p.y < box.y + box.h) c = 'move';
        this.over.style.cursor = c;
        this.lastPoint = p;
    }

    toggleMirror() {
        this.mirror = !this.mirror;
        this.mirrorButton.classList.toggle('on', this.mirror);
    }

    drawPalette() {
        this.swatches.innerHTML = '';
        this.rgb.forEach((c, i) => {
            const s = document.createElement('button');
            s.className = 'swatch';
            s.style.background = hex(shown(c));
            s.title = i + ': ' + hex(c) +
                ' (left click draws with it, right click is the second colour)';
            s.classList.toggle('first', i === this.colour[0]);
            s.classList.toggle('second', i === this.colour[1]);
            s.onclick = () => { this.colour[0] = i; this.drawPalette(); };
            s.oncontextmenu = (e) => { e.preventDefault(); this.colour[1] = i; this.drawPalette(); };
            this.swatches.append(s);
        });
    }

    /* ---- drawing it -------------------------------------------------- */

    render() {
        const d = this.image.data;
        for (let i = 0; i < this.px.length; i++) {
            const c = shown(this.rgb[this.px[i]] || 0);
            d[i * 4] = c >> 16; d[i * 4 + 1] = c >> 8 & 255; d[i * 4 + 2] = c & 255; d[i * 4 + 3] = 255;
        }
        const tmp = this.tmp || (this.tmp = document.createElement('canvas'));
        tmp.width = this.w;
        tmp.height = this.h;
        tmp.getContext('2d').putImageData(this.image, 0, 0);
        const g = this.canvas.getContext('2d'), o = this.off, W = this.w * this.zoom, H = this.h * this.zoom;
        g.imageSmoothingEnabled = false;
        for (let j = -o; j <= o; j++)
            for (let i = -o; i <= o; i++) g.drawImage(tmp, (i + o) * W, (j + o) * H, W, H);
        this.overlay();
    }

    /* the grids, the selection, a floating picture and a shape's preview */
    overlay(preview) {
        const g = this.over.getContext('2d'), z = this.zoom;
        g.setTransform(1, 0, 0, 1, 0, 0);
        g.clearRect(0, 0, this.over.width, this.over.height);
        if (this.off) {
            /* the copies dimmed, the picture itself clear */
            g.fillStyle = 'rgba(0,0,0,0.4)';
            g.fillRect(0, 0, this.over.width, this.over.height);
            g.clearRect(this.w * z, this.h * z, this.w * z, this.h * z);
            g.translate(this.w * z, this.h * z);
        }
        if (this.onion) {
            /* faintly over the picture, wherever the other frame differs */
            g.globalAlpha = 0.45;
            for (let y = 0; y < this.h; y++)
                for (let x = 0; x < this.w; x++) {
                    const v = this.onion[y * this.w + x];
                    if (!v || v === this.px[y * this.w + x]) continue;
                    g.fillStyle = hex(shown(this.rgb[v] || 0));
                    g.fillRect(x * z, y * z, z, z);
                }
            g.globalAlpha = 1;
        }
        if (z >= 6) {
            g.strokeStyle = 'rgba(128,128,128,0.25)';
            g.beginPath();
            for (let x = 1; x < this.w; x++) { g.moveTo(x * z + 0.5, 0); g.lineTo(x * z + 0.5, this.h * z); }
            for (let y = 1; y < this.h; y++) { g.moveTo(0, y * z + 0.5); g.lineTo(this.w * z, y * z + 0.5); }
            g.stroke();
        }
        const grid = this.o.grid;
        if (grid) {
            g.strokeStyle = 'rgba(128,128,128,0.7)';
            g.beginPath();
            for (let x = grid; x < this.w; x += grid) { g.moveTo(x * z + 0.5, 0); g.lineTo(x * z + 0.5, this.h * z); }
            for (let y = grid; y < this.h; y += grid) { g.moveTo(0, y * z + 0.5); g.lineTo(this.w * z, y * z + 0.5); }
            g.stroke();
        }
        if (this.o.baseline != null) {
            g.strokeStyle = 'rgba(110,160,255,0.85)';
            g.lineWidth = 2;
            g.beginPath();
            g.moveTo(0, this.o.baseline * z);
            g.lineTo(this.w * z, this.o.baseline * z);
            g.stroke();
            g.lineWidth = 1;
        }
        const pixel = (x, y, c) => {
            const s = shown(this.rgb[c] || 0);
            g.fillStyle = hex(s);
            g.fillRect(x * z, y * z, z, z);
        };
        if (this.float) {
            const f = this.float;
            for (let y = 0; y < f.h; y++)
                for (let x = 0; x < f.w; x++) pixel(f.x + x, f.y + y, f.px[y * f.w + x]);
        }
        if (preview) for (const [x, y, c] of preview) pixel(x, y, c);
        const box = this.float || this.sel;
        if (box) {
            g.setLineDash([4, 3]);
            g.strokeStyle = '#ffffff';
            g.strokeRect(box.x * z + 0.5, box.y * z + 0.5, box.w * z - 1, box.h * z - 1);
            g.strokeStyle = '#000000';
            g.lineDashOffset = 3;
            g.strokeRect(box.x * z + 0.5, box.y * z + 0.5, box.w * z - 1, box.h * z - 1);
            g.setLineDash([]);
            g.lineDashOffset = 0;
        }
    }

    /* ---- changing it ------------------------------------------------- */

    keep() {
        this.undo.push(this.px.slice());
        if (this.undo.length > MAX_UNDO) this.undo.shift();
        this.redo = [];
    }

    changed() {
        this.render();
        this.o.changed(this.px);
    }

    undoOne(back) {
        const from = back ? this.undo : this.redo, to = back ? this.redo : this.undo;
        if (!from.length) return;
        to.push(this.px.slice());
        this.px.set(from.pop());
        this.float = null;
        this.changed();
    }

    put(x, y, c) {
        if (this.tile) { x = ((x % this.w) + this.w) % this.w; y = ((y % this.h) + this.h) % this.h; }
        if (x < 0 || y < 0 || x >= this.w || y >= this.h) return;
        this.px[y * this.w + x] = c;
        if (this.mirror) this.px[y * this.w + this.w - 1 - x] = c;
    }

    /* the picture's pixel under the pointer: past its edges in tile mode */
    at(e) {
        const r = this.canvas.getBoundingClientRect();
        return { x: Math.floor((e.clientX - r.left) / this.zoom) - this.off * this.w,
                 y: Math.floor((e.clientY - r.top) / this.zoom) - this.off * this.h };
    }

    /* a pixel past the edges, brought back in tile mode */
    wrap(p) {
        if (!this.tile) return p;
        return { x: ((p.x % this.w) + this.w) % this.w, y: ((p.y % this.h) + this.h) % this.h };
    }

    /* the pixels of a shape from a to b: a line, or a rectangle's or an
     * oval's outline or fill */
    shape(kind, a, b, filled) {
        const out = [];
        if (kind === 'line') {
            let x = a.x, y = a.y;
            const dx = Math.abs(b.x - x), dy = -Math.abs(b.y - y), sx = x < b.x ? 1 : -1, sy = y < b.y ? 1 : -1;
            let err = dx + dy;
            for (;;) {
                out.push([x, y]);
                if (x === b.x && y === b.y) break;
                const e2 = 2 * err;
                if (e2 >= dy) { err += dy; x += sx; }
                if (e2 <= dx) { err += dx; y += sy; }
            }
            return out;
        }
        const x0 = Math.min(a.x, b.x), x1 = Math.max(a.x, b.x), y0 = Math.min(a.y, b.y), y1 = Math.max(a.y, b.y);
        if (kind === 'rect') {
            for (let y = y0; y <= y1; y++)
                for (let x = x0; x <= x1; x++)
                    if (filled || x === x0 || x === x1 || y === y0 || y === y1) out.push([x, y]);
            return out;
        }
        /* the oval inside the box: each row's span, and the outline as the
         * pixels of the filled shape with an edge outside it */
        const cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = (x1 - x0) / 2 + 0.5, ry = (y1 - y0) / 2 + 0.5;
        const inside = (x, y) => ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1;
        for (let y = y0; y <= y1; y++)
            for (let x = x0; x <= x1; x++)
                if (inside(x, y) && (filled || !inside(x - 1, y) || !inside(x + 1, y) ||
                                     !inside(x, y - 1) || !inside(x, y + 1)))
                    out.push([x, y]);
        return out;
    }

    /* every pixel of colour from, in the selection or else the whole
     * picture, made colour to; how many */
    replace(from, to) {
        const b = this.sel || { x: 0, y: 0, w: this.w, h: this.h };
        let n = 0;
        for (let y = b.y; y < b.y + b.h; y++)
            for (let x = b.x; x < b.x + b.w; x++)
                if (this.px[y * this.w + x] === from) { this.px[y * this.w + x] = to; n++; }
        return n;
    }

    flood(x, y, c) {
        if (x < 0 || y < 0 || x >= this.w || y >= this.h) return;
        const from = this.px[y * this.w + x];
        if (from === c) return;
        const stack = [[x, y]];
        while (stack.length) {
            const [px, py] = stack.pop();
            if (px < 0 || py < 0 || px >= this.w || py >= this.h || this.px[py * this.w + px] !== from) continue;
            this.px[py * this.w + px] = c;
            stack.push([px + 1, py], [px - 1, py], [px, py + 1], [px, py - 1]);
        }
    }

    /* ---- the selection, and the clipboard ---------------------------- */

    /* the pixels under a box */
    grab(b) {
        const out = new Uint8Array(b.w * b.h);
        for (let y = 0; y < b.h; y++)
            for (let x = 0; x < b.w; x++) out[y * b.w + x] = this.px[(b.y + y) * this.w + b.x + x];
        return out;
    }

    /* a floating picture written down where it is */
    drop() {
        const f = this.float;
        if (!f) return;
        this.float = null;
        for (let y = 0; y < f.h; y++)
            for (let x = 0; x < f.w; x++) {
                const tx = f.x + x, ty = f.y + y;
                if (tx >= 0 && ty >= 0 && tx < this.w && ty < this.h) this.px[ty * this.w + tx] = f.px[y * f.w + x];
            }
        this.sel = { x: f.x, y: f.y, w: f.w, h: f.h };
        this.changed();
    }

    /* the selection lifted off the picture to move, leaving colour 0 */
    lift() {
        if (this.float || !this.sel) return;
        this.keep();
        const s = this.sel;
        this.float = { ...s, px: this.grab(s) };
        for (let y = 0; y < s.h; y++) this.px.fill(0, (s.y + y) * this.w + s.x, (s.y + y) * this.w + s.x + s.w);
        this.sel = null;
    }

    /* what copy takes: the floating picture, the selection, or all */
    copied() {
        if (this.float) return { w: this.float.w, h: this.float.h, px: this.float.px.slice() };
        const b = this.sel || { x: 0, y: 0, w: this.w, h: this.h };
        return { w: b.w, h: b.h, px: this.grab(b) };
    }

    async copy() {
        const c = this.copied();
        PixelEditor.clip = c;
        /* for other programs a PNG; for the IDE, the palette numbers */
        try {
            const cv = document.createElement('canvas');
            cv.width = c.w;
            cv.height = c.h;
            const im = new ImageData(c.w, c.h);
            for (let i = 0; i < c.px.length; i++) {
                const s = shown(this.rgb[c.px[i]] || 0);
                im.data[i * 4] = s >> 16; im.data[i * 4 + 1] = s >> 8 & 255; im.data[i * 4 + 2] = s & 255;
                im.data[i * 4 + 3] = c.px[i] === 0 ? 0 : 255;
            }
            cv.getContext('2d').putImageData(im, 0, 0);
            const png = await new Promise((done) => cv.toBlob(done, 'image/png'));
            const text = new Blob([MARK + JSON.stringify({ w: c.w, h: c.h, px: Array.from(c.px) })],
                                  { type: 'text/plain' });
            await navigator.clipboard.write([new ClipboardItem({ 'image/png': png, 'text/plain': text })]);
            this.o.say('Copied ' + c.w + 'x' + c.h + ' pixels.');
        } catch (e) {
            this.o.say('Copied ' + c.w + 'x' + c.h + ' pixels, for pasting in the IDE: the browser would not ' +
                       'put them on the clipboard for other programs (' + e.message + ').');
        }
    }

    cut() {
        this.copy();
        this.erase();
    }

    erase() {
        if (this.float) { this.float = null; this.render(); return; }
        if (!this.sel) return;
        this.keep();
        const s = this.sel;
        for (let y = 0; y < s.h; y++) this.px.fill(0, (s.y + y) * this.w + s.x, (s.y + y) * this.w + s.x + s.w);
        this.changed();
    }

    /* a picture placed floating at the selection's corner, or where the
     * view is, to be moved and then dropped */
    place(c) {
        if ((c.w > this.w || c.h > this.h) && this.o.tooBig && this.o.tooBig(c)) return;
        this.drop();
        this.keep();
        this.use('select');
        const view = this.area.getBoundingClientRect(), r = this.canvas.getBoundingClientRect();
        const x = this.sel ? this.sel.x : Math.max(0, Math.floor((view.left - r.left) / this.zoom));
        const y = this.sel ? this.sel.y : Math.max(0, Math.floor((view.top - r.top) / this.zoom));
        this.float = { x, y, w: c.w, h: c.h, px: c.px };
        this.sel = null;
        this.render();
    }

    async pasteEvent(e) {
        const data = e.clipboardData;
        const text = data && data.getData('text/plain');
        if (text && text.startsWith(MARK)) {
            e.preventDefault();
            const c = JSON.parse(text.slice(MARK.length));
            this.place({ w: c.w, h: c.h, px: Uint8Array.from(c.px) });
            return;
        }
        const file = data && [...data.items].find((i) => i.kind === 'file' && /^image\//.test(i.type));
        if (file) {
            e.preventDefault();
            await this.pasteImage(file.getAsFile());
            return;
        }
        if (PixelEditor.clip) {
            e.preventDefault();
            this.place(PixelEditor.clip);
        }
    }

    /* a picture from another program: its colours matched to the palette */
    async pasteImage(blob) {
        let bmp;
        try { bmp = await createImageBitmap(blob); } catch (e) {
            this.o.say('That picture could not be read: ' + e.message, true);
            return;
        }
        const cv = document.createElement('canvas');
        cv.width = bmp.width;
        cv.height = bmp.height;
        const g = cv.getContext('2d');
        g.drawImage(bmp, 0, 0);
        const rgba = g.getImageData(0, 0, bmp.width, bmp.height).data;
        const px = this.o.fitColours ? await this.o.fitColours(rgba, bmp.width, bmp.height) : toPalette(rgba, this.rgb).px;
        if (!px) return;
        this.place({ w: bmp.width, h: bmp.height, px });
        this.o.say('Pasted ' + bmp.width + 'x' + bmp.height + ' pixels. Move it, then press Enter ' +
                   'or choose another tool to place it.');
    }

    /* the palette changed: colours added, or set */
    setPalette(rgb) {
        this.rgb = rgb;
        this.drawPalette();
        this.render();
    }

    /* ---- turning and flipping: the floating picture, the selection, or all */

    target() {
        if (this.sel && !this.float) this.lift();
        if (this.float) return this.float;
        this.keep();
        return { x: 0, y: 0, w: this.w, h: this.h, px: this.px, whole: true };
    }

    flip(across) {
        const t = this.target(), out = new Uint8Array(t.px.length);
        for (let y = 0; y < t.h; y++)
            for (let x = 0; x < t.w; x++)
                out[y * t.w + x] = t.px[across ? y * t.w + (t.w - 1 - x) : (t.h - 1 - y) * t.w + x];
        t.px.set(out);
        if (t.whole) this.changed(); else this.render();
    }

    /* each colour to the palette's nearest to its opposite, colour 0
     * left clear and never chosen */
    invert() {
        const t = this.target(), to = new Uint8Array(256);
        for (let v = 1; v < this.rgb.length; v++) {
            const c = this.rgb[v];
            to[v] = 1 + nearest(this.rgb.slice(1), 255 - (c >> 16 & 255), 255 - (c >> 8 & 255), 255 - (c & 255));
        }
        for (let i = 0; i < t.px.length; i++) t.px[i] = to[t.px[i]];
        if (t.whole) this.changed(); else this.render();
    }

    turn() {
        const t = this.target();
        if (t.whole && t.w !== t.h) {
            this.undo.pop();
            this.o.say('Only a square picture turns whole; select a part of it to turn.', true);
            return;
        }
        const out = new Uint8Array(t.px.length);
        for (let y = 0; y < t.h; y++)
            for (let x = 0; x < t.w; x++) out[x * t.h + (t.h - 1 - y)] = t.px[y * t.w + x];
        t.px.set(out);
        if (t.whole) { this.changed(); return; }
        [t.w, t.h] = [t.h, t.w];
        this.render();
    }

    /* ---- a new size -------------------------------------------------- */

    /* the canvas made a new size, the picture placed in it where asked:
     * what falls outside is cut off, and new room is colour 0 */
    async canvasSize() {
        const a = await this.o.askSize(this.w, this.h);
        if (!a) return;
        this.drop();
        const ox = [0, (a.w - this.w) >> 1, a.w - this.w][a.ax];
        const oy = [0, (a.h - this.h) >> 1, a.h - this.h][a.ay];
        const out = new Uint8Array(a.w * a.h);
        for (let y = 0; y < this.h; y++)
            for (let x = 0; x < this.w; x++) {
                const tx = x + ox, ty = y + oy;
                if (tx >= 0 && ty >= 0 && tx < a.w && ty < a.h) out[ty * a.w + tx] = this.px[y * this.w + x];
            }
        this.o.resize(a.w, a.h, out);
    }

    /* the picture cut down to the selection */
    crop() {
        this.drop();
        if (!this.sel) {
            this.o.say('Select the part to keep, then crop.', true);
            return;
        }
        const s = this.sel;
        this.o.resize(s.w, s.h, this.grab(s));
    }

    /* ---- the pointer and the keys ------------------------------------ */

    listen() {
        let down = null, last = null, moving = null;
        this.over.addEventListener('contextmenu', (e) => e.preventDefault());
        this.over.addEventListener('pointerdown', (e) => {
            this.root.focus();
            try { this.over.setPointerCapture(e.pointerId); } catch (x) { /* gone */ }
            const p = this.at(e), c = this.colour[e.button === 2 ? 1 : 0];
            down = { p, c, shift: e.shiftKey, moved: false };
            last = p;
            const tool = e.altKey ? 'pick' : this.tool;
            const q = this.wrap(p);
            if (tool === 'pick') {
                const v = this.px[q.y * this.w + q.x];
                if (v !== undefined) { this.colour[e.button === 2 ? 1 : 0] = v; this.drawPalette(); }
                down = null;
            } else if (tool === 'fill') {
                this.keep();
                this.flood(q.x, q.y, c);
                this.changed();
                down = null;
            } else if (tool === 'replace') {
                const from = this.px[q.y * this.w + q.x];
                if (from !== undefined && from !== c) {
                    this.keep();
                    const n = this.replace(from, c);
                    this.changed();
                    this.o.say('Colour ' + from + ' made colour ' + c + ': ' + n + (n === 1 ? ' pixel' : ' pixels') +
                               (this.sel ? ' in the selection.' : '.'));
                }
                down = null;
            } else if (tool === 'pencil') {
                this.keep();
                this.put(p.x, p.y, c);
                this.render();
            } else if (tool === 'select') {
                const box = this.float || this.sel;
                /* lifted to move once the pointer moves, so a click only lets go */
                if (box && p.x >= box.x && p.y >= box.y && p.x < box.x + box.w && p.y < box.y + box.h) {
                    moving = { lazy: true };
                } else {
                    this.drop();
                    this.sel = null;
                    moving = null;
                }
            }
        });
        /* A drag past the view's edge scrolls it, faster the further out,
         * and the drag goes on from where the pointer now is over the
         * picture: the browser does not scroll a view whose pointer is
         * captured. */
        const EDGE = 24;
        let held = null, scrolling = 0;
        const scroll = () => {
            scrolling = 0;
            if (!down || !held) return;
            const r = this.area.getBoundingClientRect();
            const push = (v, lo, hi) => v < lo + EDGE ? -Math.ceil((lo + EDGE - v) / 4)
                                       : v > hi - EDGE ? Math.ceil((v - hi + EDGE) / 4) : 0;
            const dx = push(held.clientX, r.left, r.right), dy = push(held.clientY, r.top, r.bottom);
            const x0 = this.area.scrollLeft, y0 = this.area.scrollTop;
            this.area.scrollLeft += dx;
            this.area.scrollTop += dy;
            if (this.area.scrollLeft === x0 && this.area.scrollTop === y0) return;
            move(held);
            scrolling = requestAnimationFrame(scroll);
        };
        const move = (e) => {
            const p = this.at(e);
            this.status.textContent = this.where(p);
            if (!down) { this.cursor(p, e.altKey); return; }
            if (p.x !== down.p.x || p.y !== down.p.y) down.moved = true;
            const tool = this.tool;
            if (tool === 'pencil') {
                for (const [x, y] of this.shape('line', last, p)) this.put(x, y, down.c);
                last = p;
                this.render();
            } else if (tool === 'line' || tool === 'rect' || tool === 'oval') {
                this.overlay(this.shape(tool, down.p, p, e.shiftKey).map(([x, y]) => [x, y, down.c]));
            } else if (tool === 'select') {
                if (moving && moving.lazy) {
                    this.lift();
                    moving = { dx: down.p.x - this.float.x, dy: down.p.y - this.float.y };
                }
                if (moving) {
                    this.float.x = p.x - moving.dx;
                    this.float.y = p.y - moving.dy;
                } else {
                    const x0 = Math.max(0, Math.min(down.p.x, p.x)), y0 = Math.max(0, Math.min(down.p.y, p.y));
                    const x1 = Math.min(this.w - 1, Math.max(down.p.x, p.x)), y1 = Math.min(this.h - 1, Math.max(down.p.y, p.y));
                    this.sel = { x: x0, y: y0, w: x1 - x0 + 1, h: y1 - y0 + 1 };
                }
                this.overlay();
            }
        };
        this.over.addEventListener('pointermove', (e) => {
            move(e);
            if (!down) return;
            held = { clientX: e.clientX, clientY: e.clientY, shiftKey: e.shiftKey, altKey: e.altKey };
            if (!scrolling) scrolling = requestAnimationFrame(scroll);
        });
        const up = (e) => {
            held = null;
            if (!down) return;
            const p = this.at(e), tool = this.tool;
            if (tool === 'pencil') this.o.changed(this.px);
            /* a click without a drag lets the selection go */
            else if (tool === 'select' && !down.moved) {
                this.drop();
                this.sel = null;
                this.overlay();
            } else if (tool === 'line' || tool === 'rect' || tool === 'oval') {
                this.keep();
                for (const [x, y] of this.shape(tool, down.p, p, e.shiftKey)) this.put(x, y, down.c);
                this.changed();
            }
            down = null;
            moving = null;
        };
        this.over.addEventListener('pointerup', up);
        this.over.addEventListener('pointercancel', up);
        this.area.addEventListener('wheel', (e) => {
            if (!e.ctrlKey) return;
            e.preventDefault();
            this.setZoom(this.zoom + (e.deltaY < 0 ? 1 : -1));
        }, { passive: false });

        this.root.addEventListener('keyup', (e) => { if (e.key === 'Alt') this.cursor(this.lastPoint, false); });
        this.root.addEventListener('keydown', (e) => {
            if (e.key === 'Alt') { this.cursor(this.lastPoint, true); e.preventDefault(); return; }
            const ctrl = e.ctrlKey || e.metaKey, k = e.key.toLowerCase();
            const done = () => { e.preventDefault(); e.stopPropagation(); };
            if (ctrl && k === 'z') { done(); this.drop(); this.undoOne(!e.shiftKey); return; }
            if (ctrl && k === 'y') { done(); this.undoOne(false); return; }
            if (ctrl && k === 'c') { done(); this.copy(); return; }
            if (ctrl && k === 'x') { done(); this.cut(); return; }
            if (ctrl && k === 'a') { done(); this.drop(); this.use('select'); this.sel = { x: 0, y: 0, w: this.w, h: this.h }; this.overlay(); return; }
            if (ctrl) return;   /* Ctrl+V arrives as the paste event */
            const keys = { p: 'pencil', l: 'line', r: 'rect', o: 'oval', f: 'fill', c: 'replace', i: 'pick', s: 'select' };
            if (keys[k]) { done(); this.use(keys[k]); return; }
            if (k === 'm') { done(); this.toggleMirror(); return; }
            if (k === 'h') { done(); this.flip(true); return; }
            if (k === 'v') { done(); this.flip(false); return; }
            if (k === 't') { done(); this.turn(); return; }
            if (k === 'n') { done(); this.invert(); return; }
            if (k === 'w') { done(); this.toggleTile(); return; }
            if (k === '+' || k === '=') { done(); this.setZoom(this.zoom + 1); return; }
            if (k === '-') { done(); this.setZoom(this.zoom - 1); return; }
            if (k === 'delete' || k === 'backspace') { done(); this.erase(); return; }
            if (k === 'enter') { done(); this.drop(); return; }
            if (k === 'escape') {
                done();
                if (this.float) { this.float = null; this.undoOne(true); }
                this.sel = null;
                this.render();
                return;
            }
            const arrows = { arrowleft: [-1, 0], arrowright: [1, 0], arrowup: [0, -1], arrowdown: [0, 1] };
            if (arrows[k] && (this.float || this.sel)) {
                done();
                if (!this.float) this.lift();
                this.float.x += arrows[k][0];
                this.float.y += arrows[k][1];
                this.overlay();
            }
        });
        this.onPaste = (e) => {
            if (this.root.isConnected && (document.activeElement === this.root || this.root.contains(document.activeElement)))
                this.pasteEvent(e);
        };
        document.addEventListener('paste', this.onPaste);
    }

    where(p) {
        if (p.x < 0 || p.y < 0 || p.x >= this.w || p.y >= this.h) return '';
        const v = this.px[p.y * this.w + p.x];
        let s = 'x ' + p.x + ', y ' + p.y + ': colour ' + v;
        if (this.o.where) s += this.o.where(p);
        if (this.sel || this.float) {
            const b = this.float || this.sel;
            s += '; selection ' + b.w + 'x' + b.h;
        }
        return s;
    }

    /* what an editor of the sheet's carries on into the next */
    get state() { return { tool: this.tool, colour: this.colour, mirror: this.mirror, tile: this.tile }; }
}

/* The sheet: its 256 sprites as a picker, a block of cells chosen there --
 * one, or a range dragged across -- open beside it in the editor, the
 * edits written back to the sheet; and the chosen sprites' eight flags.
 * A picture pasted into the block that is too big for it widens the block
 * to fit, from its top left, as far as the sheet goes.
 *
 * An animation is blocks of one size running rightwards from a first,
 * made from a selection of several cells: played under the picker at its
 * speed, with the line of code that plays it. The onion skin is another
 * block shown faintly under the one being drawn: one right-clicked on the
 * picker, or in an animation the frame before. */
export class SheetEditor {
    /* opts: px, width and height (the cart's sheet, 128x128, or an
     * image's), image (the image's name, for IMG.<name>; none for the
     * sheet), rgb, changed(px); for the sheet only, flags (256),
     * flagsChanged(flags), flagNames() (the eight names cart.json gives,
     * null where none) and nameFlag(b) (to ask for flag b's name); anim
     * ({ x, y, w, h, n, fps }, kept by the IDE between openings) and animChanged(anim), fitColours (as the
     * PixelEditor's) and say(text, error) */
    constructor(parent, opts) {
        this.o = opts;
        this.px = opts.px;
        this.W = opts.width || 128;
        this.H = opts.height || 128;
        /* the sprites are the whole 8x8 cells, numbered along the rows */
        this.cols = this.W >> 3;
        this.rows = this.H >> 3;
        /* the picker 256 pixels across, or the image's own size past that */
        this.zoom = Math.max(1, Math.min(4, Math.floor(256 / this.W)));
        this.cell = { x: 0, y: 0, w: 1, h: 1 };
        this.onion = null;        /* null, 'prev' or the { x, y } of a block */
        const root = this.root = document.createElement('div');
        root.className = 'sheet';
        this.side = sidePanel('Sprites');
        const bar = toolbar();
        this.animButton = bar.add('animate', 'Animate the chosen cells: frames left to right', () => this.animate());
        this.onionButton = bar.add('onion', 'Onion skin: the frame or sprites before, faintly over these. ' +
                                   'Right-click a cell to show that one instead', () => this.toggleOnion());
        this.picker = document.createElement('canvas');
        this.picker.className = 'sheet-picker';
        this.picker.width = this.W * this.zoom;
        this.picker.height = this.H * this.zoom;
        this.picker.style.width = this.picker.width + 'px';
        this.picker.style.height = this.picker.height + 'px';
        this.info = document.createElement('div');
        this.info.className = 'sheet-info';
        this.animBox = document.createElement('div');
        this.animBox.className = 'sheet-anim';
        const body = document.createElement('div');
        body.className = 'sheet-body';
        body.append(this.picker, this.info, this.animBox);
        this.side.body.append(bar.el, body);
        this.edit = document.createElement('div');
        this.edit.className = 'sheet-edit';
        root.append(this.edit);
        parent.append(root);
        this.pick();
        this.open();
    }

    destroy() {
        clearInterval(this.playing);
        if (this.editor) this.editor.destroy();
        this.root.remove();
    }

    focus() { if (this.editor) this.editor.root.focus(); }

    /* sprites of the chosen block, by number */
    sprites() {
        const out = [];
        for (let y = this.cell.y; y < this.cell.y + this.cell.h; y++)
            for (let x = this.cell.x; x < this.cell.x + this.cell.w; x++) out.push(y * this.cols + x);
        return out;
    }

    pick() {
        const cv = this.picker;
        let from = null;
        const at = (e) => {
            const r = cv.getBoundingClientRect();
            const k = r.width / this.W * 8;
            return { x: Math.max(0, Math.min(this.cols - 1, Math.floor((e.clientX - r.left) / k))),
                     y: Math.max(0, Math.min(this.rows - 1, Math.floor((e.clientY - r.top) / k))) };
        };
        cv.addEventListener('contextmenu', (e) => e.preventDefault());
        cv.addEventListener('pointerdown', (e) => {
            if (e.button === 2) {
                const p = at(e), o = this.onion;
                this.onion = o && o.x === p.x && o.y === p.y ? null : p;
                this.refresh();
                return;
            }
            if (e.button !== 0) return;
            try { cv.setPointerCapture(e.pointerId); } catch (x) { /* gone */ }
            from = at(e);
            this.cell = { x: from.x, y: from.y, w: 1, h: 1 };
            this.drawPicker();
        });
        cv.addEventListener('pointermove', (e) => {
            if (!from) return;
            const p = at(e);
            this.cell = { x: Math.min(from.x, p.x), y: Math.min(from.y, p.y),
                          w: Math.abs(p.x - from.x) + 1, h: Math.abs(p.y - from.y) + 1 };
            this.drawPicker();
        });
        const up = () => { if (from) { from = null; this.open(); } };
        cv.addEventListener('pointerup', up);
        cv.addEventListener('pointercancel', up);
    }

    /* the pixels of a block of cells, blank past the sheet's edge */
    block(x0, y0, w, h) {
        const out = new Uint8Array(w * h * 64);
        for (let y = 0; y < h * 8; y++)
            for (let x = 0; x < w * 8; x++) {
                const sx = x0 * 8 + x, sy = y0 * 8 + y;
                if (sx < this.W && sy < this.H) out[y * w * 8 + x] = this.px[sy * this.W + sx];
            }
        return out;
    }

    /* the chosen block in the editor, as its own picture */
    open(paste) {
        const c = this.cell, w = c.w * 8, h = c.h * 8;
        const was = this.editor && this.editor.state;
        if (this.editor) this.editor.destroy();
        this.editor = new PixelEditor(this.edit, {
            width: w, height: h, px: this.block(c.x, c.y, c.w, c.h), rgb: this.o.rgb, grid: 8, was: was,
            changed: (b) => {
                for (let y = 0; y < h; y++)
                    this.px.set(b.subarray(y * w, (y + 1) * w), (c.y * 8 + y) * this.W + c.x * 8);
                this.drawPicker();
                this.o.changed(this.px);
            },
            where: (p) => '; sprite ' + ((c.y + (p.y >> 3)) * this.cols + c.x + (p.x >> 3)) +
                (this.frameOf() >= 0 ? '; frame ' + (this.frameOf() + 1) + ' of ' + this.o.anim.n : ''),
            tooBig: (pic) => this.grow(pic),
            fitColours: this.o.fitColours,
            say: this.o.say
        });
        /* the chosen sprites' flags on the end of the drawing toolbar */
        if (this.o.flags) {
            const sep = document.createElement('span');
            sep.className = 'sep';
            this.flagDrop = dropdown('Flags', 'The chosen sprites\' flags', (list) => this.flagList(list));
            this.editor.bar.append(sep, this.flagDrop.el);
            this.drawFlags();
        }
        this.drawAnim();
        this.refresh();
        if (paste) this.editor.place(paste);
        this.editor.root.focus();
    }

    /* the toolbar, the picker and the onion skin brought up to date */
    refresh() {
        this.animButton.disabled = !this.framing();
        if (this.onion === 'prev' && !this.before()) this.onion = null;
        this.onionButton.disabled = !this.onion && !this.before();
        this.onionButton.classList.toggle('on', !!this.onion);
        const c = this.cell, o = this.onion === 'prev' ? this.before() : this.onion;
        const skin = o ? this.block(o.x, o.y, c.w, c.h) : null;
        this.editor.setOnion(skin);
        this.drawPicker();
    }

    /* the block before the chosen one: the frame before in its animation,
     * else the block to its left, or at the end of the row above */
    before() {
        const c = this.cell, a = this.o.anim, k = this.frameOf();
        if (k >= 0) return { x: a.x + (k + a.n - 1) % a.n * a.w, y: a.y };
        if (c.x >= c.w) return { x: c.x - c.w, y: c.y };
        if (c.y >= c.h) return { x: this.cols - c.w, y: c.y - c.h };
        return null;
    }

    toggleOnion() {
        this.onion = this.onion ? null : 'prev';
        this.refresh();
        this.focus();
    }

    /* ---- animation ---- */

    /* the frames the chosen cells make: square ones where they divide the
     * row, else a cell wide; null for fewer than two */
    framing() {
        const c = this.cell, w = c.w % c.h === 0 ? c.h : 1, n = c.w / w;
        return n >= 2 ? { w, n } : null;
    }

    animate() {
        const f = this.framing(), c = this.cell, a = this.o.anim;
        if (!f) return;
        this.setAnim({ x: c.x, y: c.y, w: f.w, h: c.h, n: f.n, fps: a ? a.fps : 8 });
        this.cell = { x: c.x, y: c.y, w: f.w, h: c.h };
        this.open();
    }

    setAnim(a) {
        this.o.anim = a;
        if (this.o.animChanged) this.o.animChanged(a);
    }

    /* the frame the chosen block is of the animation, or -1 */
    frameOf() {
        const a = this.o.anim, c = this.cell;
        if (!a || c.w !== a.w || c.h !== a.h || c.y !== a.y) return -1;
        const k = (c.x - a.x) / a.w;
        return Number.isInteger(k) && k >= 0 && k < a.n ? k : -1;
    }

    /* frame k's pixels */
    frame(k) {
        const a = this.o.anim;
        return this.block(a.x + k * a.w, a.y, a.w, a.h);
    }

    /* the animation's panel: frame width and speed, playing, the code */
    drawAnim() {
        clearInterval(this.playing);
        const box = this.animBox, a = this.o.anim;
        box.innerHTML = '';
        if (!a) return;
        const head = document.createElement('div');
        head.className = 'ed-sub';
        head.innerHTML = '<span class="grow">Animation</span>';
        head.append(iconButton('close', 'No animation', () => {
            this.setAnim(null);
            this.drawAnim();
            this.refresh();
        }));
        const total = a.n * a.w;
        const bar = toolbar();
        bar.el.classList.add('flat');
        bar.put(numberBox('Frame width', a.w, 1, total, 'Cells across each frame', (w, i) => {
            if (Math.floor(total / w) < 2) { i.value = a.w; return; }
            this.setAnim(Object.assign({}, a, { w, n: Math.floor(total / w) }));
            this.cell = { x: a.x, y: a.y, w, h: a.h };
            this.open();
        }), numberBox('a second', a.fps, 1, 60, 'Frames a second', (fps) => {
            this.setAnim(Object.assign({}, a, { fps }));
            this.drawAnim();
        }));
        const view = document.createElement('canvas');
        const w = a.w * 8, h = a.h * 8, k = Math.max(1, Math.floor(96 / Math.max(w, h)));
        view.width = w * k;
        view.height = h * k;
        view.className = 'sheet-anim-view';
        view.title = 'Click to draw the first frame';
        view.onclick = () => { this.cell = { x: a.x, y: a.y, w: a.w, h: a.h }; this.open(); };
        const code = document.createElement('code');
        code.className = 'sheet-anim-code';
        code.textContent = this.use() + 'spr(' + (a.y * this.cols + a.x) + ' + flr(t() * ' + a.fps + ') % ' + a.n +
            (a.w > 1 ? ' * ' + a.w : '') + ', x, y' + (a.w > 1 || a.h > 1 ? ', ' + a.w + ', ' + a.h : '') + ')';
        box.append(head, bar.el, view, code);
        const g = view.getContext('2d'), im = new ImageData(w, h), tmp = document.createElement('canvas');
        tmp.width = w;
        tmp.height = h;
        let at = 0;
        const draw = () => {
            const px = this.frame(at % a.n);
            for (let i = 0; i < px.length; i++) {
                const s = shown(this.o.rgb[px[i]] || 0);
                im.data[i * 4] = s >> 16; im.data[i * 4 + 1] = s >> 8 & 255; im.data[i * 4 + 2] = s & 255;
                im.data[i * 4 + 3] = 255;
            }
            tmp.getContext('2d').putImageData(im, 0, 0);
            g.imageSmoothingEnabled = false;
            g.drawImage(tmp, 0, 0, w * k, h * k);
            at++;
        };
        draw();
        this.playing = setInterval(draw, 1000 / a.fps);
    }

    /* the call that makes this the sprites' sheet, before the code shown */
    use() {
        return this.o.image ? 'sheet(IMG.' + this.o.image + '); ' : '';
    }

    setPalette(rgb) {
        this.o.rgb = rgb;
        this.editor.setPalette(rgb);
        this.drawPicker();
    }

    /* a picture too big for the block: the block widened to hold it */
    grow(pic) {
        const c = this.cell;
        const w = Math.min(this.cols - c.x, Math.max(c.w, Math.ceil(pic.w / 8)));
        const h = Math.min(this.rows - c.y, Math.max(c.h, Math.ceil(pic.h / 8)));
        if (w === c.w && h === c.h) return false;
        this.cell = { x: c.x, y: c.y, w, h };
        this.open(pic);
        return true;
    }

    drawPicker() {
        const g = this.picker.getContext('2d'), z = this.zoom, W = this.W, H = this.H;
        const im = new ImageData(W, H);
        for (let i = 0; i < this.px.length; i++) {
            const s = shown(this.o.rgb[this.px[i]] || 0);
            im.data[i * 4] = s >> 16; im.data[i * 4 + 1] = s >> 8 & 255; im.data[i * 4 + 2] = s & 255;
            im.data[i * 4 + 3] = 255;
        }
        const tmp = document.createElement('canvas');
        tmp.width = W;
        tmp.height = H;
        tmp.getContext('2d').putImageData(im, 0, 0);
        g.imageSmoothingEnabled = false;
        g.drawImage(tmp, 0, 0, W * z, H * z);
        g.strokeStyle = 'rgba(128,128,128,0.5)';
        g.beginPath();
        for (let k = 8; k <= this.cols * 8 && k < W; k += 8) {
            g.moveTo(k * z + 0.5, 0); g.lineTo(k * z + 0.5, this.rows * 8 * z);
        }
        for (let k = 8; k <= this.rows * 8 && k < H; k += 8) {
            g.moveTo(0, k * z + 0.5); g.lineTo(this.cols * 8 * z, k * z + 0.5);
        }
        g.stroke();
        const an = this.o.anim, c = this.cell, o = this.onion === 'prev' ? this.before() : this.onion;
        if (an) {
            g.strokeStyle = 'rgba(110,160,255,0.9)';
            g.setLineDash([3, 2]);
            g.strokeRect(an.x * 8 * z + 0.5, an.y * 8 * z + 0.5, an.n * an.w * 8 * z - 1, an.h * 8 * z - 1);
            g.setLineDash([]);
        }
        if (o) {
            g.strokeStyle = 'rgba(200,120,255,0.95)';
            g.setLineDash([2, 2]);
            g.strokeRect(o.x * 8 * z + 1.5, o.y * 8 * z + 1.5, c.w * 8 * z - 3, c.h * 8 * z - 3);
            g.setLineDash([]);
        }
        g.strokeStyle = '#ffd23c';
        g.lineWidth = 2;
        g.strokeRect(c.x * 8 * z + 1, c.y * 8 * z + 1, c.w * 8 * z - 2, c.h * 8 * z - 2);
        g.lineWidth = 1;
        const n = this.sprites();
        this.info.textContent = n.length === 1
            ? 'Sprite ' + n[0] + (this.o.image ? ': ' + this.use() + 'spr(' + n[0] + ', x, y)' : '')
            : 'Sprites ' + n[0] + ' to ' + n[n.length - 1] + ', ' + c.w + ' by ' + c.h +
              ': ' + this.use() + 'spr(' + n[0] + ', x, y, ' + c.w + ', ' + c.h + ')';
    }

    /* each flag on for all the chosen sprites, for some, or for none; a
     * click sets it on all of them, or clears it where all had it */
    drawFlags() {
        if (!this.o.flags || !this.flagDrop) return;
        const f = this.o.flags, n = this.sprites(), names = this.o.flagNames();
        const set = [];
        for (let b = 0; b < 8; b++)
            if (n.every((s) => f[s] >> b & 1)) set.push(names[b] || b);
        this.flagDrop.setLabel('Flags: ' + (set.length ? set.join(', ') : 'none'));
        this.flagDrop.refill();
    }

    /* the eight flags in the dropdown: each ticked where all the chosen
     * sprites have it, a dash where some do; a click sets it on all of
     * them, or clears it where all had it */
    flagList(list) {
        const f = this.o.flags, n = this.sprites(), names = this.o.flagNames();
        for (let b = 0; b < 8; b++) {
            const on = n.filter((s) => f[s] >> b & 1).length;
            const row = document.createElement('div');
            row.className = 'drop-row';
            const t = document.createElement('button');
            t.className = 'drop-item' + (on === n.length ? ' on' : on ? ' some' : '');
            t.textContent = names[b] ? names[b] : 'Flag ' + b;
            t.title = names[b] ? 'FLAG.' + names[b] + ', flag ' + b : 'Flag ' + b;
            t.onclick = () => {
                for (const s of n) f[s] = on === n.length ? f[s] & ~(1 << b) : f[s] | 1 << b;
                this.o.flagsChanged(f);
                this.drawFlags();
            };
            const name = iconButton('pencil', 'Name flag ' + b, () => { this.flagDrop.close(); this.o.nameFlag(b); });
            row.append(t, name);
            list.append(row);
        }
    }
}
