/* The web page's two parts for src/webpage.js: play.html, with the
 * licences of the text font and of the engine's C library in it
 * (fonts/LICENSE-world.txt, wasm/LICENSE-libc.txt), and play.js bundled
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
    const licences = ['fonts/LICENSE-world.txt', 'wasm/LICENSE-libc.txt']
        .map((f) => fs.readFileSync(path.join(here, '..', f), 'utf8').replace(/\r/g, ''))
        .join('\n' + '='.repeat(78) + '\n\n');
    if (/<\/script|<!--/i.test(licences)) throw new Error('a licence would end its script element');
    const template = fs.readFileSync(path.join(here, 'play.html'), 'utf8')
        .replace('<script id="licences" type="text/plain"></script>',
                 () => '<script id="licences" type="text/plain">\n' + licences + '</script>');
    /* the page escapes </script in the script; a <!-- in it is refused,
     * since escaping one safely depends on where the minifier put it */
    if (out.outputFiles[0].text.includes('<!--'))
        throw new Error('play.js bundles with <!--, which would break its page');
    return { template: template, script: out.outputFiles[0].text };
}
