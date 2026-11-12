#!/usr/bin/env node
/* The build action's contents, made from this checkout:
 *
 *   node arcade/builder/build.mjs <out dir>
 *
 * writes <out dir>/pbgc.mjs -- the SDK's compiler as one file, with its
 * npm packages and the engine's .def files built in, so it runs on any
 * Node with nothing installed -- beside action.yml, README.md and the
 * licences of what it carries. The compiler is then checked: every cart in
 * arcade/games and the template's starter cart must build to the same
 * bytes as the SDK's own pbgc.js builds them, or nothing is left in
 * <out dir> and this fails.
 *
 * The build action only builds carts. pbgc html is left out: it needs the
 * web player, which the IDE exports instead. */

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const sdk = path.join(root, 'arcade', 'sdk');
const out = process.argv[2];
if (!out) {
    console.error('usage: node arcade/builder/build.mjs <out dir>');
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
    /* pbgc html's player: not carried */
    external: ['./web/bundle.js'],
    banner: { js: '#!/usr/bin/env node\n/* PodBox Arcade cart compiler: built from arcade/sdk by ' +
                  'arcade/builder/build.mjs. GPL-2.0-only; see LICENSE and THIRD-PARTY.txt. */' },
    legalComments: 'none',
    logLevel: 'warning'
});
for (const f of ['action.yml', 'README.md'])
    fs.copyFileSync(path.join(here, f), path.join(out, f));
/* the licences of what the bundle carries, from the packages themselves */
const notices = [
    ['acorn', path.join(sdk, 'node_modules', 'acorn', 'LICENSE')],
    ['fflate', path.join(sdk, 'node_modules', 'fflate', 'LICENSE')],
    ['sfxr (the sound generator in src/sfxr.js)', path.join(sdk, 'src', 'LICENSE-sfxr.txt')]
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
fs.rmSync(tmp, { recursive: true, force: true });
if (bad) {
    fs.rmSync(out, { recursive: true, force: true });
    console.error('build.mjs: the one-file compiler builds ' + bad + ' cart(s) differently');
    process.exit(1);
}
console.log(bundle + ': ' + Math.round(fs.statSync(bundle).size / 1024) + ' KB');
