/* The profiler: where a running cart's time goes, on the player it runs
 * as. While it records, the engine notes where the cart is every 2048
 * units -- the function, the line, and the API call if it is in one -- and
 * each frame's cost is drawn against the time a frame has.
 *
 *   new Profiler(el, opts)  opts: goTo(file, line), apiName(slot),
 *                           device() ({ name, unit: ms a unit }),
 *                           recording(on) (told when recording starts or
 *                           stops, to sample the engine or not)
 *   .begin(fns)             a run starts, with the cart's functions
 *   .frame(samples, ms, period, screen, sections, units)  a frame ran:
 *                           its samples, its cost, the time it had, and
 *                           its cost of sending the screen, which no
 *                           sample sees, in ms; the units of each of the
 *                           cart's prof() sections in it, and its units
 *
 * A function's self is the samples taken in its own code, its total those
 * taken in it or anything it called; an API call's are the samples taken
 * inside it. Each sample is 2048 units, so a frame of under 2048 may show
 * none: the counts mean something over many frames.
 *
 * Parts: the panel; recording; the frames' graph; the table. */

import { iconButton } from './icons.js';
import { toolbar } from './chrome.js';

const SAMPLE_UNITS = 2048;
const WORDS = 12;
const KEEP = 600;            /* frames shown in the graph */

export class Profiler {
    constructor(el, opts) {
        this.o = opts;
        this.on = false;
        this.view = 'functions';
        this.sort = 'self';
        this.fns = [];
        this.clear();
        const bar = toolbar();
        this.recordButton = bar.add('record', 'Record: sample the cart while it runs', () => this.record(!this.on));
        bar.add('clear', 'Clear what was recorded', () => { this.clear(); this.draw(); });
        bar.sep();
        this.viewButtons = {
            functions: bar.add('functions', 'By function, and by API call', () => this.setView('functions')),
            lines: bar.add('lines', 'By line', () => this.setView('lines'))
        };
        this.summary = document.createElement('span');
        this.summary.className = 'note';
        bar.put(this.summary);
        this.graph = document.createElement('canvas');
        this.graph.className = 'prof-graph';
        this.graph.height = 64;
        this.table = document.createElement('div');
        this.table.className = 'prof-table';
        el.className += ' prof';
        el.append(bar.el, this.graph, this.table);
        new ResizeObserver(() => this.drawGraph()).observe(this.graph);
        this.setView('functions');
    }

    /* ---- recording ---- */

    clear() {
        this.frames = [];          /* [ms, period] */
        this.count = 0;            /* frames recorded */
        this.samples = 0;
        this.self = new Map();     /* key -> samples */
        this.total = new Map();
        this.lineSelf = new Map();
        this.screen = 0;           /* ms sending the screen, in all */
        this.sections = new Map();  /* prof() section -> units, in all */
        this.units = 0;            /* the frames' units, in all */
    }

    record(on) {
        this.on = on;
        this.recordButton.classList.toggle('on', on);
        this.recordButton.title = on ? 'Stop recording' : 'Record: sample the cart while it runs';
        this.o.recording(on);
        this.draw();
    }

    begin(fns) {
        this.fns = fns;
        this.clear();
        this.draw();
    }

    frame(s, ms, period, screen, sections, units) {
        if (!this.on) return;
        this.count++;
        this.screen += screen;
        this.units += units || 0;
        if (sections) for (const [k, v] of sections) this.sections.set(k, (this.sections.get(k) || 0) + v);
        this.frames.push([ms, period]);
        if (this.frames.length > KEEP) this.frames.shift();
        const add = (m, k) => m.set(k, (m.get(k) || 0) + 1);
        for (let i = 0; i < s.length; i += WORDS) {
            const native = s[i], pc = s[i + 1], depth = s[i + 2], root = s[i + 3];
            this.samples++;
            if (depth === 0) { add(this.self, 'engine'); continue; }
            const inner = s[i + 4];
            add(this.self, native >= 0 ? 'api:' + native : 'fn:' + inner);
            add(this.lineSelf, inner + ':' + this.lineOf(inner, pc) + ':' + native);
            const seen = new Set();
            for (let k = 0; k < Math.min(depth, 8); k++) seen.add(s[i + 4 + k]);
            if (depth > 8) seen.add(root);
            for (const f of seen) add(this.total, 'fn:' + f);
            if (native >= 0) add(this.total, 'api:' + native);
        }
        /* the table a few times a second, the graph every frame */
        this.drawGraph();
        if (this.count % 10 === 1) this.drawTable();
    }

    lineOf(fn, pc) {
        const f = this.fns[fn];
        let line = 0;
        if (f) for (const [at, l] of f.lines) if (at <= pc) line = l;
        return line;
    }

    setView(v) {
        this.view = v;
        for (const [k, b] of Object.entries(this.viewButtons)) b.classList.toggle('on', k === v);
        this.drawTable();
    }

    draw() {
        this.drawGraph();
        this.drawTable();
    }

    /* ---- the frames' graph ---- */

    /* a bar a frame, its cost on the player, against the line of the time
     * a frame has; a frame over it red */
    drawGraph() {
        const cv = this.graph, w = cv.clientWidth, h = cv.height;
        if (!w) return;
        cv.width = w;
        const g = cv.getContext('2d'), css = getComputedStyle(cv);
        g.clearRect(0, 0, w, h);
        const n = Math.min(this.frames.length, Math.floor(w / 3));
        const shown = this.frames.slice(this.frames.length - n);
        const period = shown.length ? shown[shown.length - 1][1] : 30;
        const top = Math.max(period * 1.5, ...shown.map((f) => f[0]));
        const y = (ms) => h - 2 - (ms / top) * (h - 4);
        shown.forEach(([ms, p], i) => {
            g.fillStyle = ms > p ? css.getPropertyValue('--bad') : css.getPropertyValue('--accent');
            const yy = y(ms);
            g.fillRect(w - (n - i) * 3, yy, 2, h - 2 - yy);
        });
        g.strokeStyle = css.getPropertyValue('--dim');
        g.setLineDash([4, 3]);
        g.beginPath();
        g.moveTo(0, Math.round(y(period)) + 0.5);
        g.lineTo(w, Math.round(y(period)) + 0.5);
        g.stroke();
        this.drawSummary();
    }

    drawSummary() {
        const d = this.o.device(), f = this.frames;
        if (!this.count) {
            this.summary.textContent = this.on ? 'Recording: run the cart' : '';
            return;
        }
        const avg = f.reduce((a, x) => a + x[0], 0) / f.length, period = f[f.length - 1][1];
        const over = f.filter((x) => x[0] > x[1]).length;
        this.summary.textContent = this.count + ' frames; ' +
            (d.unit ? avg.toFixed(1) + ' ms of ' + period.toFixed(1) + ' on the ' + d.name +
                      ' (' + Math.round(100 * avg / period) + '%)' + (over ? ', ' + over + ' over' : '')
                    : Math.round(this.samples * SAMPLE_UNITS / this.count) + ' units a frame');
    }

    /* ---- the table ---- */

    /* each row: its name, where it is, its self and total as shares of the
     * samples, and its self as time a frame on the player */
    drawTable() {
        const t = this.table, d = this.o.device(), all = this.samples || 1;
        t.innerHTML = '';
        const head = (label, key, title) => {
            const c = document.createElement('div');
            c.className = 'prof-th' + (key ? ' sortable' + (this.sort === key ? ' on' : '') : '');
            c.textContent = label;
            if (title) c.title = title;
            if (key) c.onclick = () => { this.sort = key; this.drawTable(); };
            return c;
        };
        const lines = this.view === 'lines';
        t.classList.toggle('lines', lines);
        t.append(head(lines ? 'Line' : 'Function or call'), head('Where'),
                 head('Self', 'self', 'Samples in its own code'),
                 ...(lines ? [] : [head('Total', 'total', 'Samples in it or anything it called')]),
                 head(d.unit ? 'ms a frame' : 'Units a frame', null, 'Its self, on average, on the ' + d.name));
        const rows = lines ? this.lineRows() : this.functionRows();
        rows.sort((a, b) => (this.sort === 'total' && !lines ? b.total - a.total : b.self - a.self) ||
                            a.name.localeCompare(b.name));
        const perFrame = (n) => {
            const u = n * SAMPLE_UNITS / Math.max(1, this.count);
            return d.unit ? (u * d.unit).toFixed(2) : String(Math.round(u));
        };
        const pct = (n) => (100 * n / all).toFixed(1) + '%';
        for (const r of rows.slice(0, 200)) {
            const cells = [r.name, r.where, pct(r.self), ...(lines ? [] : [pct(r.total)]), perFrame(r.self)];
            cells.forEach((text, i) => {
                const c = document.createElement('div');
                c.className = 'prof-td' + (i >= 2 ? ' num' : '') + (r.api && i === 0 ? ' api' : '');
                c.textContent = text;
                if (i === 2) c.style.setProperty('--share', (r.self / all * 100) + '%');
                if (r.file) {
                    c.classList.add('link');
                    c.onclick = () => this.o.goTo(r.file, r.line);
                }
                t.append(c);
            });
        }
        /* the cart's prof() sections, timed exactly, as shares of all the
         * frames' units */
        if (this.count && !lines) {
            for (const [k, v] of [...this.sections].sort((a, b) => b[1] - a[1])) {
                const share = (100 * v / Math.max(1, this.units)).toFixed(1) + '%';
                const a = v / this.count;
                ['section ' + k, 'prof()', share, share, d.unit ? (a * d.unit).toFixed(2) : String(Math.round(a))]
                    .forEach((text, i) => {
                        const c = document.createElement('div');
                        c.className = 'prof-td' + (i >= 2 ? ' num' : i === 0 ? ' section' : '');
                        c.textContent = text;
                        t.append(c);
                    });
            }
        }
        /* sending the screen, after the cart's own */
        if (d.unit && this.count && !lines) {
            ['sending the screen', '', '', '', (this.screen / this.count).toFixed(2)].forEach((text, i) => {
                const c = document.createElement('div');
                c.className = 'prof-td' + (i ? ' num' : ' api');
                c.textContent = text;
                t.append(c);
            });
        }
        if (!rows.length) {
            const p = document.createElement('div');
            p.className = 'prof-none';
            p.textContent = this.on ? 'Nothing sampled yet.' : 'Press Record, then run the cart.';
            t.append(p);
        }
    }

    functionRows() {
        const out = [];
        for (const [key, self] of this.self) out.push(this.row(key, self, this.total.get(key) || self));
        for (const [key, total] of this.total) if (!this.self.has(key)) out.push(this.row(key, 0, total));
        return out;
    }

    row(key, self, total) {
        if (key === 'engine') return { name: 'the engine', where: '', self, total };
        const [kind, n] = key.split(':');
        if (kind === 'api') return { name: this.o.apiName(+n), where: '', self, total, api: true };
        const f = this.fns[+n] || { name: '?', lines: [] };
        const line = f.lines.length ? f.lines[0][1] : 0;
        return { name: f.name, where: f.file ? f.file + ':' + line : '', file: f.file, line, self, total };
    }

    lineRows() {
        const out = [];
        for (const [key, self] of this.lineSelf) {
            const [fn, line, native] = key.split(':').map(Number);
            const f = this.fns[fn] || { name: '?' };
            out.push({ name: f.file + ':' + line + (native >= 0 ? ', ' + this.o.apiName(native) : ''),
                       where: f.name, file: f.file, line, self });
        }
        return out;
    }
}
