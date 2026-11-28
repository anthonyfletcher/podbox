/* The open theme: its files and where they live.
 *
 * A theme folder is the one holding .rockbox; every path here is relative to
 * it, spelt as the folder spells it. Two places a theme can come from:
 *
 *   serve.py    reads and writes the folder it was started on, and says
 *               when a file changes on disk (/api/tree, /api/file, /api/events)
 *   a folder    picked with an <input>, and written in place where the
 *               browser has file access (Chromium)
 *
 * Each source has list() -> [path], read(path) -> Uint8Array, and, when it
 * can, write(path, text). */

import { isText } from './names.js';

const SKIP = new Set(['.git', '.svn', 'node_modules', '__pycache__', '.idea']);

export class ServerSource {
    constructor(info) { this.info = info; this.writable = true; this.name = info.name; }
    static async connect() {
        try {
            const r = await fetch('/api/info', { cache: 'no-store' });
            return r.ok ? new ServerSource(await r.json()) : null;
        } catch (e) { return null; }
    }
    url(p) { return '/api/file/' + p.split('/').map(encodeURIComponent).join('/'); }
    async list() {
        const r = await fetch('/api/tree', { cache: 'no-store' });
        return (await r.json()).files.map((f) => f.path);
    }
    async read(p) {
        const r = await fetch(this.url(p), { cache: 'no-store' });
        if (!r.ok) throw new Error('could not read ' + p);
        return new Uint8Array(await r.arrayBuffer());
    }
    async write(p, text) {
        const r = await fetch(this.url(p), { method: 'PUT', body: text,
                                              headers: { 'Content-Type': 'text/plain; charset=utf-8' } });
        if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error || 'could not save ' + p);
    }
    /* disk changes, and ctl.py's commands, arrive on one stream */
    listen(onChange, onCommand) {
        const es = new EventSource('/api/events');
        es.onmessage = (ev) => {
            let msg; try { msg = JSON.parse(ev.data); } catch (e) { return; }
            if (msg.cmd) onCommand(msg.id, msg.cmd); else onChange(msg.changed || [], msg.removed || []);
        };
    }
}

/* Read from the <input>'s files; written through the folder's handle, asked
 * for at the first save. Trap: the browser's folder picker hides some file
 * types from a page -- Edge lists no .cfg -- so it must not do the reading. */
export class FileListSource {
    constructor(files) {
        this.writable = !!window.showDirectoryPicker;
        this.dir = null;
        this.files = new Map();
        for (const f of files) {
            const parts = f.webkitRelativePath.split('/').slice(1);
            if (!parts.some((p) => SKIP.has(p))) this.files.set(parts.join('/'), f);
        }
        this.name = files.length ? files[0].webkitRelativePath.split('/')[0] : 'theme';
    }
    async list() { return rooted(this, [...this.files.keys()]); }
    async read(p) { return new Uint8Array(await this.files.get(unrooted(this, p)).arrayBuffer()); }
    async write(p, text) {
        if (!this.dir) {
            let dir;
            try { dir = await window.showDirectoryPicker({ mode: 'readwrite' }); }
            catch (e) { throw new Error(e.name === 'AbortError' ? 'Not saved: no folder was chosen' : e.message); }
            if (dir.name !== this.name) throw new Error('Not saved: choose the theme\'s own folder, ' + this.name);
            this.dir = dir;
        }
        const parts = unrooted(this, p).split('/');
        let d = this.dir;
        for (const s of parts.slice(0, -1)) d = await d.getDirectoryHandle(s);
        const w = await (await d.getFileHandle(parts[parts.length - 1])).createWritable();
        await w.write(text);
        await w.close();
    }
}

/* A folder picked as .rockbox itself rather than the folder holding it: its
   paths are given the .rockbox/ they lack, and lose it again to be read. */
function rooted(source, paths) {
    source.prefix = '';
    if (!paths.some((p) => p.toLowerCase().startsWith('.rockbox/')) &&
        paths.some((p) => /^(wps|themes|fonts|icons|backdrops)\//i.test(p)))
        source.prefix = '.rockbox/';
    return paths.map((p) => source.prefix + p);
}
const unrooted = (source, p) => source.prefix && p.startsWith(source.prefix) ? p.slice(source.prefix.length) : p;

/* the theme: path -> { path, data, text, saved, dirty } */
export class Theme {
    constructor(source) { this.source = source; this.files = new Map(); this.name = source.name; }

    /* unreadable files are left out, and named in this.unreadable */
    async load() {
        const paths = await this.source.list();
        this.files.clear();
        this.unreadable = [];
        await Promise.all(paths.map(async (p) => {
            try { this.files.set(p, this.entry(p, await this.source.read(p))); }
            catch (e) { this.unreadable.push(p); }
        }));
    }

    entry(path, data) {
        const text = isText(path)
            ? new TextDecoder().decode(data).replace(/\r\n/g, '\n') : null;
        return { path, data, text, saved: text };
    }

    get(path) { return this.files.get(path); }
    find(re) { return [...this.files.keys()].filter((p) => re.test(p)); }
    dirty() { return [...this.files.values()].filter((f) => f.text !== null && f.text !== f.saved); }

    setText(path, text) {
        const f = this.files.get(path);
        if (!f) return;
        f.text = text;
        f.data = new TextEncoder().encode(text);
    }

    /* files added and removed on disk, leaving every other file as it is
       here, edits and all; true when anything changed */
    async sync() {
        const paths = new Set(await this.source.list());
        let any = false;
        for (const p of [...this.files.keys()])
            if (!paths.has(p)) { this.files.delete(p); any = true; }
        await Promise.all([...paths].filter((p) => !this.files.has(p)).map(async (p) => {
            try { this.files.set(p, this.entry(p, await this.source.read(p))); any = true; }
            catch (e) { /* unreadable: left out */ }
        }));
        return any;
    }

    async save(path) {
        const f = this.files.get(path);
        if (!f || f.text === null) return;
        if (!this.source.writable) throw new Error('this folder was opened read-only');
        const text = f.text;
        await this.source.write(path, text);
        f.saved = text;
    }

    /* A file changed on disk: 'taken', 'same' when it is what was last saved
       or read here (the IDE's own save, seen coming back), or 'kept' when
       there are edits here it would overwrite. force takes it regardless. */
    async reread(path, force) {
        const data = await this.source.read(path);
        const f = this.files.get(path);
        const e = this.entry(path, data);
        if (!force && f && f.text !== null) {
            if (e.text === f.saved) return 'same';
            if (f.text !== f.saved && e.text !== f.text) return 'kept';
        }
        this.files.set(path, e);
        return 'taken';
    }

    /* the .cfg that names a skin, or the one with the theme's name */
    cfgFor(skinPath) {
        const cfgs = this.find(/\.cfg$/i).filter((p) => !/(^|\/)config\.cfg$/i.test(p));
        const base = (skinPath || '').split('/').pop().toLowerCase();
        for (const c of cfgs) {
            const t = this.files.get(c).text || '';
            if (base && t.toLowerCase().split('\n').some((l) => /^\s*(wps|sbs|fms)\s*:/.test(l) &&
                                                                 l.toLowerCase().includes(base))) return c;
        }
        return cfgs[0] || null;
    }
}
