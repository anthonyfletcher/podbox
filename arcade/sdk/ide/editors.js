/* A cart's own editors: editors/<name>.html, a page the cart carries that
 * edits its data -- data/<name>.json or .txt -- in the IDE, as the map and
 * sound editors edit theirs.
 *
 * Such a page is someone else's code, so it runs walled off: in a frame
 * sandboxed to scripts alone (an origin of its own, so none of the IDE's
 * storage), under a policy that lets it reach no network, and only after
 * the owner has said yes to that page. It reaches the cart through
 * window.cart, a few messages to the IDE:
 *
 *   cart.file            the file it was opened for, or null
 *   cart.read(name)      a Promise of the file: text, or a Uint8Array for
 *                        a picture; undefined when there is none
 *   cart.write(name, v)  a Promise, settled once it is written; refused
 *                        for a name no cart folder holds, or an editor
 *   cart.list()          a Promise of every file's name
 *   cart.run(files, ticks)
 *                        a Promise of { lines, error }: the cart built and
 *                        run unseen, for up to ticks ticks or until it
 *                        prints "done", and every line it printed. Its code
 *                        is the .js files named in files and no others --
 *                        each the text to build with, or true for the
 *                        cart's own -- and any other file named is used in
 *                        place of the cart's. How an editor checks its data
 *                        by the cart's own rules, rather than a copy of them
 *
 * Parts: which editor a file has; leave to run one; the page in its frame;
 * a new editor's starting page. */

import { isBinary } from '../src/folder.js';

/* the files a cart folder holds, as the IDE keeps them */
export const CART_FILE =
    /^((palettes|maps|images|sounds|data|fonts)\/)?[^/]+\.(js|json|png|gpl|txt|bdf)$/;
const EDITOR = /^editors\/([A-Za-z_]\w*)\.html$/;

/* ---- which editor a file has --------------------------------------------- */

export const isEditor = (file) => EDITOR.test(file);

/* the editor of a data file, by its name, or null */
export function editorOf(folder, file) {
    const m = /^data\/([A-Za-z_]\w*)\.(json|txt)$/.exec(file);
    const e = m && 'editors/' + m[1] + '.html';
    return e && folder[e] !== undefined ? e : null;
}

/* ---- leave to run one ------------------------------------------------------ */

const OK_KEY = 'arcade-editors-ok';

/* SHA-256 of the page where the browser has it, else FNV-1a: what the
 * owner said yes to, so a page changed since asks again */
async function digest(text) {
    const bytes = new TextEncoder().encode(text);
    if (globalThis.crypto && crypto.subtle) {
        const h = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
        return Array.from(h, (b) => b.toString(16).padStart(2, '0')).join('');
    }
    let h = 0x811c9dc5;
    for (const b of bytes) h = Math.imul(h ^ b, 0x01000193) >>> 0;
    return 'fnv' + h.toString(16) + '-' + bytes.length;
}

function approvals() {
    try { return JSON.parse(localStorage.getItem(OK_KEY)) || []; } catch (e) { return []; }
}

export async function approved(text) {
    return approvals().includes(await digest(text));
}

/* the page let run from now on; the last 200 are kept */
export async function approve(text) {
    const d = await digest(text), all = approvals().filter((x) => x !== d);
    all.push(d);
    try { localStorage.setItem(OK_KEY, JSON.stringify(all.slice(-200))); } catch (e) { /* asked again */ }
}

/* ---- the page in its frame ------------------------------------------------- */

/* Ahead of the page's own: no network, and window.cart */
function bridge(file) {
    return '<meta http-equiv="Content-Security-Policy" content="default-src \'none\'; ' +
        'script-src \'unsafe-inline\'; style-src \'unsafe-inline\'; img-src data: blob:; ' +
        'font-src data:; media-src data: blob:">\n' +
        '<script>(function () {\n' +
        '  var n = 0, waiting = {};\n' +
        '  addEventListener("message", function (e) {\n' +
        '    var m = e.data;\n' +
        '    if (e.source !== parent || !m || !waiting[m.id]) return;\n' +
        '    var w = waiting[m.id];\n' +
        '    delete waiting[m.id];\n' +
        '    if (m.ok) w[0](m.value); else w[1](new Error(m.error));\n' +
        '  });\n' +
        '  function ask(op, name, value) {\n' +
        '    return new Promise(function (done, fail) {\n' +
        '      var id = ++n;\n' +
        '      waiting[id] = [done, fail];\n' +
        '      parent.postMessage({ arcadeEditor: 1, id: id, op: op, name: name, value: value }, "*");\n' +
        '    });\n' +
        '  }\n' +
        '  window.cart = Object.freeze({ file: ' + JSON.stringify(file).replace(/</g, '\\u003c') + ',\n' +
        '    read: function (name) { return ask("read", name); },\n' +
        '    write: function (name, value) { return ask("write", name, value); },\n' +
        '    list: function () { return ask("list"); },\n' +
        '    run: function (files, ticks) { return ask("run", null, { files: files, ticks: ticks }); } });\n' +
        '})();</script>\n';
}

/* the page with the bridge ahead of everything it holds: the parser puts
 * the bridge in the head whatever the page's own markup, and no comment or
 * script of the page's can come before it */
export function framed(html, file) {
    return '<!doctype html>\n' + bridge(file) + html.replace(/^﻿?\s*<!doctype[^>]*>/i, '');
}

/* An editor's page in a frame in pane. o: folder (the cart's files, read
 * and written in place), file (the file it is for, or null), html,
 * written(name, isNew) after the page writes one, and run(files, ticks), a
 * Promise of { lines, error } for cart.run(). destroy() takes it away. */
export class CartEditor {
    constructor(pane, o) {
        this.o = o;
        this.frame = document.createElement('iframe');
        this.frame.className = 'cart-editor';
        this.frame.setAttribute('sandbox', 'allow-scripts');
        this.frame.setAttribute('referrerpolicy', 'no-referrer');
        this.frame.srcdoc = framed(o.html, o.file);
        this.onMessage = (e) => this.answer(e);
        addEventListener('message', this.onMessage);
        pane.append(this.frame);
    }

    answer(e) {
        if (e.source !== this.frame.contentWindow) return;
        const m = e.data;
        if (!m || m.arcadeEditor !== 1) return;
        const reply = (ok, value, error) =>
            this.frame.contentWindow.postMessage({ id: m.id, ok: ok, value: value, error: error }, '*');
        const folder = this.o.folder;
        try {
            if (m.op === 'list') return reply(true, Object.keys(folder).sort());
            if (m.op === 'run') {
                const v = m.value || {};
                if (!v.files || typeof v.files !== 'object') return reply(false, null, 'cart.run() needs the files to run');
                for (const [n, f] of Object.entries(v.files))
                    if (!CART_FILE.test(n) || (f !== true && (isBinary(n) ? !(f instanceof Uint8Array) : typeof f !== 'string')))
                        return reply(false, null, n + ' cannot be run with');
                this.o.run(v.files, v.ticks | 0).then((r) => reply(true, r), (x) => reply(false, null, String(x && x.message || x)));
                return;
            }
            const name = String(m.name);
            if (m.op === 'read') {
                const v = folder[name];
                return reply(true, v === undefined ? undefined : isBinary(name) ? new Uint8Array(v) : String(v));
            }
            if (m.op === 'write') {
                if (!CART_FILE.test(name)) return reply(false, null, name + ' is not a file a cart holds');
                const v = m.value;
                if (isBinary(name) ? !(v instanceof Uint8Array) : typeof v !== 'string')
                    return reply(false, null, name + (isBinary(name) ? ' takes a Uint8Array' : ' takes text'));
                const isNew = folder[name] === undefined;
                folder[name] = isBinary(name) ? new Uint8Array(v) : v;
                this.o.written(name, isNew);
                return reply(true);
            }
            reply(false, null, 'no such request: ' + m.op);
        } catch (x) { reply(false, null, String(x && x.message || x)); }
    }

    destroy() {
        removeEventListener('message', this.onMessage);
        this.frame.remove();
    }
}

/* ---- a new editor's starting page ------------------------------------------ */

/* data/<name>.json shown as text to edit, written back by Save: a page to
 * build an editor of the cart's own from */
export function starterEditor(name) {
    return `<!doctype html>
<html>
<head>
<meta charset="utf-8">
<style>
  body { margin: 0; padding: 12px; background: #1e1f24; color: #e6e4de;
         font: 13px/1.4 system-ui, sans-serif; display: flex; flex-direction: column;
         gap: 8px; height: 100vh; box-sizing: border-box; }
  textarea { flex: 1; background: #15161a; color: inherit; border: 1px solid #3a3b43;
             font: 12px/1.4 ui-monospace, monospace; padding: 8px; }
  button { align-self: flex-start; }
  .bad { color: #ff7b8b; }
</style>
</head>
<body>
<div>Editing <b id="name"></b>: change it, then Save.</div>
<textarea id="text" spellcheck="false"></textarea>
<div><button id="save">Save</button> <span id="said"></span></div>
<script>
  // The cart's own editor for data/${name}.json. It runs in the IDE,
  // reaching the cart through window.cart: read, write and list.
  var file = cart.file || "data/${name}.json";
  document.getElementById("name").textContent = file;
  cart.read(file).then(function (t) { document.getElementById("text").value = t || "{\\n}\\n"; });
  document.getElementById("save").onclick = function () {
    var t = document.getElementById("text").value, said = document.getElementById("said");
    try { JSON.parse(t); } catch (e) { said.textContent = e.message; said.className = "bad"; return; }
    cart.write(file, t).then(function () { said.textContent = "Saved."; said.className = ""; },
                             function (e) { said.textContent = e.message; said.className = "bad"; });
  };
</script>
</body>
</html>
`;
}
