/* The engine, wasm/arcade.wasm, driven from JavaScript: pbgrun's and the
 * IDE's. The host's calls arrive as imports and go to `host`, an object
 * of the functions below, any left out answering as the player does with
 * nothing there:
 *
 *   print(text)               a printh line
 *   date()                    [year, month, day, hour, minute, second, weekday]
 *   click(beep)               the wheel's tick, or its beep
 *   setting(name)             sys.setting: a number, a string, or undefined
 *   musicSay(line)            what a cart asked of the made-up music:
 *                             "seek 30000", "control 1"
 *   frame()                   a dialog's next tick: { held, wheel }, or null
 *                             to back out of it; called with no dialog open
 *                             (done) when it is over
 *   say(text, queue), speechBusy(), speechStop(), speechOn()
 *   storeRead(name)           Uint8Array or undefined
 *   storeWrite(name, bytes)   true when kept
 *   storeRemove(name)         true when there was one
 *   storeList()               the names, sorted
 *   glyph(cp)                 13 bytes of FONT.WORLD glyph, or undefined
 *   watch(name, value)        a watch() call, both as text
 *   prof(name)                a prof() section opening, or with null the
 *                             innermost ending; unitsNow() reads the units
 *
 * frame() may also return a Promise, in the IDE, where the next tick comes
 * from the page: the engine then pauses inside the call (Asyncify) and
 * tick() returns a Promise too, settled once the dialog lets the tick end.
 */

export class Engine {
    constructor(wasmModule, host) {
        this.host = host || {};
        const imports = { env: this.imports() };
        this.inst = new WebAssembly.Instance(wasmModule, imports);
        this.x = this.inst.exports;
        this.pending = null;
    }

    get mem() { return new Uint8Array(this.x.memory.buffer); }
    get view() { return new DataView(this.x.memory.buffer); }

    cstr(ptr, len) {
        const m = this.mem;
        let end = ptr;
        if (len === undefined) while (m[end]) end++;
        else end = ptr + len;
        return new TextDecoder().decode(m.subarray(ptr, end));
    }

    /* text into memory at ptr, at most cap bytes with its end */
    put(ptr, cap, s) {
        const b = new TextEncoder().encode(s).subarray(0, Math.max(0, cap - 1));
        this.mem.set(b, ptr);
        this.mem[ptr + b.length] = 0;
    }

    imports() {
        const h = () => this.host;
        return {
            print: (p, n) => h().print && h().print(this.cstr(p, n)),
            date: (out) => {
                const d = h().date ? h().date() : [2026, 11, 3, 12, 0, 0, 2];
                for (let i = 0; i < 7; i++) this.view.setInt32(out + i * 4, d[i], true);
            },
            click: (beep) => h().click && h().click(beep),
            setting: (name, num, buf, cap) => {
                const v = h().setting ? h().setting(this.cstr(name)) : undefined;
                if (typeof v === 'number') { this.view.setInt32(num, v, true); return 1; }
                if (typeof v === 'string') { this.put(buf, cap, v); return 2; }
                return 0;
            },
            music_say: (p, n) => h().musicSay && h().musicSay(this.cstr(p, n)),
            watch: (n, v) => h().watch && h().watch(this.cstr(n), this.cstr(v)),
            prof: (n) => h().prof && h().prof(n ? this.cstr(n) : null),
            frame: (held, wheel) => this.frame(held, wheel),
            say: (t, q) => h().say && h().say(this.cstr(t), q),
            speech_busy: () => (h().speechBusy && h().speechBusy() ? 1 : 0),
            speech_stop: () => h().speechStop && h().speechStop(),
            speech_on: () => (h().speechOn && h().speechOn() ? 1 : 0),
            store_read: (name, buf, cap) => {
                const b = h().storeRead && h().storeRead(this.cstr(name));
                if (!b) return -1;
                if (cap > 0) this.mem.set(b.subarray(0, Math.min(cap, b.length)), buf);
                return cap > 0 ? Math.min(cap, b.length) : b.length;
            },
            store_write: (name, buf, n) =>
                (h().storeWrite && h().storeWrite(this.cstr(name), this.mem.slice(buf, buf + n)) ? 1 : 0),
            store_remove: (name) => (h().storeRemove && h().storeRemove(this.cstr(name)) ? 1 : 0),
            store_list: (names, max) => {
                const all = h().storeList ? h().storeList() : [];
                const n = Math.min(all.length, max);
                for (let i = 0; i < n; i++) this.put(names + i * 33, 33, all[i]);
                return n;
            },
            glyph: (cp, out) => {
                const g = h().glyph && h().glyph(cp);
                if (!g) return 0;
                this.mem.set(g.subarray(0, 13), out);
                return 1;
            }
        };
    }

    /* env.frame: answered at once, or, for a Promise, the engine unwound
     * here and rewound into it with the answer */
    frame(held, wheel) {
        const x = this.x;
        if (x.asyncify_get_state() === 2) {
            /* rewinding: the answer the Promise gave */
            x.asyncify_stop_rewind();
            const r = this.answer;
            this.answer = null;
            return this.give(r, held, wheel);
        }
        if (!this.host.frame) return held ? 0 : 1;
        const r = this.host.frame(held !== 0);
        if (r && typeof r.then === 'function') {
            this.pending = r;
            x.asyncify_start_unwind(this.asyncData);
            return 0;
        }
        return this.give(r, held, wheel);
    }

    give(r, held, wheel) {
        if (!held) return 1;
        if (!r) return 0;
        this.view.setUint32(held, r.held >>> 0, true);
        this.view.setInt32(wheel, r.wheel | 0, true);
        return 1;
    }

    /* fn called again until no frame() is waiting: its result, or a
     * Promise of it when one was */
    run(fn) {
        const r = fn();
        if (this.x.asyncify_get_state() !== 1) return r;
        this.x.asyncify_stop_unwind();
        const p = this.pending;
        this.pending = null;
        return p.then((answer) => {
            this.answer = answer;
            this.x.asyncify_start_rewind(this.asyncData);
            return this.run(fn);
        });
    }

    load(cart, seed = 1n) {
        const x = this.x;
        if (cart.length > x.web_cart_size()) return false;
        this.mem.set(cart, x.web_cart());
        this.asyncData = x.web_async_data();
        const s = BigInt(seed);
        return x.web_load(cart.length, Number(s & 0xffffffffn), Number(s >> 32n)) !== 0;
    }

    start() { return this.run(() => this.x.web_start() !== 0); }
    /* the cart closing: its ending(), as the player runs it */
    end() { return this.run(() => this.x.web_end() !== 0); }
    /* ms: how far the music has played since the last tick */
    tick(held, clicks, pos, draw = true, ms = 0) {
        return this.run(() => this.x.web_tick(held, clicks, pos, draw ? 1 : 0, ms | 0) !== 0);
    }

    /* Before load: whether playback.playing() and beat report music (as
     * pbgvm's --playing and --beat), the music playing at first, and one
     * tempo for every track (0, each its own) */
    music(o) {
        this.x.web_music(o.playingFn ? 1 : 0, o.beat ? 1 : 0, o.playing ? 1 : 0, o.bpm | 0);
    }
    musicControl(what) { this.x.web_music_control(what); }

    /* what is playing: { id, ms, length, bpm, playing, title, artist, album } */
    nowPlaying() {
        const p = this.x.web_music_now(), v = new Int32Array(this.x.memory.buffer, p, 5);
        const text = (k) => this.cstr(this.x.web_music_text(k));
        return { id: v[0], ms: v[1], length: v[2], bpm: v[3], playing: v[4] !== 0,
                 title: text(0), artist: text(1), album: text(2) };
    }
    idle() { this.x.web_idle(); }
    rate() { return this.x.web_rate(); }
    menuIsGame() { return this.x.web_menu_is_game() !== 0; }
    units() { return this.x.web_units() >>> 0; }
    /* ---- the pause menu's contents, and what a host keeps between runs */

    /* records go through the module's buffer: in before a restore, out after a pack */
    recordIn(bytes) { this.mem.set(bytes, this.x.web_record()); }
    recordOut(n) { const p = this.x.web_record(); return this.mem.slice(p, p + n); }

    /* menuitem(i)'s label, 1 to 5, or null; choosing it runs its function */
    menuItem(i) { const p = this.x.web_menu_item(i); return p ? this.cstr(p) : null; }
    menuChoose(i) { return this.run(() => this.x.web_menu_choose(i)); }

    option(i) { return this.x.web_option(i); }
    setOption(i, v) { this.x.web_option_set(i, v | 0); }
    options() { return this.recordOut(this.x.web_options_pack()); }
    setOptions(bytes) { this.recordIn(bytes); this.x.web_options_unpack(bytes.length); }

    /* the scores record, and whether a score was recorded since the last call */
    scores() {
        const bytes = this.recordOut(this.x.web_scores_pack());
        return { bytes, dirty: this.x.web_scores_dirty() !== 0 };
    }
    setScores(bytes) { this.recordIn(bytes); return this.x.web_scores_restore(bytes.length) !== 0; }
    /* board i: { title, rows: [[value, name, date]] }, or null */
    board(i) {
        const p = this.x.web_board(i);
        if (!p) return null;
        const [title, ...rows] = this.cstr(p).split('\n');
        return { title, rows: rows.map((r) => r.split('\t')) };
    }
    /* achievement i: { title, text, date } (date 0 not yet), or null */
    achievement(i) {
        const p = this.x.web_achievement(i);
        if (!p) return null;
        const [title, text, date] = this.cstr(p).split('\n');
        return { title, text, date: +date };
    }
    /* score.show() since the last call: { board, rank }, or null */
    scoreShown() { const v = this.x.web_score_shown(); return v < 0 ? null : { board: v >> 8, rank: v & 255 }; }
    /* an achievement newly earned, by index, or -1 */
    achieved() { return this.x.web_achieved(); }
    quitAsked() { return this.x.web_quit_asked() !== 0; }

    /* the 256 saved numbers, and whether any changed since the last call */
    saves() { const dirty = this.x.web_saves_pack() !== 0; return { bytes: this.recordOut(1056), dirty }; }
    setSaves(bytes) { this.recordIn(bytes); this.x.web_saves_restore(); }

    /* inside a call, from the host's prof(): the units so far in it */
    unitsNow() { return this.x.web_units_now() >>> 0; }
    /* What the arena holds, between ticks: the totals as struct
     * arcade_memory has them, kinds[tag] = { count, bytes } and globals[slot]
     * the bytes each global reaches that no earlier one does. Nothing the
     * cart sees changes. */
    memory() {
        const p = this.x.web_memory() >>> 2, u = new Uint32Array(this.x.memory.buffer);
        const names = ['arena', 'tables', 'heap', 'held', 'garbage', 'free', 'largest',
                       'constants', 'engine'];
        const m = { kinds: [], globals: null };
        names.forEach((k, i) => { m[k] = u[p + i]; });
        const KINDS = 16;
        for (let k = 0; k < KINDS; k++)
            m.kinds.push({ bytes: u[p + names.length + k], count: u[p + names.length + KINDS + k] });
        const g = this.x.web_memory_globals() >>> 2;
        m.globals = u.slice(g, g + 65536);
        return m;
    }

    /* sampling for the profiler, every 2048 units, on or off */
    profile(on) { this.x.web_profile(on ? 1 : 0); }
    /* the samples since the last take, 12 words each: the native call (or
     * -1), the offset in the innermost function, the depth, the outermost
     * function, then the innermost eight, innermost first (-1 past the
     * depth) */
    samples() {
        const n = this.x.web_prof_take(), p = this.x.web_prof_data();
        return new Int32Array(this.mem.buffer.slice(p, p + n * 48));
    }
    ceiling(u) { this.x.web_ceiling(u); }
    skipped(n) { this.x.web_skipped(n); }
    /* the game paused, as resumed() reports */
    paused() { this.x.web_paused(); }
    error() { return this.cstr(this.x.web_error()); }
    dialog() { return this.cstr(this.x.web_dialog()); }
    dirty() { const d = this.x.web_dirty(); return [d >> 16, (d << 16) >> 16]; }
    paletteChanged() { return this.x.web_palette_changed() !== 0; }

    /* the screen: 160x120 RGBA, and its palette numbers */
    rgba() { const p = this.x.web_screen(); return this.mem.slice(p, p + 160 * 120 * 4); }
    pixels() { const p = this.x.web_pixels(); return this.mem.slice(p, p + 160 * 120); }

    /* frames of stereo 16-bit sound at rate Hz */
    audio(frames, rate) {
        const p = this.x.web_audio(frames, rate);
        return new Int16Array(this.x.memory.buffer.slice(p, p + frames * 4));
    }
}

/* host.glyph from font.bin, as gen_textfont.py --file lays it out: 256
 * pages of an offset and a bitmap of the codepoints present, each page's
 * glyphs 13 bytes apiece. As on the player, a file that is not PBTF
 * version 1 with its whole page table has no glyphs, and a glyph running
 * past the file's end is none. */
export function fontGlyphs(file) {
    const dv = new DataView(file.buffer, file.byteOffset, file.byteLength);
    if (file.length < 8 + 256 * 36 || new TextDecoder().decode(file.subarray(0, 4)) !== 'PBTF' ||
        file[4] !== 1)
        return () => undefined;
    return (cp) => {
        if (cp > 0xffff || cp === 0) return undefined;
        const at = 8 + (cp >> 8) * 36, off = dv.getUint32(at, true), lo = cp & 0xff;
        const has = (i) => file[at + 4 + (i >> 3)] & (1 << (i & 7));
        if (!off || !has(lo)) return undefined;
        let k = 0;
        for (let i = 0; i < lo; i++) if (has(i)) k++;
        if (off + k * 13 + 13 > file.length) return undefined;
        return file.slice(off + k * 13, off + k * 13 + 13);
    };
}
