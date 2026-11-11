#!/usr/bin/env node
/* pbgrun: runs a cart headless on wasm/arcade.wasm, the engine the IDE
 * runs, with pbgvm's options and output:
 *
 *   pbgrun cart.pbg [--ticks n] [--seed s] [--ceiling units] [--units]
 *                   [--shot file.png] [--dump x,y,w,h] [--dirty]
 *                   [--buttons t:mask,...] [--wheel t:clicks[:pos],...]
 *                   [--skip t:n,...] [--setting name=value]... [--speech]
 *                   [--font font.bin] [--playing] [--beat bpm]
 *                   [--store dir]
 *
 * printh() lines go to stdout; a runtime error to stderr as file:line:
 * message (in function), with exit status 1; --units prints each call's
 * units to stderr, and each prof() section's as "prof name units".
 * watch() prints "watch name value" as a value changes. Each ui.* dialog
 * prints what it showed, and is worked a tick at a time from --buttons
 * and --wheel, backed out of when the ticks run out (--shot then shows
 * it). --store keeps store.* files in dir, as name.json, as pbgvm does.
 * An option not listed here is refused. See pbgvm.c for the rest. */

import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { fileURLToPath } from 'node:url';
import { Engine, fontGlyphs } from './src/runner.js';

const here = path.dirname(fileURLToPath(import.meta.url));

function args(argv) {
    const o = { ticks: 0, seed: 1n, settings: {}, pos: -1 };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i], v = () => argv[++i];
        if (a === '--ticks') o.ticks = +v();
        else if (a === '--seed') o.seed = BigInt(v());
        else if (a === '--ceiling') o.ceiling = +v();
        else if (a === '--units') o.units = true;
        else if (a === '--shot') o.shot = v();
        else if (a === '--dump') o.dump = v();
        else if (a === '--dirty') o.dirty = true;
        else if (a === '--buttons') o.buttons = v();
        else if (a === '--wheel') o.wheel = v();
        else if (a === '--skip') o.skip = v();
        else if (a === '--speech') o.speech = true;
        else if (a === '--playing') o.playing = true;
        else if (a === '--beat') { o.beat = true; o.bpm = +v(); }
        else if (a === '--font') o.font = fs.readFileSync(v());
        else if (a === '--store') o.store = v();
        else if (a === '--setting') {
            const [k, ...rest] = v().split('=');
            const s = rest.join('=');
            o.settings[k] = /^-?\d+$/.test(s) ? +s : s;
        } else if (a.startsWith('--')) throw new Error('pbgrun has no ' + a);
        else o.cart = a;
    }
    return o;
}

/* "t:a:b,..." as [[t, a, b], ...] */
function entries(spec) {
    return spec ? spec.split(',').map((e) => e.split(':').map(Number)) : [];
}

/* pbgvm's input: the latest --buttons mask at or before t, and the wheel's
 * clicks at t, the finger moved as --wheel says */
function input(o) {
    let held = 0, pos = o.pos;
    const buttons = entries(o.buttons), wheel = entries(o.wheel);
    return (t) => {
        for (const [at, mask] of buttons) if (at === t) held = mask || 0;
        let clicks = 0;
        for (const [at, n, p] of wheel)
            if (at === t) { clicks = n || 0; if (p !== undefined) pos = p; }
        return { held, clicks, pos };
    };
}

function png(rgb, w, h) {
    const raw = Buffer.alloc((w * 3 + 1) * h);
    for (let y = 0; y < h; y++) rgb.copy(raw, y * (w * 3 + 1) + 1, y * w * 3, (y + 1) * w * 3);
    const chunk = (type, data) => {
        const len = Buffer.alloc(4);
        len.writeUInt32BE(data.length);
        const td = Buffer.concat([Buffer.from(type), data]);
        const crc = Buffer.alloc(4);
        crc.writeUInt32BE(zlib.crc32(td));
        return Buffer.concat([len, td, crc]);
    };
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(w, 0);
    ihdr.writeUInt32BE(h, 4);
    ihdr.set([8, 2, 0, 0, 0], 8);
    return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr),
                          chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}

/* the panel as the player shows it: the screen doubled */
function shot(engine, file) {
    const s = engine.rgba(), rgb = Buffer.alloc(320 * 240 * 3);
    for (let y = 0; y < 240; y++)
        for (let x = 0; x < 320; x++) {
            const i = ((y >> 1) * 160 + (x >> 1)) * 4, o = (y * 320 + x) * 3;
            rgb[o] = s[i]; rgb[o + 1] = s[i + 1]; rgb[o + 2] = s[i + 2];
        }
    fs.writeFileSync(file, png(rgb, 320, 240));
}

function main() {
    let o;
    try { o = args(process.argv.slice(2)); } catch (e) { console.error(e.message); return 2; }
    if (!o.cart) {
        console.error('usage: pbgrun cart.pbg [--ticks n] [--seed s] [--units] [--shot file.png] ...');
        return 2;
    }
    const out = [];
    const flush = () => { if (out.length) { process.stdout.write(out.join('')); out.length = 0; } };
    const at = input(o);
    let t = 0, said = false, shotTaken = false, engine;
    /* pbgvm's watch() and prof(): 32 names watched, prof() 8 deep */
    const watched = new Map(), open = [];
    let depth = 0;
    const host = {
        watch: (name, value) => {
            if (watched.get(name) === value) return;
            if (watched.has(name) || watched.size < 32) watched.set(name, value);
            out.push('watch ' + name + ' ' + value + '\n');
        },
        prof: (name) => {
            const now = engine.unitsNow();
            if (name !== null) {
                if (depth < 8) open[depth] = [name, now];
                depth++;
            } else if (depth > 0) {
                depth--;
                if (o.units && depth < 8) {
                    flush();
                    console.error('prof ' + open[depth][0] + ' ' + ((now - open[depth][1]) >>> 0));
                }
            }
        },
        print: (s) => out.push(s + '\n'),
        /* store.*: name.json files in --store's folder, none without it */
        storeRead: (name) => {
            if (!o.store) return undefined;
            const f = path.join(o.store, name + '.json');
            return fs.existsSync(f) ? new Uint8Array(fs.readFileSync(f)) : undefined;
        },
        storeWrite: (name, bytes) => {
            if (!o.store) return false;
            fs.mkdirSync(o.store, { recursive: true });
            fs.writeFileSync(path.join(o.store, name + '.json'), bytes);
            return true;
        },
        storeRemove: (name) => {
            const f = o.store && path.join(o.store, name + '.json');
            if (!f || !fs.existsSync(f)) return false;
            fs.unlinkSync(f);
            return true;
        },
        storeList: () => !o.store || !fs.existsSync(o.store) ? [] :
            fs.readdirSync(o.store).filter((n) => n.endsWith('.json'))
                .map((n) => n.slice(0, -5)).sort(),
        click: (beep) => out.push('click ' + beep + '\n'),
        setting: (name) => o.settings[name],
        musicSay: (line) => out.push(line + '\n'),
        say: (text, queue) => out.push((queue ? 'queue ' : 'say ') + text + '\n'),
        speechStop: () => out.push('stop\n'),
        speechOn: () => !!o.speech,
        glyph: o.font ? fontGlyphs(new Uint8Array(o.font)) : undefined,
        /* a dialog's frame: the next tick's input, as pbgvm works it */
        frame: (open) => {
            if (!open) { said = false; return null; }
            if (!said) {
                const d = engine.dialog();
                if (d) { said = true; out.push(d + '\n'); }
            }
            if (t + 1 >= o.ticks) {
                if (o.shot && !shotTaken) { shot(engine, o.shot); shotTaken = true; }
                return null;
            }
            const i = at(++t);
            return { held: i.held, wheel: i.clicks };
        }
    };
    const wasm = new WebAssembly.Module(fs.readFileSync(path.join(here, 'wasm', 'arcade.wasm')));
    engine = new Engine(wasm, host);
    /* the made-up music: there to read with --playing or --beat, its clock
     * running with the ticks, as pbgvm's */
    engine.music({ playingFn: o.playing || o.beat, beat: o.beat,
                   playing: o.playing || o.beat, bpm: o.bpm || 0 });
    const report = () => { flush(); console.error(engine.error()); return 1; };
    if (!engine.load(new Uint8Array(fs.readFileSync(o.cart)), o.seed)) return report();
    if (o.ceiling) engine.ceiling(o.ceiling);
    if (!engine.start()) return report();
    if (o.units) { flush(); console.error('start ' + engine.units()); }
    const skips = entries(o.skip);
    let trackMs = 0, trackAt = 0;
    for (; t < o.ticks; t++) {
        /* the tick it began as, though a dialog may run on past it */
        const begun = t, i = at(t);
        /* a late tick: its skipped frames told, and it runs without drawing */
        let late = false;
        for (const [when, n] of skips) if (when === t) { engine.skipped(n || 0); late = true; }
        let ms = 0;
        if (o.beat || o.playing) {
            trackMs += 1000 / engine.rate();
            ms = Math.trunc(trackMs) - trackAt;
            trackAt = Math.trunc(trackMs);
        }
        if (!engine.tick(i.held, i.clicks, i.pos, !late, ms)) return report();
        if (o.units) { flush(); console.error('tick ' + (begun + 1) + ' ' + engine.units()); }
        engine.idle();
        if (engine.quitAsked()) { out.push('quit\n'); t++; break; }
    }
    if (o.dump) {
        const [x, y, w, h] = o.dump.split(',').map(Number), px = engine.pixels();
        for (let j = y; j < y + h; j++) {
            let line = '';
            for (let k = x; k < x + w; k++)
                line += (k >= 0 && j >= 0 && k < 160 && j < 120 ? px[j * 160 + k] & 15 : 0).toString(16);
            out.push(line + '\n');
        }
    }
    if (o.dirty) { const [lo, hi] = engine.dirty(); out.push('dirty ' + lo + ' ' + hi + '\n'); }
    if (o.shot && !shotTaken) shot(engine, o.shot);
    flush();
    return 0;
}

process.exitCode = main();
