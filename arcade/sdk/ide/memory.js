/* The memory view: what a running cart's heap holds, from the engine's
 * own walk of it (Engine.memory()), the same arena size as the player's.
 *
 *   new MemoryView(el)
 *   .begin(names)    a run starts, with the cart's globals' names by slot
 *   .show(m)         what Engine.memory() gave, drawn
 *
 * The heap's bytes are held (reached from a global or the engine),
 * garbage (dropped, until the next collection frees it) or free; an
 * allocation larger than the largest free block makes the engine collect,
 * and fails with "out of memory" if none is then large enough. Each
 * global is charged what it reaches that no earlier global does.
 *
 * Parts: the panel; the bar; the tables. */

const KIND_NAMES = { 2: 'strings', 3: 'arrays', 4: 'Int32Arrays', 5: 'Uint8Arrays',
                     6: 'records', 8: 'array and record storage', 10: 'images',
                     11: 'FixArrays', 12: 'lazy images' };
const GLOBALS_SHOWN = 40;

const kb = (n) => (n < 1024 ? n + ' B' : (n / 1024).toFixed(n < 10240 ? 1 : 0) + ' KB');

export class MemoryView {
    constructor(el) {
        this.el = el;
        el.classList.add('mem');
        this.head = document.createElement('div');
        this.head.className = 'mem-head';
        this.bar = document.createElement('div');
        this.bar.className = 'mem-bar';
        this.table = document.createElement('div');
        this.table.className = 'prof-table mem-table';
        el.append(this.head, this.bar, this.table);
        this.begin([]);
    }

    begin(names) {
        this.names = names;
        this.most = 0;
        this.head.textContent = '';
        this.bar.innerHTML = '';
        this.table.innerHTML = '<div class="prof-none">Run the cart to see what its memory holds.</div>';
    }

    show(m) {
        this.most = Math.max(this.most, m.held);
        this.head.textContent = 'Heap ' + kb(m.heap) + ' of the ' + kb(m.arena) +
            ' arena: ' + kb(m.held) + ' held (most ' + kb(this.most) + '), ' +
            kb(m.garbage) + ' garbage, ' + kb(m.free) + ' free; the largest free block ' +
            kb(m.largest) + '.';
        this.head.title = 'The rest of the arena, ' + kb(m.tables) +
            ', is the engine\'s state and the loaded cart: its code, constants, sheet, map and images.';
        this.bar.innerHTML = '';
        for (const [k, what] of [['held', 'held'], ['garbage', 'garbage, freed at the next collection'],
                                 ['free', 'free']]) {
            const s = document.createElement('span');
            s.className = 'mem-' + k;
            s.style.flexGrow = String(m[k]);
            s.title = kb(m[k]) + ' ' + what;
            this.bar.append(s);
        }

        const rows = [];
        const head = (a, b, c) => rows.push(['th', a], ['th', b], ['th', c]);
        const row = (name, count, bytes, of) => {
            rows.push(['td', name], ['td num', count === null ? '' : String(count)],
                      ['td num share', kb(bytes), of ? bytes / of : 0]);
        };
        head('Held, by kind', 'How many', 'Bytes');
        m.kinds.map((k, i) => ({ i, ...k })).filter((k) => k.count)
            .sort((a, b) => b.bytes - a.bytes)
            .forEach((k) => row(KIND_NAMES[k.i] || 'other', k.count, k.bytes, m.held));
        head('Held, by global', '', 'Bytes');
        const globals = [];
        for (let i = 0; i < this.names.length; i++)
            if (m.globals[i]) globals.push({ name: this.names[i], bytes: m.globals[i] });
        globals.sort((a, b) => b.bytes - a.bytes);
        globals.slice(0, GLOBALS_SHOWN).forEach((g) => row(g.name, null, g.bytes, m.held));
        if (globals.length > GLOBALS_SHOWN) {
            const rest = globals.slice(GLOBALS_SHOWN).reduce((a, g) => a + g.bytes, 0);
            row((globals.length - GLOBALS_SHOWN) + ' more', null, rest, m.held);
        }
        if (m.constants) row('the cart\'s strings', null, m.constants, m.held);
        if (m.engine) row('the engine: the target, the sheet, the menu', null, m.engine, m.held);

        this.table.innerHTML = '';
        for (const [cls, text, share] of rows) {
            const d = document.createElement('div');
            d.className = 'prof-' + cls;
            d.textContent = text;
            if (share !== undefined) d.style.setProperty('--share', (share * 100).toFixed(1) + '%');
            this.table.append(d);
        }
    }
}
