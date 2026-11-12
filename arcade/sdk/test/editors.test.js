/* A cart's own editors (ide/editors.js): which data file each edits, the
 * bridge and the policy put ahead of the page, and leave asked again for
 * a page that changed. The frame itself is the browser's: tried in the
 * IDE with ide/try.mjs. */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { editorOf, isEditor, framed, starterEditor, CART_FILE } from '../ide/editors.js';
import { compileFiles } from '../src/node.js';
import { unpackCart } from '../src/unpack.js';
import zlib from 'node:zlib';

test('a data file is edited by the editor of its name, if the cart has one', () => {
    const folder = { 'data/phrases.json': '[]', 'data/story.txt': '', 'editors/phrases.html': '<html></html>' };
    assert.equal(editorOf(folder, 'data/phrases.json'), 'editors/phrases.html');
    assert.equal(editorOf(folder, 'data/story.txt'), null);
    assert.equal(editorOf(folder, 'maps/phrases.json'), null);
    assert.ok(isEditor('editors/phrases.html'));
    assert.ok(!isEditor('editors/a/b.html') && !isEditor('editors/x.js'));
    /* an editor may write the cart's files, never an editor */
    assert.ok(CART_FILE.test('data/phrases.json') && CART_FILE.test('main.js'));
    assert.ok(!CART_FILE.test('editors/phrases.html') && !CART_FILE.test('../main.js'));
});

test('the page gets the no-network policy and window.cart before its own scripts', () => {
    const page = framed('<!doctype html><html><head><title>x</title><script>go()</script></head></html>',
                        'data/a</script>.json');
    const policy = page.indexOf('Content-Security-Policy'), own = page.indexOf('go()');
    assert.ok(page.startsWith('<!doctype html>\n<meta http-equiv="Content-Security-Policy"'));
    assert.ok(policy < own && page.indexOf('<!doctype', 1) < 0);
    assert.match(page, /default-src 'none'/);
    assert.ok(page.indexOf('window.cart') < own);
    /* the file's name cannot close the bridge's script */
    assert.ok(!page.includes('a</script>.json'));
    assert.match(framed('<p>no head</p>', null), /^<!doctype html>\n<meta http-equiv="Content-Security-Policy"/);
    /* a <head> in a comment ahead of the real one does not move it */
    const tricked = framed('<!-- <head> --><script>go()</script><head></head>', null);
    assert.ok(tricked.indexOf('Content-Security-Policy') < tricked.indexOf('<!--'));
});

test('a cart carries its editors, and unpack gives them back', () => {
    const html = starterEditor('phrases');
    const res = compileFiles({ 'cart.json': '{"id":"00000000000ed171","name":"E"}',
                               'main.js': 'function init() {}\nfunction update() {}\n' },
                             { data: { phrases: '[]\n' }, editors: { phrases: html } });
    assert.deepEqual(res.errors, []);
    const files = unpackCart(res.cart, (b) => new Uint8Array(zlib.inflateRawSync(b)),
                             (b) => new Uint8Array(zlib.deflateSync(b)));
    assert.equal(new TextDecoder().decode(files['editors/phrases.html']), html);
});
