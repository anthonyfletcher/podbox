/* The web page's two parts for src/webpage.js: play.html, with the text
 * font's licences in it (fonts/LICENSE-world.txt), and play.js bundled
 * into one script. pbgc's html command and the IDE's build both take them
 * from here. */

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));

export async function playerParts() {
    const esbuild = await import('esbuild');
    const out = await esbuild.build({
        entryPoints: [path.join(here, 'play.js')],
        bundle: true,
        format: 'iife',
        minify: true,
        write: false,
        target: ['es2020'],
        legalComments: 'eof'
    });
    const licences = fs.readFileSync(path.join(here, '..', 'fonts', 'LICENSE-world.txt'), 'utf8');
    if (/<\/script|<!--/i.test(licences)) throw new Error('LICENSE-world.txt would end its script element');
    const template = fs.readFileSync(path.join(here, 'play.html'), 'utf8')
        .replace('<script id="font-licences" type="text/plain"></script>',
                 () => '<script id="font-licences" type="text/plain">\n' + licences + '</script>');
    return { template: template, script: out.outputFiles[0].text };
}
