/* The map editor: a map's cells painted with the sheet's sprites, and its
 * objects placed over them.
 *
 * Parts: reading and writing the map's JSON; the editor -- the tile picker
 * beside it, the toolbar, the map drawn zoomed with its grid, a flag's
 * cells marked and the objects over it; painting; the objects and their
 * inspector; the keys; undo.
 *
 * Cell 0 is empty, as map() leaves it, and colour 0 of a sprite is clear.
 * The brush is a block of the sheet: painting stamps it in steps of its
 * own size, and a rectangle or a fill repeats it. Objects are in pixels,
 * placed on whole cells unless Shift is held. */

import { shown } from './pixels.js';
import { iconButton } from './icons.js';
import { sidePanel, toolbar } from './chrome.js';

const MAX = 256;
const NAME = /^[A-Za-z_][A-Za-z0-9_]*$/;

/* ---- the JSON ------------------------------------------------------------ */

/* { w, h, cells, objects, rest }: rest is any other field, kept */
export function readMap(text) {
    const m = JSON.parse(text);
    const w = m.width, h = m.height;
    if (!(Number.isInteger(w) && Number.isInteger(h) && w >= 1 && h >= 1 && w <= MAX && h <= MAX))
        throw new Error('width and height are 1 to ' + MAX + ' cells');
    const cells = new Uint8Array(w * h);
    const rows = m.rows || [];
    if (!Array.isArray(rows)) throw new Error('rows is a list of strings');
    rows.forEach((r, y) => {
        if (typeof r !== 'string' || !/^([0-9a-fA-F]{2})*$/.test(r))
            throw new Error('row ' + y + ' is not two hex digits a cell');
        if (y < h) for (let x = 0; x < Math.min(w, r.length / 2); x++) cells[y * w + x] = parseInt(r.substr(x * 2, 2), 16);
    });
    if (m.objects !== undefined && !Array.isArray(m.objects)) throw new Error('objects is a list');
    const objects = (m.objects || []).map((o) => Object.assign({}, o, o.props ? { props: Object.assign({}, o.props) } : {}));
    const rest = Object.assign({}, m);
    for (const k of ['width', 'height', 'rows', 'objects']) delete rest[k];
    return { w, h, cells, objects, rest };
}

/* the map as JSON: a row of cells to a line, an object to a line */
export function writeMap(m) {
    const one = (o) => '{ ' + Object.entries(o)
        .filter(([k, v]) => v !== undefined && !(k === 'props' && !Object.keys(v).length))
        .map(([k, v]) => JSON.stringify(k) + ': ' + JSON.stringify(v)).join(', ') + ' }';
    const out = ['{', ' "width": ' + m.w + ',', ' "height": ' + m.h + ','];
    for (const [k, v] of Object.entries(m.rest)) out.push(' ' + JSON.stringify(k) + ': ' + JSON.stringify(v) + ',');
    out.push(' "rows": [');
    for (let y = 0; y < m.h; y++) {
        let s = '';
        for (let x = 0; x < m.w; x++) s += m.cells[y * m.w + x].toString(16).padStart(2, '0');
        out.push('  "' + s + '"' + (y < m.h - 1 ? ',' : ''));
    }
    out.push(' ],', ' "objects": [');
    m.objects.forEach((o, i) => out.push('  ' + one(o) + (i < m.objects.length - 1 ? ',' : '')));
    out.push(' ]', '}');
    return out.join('\n') + '\n';
}

/* the sheet as a 128x128 canvas, colour 0 clear */
function sheetCanvas(px, rgb) {
    const c = document.createElement('canvas');
    c.width = c.height = 128;
    const im = new ImageData(128, 128);
    for (let i = 0; px && i < px.length; i++) {
        if (!px[i]) continue;
        const s = shown(rgb[px[i]] || 0);
        im.data[i * 4] = s >> 16; im.data[i * 4 + 1] = s >> 8 & 255; im.data[i * 4 + 2] = s & 255;
        im.data[i * 4 + 3] = 255;
    }
    c.getContext('2d').putImageData(im, 0, 0);
    return c;
}

/* ---- the editor ---------------------------------------------------------- */

export class MapEditor {
    /* opts: text (the JSON; readMap's error is thrown), sheet (the sheet's
     * pixels, 128x128, or null), rgb, flags (256), flagNames() (the
     * eight, null where none), changed(text), ask (the IDE's dialog),
     * say(text, error), asText(), picker and pickerShown(open) */
    constructor(parent, opts) {
        this.o = opts;
        this.m = readMap(opts.text);
        this.brush = { x: 1, y: 0, w: 1, h: 1 };
        this.tool = 'pencil';
        this.grid = true;
        this.layer = -1;          /* the flag whose cells are marked, or -1 */
        this.sel = -1;            /* the chosen object */
        this.kind = 'thing';      /* the kind a new object takes */
        this.undos = [];
        this.redos = [];
        this.hover = null;
        this.drag = null;
        this.tiles = sheetCanvas(opts.sheet, opts.rgb);
        this.build(parent);
        const room = Math.min(this.area.clientWidth - 24, this.area.clientHeight - 24);
        this.setZoom(Math.floor(room / (Math.max(this.m.w, this.m.h) * 8)));
        this.drawPicker();
        this.drawObjects();
    }

    build(parent) {
        const root = this.root = document.createElement('div');
        root.className = 'map';
        this.side = sidePanel('Tiles', (open) => this.o.pickerShown(open));
        this.picker = document.createElement('canvas');
        this.picker.className = 'sheet-picker';
        this.picker.width = this.picker.height = 256;
        this.info = document.createElement('div');
        this.info.className = 'sheet-info';
        const objHead = document.createElement('div');
        objHead.className = 'ed-sub';
        objHead.innerHTML = '<span class="grow">Objects</span>';
        this.list = document.createElement('div');
        this.list.className = 'map-objs';
        this.insp = document.createElement('div');
        this.insp.className = 'map-insp';
        const body = document.createElement('div');
        body.className = 'sheet-body';
        body.append(this.picker, this.info, objHead, this.list, this.insp);
        this.side.body.append(body);
        this.setPicker(this.o.picker !== false);

        const main = this.main = document.createElement('div');
        main.className = 'map-main';
        main.tabIndex = 0;
        const bar = toolbar();
        this.toolButtons = {};
        for (const [id, ic, title] of [
            ['pencil', 'pencil', 'Paint (P); the right button erases'],
            ['rect', 'rect', 'Rectangle (R)'], ['fill', 'fill', 'Fill (F)'],
            ['pick', 'pick', 'Pick a tile (I); Alt+click picks with any tool'],
            ['erase', 'erase', 'Erase (E)'],
            ['object', 'object', 'Objects (O): drag on the map to place one, click to choose, drag to move; ' +
                                 'Shift places off the grid']])
            this.toolButtons[id] = bar.add(ic, title, () => this.use(id));
        bar.sep();
        this.gridButton = bar.add('grid', 'Grid (G)', () => { this.grid = !this.grid; this.refresh(); });
        const layer = document.createElement('label');
        layer.title = 'Mark the cells whose sprite has a flag';
        layer.innerHTML = iconButton('layers').innerHTML;
        this.layerChoice = document.createElement('select');
        layer.append(this.layerChoice);
        this.layerChoice.onchange = () => { this.layer = +this.layerChoice.value; this.drawMap(); };
        bar.put(layer);
        bar.sep();
        bar.add('zoomOut', 'Zoom out (-)', () => this.setZoom(this.zoom - 1));
        bar.add('zoomIn', 'Zoom in (+)', () => this.setZoom(this.zoom + 1));
        bar.sep();
        bar.add('resize', 'Map size', () => this.resize());
        const grow = document.createElement('span');
        grow.className = 'grow';
        bar.put(grow);
        bar.add('text', 'Edit as text', () => this.o.asText());

        this.area = document.createElement('div');
        this.area.className = 'pix-area';
        const stack = document.createElement('div');
        stack.className = 'pix-stack';
        this.canvas = document.createElement('canvas');
        this.over = document.createElement('canvas');
        this.over.className = 'pix-over';
        stack.append(this.canvas, this.over);
        this.area.append(stack);
        this.status = document.createElement('div');
        this.status.className = 'pix-status';
        main.append(bar.el, this.area, this.status);
        root.append(this.side.el, main);
        parent.append(root);
        this.fillLayers();
        this.listen();
        this.use(this.tool);
    }

    destroy() { this.root.remove(); }

    setPicker(open) { this.side.setOpen(open); }

    focus() { this.main.focus(); }

    setPalette(rgb) {
        this.o.rgb = rgb;
        this.tiles = sheetCanvas(this.o.sheet, rgb);
        this.drawPicker();
        this.drawMap();
    }

    fillLayers() {
        const names = this.o.flagNames();
        this.layerChoice.innerHTML = '';
        const opt = (v, t) => { const o = document.createElement('option'); o.value = v; o.textContent = t; this.layerChoice.append(o); };
        opt(-1, 'No flag');
        for (let b = 0; b < 8; b++) opt(b, names[b] ? names[b] + ' (' + b + ')' : 'Flag ' + b);
        this.layerChoice.value = this.layer;
    }

    use(tool) {
        this.tool = tool;
        for (const [id, b] of Object.entries(this.toolButtons)) b.classList.toggle('on', id === tool);
        this.over.style.cursor = tool === 'object' ? 'default' : 'crosshair';
        this.refresh();
        this.main.focus();
    }

    /* whole pixels a map pixel; a canvas no wider than 8192 */
    setZoom(z) {
        const most = Math.max(1, Math.floor(8192 / (Math.max(this.m.w, this.m.h) * 8)));
        this.zoom = Math.max(1, Math.min(8, most, z || 1));
        for (const c of [this.canvas, this.over]) {
            c.width = this.m.w * 8 * this.zoom;
            c.height = this.m.h * 8 * this.zoom;
        }
        this.drawMap();
    }

    /* ---- drawing ---- */

    drawMap() {
        const g = this.canvas.getContext('2d');
        g.imageSmoothingEnabled = false;
        g.clearRect(0, 0, this.canvas.width, this.canvas.height);
        for (let y = 0; y < this.m.h; y++)
            for (let x = 0; x < this.m.w; x++) this.drawCell(g, x, y);
        this.refresh();
    }

    drawCell(g, x, y) {
        const v = this.m.cells[y * this.m.w + x], k = 8 * this.zoom;
        g.clearRect(x * k, y * k, k, k);
        if (v) g.drawImage(this.tiles, (v & 15) * 8, (v >> 4) * 8, 8, 8, x * k, y * k, k, k);
        if (this.layer >= 0) {
            g.fillStyle = v && this.o.flags[v] >> this.layer & 1 ? 'rgba(110,160,255,0.45)' : 'rgba(0,0,0,0.55)';
            g.fillRect(x * k, y * k, k, k);
        }
    }

    /* the overlay: the grid, the objects, the brush where it would go and
     * anything being dragged out; and the status line */
    refresh() {
        const g = this.over.getContext('2d'), k = 8 * this.zoom, W = this.over.width, H = this.over.height;
        this.gridButton.classList.toggle('on', this.grid);
        g.clearRect(0, 0, W, H);
        if (this.grid && this.zoom >= 2) {
            g.strokeStyle = 'rgba(128,128,128,0.3)';
            g.beginPath();
            for (let x = 1; x < this.m.w; x++) { g.moveTo(x * k + 0.5, 0); g.lineTo(x * k + 0.5, H); }
            for (let y = 1; y < this.m.h; y++) { g.moveTo(0, y * k + 0.5); g.lineTo(W, y * k + 0.5); }
            g.stroke();
        }
        const z = this.zoom;
        g.font = '11px system-ui, sans-serif';
        this.m.objects.forEach((o, i) => {
            const on = i === this.sel, w = Math.max(o.w || 0, 1), h = Math.max(o.h || 0, 1);
            g.strokeStyle = on ? '#ffd23c' : 'rgba(255,140,60,0.95)';
            g.lineWidth = on ? 2 : 1;
            g.strokeRect(o.x * z + 0.5, o.y * z + 0.5, w * z - 1, h * z - 1);
            if (this.tool === 'object' || on) {
                g.fillStyle = on ? '#ffd23c' : 'rgba(255,140,60,0.95)';
                g.fillText(o.kind, o.x * z + 2, o.y * z - 3);
            }
            if (on) g.fillRect((o.x + w) * z - 4, (o.y + h) * z - 4, 7, 7);
        });
        g.lineWidth = 1;
        const d = this.drag, hv = this.hover;
        if (d && d.rect) {
            const r = d.rect;
            g.strokeStyle = '#ffd23c';
            g.setLineDash([4, 3]);
            g.strokeRect(r.x * z + 0.5, r.y * z + 0.5, r.w * z - 1, r.h * z - 1);
            g.setLineDash([]);
        } else if (hv && this.tool !== 'object') {
            const b = this.tool === 'pencil' ? this.brush : { w: 1, h: 1 };
            g.strokeStyle = this.tool === 'erase' ? 'rgba(255,90,90,0.9)' : 'rgba(255,255,255,0.8)';
            g.strokeRect(hv.cx * k + 0.5, hv.cy * k + 0.5, b.w * k - 1, b.h * k - 1);
        }
        let s = this.m.w + ' by ' + this.m.h + ' cells, ' + this.m.objects.length +
            (this.m.objects.length === 1 ? ' object' : ' objects');
        if (hv) {
            const v = this.cell(hv.cx, hv.cy), names = this.o.flagNames();
            const fl = [];
            for (let b = 0; b < 8; b++) if (this.o.flags[v] >> b & 1) fl.push(names[b] || b);
            s = 'Cell ' + hv.cx + ', ' + hv.cy + ' (pixel ' + hv.px + ', ' + hv.py + '): ' +
                (v ? 'sprite ' + v + (fl.length ? ', flags ' + fl.join(', ') : '') : 'empty') + '; ' + s;
        }
        this.status.textContent = s;
    }

    drawPicker() {
        const g = this.picker.getContext('2d');
        g.imageSmoothingEnabled = false;
        g.fillStyle = '#101014';
        g.fillRect(0, 0, 256, 256);
        g.drawImage(this.tiles, 0, 0, 256, 256);
        g.strokeStyle = 'rgba(128,128,128,0.5)';
        g.beginPath();
        for (let k = 16; k < 256; k += 16) {
            g.moveTo(k + 0.5, 0); g.lineTo(k + 0.5, 256);
            g.moveTo(0, k + 0.5); g.lineTo(256, k + 0.5);
        }
        g.stroke();
        const b = this.brush;
        g.strokeStyle = '#ffd23c';
        g.lineWidth = 2;
        g.strokeRect(b.x * 16 + 1, b.y * 16 + 1, b.w * 16 - 2, b.h * 16 - 2);
        g.lineWidth = 1;
        const first = b.y * 16 + b.x;
        this.info.textContent = b.w * b.h === 1 ? 'Sprite ' + first
            : 'Sprites ' + first + ' to ' + ((b.y + b.h - 1) * 16 + b.x + b.w - 1) + ', ' + b.w + ' by ' + b.h;
    }

    /* ---- the map's cells ---- */

    cell(x, y) { return x >= 0 && y >= 0 && x < this.m.w && y < this.m.h ? this.m.cells[y * this.m.w + x] : 0; }

    set(x, y, v) {
        if (x < 0 || y < 0 || x >= this.m.w || y >= this.m.h) return;
        this.m.cells[y * this.m.w + x] = v;
        this.drawCell(this.canvas.getContext('2d'), x, y);
    }

    /* the brush's sprite for a cell, the brush repeated from x0, y0 */
    pattern(x, y, x0, y0) {
        const b = this.brush;
        const dx = ((x - x0) % b.w + b.w) % b.w, dy = ((y - y0) % b.h + b.h) % b.h;
        return (b.y + dy) * 16 + b.x + dx;
    }

    stamp(cx, cy, erase) {
        const b = this.brush;
        for (let dy = 0; dy < b.h; dy++)
            for (let dx = 0; dx < b.w; dx++) this.set(cx + dx, cy + dy, erase ? 0 : (b.y + dy) * 16 + b.x + dx);
    }

    fillRect(r, erase) {
        for (let y = r.y; y < r.y + r.h; y++)
            for (let x = r.x; x < r.x + r.w; x++) this.set(x, y, erase ? 0 : this.pattern(x, y, r.x, r.y));
    }

    /* the cells joined to x, y that share its sprite, filled */
    flood(x, y, erase) {
        const W = this.m.w, from = this.cell(x, y), seen = new Uint8Array(W * this.m.h), todo = [[x, y]];
        while (todo.length) {
            const [px, py] = todo.pop();
            if (px < 0 || py < 0 || px >= W || py >= this.m.h || seen[py * W + px] || this.cell(px, py) !== from) continue;
            seen[py * W + px] = 1;
            todo.push([px + 1, py], [px - 1, py], [px, py + 1], [px, py - 1]);
        }
        for (let i = 0; i < seen.length; i++)
            if (seen[i]) this.set(i % W, Math.floor(i / W), erase ? 0 : this.pattern(i % W, Math.floor(i / W), x, y));
    }

    pickAt(cx, cy) {
        const v = this.cell(cx, cy);
        this.brush = { x: v & 15, y: v >> 4, w: 1, h: 1 };
        this.drawPicker();
        if (this.tool === 'pick' || this.tool === 'erase') this.use('pencil');
    }

    /* ---- the pointer ---- */

    listen() {
        const cv = this.picker;
        let from = null;
        const cellOf = (e) => {
            const r = cv.getBoundingClientRect();
            return { x: Math.max(0, Math.min(15, Math.floor((e.clientX - r.left) / (r.width / 16)))),
                     y: Math.max(0, Math.min(15, Math.floor((e.clientY - r.top) / (r.height / 16)))) };
        };
        cv.addEventListener('pointerdown', (e) => {
            try { cv.setPointerCapture(e.pointerId); } catch (x) { /* gone */ }
            from = cellOf(e);
            this.brush = { x: from.x, y: from.y, w: 1, h: 1 };
            this.drawPicker();
        });
        cv.addEventListener('pointermove', (e) => {
            if (!from) return;
            const p = cellOf(e);
            this.brush = { x: Math.min(from.x, p.x), y: Math.min(from.y, p.y),
                           w: Math.abs(p.x - from.x) + 1, h: Math.abs(p.y - from.y) + 1 };
            this.drawPicker();
        });
        const up = () => {
            if (!from) return;
            from = null;
            if (this.tool === 'pick' || this.tool === 'erase' || this.tool === 'object') this.use('pencil');
            this.main.focus();
        };
        cv.addEventListener('pointerup', up);
        cv.addEventListener('pointercancel', up);

        const ov = this.over;
        const at = (e) => {
            const r = ov.getBoundingClientRect(), z = this.zoom;
            const px = Math.floor((e.clientX - r.left) / z), py = Math.floor((e.clientY - r.top) / z);
            return { px, py, cx: Math.floor(px / 8), cy: Math.floor(py / 8) };
        };
        ov.addEventListener('contextmenu', (e) => e.preventDefault());
        ov.addEventListener('pointerdown', (e) => {
            const p = at(e), erase = e.button === 2 || this.tool === 'erase';
            this.main.focus();
            try { ov.setPointerCapture(e.pointerId); } catch (x) { /* gone */ }
            if (this.tool === 'object') { this.objectDown(p, e); return; }
            if (e.altKey || this.tool === 'pick') { this.pickAt(p.cx, p.cy); return; }
            this.save();
            if (this.tool === 'fill') { this.flood(p.cx, p.cy, erase); this.commit(); return; }
            if (this.tool === 'rect') { this.drag = { from: p, erase, rect: this.cellRect(p, p) }; this.refresh(); return; }
            this.drag = { from: p, erase, last: null };
            this.paintAt(p);
        });
        ov.addEventListener('pointermove', (e) => {
            const p = at(e), d = this.drag;
            this.hover = p;
            if (d && d.object) this.objectMove(p, e);
            else if (d && this.tool === 'rect') d.rect = this.cellRect(d.from, p);
            else if (d) this.paintAt(p);
            this.refresh();
        });
        ov.addEventListener('pointerleave', () => { this.hover = null; this.refresh(); });
        const end = () => {
            const d = this.drag;
            if (!d) return;
            this.drag = null;
            if (d.object) { this.objectUp(d); return; }
            if (this.tool === 'rect') {
                const r = d.rect;
                this.fillRect({ x: r.x / 8, y: r.y / 8, w: r.w / 8, h: r.h / 8 }, d.erase);
            }
            this.commit();
        };
        ov.addEventListener('pointerup', end);
        ov.addEventListener('pointercancel', end);
        this.main.addEventListener('keydown', (e) => this.key(e));
    }

    /* the cells between two points, in map pixels */
    cellRect(a, b) {
        const x0 = Math.max(0, Math.min(a.cx, b.cx)), y0 = Math.max(0, Math.min(a.cy, b.cy));
        const x1 = Math.min(this.m.w - 1, Math.max(a.cx, b.cx)), y1 = Math.min(this.m.h - 1, Math.max(a.cy, b.cy));
        return { x: x0 * 8, y: y0 * 8, w: (x1 - x0 + 1) * 8, h: (y1 - y0 + 1) * 8 };
    }

    /* painting steps by the brush's size from where the stroke began */
    paintAt(p) {
        const d = this.drag, b = this.tool === 'pencil' && !d.erase ? this.brush : { w: 1, h: 1 };
        const cx = d.from.cx + Math.floor((p.cx - d.from.cx) / b.w) * b.w;
        const cy = d.from.cy + Math.floor((p.cy - d.from.cy) / b.h) * b.h;
        if (d.last && d.last.cx === cx && d.last.cy === cy) return;
        d.last = { cx, cy };
        if (d.erase) this.set(cx, cy, 0); else this.stamp(cx, cy, false);
    }

    /* ---- objects ---- */

    snap(v, e) { return e.shiftKey ? v : Math.round(v / 8) * 8; }

    objectDown(p, e) {
        const o = this.m.objects[this.sel], z = this.zoom;
        const near = (x, y) => Math.abs(p.px - x) * z <= 5 && Math.abs(p.py - y) * z <= 5;
        if (o && near(o.x + Math.max(o.w || 0, 1), o.y + Math.max(o.h || 0, 1))) {
            this.save();
            this.drag = { object: 'size', from: p, was: Object.assign({}, o) };
            return;
        }
        let hit = -1;
        this.m.objects.forEach((q, i) => {
            if (p.px >= q.x && p.py >= q.y && p.px < q.x + Math.max(q.w || 0, 1) && p.py < q.y + Math.max(q.h || 0, 1)) hit = i;
        });
        if (hit >= 0) {
            this.choose(hit);
            this.save();
            this.drag = { object: 'move', from: p, was: Object.assign({}, this.m.objects[hit]) };
            return;
        }
        const x = this.snap(p.px, e), y = this.snap(p.py, e);
        this.drag = { object: 'new', from: p, rect: { x, y, w: 0, h: 0 } };
    }

    objectMove(p, e) {
        const d = this.drag, o = this.m.objects[this.sel];
        if (d.object === 'new') {
            const x0 = this.snap(d.from.px, e), y0 = this.snap(d.from.py, e);
            const x1 = e.shiftKey ? p.px + 1 : Math.ceil((p.px + 1) / 8) * 8, y1 = e.shiftKey ? p.py + 1 : Math.ceil((p.py + 1) / 8) * 8;
            d.rect = { x: Math.min(x0, x1), y: Math.min(y0, y1), w: Math.abs(x1 - x0), h: Math.abs(y1 - y0) };
            return;
        }
        const dx = p.px - d.from.px, dy = p.py - d.from.py;
        if (d.object === 'move') {
            o.x = e.shiftKey ? d.was.x + dx : Math.round((d.was.x + dx) / 8) * 8;
            o.y = e.shiftKey ? d.was.y + dy : Math.round((d.was.y + dy) / 8) * 8;
        } else {
            o.w = Math.max(1, e.shiftKey ? (d.was.w || 0) + dx : Math.round(((d.was.w || 0) + dx) / 8) * 8);
            o.h = Math.max(1, e.shiftKey ? (d.was.h || 0) + dy : Math.round(((d.was.h || 0) + dy) / 8) * 8);
        }
    }

    objectUp(d) {
        if (d.object === 'new') {
            if (!d.rect.w || !d.rect.h) { this.choose(-1); return; }
            this.save();
            this.m.objects.push(Object.assign({ kind: this.kind }, d.rect));
            this.choose(this.m.objects.length - 1);
            this.commit();
            this.insp.querySelector('input').select();
            return;
        }
        const o = this.m.objects[this.sel];
        if (['x', 'y', 'w', 'h'].every((k) => o[k] === d.was[k])) { this.undos.pop(); return; }
        this.commit();
    }

    choose(i) {
        this.sel = i;
        this.drawObjects();
        this.refresh();
    }

    /* the list of objects, and the chosen one's kind, place, size and
     * properties */
    drawObjects() {
        this.list.innerHTML = '';
        this.m.objects.forEach((o, i) => {
            const b = document.createElement('button');
            b.textContent = o.kind + '  ' + o.x + ', ' + o.y;
            b.classList.toggle('on', i === this.sel);
            b.onclick = () => {
                this.choose(i);
                const z = this.zoom;
                this.area.scrollTo({ left: o.x * z - this.area.clientWidth / 2, top: o.y * z - this.area.clientHeight / 2 });
            };
            this.list.append(b);
        });
        if (!this.m.objects.length) {
            const p = document.createElement('span');
            p.className = 'map-none';
            p.textContent = 'None. Choose Objects on the toolbar and drag on the map.';
            this.list.append(p);
        }
        this.insp.innerHTML = '';
        const o = this.m.objects[this.sel];
        if (!o) return;
        const head = document.createElement('div');
        head.className = 'ed-sub';
        head.innerHTML = '<span class="grow">Object ' + this.sel + '</span>';
        head.append(iconButton('trash', 'Delete it (Delete)', () => this.removeObject()));
        const kinds = document.createElement('datalist');
        kinds.id = 'map-kinds';
        for (const k of new Set(this.m.objects.map((q) => q.kind))) {
            const opt = document.createElement('option');
            opt.value = k;
            kinds.append(opt);
        }
        const input = (value, set, type) => {
            const i = document.createElement('input');
            i.type = type || 'text';
            i.value = value;
            i.onchange = () => {
                this.save();
                if (set(i.value) === false) { this.undos.pop(); i.value = value; return; }
                this.commit();
                this.drawObjects();
            };
            return i;
        };
        const kind = input(o.kind, (v) => {
            if (!NAME.test(v)) { this.o.say('A kind is a name: letters, digits and _.', true); return false; }
            o.kind = this.kind = v;
        });
        kind.setAttribute('list', 'map-kinds');
        kind.title = 'Its kind: OBJ.' + o.kind + ' in code';
        const place = document.createElement('div');
        place.className = 'map-xywh';
        for (const k of ['x', 'y', 'w', 'h']) {
            const l = document.createElement('label');
            const i = input(o[k] || 0, (v) => {
                const n = Math.round(+v);
                if (!Number.isFinite(n) || n < -32768 || n > 32767) return false;
                o[k] = n;
            }, 'number');
            l.append(k, i);
            place.append(l);
        }
        const props = document.createElement('div');
        props.className = 'map-props';
        const p = o.props || {};
        for (const [name, value] of Object.entries(p)) {
            const row = document.createElement('div');
            row.className = 'map-prop';
            const n = input(name, (v) => {
                if (!NAME.test(v) || (v !== name && v in p)) return false;
                o.props = Object.fromEntries(Object.entries(p).map(([a, b]) => [a === name ? v : a, b]));
            });
            n.title = 'mapprop(i, "' + name + '")';
            const val = input(value, (v) => { p[name] = /^-?\d+$/.test(v.trim()) ? +v : v; o.props = p; });
            val.title = 'A whole number, or else a string';
            row.append(n, val, iconButton('close', 'Remove it', () => {
                this.save();
                delete p[name];
                o.props = p;
                this.commit();
                this.drawObjects();
            }));
            props.append(row);
        }
        const add = iconButton('plus', 'Add a property', () => {
            this.save();
            let k = 1;
            while (('prop' + k) in p) k++;
            p['prop' + k] = 0;
            o.props = p;
            this.commit();
            this.drawObjects();
        });
        const propHead = document.createElement('div');
        propHead.className = 'ed-sub';
        propHead.innerHTML = '<span class="grow">Properties</span>';
        propHead.append(add);
        const kl = document.createElement('label');
        kl.className = 'map-kind';
        kl.append('Kind', kind);
        this.insp.append(head, kinds, kl, place, propHead, props);
    }

    removeObject() {
        if (this.sel < 0) return;
        this.save();
        this.m.objects.splice(this.sel, 1);
        this.sel = -1;
        this.commit();
        this.drawObjects();
    }

    async resize() {
        const a = await this.o.ask('Map size', [
            { name: 'w', label: 'Width', type: 'number', value: this.m.w },
            { name: 'h', label: 'Height', type: 'number', value: this.m.h, note: 'In cells, 1 to ' + MAX +
              '. The map keeps its top left; cells past a smaller size are lost.' }], 'Resize');
        if (!a) return;
        const w = Math.round(+a.w), h = Math.round(+a.h);
        if (!(w >= 1 && h >= 1 && w <= MAX && h <= MAX)) { this.o.say('A map is 1 to ' + MAX + ' cells each way.', true); return; }
        this.save();
        const cells = new Uint8Array(w * h);
        for (let y = 0; y < Math.min(h, this.m.h); y++)
            for (let x = 0; x < Math.min(w, this.m.w); x++) cells[y * w + x] = this.m.cells[y * this.m.w + x];
        Object.assign(this.m, { w, h, cells });
        this.commit();
        this.setZoom(this.zoom);
    }

    /* ---- the keys ---- */

    key(e) {
        if (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT') return;
        const k = e.key.toLowerCase(), o = this.m.objects[this.sel];
        if (e.ctrlKey || e.metaKey) {
            if (k === 'z' && !e.shiftKey) { e.preventDefault(); this.undoEdit(); }
            else if (k === 'y' || (k === 'z' && e.shiftKey)) { e.preventDefault(); this.redoEdit(); }
            return;
        }
        const tools = { p: 'pencil', r: 'rect', f: 'fill', i: 'pick', e: 'erase', o: 'object' };
        if (tools[k]) this.use(tools[k]);
        else if (k === 'g') { this.grid = !this.grid; this.refresh(); }
        else if (k === '+' || k === '=') this.setZoom(this.zoom + 1);
        else if (k === '-') this.setZoom(this.zoom - 1);
        else if ((k === 'delete' || k === 'backspace') && o) this.removeObject();
        else if (k === 'escape') this.choose(-1);
        else if (o && k.startsWith('arrow')) {
            const step = e.shiftKey ? 8 : 1;
            this.save();
            if (k === 'arrowleft') o.x -= step;
            if (k === 'arrowright') o.x += step;
            if (k === 'arrowup') o.y -= step;
            if (k === 'arrowdown') o.y += step;
            this.commit();
            this.drawObjects();
        } else return;
        e.preventDefault();
    }

    /* ---- undo ---- */

    snapshot() { return { w: this.m.w, h: this.m.h, cells: this.m.cells.slice(), objects: JSON.parse(JSON.stringify(this.m.objects)) }; }

    save() {
        this.undos.push(this.snapshot());
        if (this.undos.length > 100) this.undos.shift();
        this.redos = [];
    }

    restore(s) {
        const resized = s.w !== this.m.w || s.h !== this.m.h;
        Object.assign(this.m, s);
        if (this.sel >= this.m.objects.length) this.sel = -1;
        this.o.changed(writeMap(this.m));
        if (resized) this.setZoom(this.zoom); else this.drawMap();
        this.drawObjects();
    }

    undoEdit() { if (this.undos.length) { this.redos.push(this.snapshot()); this.restore(this.undos.pop()); } }

    redoEdit() { if (this.redos.length) { this.undos.push(this.snapshot()); this.restore(this.redos.pop()); } }

    commit() {
        this.o.changed(writeMap(this.m));
        this.refresh();
    }
}
