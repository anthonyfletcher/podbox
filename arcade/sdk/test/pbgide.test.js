/* pbgide: the IDE's page served from this computer, and a cart folder read
 * and written for it -- only with the token its address carries, only for
 * this host, and only a cart's own files. */

import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const host = path.join(here, '..', 'host');
const exe = path.join(host, process.platform === 'win32' ? 'pbgide.exe' : 'pbgide');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arcade-pbgide-'));
const cart = path.join(tmp, 'cart');
const page = path.join(tmp, 'ide.html');
let server, base, token;

before(async () => {
    const r = spawnSync('sh', [path.join(host, 'build.sh')], { encoding: 'utf8' });
    assert.equal(r.status, 0, 'building failed:\n' + r.stdout + r.stderr);
    fs.mkdirSync(path.join(cart, 'images'), { recursive: true });
    fs.writeFileSync(path.join(cart, 'cart.json'), '{ "id": "0123456789abcdef", "name": "T" }\n');
    fs.writeFileSync(path.join(cart, 'main.js'), 'function init() {}\nfunction update() {}\n');
    fs.writeFileSync(path.join(cart, 'images', 'a.png'), Buffer.from([137, 80, 78, 71, 0, 255]));
    fs.writeFileSync(path.join(cart, 'notes.doc'), 'not a cart file');
    fs.writeFileSync(page, '<!doctype html><title>IDE</title>');
    const port = 20000 + Math.floor(Math.random() * 20000);
    server = spawn(exe, ['--no-browser', '--port', String(port), '--ide', page, cart]);
    const url = await new Promise((done, fail) => {
        let out = '';
        server.stdout.on('data', (d) => {
            out += d;
            const m = /(http:\/\/127\.0\.0\.1:\d+)\/\?token=([0-9a-f]+)/.exec(out);
            if (m) done(m);
        });
        server.on('exit', () => fail(new Error('pbgide stopped: ' + out)));
    });
    base = url[1];
    token = url[2];
});

after(() => { if (server) server.kill(); });

const ask = (p, init = {}, withToken = true) =>
    fetch(base + p, Object.assign({}, init, {
        headers: Object.assign(withToken ? { 'X-Arcade-Token': token } : {}, init.headers) }));

test('the page is served to anyone on this computer, the files only with the token', async () => {
    const r = await ask('/?token=' + token, {}, false);
    assert.equal(r.status, 200);
    assert.match(await r.text(), /<title>IDE<\/title>/);
    assert.equal((await ask('/cart/list', {}, false)).status, 403);
    assert.equal((await ask('/cart/list', { headers: { 'X-Arcade-Token': 'f'.repeat(32) } }, false)).status, 403);
});

test('the list is the cart\'s own files, and each reads back as it is', async () => {
    const list = await (await ask('/cart/list')).json();
    assert.equal(list.name, 'cart');
    assert.deepEqual(list.files.map((f) => f.path).sort(), ['cart.json', 'images/a.png', 'main.js']);
    const png = new Uint8Array(await (await ask('/cart/file?path=images%2Fa.png')).arrayBuffer());
    assert.deepEqual([...png], [137, 80, 78, 71, 0, 255]);
});

test('a write lands in the folder, a new folder made for it, and a delete removes it', async () => {
    const body = new Uint8Array([1, 2, 3, 0, 250]);
    assert.equal((await ask('/cart/file?path=sounds%2Fz.json', { method: 'PUT', body })).status, 200);
    assert.deepEqual([...fs.readFileSync(path.join(cart, 'sounds', 'z.json'))], [1, 2, 3, 0, 250]);
    assert.equal((await ask('/cart/file?path=sounds%2Fz.json', { method: 'DELETE' })).status, 200);
    assert.equal(fs.existsSync(path.join(cart, 'sounds', 'z.json')), false);
});

test('nothing outside the cart\'s files is read or written', async () => {
    for (const p of ['..%2Fide.html', 'notes.doc', 'images%2F..%2F..%2Fx.js', 'other%2Fa.js', '.hidden.js'])
        assert.equal((await ask('/cart/file?path=' + p)).status, 400, p);
    assert.equal((await ask('/cart/file?path=..%2Fevil.js', { method: 'PUT', body: 'x' })).status, 400);
    assert.equal(fs.existsSync(path.join(tmp, 'evil.js')), false);
});

test('a request naming another host is refused', async () => {
    const r = await new Promise((done) => {
        const u = new URL(base);
        const s = net.connect(+u.port, '127.0.0.1', () => {
            s.write('GET /cart/list HTTP/1.1\r\nHost: evil.example:' + u.port + '\r\nX-Arcade-Token: ' + token + '\r\n\r\n');
        });
        let out = '';
        s.on('data', (d) => { out += d; });
        s.on('end', () => done(out));
    });
    assert.match(r, /^HTTP\/1\.1 403/);
});

test('a file changed on the disk lists with a new fingerprint, and only that one', async () => {
    const before = Object.fromEntries((await (await ask('/cart/list')).json()).files.map((f) => [f.path, f.hash]));
    fs.writeFileSync(path.join(cart, 'main.js'), 'function init() {}\nfunction update() { }\n');
    const after = Object.fromEntries((await (await ask('/cart/list')).json()).files.map((f) => [f.path, f.hash]));
    assert.notEqual(after['main.js'], before['main.js']);
    assert.equal(after['cart.json'], before['cart.json']);
    assert.equal(after['images/a.png'], before['images/a.png']);
});
