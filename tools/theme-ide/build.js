#!/usr/bin/env node
/* Builds the IDE's page into dist/: index.html, ide.js (src/ and the
 * libraries, bundled), skin-core.js, LICENSES.txt, and tags.js -- every tag
 * the firmware has and the arguments each takes, read out of
 * lib/skin_parser/tag_table.c and apps-ipod/skin/custom_tags.c, so what the
 * editor accepts is what the player does. The player it runs is player/,
 * which build.sh makes, with its own LICENSES.txt.
 *
 *   node build.js */

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const here = path.dirname(fileURLToPath(import.meta.url));
const dist = path.join(here, 'dist');
fs.mkdirSync(dist, { recursive: true });

const out = await esbuild.build({
    entryPoints: [path.join(here, 'src', 'main.js')],
    bundle: true,
    format: 'iife',
    minify: true,
    target: ['es2020'],
    legalComments: 'eof',
    metafile: true,
    outfile: path.join(dist, 'ide.js')
});

/* The licence of every package the bundle took code from, each text once
 * with the packages it covers: minifying drops most of their notices. */
const byText = new Map();
for (const input of Object.keys(out.metafile.inputs)) {
    const m = /node_modules\/((?:@[^/]+\/)?[^/]+)\//.exec(input.replace(/\\/g, '/'));
    if (!m) continue;
    const name = m[1], dir = path.join(here, 'node_modules', name);
    const lic = fs.readdirSync(dir).find((n) => /^licen[cs]e/i.test(n));
    if (!lic) throw new Error(name + ' has no licence file to put in LICENSES.txt');
    const text = fs.readFileSync(path.join(dir, lic), 'utf8').replace(/\r/g, '').trim();
    if (!byText.has(text)) byText.set(text, new Set());
    byText.get(text).add(name + ' ' + JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8')).version);
}
const rule = '='.repeat(78);
let licences = "The open-source code inside the Theme IDE's page, and its licences.\n" +
    "The player's are in player/LICENSES.txt.\n";
for (const [text, names] of byText)
    licences += '\n' + rule + '\n' + [...names].sort().join(', ') + '\n' + rule + '\n\n' + text + '\n';
fs.writeFileSync(path.join(dist, 'LICENSES.txt'), licences);
/* TAG(type, "name", "params", flags), as both tables write them; the params
 * may be a macro #defined to a string (BAR_PARAMS) */
const tags = (file) => {
    const out = {}, defs = {};
    const text = fs.readFileSync(path.join(here, '..', '..', file), 'utf8');
    for (const m of text.matchAll(/^#define\s+(\w+)\s+"([^"]*)"/gm)) defs[m[1]] = m[2];
    for (const m of text.matchAll(/^\s*TAG\(\s*\w+\s*,\s*"([^"]+)"\s*,\s*(?:"([^"]*)"|(\w+))/gm)) {
        const spec = m[2] !== undefined ? m[2] : defs[m[3]];
        if (spec === undefined) throw new Error(file + ': %' + m[1] + ' takes ' + m[3] + ', which is not a string');
        out[m[1]] = spec;
    }
    return out;
};
const firmware = { upstream: tags('lib/skin_parser/tag_table.c'), custom: tags('apps-ipod/skin/custom_tags.c') };
/* the highest screen id %cs can give: enum current_activity counts from 0 */
const activity = fs.readFileSync(path.join(here, '..', '..', 'apps-ipod/system/activity.h'), 'utf8');
const block = activity.slice(activity.indexOf('enum current_activity'));
const activityMax = (block.slice(0, block.indexOf('};')).match(/\bACTIVITY_\w+/g) || []).length - 1;
fs.writeFileSync(path.join(dist, 'tags.js'), '/* written by build.js */\nwindow.FIRMWARE_TAGS = ' +
                 JSON.stringify(firmware) + ';\nwindow.FIRMWARE_ACTIVITY_MAX = ' + activityMax + ';\n');
fs.copyFileSync(path.join(here, 'src', 'index.html'), path.join(dist, 'index.html'));
fs.copyFileSync(path.join(here, 'skin-core.js'), path.join(dist, 'skin-core.js'));
console.log(dist + ': ' + Math.round(fs.statSync(path.join(dist, 'ide.js')).size / 1024) + ' KB of script');
