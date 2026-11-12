/* Looking at the IDE without a person: a copy of the built page with a
 * cart folder loaded and run, screenshot by headless Edge after `ms` of
 * the page's time, with what the console then says.
 *
 *   node ide/try.mjs out.png [cart folder] [ms] [key script]
 *
 * The key script is the page's own: "Enter@500,ArrowDown@800" presses
 * Enter at 500 ms of the run and the wheel's down at 800; "Turn@1000"
 * drags a quarter turn clockwise round the drawn wheel, and
 * "Click:<selector>@t" clicks what the selector finds, and "Mark@t" puts
 * the editor's cursor at the word HERE, and "PasteImage@t" pastes a
 * 24x16 rainbow, its bottom rows clear, as another program would;
 * "PasteEmpty@t" pastes with nothing on the clipboard, and
 * "Key:<selector>:<key>[:ctrl]@t" presses a key on what the selector finds,
 * "Drag:<selector>:fx0;fy0;fx1;fy1@t" drags across it, by fractions,
 * "RightClick:<selector>:fx;fy@t" right-clicks a point of it,
 * "Hover:<selector>:fx;fy[:alt]@t" moves the pointer over one, and
 * "Choose:<selector>:<value>@t" sets a choice or an input's value, and
 * "Text:<selector>@t" prints what one holds: its text, then its inputs'
 * values ([placeholder] where empty, # before a disabled one). WINDOW
 * sets the window's size, "1280,760" when unset. BROWSER names a
 * Chromium-family browser other than Edge; only its --screenshot and
 * --dump-dom are used, so a browser whose remote control is shut off by
 * policy still serves. */

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { readFolder, readAssets } from '../src/node.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const [out, cartDir, ms = '2000', keyScript = ''] = process.argv.slice(2);
const EDGE = process.env.BROWSER || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';

const folder = {};
/* a .pbg is opened as the IDE's Open does */
const pbg = cartDir && /.pbg$/i.test(cartDir) ? fs.readFileSync(cartDir).toString('base64') : null;
if (cartDir && !pbg) {
    Object.assign(folder, readFolder(cartDir));
    const a = readAssets(cartDir);
    for (const [k, dir, ext] of [['palettes', 'palettes/', '.gpl'], ['maps', 'maps/', '.json'],
        ['sounds', 'sounds/', '.json'], ['data', 'data/', '.json'], ['texts', 'data/', '.txt'],
        ['fonts', 'fonts/', '.bdf'], ['images', 'images/', '.png'], ['editors', 'editors/', '.html']])
        for (const [n, v] of Object.entries(a[k] || {})) folder[dir + n + ext] = v;
    if (a.flags) folder['flags.json'] = a.flags;
    if (a.objects) folder['objects.json'] = a.objects;
    if (a.sheet) folder['sheet.png'] = a.sheet;
    if (a.logo) folder['logo.png'] = a.logo;
}
const enc = {};
for (const [k, v] of Object.entries(folder))
    enc[k] = typeof v === 'string' ? { t: v } : { b: Buffer.from(v).toString('base64') };

const script = `<script>
(async function () {
    while (!window.arcadeIde) await new Promise((r) => setTimeout(r, 20));
    await new Promise((r) => setTimeout(r, 1200));
    const enc = ${JSON.stringify(enc).replace(/</g, '\\u003c')};
    const pbg = ${JSON.stringify(pbg)};
    if (pbg) window.arcadeIde.openBytes(Uint8Array.from(atob(pbg), (c) => c.charCodeAt(0)), null);
    else if (Object.keys(enc).length) {
        const f = {};
        for (const [k, v] of Object.entries(enc))
            f[k] = v.t !== undefined ? v.t : Uint8Array.from(atob(v.b), (c) => c.charCodeAt(0));
        window.arcadeIde.load(f);
    }
    window.arcadeIde.run();
    /* fn once sel is in the page: what a step acts on may come late */
    const there = (sel, fn, tries = 100) => {
        const el = document.querySelector(sel);
        if (el) fn(el); else if (tries > 0) setTimeout(() => there(sel, fn, tries - 1), 50);
    };
    for (const k of ${JSON.stringify(keyScript.split(',').filter(Boolean))}) {
        const [key, at] = k.split('@');
        if (key.startsWith('Key:')) {
            /* Key:<selector>:<key>[:ctrl] */
            const [, sel, name, mod] = key.split(':');
            setTimeout(() => there(sel, (el) => {
                el.focus();
                el.dispatchEvent(new KeyboardEvent('keydown', { key: name, ctrlKey: mod === 'ctrl', bubbles: true }));
            }), +at);
            continue;
        }
        if (key.startsWith('Drag:')) {
            /* Drag:<selector>:fx0;fy0;fx1;fy1, as fractions of its box */
            const [, sel, spec] = key.split(':');
            const [fx0, fy0, fx1, fy1] = spec.split(';').map(Number);
            setTimeout(() => {
                const el = document.querySelector(sel), r = el.getBoundingClientRect();
                /* to what is under the point, as a real pointer's go */
                const ev = (type, fx, fy) => {
                    const x = r.left + r.width * fx, y = r.top + r.height * fy;
                    (document.elementFromPoint(x, y) || el).dispatchEvent(new PointerEvent(type,
                        { bubbles: true, pointerId: 1, clientX: x, clientY: y }));
                };
                ev('pointerdown', fx0, fy0);
                ev('pointermove', (fx0 + fx1) / 2, (fy0 + fy1) / 2);
                ev('pointermove', fx1, fy1);
                ev('pointerup', fx1, fy1);
            }, +at);
            continue;
        }
        if (key.startsWith('Hover:')) {
            /* Hover:<selector>:fx;fy[:alt], the pointer moved over a point of it */
            const [, sel, spec, mod] = key.split(':');
            const [fx, fy] = spec.split(';').map(Number);
            setTimeout(() => {
                const el = document.querySelector(sel), r = el.getBoundingClientRect();
                el.dispatchEvent(new PointerEvent('pointermove', { bubbles: true, pointerId: 1, altKey: mod === 'alt',
                    clientX: r.left + r.width * fx, clientY: r.top + r.height * fy }));
            }, +at);
            continue;
        }
        if (key.startsWith('RightClick:')) {
            /* RightClick:<selector>:fx;fy, as fractions of its box */
            const [, sel, spec] = key.split(':');
            const [fx, fy] = spec.split(';').map(Number);
            setTimeout(() => {
                const el = document.querySelector(sel), r = el.getBoundingClientRect();
                const x = r.left + r.width * fx, y = r.top + r.height * fy;
                for (const type of ['pointerdown', 'pointerup'])
                    el.dispatchEvent(new PointerEvent(type, { bubbles: true, pointerId: 1, button: 2,
                                                             clientX: x, clientY: y }));
            }, +at);
            continue;
        }
        if (key.startsWith('Choose:')) {
            /* Choose:<select selector>:<value>, the value after the last colon */
            const sel = key.slice(7, key.lastIndexOf(':')), value = key.slice(key.lastIndexOf(':') + 1);
            setTimeout(() => there(sel, (el) => {
                el.value = value;
                el.dispatchEvent(new Event('input', { bubbles: true }));
                el.dispatchEvent(new Event('change', { bubbles: true }));
            }), +at);
            continue;
        }
        if (key.startsWith('Text:')) {
            /* what the selector finds, its text and its inputs' values and
             * placeholders, into the console */
            setTimeout(() => {
                const el = document.querySelector(key.slice(5));
                const d = document.createElement('div');
                d.textContent = !el ? 'none' : el.textContent.replace(/\\s+/g, ' ') + ' | ' +
                    [...el.querySelectorAll('input')].map((i) =>
                        (i.disabled ? '#' : '') + (i.value || '[' + i.placeholder + ']')).join(' ');
                document.getElementById('console').append(d);
            }, +at);
            continue;
        }
        if (key === 'Dump') {
            /* the cart's files and the open editor's state, into the console */
            setTimeout(() => {
                const d = document.createElement('div');
                const ed = document.querySelector('.sheet-edit, #editor');
                d.textContent = 'files: ' + Object.keys(window.arcadeIde.folder()).join(' ') +
                    ' | canvases: ' + (ed ? ed.querySelectorAll('canvas').length : -1) +
                    ' | sizes: ' + [...(ed ? ed.querySelectorAll('canvas') : [])].map((c) => c.width + 'x' + c.height).join(',') +
                    ' | cursor: ' + [...document.querySelectorAll('.pix-over')].map((c) => {
                        const s = c.style.cursor, m = /path%20d%3D%22([^%]{0,10})/.exec(s);
                        return m ? 'icon(' + m[1] + ') ' + s.slice(s.lastIndexOf(')') + 2) : s;
                    }).join(',');
                document.getElementById('console').append(d);
            }, +at);
            continue;
        }
        if (key === 'PasteEmpty') {
            setTimeout(() => {
                document.querySelector('.pix').focus();
                document.dispatchEvent(new ClipboardEvent('paste', { clipboardData: new DataTransfer() }));
            }, +at);
            continue;
        }
        if (key === 'PasteImage') {
            setTimeout(async () => {
                const c = document.createElement('canvas');
                c.width = 24; c.height = 16;
                const g = c.getContext('2d');
                for (let x = 0; x < 24; x++) { g.fillStyle = 'hsl(' + x * 15 + ',80%,55%)'; g.fillRect(x, 0, 1, 16); }
                g.clearRect(0, 12, 24, 4);
                const blob = await new Promise((d) => c.toBlob(d, 'image/png'));
                const dt = new DataTransfer();
                dt.items.add(new File([blob], 'p.png', { type: 'image/png' }));
                document.querySelector('.pix').focus();
                document.dispatchEvent(new ClipboardEvent('paste', { clipboardData: dt }));
            }, +at);
            continue;
        }
        if (key === 'Mark') {
            setTimeout(() => {
                const v = window.arcadeIde.view(), at = v.state.doc.toString().indexOf('HERE');
                v.focus();
                v.dispatch({ selection: { anchor: at }, scrollIntoView: true });
            }, +at);
            continue;
        }
        if (key.startsWith('Click:')) {
            setTimeout(() => there(key.slice(6), (el) => setTimeout(() => el.click(), 150)), +at);
            continue;
        }
        if (key === 'Turn') {
            setTimeout(() => {
                const w = document.getElementById('wheel'), r = w.getBoundingClientRect();
                const at = (t) => ({ clientX: r.left + r.width / 2 + Math.sin(t * 2 * Math.PI) * r.width * 0.35,
                                     clientY: r.top + r.height / 2 - Math.cos(t * 2 * Math.PI) * r.height * 0.35,
                                     pointerId: 1, bubbles: true });
                w.dispatchEvent(new PointerEvent('pointerdown', at(0)));
                for (let i = 1; i <= 10; i++)
                    setTimeout(() => w.dispatchEvent(new PointerEvent('pointermove', at(i * 0.025))), i * 30);
                setTimeout(() => w.dispatchEvent(new PointerEvent('pointerup', at(0.25))), 400);
            }, +at);
            continue;
        }
        setTimeout(() => {
            const s = document.getElementById('screen');
            s.dispatchEvent(new KeyboardEvent('keydown', { key: key }));
            setTimeout(() => s.dispatchEvent(new KeyboardEvent('keyup', { key: key })), 120);
        }, +at);
    }
})();
</script></body>`;
/* at the page's own </body>, the last: the web page template inside the
 * IDE's script has one too */
const ide = fs.readFileSync(path.join(here, 'dist', 'arcade-ide.html'), 'utf8');
const end = ide.lastIndexOf('</body>');
const page = ide.slice(0, end) + script + ide.slice(end + '</body>'.length);
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-try-'));
const file = path.join(tmp, 'try.html');
fs.writeFileSync(file, page);
const common = ['--headless=new', '--disable-gpu', '--user-data-dir=' + path.join(tmp, 'profile'),
                '--window-size=' + (process.env.WINDOW || '1280,760'), '--virtual-time-budget=' + (+ms + 1500),
                ...(process.env.LIGHT ? ['--blink-settings=preferredColorScheme=1'] : [])];
execFileSync(EDGE, [...common, '--screenshot=' + path.resolve(out), 'file:///' + file.replace(/\\/g, '/')],
             { stdio: 'ignore' });
const dom = execFileSync(EDGE, [...common, '--dump-dom', 'file:///' + file.replace(/\\/g, '/')],
                         { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'], maxBuffer: 64 << 20 });
const text = (id) => {
    const m = new RegExp('id="' + id + '"[^>]*>([\\s\\S]*?)</div></div>|id="' + id + '"[^>]*>([\\s\\S]*?)</(div|span)>').exec(dom);
    return m ? (m[1] || m[2] || '').replace(/<[^>]+>/g, '\n').replace(/\n+/g, '\n').trim() : '';
};
console.log('status: ' + text('status'));
console.log('meter: ' + text('meter-text'));
console.log('console:\n' + text('console'));
