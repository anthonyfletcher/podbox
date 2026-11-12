/* The manual in arcade/manual: every link between its pages reaches a
 * page, and an anchor a heading on it, as GitHub names them; every picture
 * it shows is there; and pictures are stored compressed -- the host's
 * screenshots are not, at about 230 KB each. */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const manual = path.join(path.dirname(fileURLToPath(import.meta.url)),
                         '..', '..', 'manual');

function pages(dir) {
    const out = [];
    for (const f of fs.readdirSync(dir)) {
        const p = path.join(dir, f);
        if (fs.statSync(p).isDirectory()) out.push(...pages(p));
        else if (f.endsWith('.md')) out.push(path.resolve(p));
    }
    return out;
}

/* a heading's anchor, as GitHub makes it */
const slug = (h) => h.replace(/`/g, '').trim().toLowerCase()
    .replace(/[^\p{L}\p{N} _-]/gu, '').replace(/ /g, '-');

function anchors(text) {
    const seen = {}, out = new Set();
    let code = false;
    for (const line of text.split('\n')) {
        if (/^\s*```/.test(line)) code = !code;
        const m = !code && /^#+\s+(.*)/.exec(line);
        if (!m) continue;
        const s = slug(m[1]), n = seen[s] || 0;
        seen[s] = n + 1;
        out.add(n ? s + '-' + n : s);
    }
    return out;
}

test('every link in the manual reaches its page and heading', () => {
    const all = pages(manual), heads = {};
    for (const p of all) heads[p] = anchors(fs.readFileSync(p, 'utf8'));
    const bad = [];
    for (const p of all) {
        const text = fs.readFileSync(p, 'utf8').replace(/```[\s\S]*?```/g, '');
        for (const [, url] of text.matchAll(/\]\(([^)\s]+)\)/g)) {
            if (/^[a-z]+:/.test(url)) continue;
            const [file, anchor] = url.split('#');
            const to = file ? path.resolve(path.dirname(p), decodeURI(file)) : p;
            const where = path.relative(manual, p) + ': ' + url;
            if (!fs.existsSync(to)) bad.push(where + ' (no such file)');
            else if (anchor && to.endsWith('.md') && !heads[to]?.has(anchor))
                bad.push(where + ' (no such heading)');
        }
    }
    assert.deepEqual(bad, []);
});

test('the manual\'s pictures are stored compressed', () => {
    const img = path.join(manual, 'img');
    const big = fs.readdirSync(img)
        .filter((f) => fs.statSync(path.join(img, f)).size > 100 * 1024);
    assert.deepEqual(big, [], 'store these as indexed or optimised PNGs');
});
