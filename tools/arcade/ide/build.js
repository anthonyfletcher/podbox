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

const here = path.dirname(fileURLToPath(import.meta.url));
const wasm = path.join(here, '..', 'wasm', 'arcade.wasm');
if (!fs.existsSync(wasm)) {
    console.error('no wasm/arcade.wasm: build it with sh wasm/build.sh');
    process.exit(1);
}

const out = await esbuild.build({
    entryPoints: [path.join(here, 'main.js')],
    bundle: true,
    format: 'iife',
    minify: true,
    write: false,
    target: ['es2020'],
    loader: { '.def': 'text', '.md': 'text', '.wasm': 'binary' },
    legalComments: 'eof'
});
const js = out.outputFiles[0].text.replace(/<\/script/gi, '<\\/script');
const f = process.argv.indexOf('--font');
const font = f > 0 ? '<script>window.ARCADE_FONT = "' +
    fs.readFileSync(process.argv[f + 1]).toString('base64') + '";</script>' : '';
const page = fs.readFileSync(path.join(here, 'index.html'), 'utf8')
    .replace('<script>/*SCRIPT*/</script>', () => font + '<script>' + js + '</script>');
fs.mkdirSync(path.join(here, 'dist'), { recursive: true });
const file = path.join(here, 'dist', 'arcade-ide.html');
fs.writeFileSync(file, page);
console.log(file + ': ' + Math.round(page.length / 1024) + ' KB');
