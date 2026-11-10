#!/usr/bin/env node
/* Builds the IDE into one HTML file, ide/dist/arcade-ide.html: its code,
 * the libraries, the engine's definitions and wasm/arcade.wasm all inside
 * it, so it works offline and opened from disk.
 *
 *   node ide/build.js [--font font.bin]
 *
 * --font puts FONT.WORLD's kana, Hangul and ideographs in too (the file
 * bundle-font.sh makes); without it they draw as ?. */

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { playerParts } from '../web/bundle.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const wasm = path.join(here, '..', 'wasm', 'arcade.wasm');
if (!fs.existsSync(wasm)) {
    console.error('no wasm/arcade.wasm: build it with sh wasm/build.sh');
    process.exit(1);
}

/* the web page a cart exports as, its template and script, as the module
 * arcade:player */
const player = await playerParts();
const playerModule = {
    name: 'arcade-player',
    setup(b) {
        b.onResolve({ filter: /^arcade:player$/ }, () => ({ path: 'player', namespace: 'arcade' }));
        b.onLoad({ filter: /.*/, namespace: 'arcade' }, () => ({
            contents: 'export default ' + JSON.stringify(player) + ';', loader: 'js'
        }));
    }
};

const out = await esbuild.build({
    plugins: [playerModule],
    entryPoints: [path.join(here, 'main.js')],
    bundle: true,
    format: 'iife',
    minify: true,
    write: false,
    target: ['es2020'],
    loader: { '.def': 'text', '.md': 'text', '.bdf': 'text', '.wasm': 'binary' },
    legalComments: 'eof',
    metafile: true
});
/* nothing in the script may end it, or open a comment that keeps the
 * next </script> from ending it */
const safe = (s) => s.replace(/<\/script/gi, '<\\/script').replace(/<!--/g, '<\\!--');
const js = safe(out.outputFiles[0].text);
const f = process.argv.indexOf('--font');
const font = f > 0 ? '<script>window.ARCADE_FONT = "' +
    fs.readFileSync(process.argv[f + 1]).toString('base64') + '";</script>' : '';

/* The licence of every package the bundle took code from, each text once
 * with the packages it covers, then sfxr's, the text font's and the
 * engine's C library's: in the page as text, since minifying drops most
 * of their notices. */
const modules = path.join(here, '..', 'node_modules');
const byText = new Map();
for (const input of Object.keys(out.metafile.inputs)) {
    const m = /node_modules\/((?:@[^/]+\/)?[^/]+)\//.exec(input.replace(/\\/g, '/'));
    if (!m) continue;
    const name = m[1], dir = path.join(modules, name);
    const lic = fs.readdirSync(dir).find((n) => /^licen[cs]e/i.test(n));
    if (!lic) throw new Error(name + ' has no licence file to put in the page');
    const text = fs.readFileSync(path.join(dir, lic), 'utf8').replace(/\r/g, '').trim();
    if (!byText.has(text)) byText.set(text, new Set());
    byText.get(text).add(name + ' ' + JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8')).version);
}
const rule = '='.repeat(78);
let licences = 'The open-source code inside ArcadeIDE, and its licences.\n';
for (const [text, names] of byText)
    licences += '\n' + rule + '\n' + [...names].sort().join(', ') + '\n' + rule + '\n\n' + text + '\n';
for (const file of [path.join(here, '..', 'src', 'LICENSE-sfxr.txt'),
                    path.join(here, '..', 'fonts', 'LICENSE-world.txt'),
                    path.join(here, '..', 'wasm', 'LICENSE-libc.txt')])
    licences += '\n' + rule + '\n\n' + fs.readFileSync(file, 'utf8').replace(/\r/g, '');

const page = fs.readFileSync(path.join(here, 'index.html'), 'utf8')
    .replace('<script>/*SCRIPT*/</script>', () => '<script id="licences" type="text/plain">\n' + safe(licences) +
             '</script>\n' + font + '<script>' + js + '</script>');
fs.mkdirSync(path.join(here, 'dist'), { recursive: true });
const file = path.join(here, 'dist', 'arcade-ide.html');
fs.writeFileSync(file, page);
console.log(file + ': ' + Math.round(page.length / 1024) + ' KB');
