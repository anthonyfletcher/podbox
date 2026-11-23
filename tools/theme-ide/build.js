#!/usr/bin/env node
/* Builds the IDE's page into dist/: index.html, ide.js (src/ and the
 * libraries, bundled), skin-core.js, and tags.js -- every tag the firmware
 * has and the arguments each takes, read out of lib/skin_parser/tag_table.c
 * and apps-ipod/skin/custom_tags.c, so what the editor accepts is what the
 * player does. The player it runs is player/, which build.sh makes.
 *
 *   node build.js */

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const here = path.dirname(fileURLToPath(import.meta.url));
const dist = path.join(here, 'dist');
fs.mkdirSync(dist, { recursive: true });

await esbuild.build({
    entryPoints: [path.join(here, 'src', 'main.js')],
    bundle: true,
    format: 'iife',
    minify: true,
    target: ['es2020'],
    legalComments: 'eof',
    outfile: path.join(dist, 'ide.js')
});
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
