#!/usr/bin/env node
/* The build action's contents, made from this checkout:
 *
 *   node arcade/builder/build.mjs <out dir> <font.bin>
 *
 * writes <out dir>/pbgc.mjs -- the SDK's compiler as one file, with its
 * npm packages and the engine's .def files built in, so it runs on any
 * Node with nothing installed -- beside action.yml, README.md and the
 * licences of what it carries. For pbgc html, the web player goes beside
 * it made: player.json (web/bundle.js's parts, which take esbuild to
 * make), wasm/arcade.wasm (wasm/build.sh's, which must be there) and
 * font.bin, the text font, for a cart whose text needs it, with the
 * LICENSE-font.txt and LICENSES-font beside it in the firmware zip.
 *
 * The compiler is then checked: every cart in arcade/games and the
 * template's starter cart must build to the same bytes as the SDK's own
 * pbgc.js builds them, and the starter cart's page too, or nothing is left
 * in <out dir> and this fails. */

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const sdk = path.join(root, 'arcade', 'sdk');
const [out, font] = process.argv.slice(2);
if (!out || !font) {
    console.error('usage: node arcade/builder/build.mjs <out dir> <font.bin>');
    process.exit(2);
}

const esbuild = await import(pathToFileURL(path.join(sdk, 'node_modules', 'esbuild', 'lib', 'main.js')).href)
    .then((m) => m.default || m);

fs.rmSync(out, { recursive: true, force: true });
fs.mkdirSync(out, { recursive: true });
const bundle = path.join(out, 'pbgc.mjs');
await esbuild.build({
    entryPoints: [path.join(here, 'entry.js')],
    outfile: bundle,
    bundle: true,
    platform: 'node',
    format: 'esm',
    target: 'node20',
    loader: { '.def': 'text' },
    /* pbgc html's player, read from player.json beside the bundle rather
     * than made with esbuild */
    plugins: [{
        name: 'player',
        setup(b) {
            b.onResolve({ filter: /web[\/]bundle\.js$/ }, (a) => ({ path: a.path, namespace: 'player' }));
            b.onLoad({ filter: /.*/, namespace: 'player' }, () => ({
                contents: [
                    "import fs from 'node:fs';",
                    'export async function playerParts() {',
                    "    return JSON.parse(fs.readFileSync(new URL('./player.json', import.meta.url), 'utf8'));",
                    '}'
                ].join('\n'),
                loader: 'js'
            }));
        }
    }],
    banner: { js: '#!/usr/bin/env node\n/* PodBox Arcade cart compiler: built from arcade/sdk by ' +
                  'arcade/builder/build.mjs. GPL-2.0-only; see LICENSE and THIRD-PARTY.txt. */' },
    legalComments: 'none',
    logLevel: 'warning'
});
for (const f of ['action.yml', 'README.md'])
    fs.copyFileSync(path.join(here, f), path.join(out, f));
const { playerParts } = await import(pathToFileURL(path.join(sdk, 'web', 'bundle.js')).href);
fs.writeFileSync(path.join(out, 'player.json'), JSON.stringify(await playerParts()));
fs.mkdirSync(path.join(out, 'wasm'));
fs.copyFileSync(path.join(sdk, 'wasm', 'arcade.wasm'), path.join(out, 'wasm', 'arcade.wasm'));
fs.copyFileSync(font, path.join(out, 'font.bin'));
/* the font's licences, as the firmware zip carries them beside it */
fs.cpSync(path.join(path.dirname(font), 'LICENSES-font'), path.join(out, 'LICENSES-font'),
          { recursive: true });
/* the licences of what the bundle carries, from the packages themselves */
const notices = [
    ['acorn', path.join(sdk, 'node_modules', 'acorn', 'LICENSE')],
    ['fflate', path.join(sdk, 'node_modules', 'fflate', 'LICENSE')],
    ['sfxr (the sound generator in src/sfxr.js)', path.join(sdk, 'src', 'LICENSE-sfxr.txt')],
    ['wasi-libc, in wasm/arcade.wasm and the pages pbgc html makes', path.join(sdk, 'wasm', 'LICENSE-libc.txt')],
    ['font.bin: Fusion Pixel (the fonts it draws on: LICENSES-font)',
     path.join(path.dirname(font), 'LICENSE-font.txt')]
];
const rule = '='.repeat(72);
fs.writeFileSync(path.join(out, 'THIRD-PARTY.txt'),
    'pbgc.mjs carries code from these projects, under these licences.\n' +
    notices.map(([name, file]) => `\n${rule}\n${name}\n${rule}\n\n` +
                fs.readFileSync(file, 'utf8').trim() + '\n').join(''));

/* the check: each cart built both ways, byte for byte */
const carts = [path.join(here, '..', 'template', 'mygame')];
for (const d of fs.readdirSync(path.join(root, 'arcade', 'games')))
    carts.push(path.join(root, 'arcade', 'games', d));
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-builder-'));
let bad = 0;
for (const dir of carts) {
    if (!fs.existsSync(path.join(dir, 'cart.json'))) continue;
    const name = path.basename(dir);
    const a = path.join(tmp, name + '.sdk.pbg'), b = path.join(tmp, name + '.one.pbg');
    execFileSync(process.execPath, [path.join(sdk, 'pbgc.js'), 'build', dir, '-o', a], { stdio: 'ignore' });
    execFileSync(process.execPath, [bundle, 'build', dir, '-o', b], { stdio: 'ignore' });
    const same = Buffer.compare(fs.readFileSync(a), fs.readFileSync(b)) === 0;
    console.log((same ? 'same  ' : 'DIFFER') + '  ' + name);
    if (!same) bad++;
}
/* and a page, the starter cart's, both ways */
{
    const dir = path.join(here, '..', 'template', 'mygame');
    const a = path.join(tmp, 'page.sdk.html'), b = path.join(tmp, 'page.one.html');
    execFileSync(process.execPath, [path.join(sdk, 'pbgc.js'), 'html', dir, '-o', a], { stdio: 'ignore' });
    execFileSync(process.execPath, [bundle, 'html', dir, '-o', b], { stdio: 'ignore' });
    const same = Buffer.compare(fs.readFileSync(a), fs.readFileSync(b)) === 0;
    console.log((same ? 'same  ' : 'DIFFER') + '  mygame.html');
    if (!same) bad++;
}
fs.rmSync(tmp, { recursive: true, force: true });
if (bad) {
    fs.rmSync(out, { recursive: true, force: true });
    console.error('build.mjs: the one-file compiler builds ' + bad + ' cart(s) differently');
    process.exit(1);
}
console.log(bundle + ': ' + Math.round(fs.statSync(bundle).size / 1024) + ' KB');
