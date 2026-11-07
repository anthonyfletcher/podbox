/* The IDE builds into one page, with the engine, the compiler and the
 * editor in it. */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');

test('the IDE builds into one HTML file holding everything it needs', () => {
    const r = spawnSync(process.execPath, [path.join(root, 'ide', 'build.js')], { encoding: 'utf8' });
    assert.equal(r.status, 0, r.stderr);
    const page = fs.readFileSync(path.join(root, 'ide', 'dist', 'arcade-ide.html'), 'utf8');
    /* nothing fetched from elsewhere: no src= or href= to another file */
    assert.doesNotMatch(page, /<script[^>]+src=|<link[^>]+href=/);
    assert.match(page, /id="screen"/);
    /* the engine's definitions and the compiler's messages came in */
    assert.match(page, /API\(ui_choose/);
    assert.match(page, /let is not supported/);
});
