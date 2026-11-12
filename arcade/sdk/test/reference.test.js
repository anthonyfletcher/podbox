/* api.md, the reference the IDE's hints come from, against the functions
 * arcade_api.def defines: one entry each, and the arguments each takes. */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadDefs } from '../src/node.js';
import { parseReference } from '../src/reference.js';

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const ref = parseReference(fs.readFileSync(path.join(root, 'api.md'), 'utf8'));

/* every function a cart can call, as it calls it: the engine's, and the two
 * the compiler works out when building */
function functions() {
    const { api } = loadDefs();
    const all = { deg: { min: 1, max: 1 }, rgb: { min: 1, max: 1 } };
    for (const [name, def] of Object.entries(api.natives)) all[name] = def;
    for (const [ns, fns] of Object.entries(api.namespaces))
        for (const [name, def] of Object.entries(fns)) all[ns + '.' + name] = def;
    return all;
}

test('api.md has every function, and only those', () => {
    const all = functions();
    assert.deepEqual(Object.keys(all).filter((n) => !ref[n]).sort(), [], 'missing from api.md');
    assert.deepEqual(Object.keys(ref).filter((n) => !all[n]).sort(), [], 'in api.md but not a function');
});

test('each signature takes the arguments the compiler accepts', () => {
    const wrong = [];
    for (const [name, def] of Object.entries(functions())) {
        const r = ref[name];
        if (!r) continue;
        const max = r.max === Infinity ? def.max : r.max;
        if (r.min !== def.min || max !== def.max)
            wrong.push(r.sig + ' takes ' + r.min + '-' + r.max + ', the engine ' + def.min + '-' + def.max);
    }
    assert.deepEqual(wrong, []);
});

test('each entry says what the function does', () => {
    assert.deepEqual(Object.values(ref).filter((r) => !r.doc).map((r) => r.sig), []);
});
