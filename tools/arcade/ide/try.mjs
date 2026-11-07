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
 * the editor's cursor at the word HERE. BROWSER names a
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
        ['fonts', 'fonts/', '.bdf'], ['images', 'images/', '.png']])
        for (const [n, v] of Object.entries(a[k] || {})) folder[dir + n + ext] = v;
    if (a.flags) folder['flags.json'] = a.flags;
    if (a.sheet) folder['sheet.png'] = a.sheet;
}
const enc = {};
for (const [k, v] of Object.entries(folder))
    enc[k] = typeof v === 'string' ? { t: v } : { b: Buffer.from(v).toString('base64') };

const script = `<script>
(async function () {
    while (!window.arcadeIde) await new Promise((r) => setTimeout(r, 20));
    await new Promise((r) => setTimeout(r, 1200));
    const enc = ${JSON.stringify(enc)};
    const pbg = ${JSON.stringify(pbg)};
    if (pbg) window.arcadeIde.openBytes(Uint8Array.from(atob(pbg), (c) => c.charCodeAt(0)), null);
    else if (Object.keys(enc).length) {
        const f = {};
        for (const [k, v] of Object.entries(enc))
            f[k] = v.t !== undefined ? v.t : Uint8Array.from(atob(v.b), (c) => c.charCodeAt(0));
        window.arcadeIde.load(f);
    }
    window.arcadeIde.run();
    for (const k of ${JSON.stringify(keyScript.split(',').filter(Boolean))}) {
        const [key, at] = k.split('@');
        if (key === 'Mark') {
            setTimeout(() => {
                const v = window.arcadeIde.view(), at = v.state.doc.toString().indexOf('HERE');
                v.focus();
                v.dispatch({ selection: { anchor: at }, scrollIntoView: true });
            }, +at);
            continue;
        }
        if (key.startsWith('Click:')) {
            setTimeout(() => document.querySelector(key.slice(6)).click(), +at);
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
const page = fs.readFileSync(path.join(here, 'dist', 'arcade-ide.html'), 'utf8').replace('</body>', () => script);
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-try-'));
const file = path.join(tmp, 'try.html');
fs.writeFileSync(file, page);
const common = ['--headless=new', '--disable-gpu', '--user-data-dir=' + path.join(tmp, 'profile'),
                '--window-size=1280,760', '--virtual-time-budget=' + (+ms + 1500),
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
