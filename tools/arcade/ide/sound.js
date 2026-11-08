/* The sound editor: a sound effect's sfxr parameters, set by slider or
 * made by one of sfxr's generators, and heard as the player will play it.
 *
 * Parts: the generators (sfxr's own, MIT, DrPetter); reading and writing
 * the JSON; the editor -- the generators beside it, the toolbar, the
 * sound's shape and the sliders; playing it; undo.
 *
 * What is heard and drawn is what the build stores: rendered with the
 * noise seed the sound's name gives, halved to 22,050 Hz and cut to 8
 * bits. */

import { DEFAULTS, RANGES, sfxrParams, sfxrRender, halve, soundSeed } from '../src/sfxr.js';
import { quantise } from '../src/assets.js';
import { icon, iconButton } from './icons.js';
import { sidePanel, toolbar } from './chrome.js';

const KEYS = Object.keys(DEFAULTS);
const range = (k) => RANGES[k] || [0, 1];
const clamp = (k, v) => Math.max(range(k)[0], Math.min(range(k)[1], v));

/* ---- the generators ------------------------------------------------------ */

export const GENERATORS = [
    ['coin', 'Pickup or coin'], ['laser', 'Laser or shot'], ['explosion', 'Explosion'],
    ['powerup', 'Power-up'], ['hit', 'Hit or hurt'], ['jump', 'Jump'], ['blip', 'Blip or select'],
    ['random', 'Random'], ['mutate', 'Mutate']
];

/* a new parameter set of a kind, as sfxr's buttons make them; random()
 * gives 0 to 1. mutate changes the one given. */
export function generate(kind, from, random) {
    const frnd = (r) => random() * r, rnd = (n) => Math.floor(random() * (n + 1));
    if (kind === 'mutate') {
        const p = Object.assign({}, from);
        for (const k of KEYS)
            if (k !== 'wave_type' && k !== 'sound_vol' && rnd(1)) p[k] = clamp(k, p[k] + frnd(0.1) - 0.05);
        return p;
    }
    const p = Object.assign({}, DEFAULTS, { sound_vol: from ? from.sound_vol : DEFAULTS.sound_vol });
    switch (kind) {
    case 'coin':
        p.p_base_freq = 0.4 + frnd(0.5);
        p.p_env_sustain = frnd(0.1);
        p.p_env_decay = 0.1 + frnd(0.4);
        p.p_env_punch = 0.3 + frnd(0.3);
        if (rnd(1)) { p.p_arp_speed = 0.5 + frnd(0.2); p.p_arp_mod = 0.2 + frnd(0.4); }
        break;
    case 'laser':
        p.wave_type = rnd(2);
        if (p.wave_type === 2 && rnd(1)) p.wave_type = rnd(1);
        p.p_base_freq = 0.5 + frnd(0.5);
        p.p_freq_limit = Math.max(0.2, p.p_base_freq - 0.2 - frnd(0.6));
        p.p_freq_ramp = -0.15 - frnd(0.2);
        if (rnd(2) === 0) { p.p_base_freq = 0.3 + frnd(0.6); p.p_freq_limit = frnd(0.1); p.p_freq_ramp = -0.35 - frnd(0.3); }
        if (rnd(1)) { p.p_duty = frnd(0.5); p.p_duty_ramp = frnd(0.2); }
        else { p.p_duty = 0.4 + frnd(0.5); p.p_duty_ramp = -frnd(0.7); }
        p.p_env_sustain = 0.1 + frnd(0.2);
        p.p_env_decay = frnd(0.4);
        if (rnd(1)) p.p_env_punch = frnd(0.3);
        if (rnd(2) === 0) { p.p_pha_offset = frnd(0.2); p.p_pha_ramp = -frnd(0.2); }
        if (rnd(1)) p.p_hpf_freq = frnd(0.3);
        break;
    case 'explosion':
        p.wave_type = 3;
        if (rnd(1)) { p.p_base_freq = 0.1 + frnd(0.4); p.p_freq_ramp = -0.1 + frnd(0.4); }
        else { p.p_base_freq = 0.2 + frnd(0.7); p.p_freq_ramp = -0.2 - frnd(0.2); }
        p.p_base_freq *= p.p_base_freq;
        if (rnd(4) === 0) p.p_freq_ramp = 0;
        if (rnd(2) === 0) p.p_repeat_speed = 0.3 + frnd(0.5);
        p.p_env_sustain = 0.1 + frnd(0.3);
        p.p_env_decay = frnd(0.5);
        if (rnd(1) === 0) { p.p_pha_offset = -0.3 + frnd(0.9); p.p_pha_ramp = -frnd(0.3); }
        p.p_env_punch = 0.2 + frnd(0.6);
        if (rnd(1)) { p.p_vib_strength = frnd(0.7); p.p_vib_speed = frnd(0.6); }
        if (rnd(2) === 0) { p.p_arp_speed = 0.6 + frnd(0.3); p.p_arp_mod = 0.8 - frnd(1.6); }
        break;
    case 'powerup':
        if (rnd(1)) p.wave_type = 1; else p.p_duty = frnd(0.6);
        p.p_base_freq = 0.2 + frnd(0.3);
        if (rnd(1)) { p.p_freq_ramp = 0.1 + frnd(0.4); p.p_repeat_speed = 0.4 + frnd(0.4); }
        else {
            p.p_freq_ramp = 0.05 + frnd(0.2);
            if (rnd(1)) { p.p_vib_strength = frnd(0.7); p.p_vib_speed = frnd(0.6); }
        }
        p.p_env_sustain = frnd(0.4);
        p.p_env_decay = 0.1 + frnd(0.4);
        break;
    case 'hit':
        p.wave_type = rnd(2);
        if (p.wave_type === 2) p.wave_type = 3;
        if (p.wave_type === 0) p.p_duty = frnd(0.6);
        p.p_base_freq = 0.2 + frnd(0.6);
        p.p_freq_ramp = -0.3 - frnd(0.4);
        p.p_env_sustain = frnd(0.1);
        p.p_env_decay = 0.1 + frnd(0.2);
        if (rnd(1)) p.p_hpf_freq = frnd(0.3);
        break;
    case 'jump':
        p.p_duty = frnd(0.6);
        p.p_base_freq = 0.3 + frnd(0.3);
        p.p_freq_ramp = 0.1 + frnd(0.2);
        p.p_env_sustain = 0.1 + frnd(0.3);
        p.p_env_decay = 0.1 + frnd(0.2);
        if (rnd(1)) p.p_hpf_freq = frnd(0.3);
        if (rnd(1)) p.p_lpf_freq = 1 - frnd(0.6);
        break;
    case 'blip':
        p.wave_type = rnd(1);
        if (p.wave_type === 0) p.p_duty = frnd(0.6);
        p.p_base_freq = 0.2 + frnd(0.4);
        p.p_env_sustain = 0.1 + frnd(0.1);
        p.p_env_decay = frnd(0.2);
        p.p_hpf_freq = 0.1;
        break;
    case 'random': {
        const pw = Math.pow;
        const s = () => frnd(2) - 1;
        p.wave_type = from ? from.wave_type : rnd(3);
        p.p_base_freq = rnd(1) ? pw(s(), 3) + 0.5 : pw(s(), 2);
        p.p_freq_ramp = pw(s(), 5);
        if (p.p_base_freq > 0.7 && p.p_freq_ramp > 0.2) p.p_freq_ramp = -p.p_freq_ramp;
        if (p.p_base_freq < 0.2 && p.p_freq_ramp < -0.05) p.p_freq_ramp = -p.p_freq_ramp;
        p.p_freq_dramp = pw(s(), 3);
        p.p_duty = s();
        p.p_duty_ramp = pw(s(), 3);
        p.p_vib_strength = pw(s(), 3);
        p.p_vib_speed = s();
        p.p_env_attack = pw(s(), 3);
        p.p_env_sustain = pw(s(), 2);
        p.p_env_decay = s();
        p.p_env_punch = Math.pow(frnd(0.8), 2);
        if (p.p_env_attack + p.p_env_sustain + p.p_env_decay < 0.2) {
            p.p_env_sustain += 0.2 + frnd(0.3);
            p.p_env_decay += 0.2 + frnd(0.3);
        }
        p.p_lpf_resonance = s();
        p.p_lpf_freq = 1 - Math.pow(frnd(1), 3);
        p.p_lpf_ramp = pw(s(), 3);
        if (p.p_lpf_freq < 0.1 && p.p_lpf_ramp < -0.05) p.p_lpf_ramp = -p.p_lpf_ramp;
        p.p_hpf_freq = Math.pow(frnd(1), 5);
        p.p_hpf_ramp = pw(s(), 5);
        p.p_pha_offset = pw(s(), 3);
        p.p_pha_ramp = pw(s(), 3);
        p.p_repeat_speed = s();
        p.p_arp_speed = s();
        p.p_arp_mod = s();
        break;
    }
    }
    for (const k of KEYS) p[k] = clamp(k, p[k]);
    return p;
}

/* ---- the JSON ------------------------------------------------------------ */

/* { params, rest }: rest is any other field jsfxr wrote, kept */
export function readSound(text) {
    const json = JSON.parse(text);
    if (!json || typeof json !== 'object' || Array.isArray(json)) throw new Error('a sound is an object of sfxr parameters');
    const params = sfxrParams(json), rest = {};
    for (const k in json) if (!(k in DEFAULTS)) rest[k] = json[k];
    return { params, rest };
}

/* every parameter, in sfxr's order, to four places */
export function writeSound(s) {
    const out = {};
    for (const k of KEYS) out[k] = k === 'wave_type' ? s.params[k] : Math.round(s.params[k] * 10000) / 10000;
    return JSON.stringify(Object.assign(out, s.rest), null, 2) + '\n';
}

/* the samples the build stores, as -1 to 1 at 22,050 Hz */
export function rendered(params, name) {
    const q = quantise(halve(sfxrRender(params, soundSeed(name))));
    const out = new Float32Array(q.bytes.length);
    for (let i = 0; i < out.length; i++) out[i] = ((q.bytes[i] << 24) >> 24) * q.scale / 32768;
    return out;
}

/* ---- the editor ---------------------------------------------------------- */

const SECTIONS = [
    ['Envelope', [['p_env_attack', 'Attack', 'How long it takes to rise'],
                  ['p_env_sustain', 'Sustain', 'How long it holds'],
                  ['p_env_punch', 'Punch', 'How much louder it starts its hold'],
                  ['p_env_decay', 'Decay', 'How long it takes to fade']]],
    ['Pitch', [['p_base_freq', 'Start', 'The pitch it starts at'],
               ['p_freq_limit', 'Cut-off', 'A pitch it stops at, sliding down'],
               ['p_freq_ramp', 'Slide', 'Sliding up or down'],
               ['p_freq_dramp', 'Curve', 'The slide speeding up or slowing']]],
    ['Vibrato', [['p_vib_strength', 'Depth', 'How far the pitch wobbles'],
                 ['p_vib_speed', 'Speed', 'How fast it wobbles']]],
    ['Arpeggio', [['p_arp_mod', 'Jump', 'A jump in pitch, up or down, part way through'],
                  ['p_arp_speed', 'When', 'How soon the jump comes']]],
    ['Square', [['p_duty', 'Duty', 'The square wave\'s shape: hollow to thin'],
                ['p_duty_ramp', 'Sweep', 'The shape changing as it plays']]],
    ['Repeat', [['p_repeat_speed', 'Speed', 'Starting again from the top, this often']]],
    ['Flanger', [['p_pha_offset', 'Offset', 'A copy, delayed, mixed in'],
                 ['p_pha_ramp', 'Sweep', 'The delay changing']]],
    ['Low-pass', [['p_lpf_freq', 'Cut-off', 'Dulls what is above it'],
                  ['p_lpf_ramp', 'Sweep', 'The cut-off moving'],
                  ['p_lpf_resonance', 'Resonance', 'A ring at the cut-off']]],
    ['High-pass', [['p_hpf_freq', 'Cut-off', 'Thins what is below it'],
                   ['p_hpf_ramp', 'Sweep', 'The cut-off moving']]],
    ['Output', [['sound_vol', 'Volume', 'How loud']]]
];

const WAVES = [['waveSquare', 'Square'], ['waveSaw', 'Sawtooth'], ['waveSine', 'Sine'], ['waveNoise', 'Noise']];

let audio = null;

export class SoundEditor {
    /* opts: text (the JSON; readSound's error is thrown), name (the
     * sound's, which seeds its noise), changed(text), say(text, error) */
    constructor(parent, opts) {
        this.o = opts;
        this.s = readSound(opts.text);
        this.undos = [];
        this.redos = [];
        this.build(parent);
        this.update();
    }

    build(parent) {
        const root = this.root = document.createElement('div');
        root.className = 'snd';
        this.side = sidePanel('Generate');
        const list = document.createElement('div');
        list.className = 'snd-gen';
        for (const [kind, label] of GENERATORS) {
            const b = document.createElement('button');
            b.type = 'button';
            b.innerHTML = icon(kind) + '<span></span>';
            b.lastChild.textContent = label;
            b.title = kind === 'mutate' ? 'Change each setting a little, at random'
                : 'A new ' + label.toLowerCase() + ' sound, at random; again for another';
            b.onclick = () => {
                this.save();
                this.s.params = generate(kind, this.s.params, Math.random);
                this.commit(true);
            };
            list.append(b);
        }
        this.side.body.append(list);

        const main = this.main = document.createElement('div');
        main.className = 'snd-main';
        main.tabIndex = 0;
        const bar = toolbar();
        this.playButton = bar.add('play', 'Play it (Space)', () => this.play());
        bar.sep();
        this.waveButtons = WAVES.map(([ic, title], w) => bar.add(ic, title + ' wave', () => {
            if (this.s.params.wave_type === w) return;
            this.save();
            this.s.params.wave_type = w;
            this.commit(true);
        }));
        bar.sep();
        this.info = document.createElement('span');
        this.info.className = 'note';
        bar.put(this.info);

        this.shape = document.createElement('canvas');
        this.shape.className = 'snd-shape';
        this.shape.title = 'The sound as the player stores it; click to play';
        this.shape.onclick = () => this.play();
        const body = document.createElement('div');
        body.className = 'snd-body';
        const grid = document.createElement('div');
        grid.className = 'snd-grid';
        this.sliders = {};
        for (const [title, rows] of SECTIONS) {
            const sec = document.createElement('div');
            sec.className = 'snd-sec';
            const h = document.createElement('div');
            h.className = 'ed-sub';
            h.textContent = title;
            sec.append(h);
            for (const [k, label, tip] of rows) sec.append(this.slider(k, label, tip));
            if (title === 'Square') this.squareSec = sec;
            grid.append(sec);
        }
        body.append(this.shape, grid);
        main.append(bar.el, body);
        root.append(main);
        parent.append(root);
        main.addEventListener('keydown', (e) => this.key(e));
        this.resizer = new ResizeObserver(() => this.drawShape());
        this.resizer.observe(this.shape);
    }

    /* a parameter's row: its name, a slider and the number; dragging
     * redraws, letting go plays */
    slider(k, label, tip) {
        const row = document.createElement('label');
        row.className = 'snd-row';
        row.title = tip + ' (' + k + ')';
        const [lo, hi] = range(k);
        const r = document.createElement('input');
        r.type = 'range';
        r.min = lo;
        r.max = hi;
        r.step = 0.001;
        if (lo < 0) r.className = 'both';
        const n = document.createElement('input');
        n.type = 'number';
        n.min = lo;
        n.max = hi;
        n.step = 0.01;
        let before = null;
        const set = (v, play) => {
            if (before === null) before = this.snapshot();
            this.s.params[k] = clamp(k, +v || 0);
            n.value = this.s.params[k].toFixed(2);
            r.value = this.s.params[k];
            if (play) {
                this.undos.push(before);
                this.redos = [];
                before = null;
                this.commit(true);
            } else this.drawShape(true);
        };
        r.oninput = () => set(r.value, false);
        r.onchange = () => set(r.value, true);
        n.onchange = () => set(n.value, true);
        r.ondblclick = () => set(DEFAULTS[k], true);
        const name = document.createElement('span');
        name.textContent = label;
        row.append(name, r, n);
        this.sliders[k] = { r, n };
        return row;
    }

    destroy() {
        this.resizer.disconnect();
        this.stop();
        this.root.remove();
    }

    focus() { this.main.focus(); }

    /* the sliders, the wave and the shape brought up to date */
    update() {
        const p = this.s.params;
        for (const [k, { r, n }] of Object.entries(this.sliders)) {
            r.value = p[k];
            n.value = p[k].toFixed(2);
        }
        this.waveButtons.forEach((b, w) => b.classList.toggle('on', p.wave_type === w));
        this.squareSec.classList.toggle('off', p.wave_type !== 0);
        this.drawShape();
    }

    drawShape(soon) {
        if (soon) {
            if (this.pending) return;
            this.pending = requestAnimationFrame(() => { this.pending = 0; this.drawShape(); });
            return;
        }
        const pcm = this.pcm = rendered(this.s.params, this.o.name);
        const c = this.shape, w = Math.max(1, c.clientWidth), h = Math.max(1, c.clientHeight);
        c.width = w;
        c.height = h;
        const g = c.getContext('2d'), style = getComputedStyle(c);
        g.clearRect(0, 0, w, h);
        g.fillStyle = style.getPropertyValue('--line');
        g.fillRect(0, h >> 1, w, 1);
        g.fillStyle = style.color;
        /* the whole sound across the width, each column its loudest */
        const per = Math.max(1, Math.ceil(pcm.length / w));
        for (let x = 0; x * per < pcm.length; x++) {
            let lo = 1, hi = -1;
            for (let i = x * per; i < Math.min(pcm.length, (x + 1) * per); i++) { lo = Math.min(lo, pcm[i]); hi = Math.max(hi, pcm[i]); }
            g.fillRect(x, (1 - hi) * h / 2, 1, Math.max(1, (hi - lo) * h / 2));
        }
        this.info.textContent = (pcm.length / 22050).toFixed(2) + ' s, ' +
            pcm.length.toLocaleString() + ' bytes in the cart';
    }

    /* ---- playing ---- */

    play() {
        this.stop();
        try {
            if (!audio) audio = new AudioContext();
            if (!this.pcm || !this.pcm.length) return;
            const buf = audio.createBuffer(1, this.pcm.length, 22050);
            buf.copyToChannel(this.pcm, 0);
            const src = this.source = audio.createBufferSource();
            src.buffer = buf;
            src.connect(audio.destination);
            src.onended = () => { if (this.source === src) this.source = null; this.playButton.classList.remove('on'); };
            src.start();
            this.playButton.classList.add('on');
        } catch (e) { this.o.say('The browser would not play it: ' + e.message, true); }
    }

    stop() {
        if (this.source) { try { this.source.stop(); } catch (e) { /* ended */ } this.source = null; }
    }

    key(e) {
        if (e.target.tagName === 'INPUT' && e.target.type !== 'range') return;
        const k = e.key.toLowerCase();
        if (e.ctrlKey || e.metaKey) {
            if (k === 'z' && !e.shiftKey) { e.preventDefault(); this.undoEdit(); }
            else if (k === 'y' || (k === 'z' && e.shiftKey)) { e.preventDefault(); this.redoEdit(); }
            return;
        }
        if (k === ' ') { e.preventDefault(); this.play(); }
    }

    /* ---- undo ---- */

    snapshot() { return Object.assign({}, this.s.params); }

    save() {
        this.undos.push(this.snapshot());
        if (this.undos.length > 100) this.undos.shift();
        this.redos = [];
    }

    undoEdit() { if (this.undos.length) { this.redos.push(this.snapshot()); this.s.params = this.undos.pop(); this.commit(true); } }

    redoEdit() { if (this.redos.length) { this.undos.push(this.snapshot()); this.s.params = this.redos.pop(); this.commit(true); } }

    commit(play) {
        this.o.changed(writeSound(this.s));
        this.update();
        if (play) this.play();
    }
}
