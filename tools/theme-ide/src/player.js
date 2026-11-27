/* The player: PodBox's own firmware, compiled to WebAssembly by build.sh.
 * Everything on its screen is the firmware's drawing -- menus, lists, the
 * quickscreen, Now Playing -- so a theme looks here exactly as on an iPod
 * Video.
 *
 * Its disk is the simulator's: player/rockboxui.data holds .rockbox without
 * fonts, the theme's files are written on top, a font the theme names but
 * does not carry is fetched from player/fonts/, and demo.js adds a library
 * of invented music. config.cfg is the first-boot defaults followed by the
 * theme's .cfg, so the player starts in the theme; after that apply() hands
 * it a .cfg to load, as choosing the theme on the player would.
 *
 * SDL draws on a canvas of its own, kept at 320x240 off the page: given the
 * visible one, it would size its window from the page's layout. Each frame
 * is copied to the visible canvas, which the page scales. Keys reach the
 * player only while that canvas has focus. */

import { install as installDemo } from './demo.js';

/* global FS, ENV, Module */

export const Player = {
    canvas: null,
    lcd: null,
    state: 'off',          /* off, loading, running, failed */
    note: '',
    busy: '',              /* what the player is waiting for while running */
    onState: null,         /* (state, note) */
    onOutput: null,        /* (line) */
    log: [],
    written: new Set(),    /* lower-cased rockbox paths of the theme's files */
    base: 'player/',

    /* "themes/x.cfg" for a theme folder's ".rockbox/themes/x.cfg"; null for a
       file outside .rockbox */
    rockboxPath(rel) {
        const p = rel.replace(/\\/g, '/');
        const at = p.toLowerCase().lastIndexOf('.rockbox/');
        return at >= 0 ? p.slice(at + '.rockbox/'.length) : null;
    },

    output(line) {
        this.log.push(line);
        if (this.log.length > 2000) this.log.splice(0, 500);
        if (this.onOutput) this.onOutput(line);
    },

    set(state, note, busy) {
        this.state = state;
        this.note = note || '';
        if (busy !== undefined) this.busy = busy;
        if (this.onState) this.onState(state, this.note);
    },

    copy() {
        if (this.canvas && this.lcd) {
            const g = this.canvas.getContext('2d');
            g.imageSmoothingEnabled = false;
            g.drawImage(this.lcd, 0, 0);
        }
        requestAnimationFrame(() => this.copy());
    },

    /* theme() -> { key, files: [{ path: rockbox path, data: Uint8Array }],
       cfg: a rockbox path }, asked as the player starts, so a theme opened
       while it loads is the one it starts with; key names the theme */
    async boot(canvas, theme) {
        if (this.state !== 'off') return;
        this.canvas = canvas;
        this.lcd = document.createElement('canvas');
        this.lcd.width = 320;
        this.lcd.height = 240;
        this.lcd.style.cssText = 'position:fixed;left:-10000px;top:0;width:320px;height:240px';
        document.body.appendChild(this.lcd);
        this.copy();
        this.set('loading', 'Starting the player');
        window.Module = {
            canvas: this.lcd,
            locateFile: (path) => this.base + path,
            print: (s) => this.output(s),
            printErr: (s) => this.output(s),
            onAbort: (r) => this.set('failed', 'The player stopped: ' + r),
            setStatus: (s) => {
                const m = /\((\d+)\/(\d+)\)/.exec(s || '');
                if (m) this.set('loading', 'Loading the player, ' + Math.floor(100 * m[1] / m[2]) + '%');
            },
            /* SDL's software renderer draws through a 2D context, so the canvas
               holds the player's exact pixels and a screenshot can read them */
            preRun: [() => {
                ENV.SDL_RENDER_DRIVER = 'software';
                ENV.SDL_EMSCRIPTEN_KEYBOARD_ELEMENT = '#' + canvas.id;
            }],
            onRuntimeInitialized: () => this.start(theme)
        };
        try {
            await this.script(this.base + 'disk.js');
            await this.script(this.base + 'rockboxui.js');
        } catch (e) {
            this.set('failed', 'The player is not built here: run build.sh (' + e.message + ')');
        }
    },

    script(url) {
        return new Promise((ok, fail) => {
            const s = document.createElement('script');
            s.src = url;
            s.onload = ok;
            s.onerror = () => fail(new Error(url + ' did not load'));
            document.head.appendChild(s);
        });
    },

    async start(theme) {
        await installDemo(this.base);
        let t;
        do {
            t = theme();
            await this.writeTheme(t.files);
        } while (theme().key !== t.key);
        if (!t.files.length) await this.fetchFonts(this.diskSkins());
        const cfg = t.cfg;
        if (cfg) {
            const read = (p) => { try { return FS.readFile(p, { encoding: 'utf8' }); } catch (e) { return ''; } };
            FS.writeFile('/simdisk/.rockbox/config.cfg',
                         read('/simdisk/.rockbox/default-config.cfg') + '\n' + read('/simdisk/.rockbox/' + cfg) + '\n');
        }
        this.set('running', '');
        try { Module.callMain(['--nobackground', '--screenshot']); }
        catch (e) { if (!(e && e.name === 'ExitStatus')) this.set('failed', String(e)); }
    },

    write(path, data) {
        const full = '/simdisk/.rockbox/' + path;
        let at = '';
        for (const d of full.split('/').slice(1, -1)) {
            at += '/' + d;
            try { FS.mkdir(at); } catch (e) { /* already there */ }
        }
        FS.writeFile(full, data);
        this.written.add(path.toLowerCase());
    },

    /* Every font a theme's text names that it does not carry, from the
       standard set. A name that is not one of them is the theme's own
       problem, and the player shows it the way an iPod would. */
    async fetchFonts(files) {
        const want = new Set();
        const dec = new TextDecoder();
        for (const f of files) {
            if (!/\.(cfg|wps|sbs|fms)$/i.test(f.path)) continue;
            for (const m of dec.decode(f.data).matchAll(/([A-Za-z0-9_.\- ]+\.fnt)\b/g))
                want.add(m[1].trim().split('/').pop());
        }
        await Promise.all([...want].map(async (name) => {
            if (this.written.has('fonts/' + name.toLowerCase())) return;
            try {
                const r = await fetch(this.base + 'fonts/' + encodeURIComponent(name));
                if (r.ok) this.write('fonts/' + name, new Uint8Array(await r.arrayBuffer()));
            } catch (e) { /* not a standard font */ }
        }));
    },

    /* the disk's own config and skins, as files for fetchFonts: with no
       theme open, these are what the player shows */
    diskSkins() {
        const out = [];
        const add = (p) => { try { out.push({ path: p, data: FS.readFile(p) }); } catch (e) { /* none */ } };
        add('/simdisk/.rockbox/config.cfg');
        for (const d of ['/simdisk/.rockbox/themes', '/simdisk/.rockbox/wps']) {
            let names = [];
            try { names = FS.readdir(d); } catch (e) { /* none */ }
            for (const n of names) add(d + '/' + n);
        }
        return out;
    },

    running() { return this.state === 'running'; },

    async writeTheme(files) {
        for (const f of files) this.write(f.path, f.data);
        await this.fetchFonts(files);
    },

    /* write theme files and have the player load the theme again */
    async update(files, cfg) {
        if (!this.running()) return;
        await this.writeTheme(files);
        this.apply(cfg);
    },

    apply(cfg) {
        if (this.running()) Module.ccall('theme_ide_apply', null, ['string'], [cfg ? '/.rockbox/' + cfg : '']);
    },

    /* settings lines ("shuffle: on") applied without loading the skins again */
    settings(lines) {
        if (!this.running()) return;
        FS.writeFile('/simdisk/.rockbox/theme-ide-state.cfg', lines.join('\n') + '\n');
        Module.ccall('theme_ide_settings', null, ['string'], ['/.rockbox/theme-ide-state.cfg']);
    },

    /* Play a folder of the player's disk ("/Music/Okapi/Concrete Bloom"),
       once the library is built: started sooner, playback holds the memory
       the database needs to be written, and the player's Music then asks
       for a restart. */
    async play(dir) {
        if (!this.running()) return;
        await this.libraryReady();
        Module.ccall('theme_ide_play', null, ['string'], [dir]);
    },

    async libraryReady() {
        for (let i = 0; i < 400; i++) {
            const fw = this.viewports('sbs');
            if (!fw || fw.library) break;
            if (!this.busy) this.set(this.state, this.note, 'Building the music library…');
            await new Promise((ok) => setTimeout(ok, 300));
        }
        if (this.busy) this.set(this.state, this.note, '');
    },

    /* the firmware's viewports for the statusbar skin ('sbs') or Now
       Playing ('wps'), and the activity on screen; see theme_ide.c */
    viewports(skin) {
        if (!this.running()) return null;
        try { return JSON.parse(Module.ccall('theme_ide_viewports', 'string', ['number'], [skin === 'wps' ? 1 : 0])); }
        catch (e) { return null; }
    },

    /* whether the browser is holding the player's sound back until a click */
    soundWaiting() {
        const c = window.Module && Module.SDL2 && Module.SDL2.audioContext;
        return !!c && c.state === 'suspended';
    },

    /* A button: menu, select, play, back, forward, or the wheel's up and down.
       ms holds it down; the player's long presses need about a second. */
    KEYS: {
        menu: ['Escape', 27], select: ['Enter', 13], play: [' ', 32],
        back: ['ArrowLeft', 37], forward: ['ArrowRight', 39],
        up: ['ArrowUp', 38], down: ['ArrowDown', 40]
    },
    key(type, button) {
        const k = this.KEYS[button];
        if (!k || !this.canvas) return;
        this.canvas.dispatchEvent(new KeyboardEvent(type, {
            key: k[0], code: k[0] === ' ' ? 'Space' : k[0], keyCode: k[1], which: k[1], bubbles: true }));
    },
    press(button, ms) {
        this.key('keydown', button);
        return new Promise((ok) => setTimeout(() => { this.key('keyup', button); ok(); }, ms || 80));
    },
    /* presses in turn, with time between for the screen they open */
    async sequence(buttons) {
        for (const b of buttons) {
            const [name, ms] = Array.isArray(b) ? b : [b, 80];
            await this.press(name, ms);
            await new Promise((ok) => setTimeout(ok, 450));
        }
    },

    shot() { return this.lcd ? this.lcd.toDataURL('image/png') : null; }
};
