/* sfxr: DrPetter's sound effect generator (MIT), ResetSample() and
 * SynthSample() line for line, in floating point at 44.1 kHz with its
 * eight sub-samples, as the original renders a WAV. Parameters are
 * jsfxr's JSON, which uses the original's names. Its noise comes from a
 * seeded generator rather than rand(), so a cart builds the same bytes
 * every time. */

export const SFXR_RATE = 44100;
const MAX_SECONDS = 10;

export const DEFAULTS = {
    wave_type: 0, p_env_attack: 0, p_env_sustain: 0.3, p_env_punch: 0,
    p_env_decay: 0.4, p_base_freq: 0.3, p_freq_limit: 0, p_freq_ramp: 0,
    p_freq_dramp: 0, p_vib_strength: 0, p_vib_speed: 0, p_arp_mod: 0,
    p_arp_speed: 0, p_duty: 0, p_duty_ramp: 0, p_repeat_speed: 0,
    p_pha_offset: 0, p_pha_ramp: 0, p_lpf_freq: 1, p_lpf_ramp: 0,
    p_lpf_resonance: 0, p_hpf_freq: 0, p_hpf_ramp: 0, sound_vol: 0.5
};

/* Each parameter's range: [low, high] */
export const RANGES = {
    wave_type: [0, 3], p_freq_ramp: [-1, 1], p_freq_dramp: [-1, 1],
    p_arp_mod: [-1, 1], p_duty_ramp: [-1, 1], p_pha_offset: [-1, 1],
    p_pha_ramp: [-1, 1], p_lpf_ramp: [-1, 1], p_hpf_ramp: [-1, 1],
    sound_vol: [0, 1]
};

/* jsfxr's JSON into a complete parameter set; throws saying what is wrong */
export function sfxrParams(json) {
    const p = Object.assign({}, DEFAULTS);
    for (const k in json) {
        if (!(k in DEFAULTS)) continue;      /* jsfxr's extras: sample_rate... */
        const v = json[k], r = RANGES[k] || [0, 1];
        if (typeof v !== 'number' || !(v >= r[0] && v <= r[1]))
            throw new Error(k + ' is a number from ' + r[0] + ' to ' + r[1]);
        p[k] = k === 'wave_type' ? Math.floor(v) : v;
    }
    return p;
}

/* The effect as samples from -1 to 1 at SFXR_RATE */
export function sfxrRender(p, seed) {
    let rng = (seed >>> 0) || 1;
    const frnd = (range) => {
        rng ^= rng << 13; rng >>>= 0;
        rng ^= rng >>> 17;
        rng ^= rng << 5; rng >>>= 0;
        return rng / 4294967296 * range;
    };
    const master_vol = 0.05;
    let phase = 0, fperiod, fmaxperiod, fslide, fdslide, period;
    let square_duty, square_slide, env_stage, env_time, env_vol;
    const env_length = [0, 0, 0];
    let fphase, fdphase, iphase, ipp;
    const phaser_buffer = new Float64Array(1024), noise_buffer = new Float64Array(32);
    let fltp, fltdp, fltw, fltw_d, fltdmp, fltphp, flthp, flthp_d;
    let vib_phase, vib_speed, vib_amp, rep_time, rep_limit, arp_time, arp_limit, arp_mod;
    let playing = true;

    function reset(restart) {
        if (!restart) phase = 0;
        fperiod = 100.0 / (p.p_base_freq * p.p_base_freq + 0.001);
        period = Math.trunc(fperiod);
        fmaxperiod = 100.0 / (p.p_freq_limit * p.p_freq_limit + 0.001);
        fslide = 1.0 - Math.pow(p.p_freq_ramp, 3.0) * 0.01;
        fdslide = -Math.pow(p.p_freq_dramp, 3.0) * 0.000001;
        square_duty = 0.5 - p.p_duty * 0.5;
        square_slide = -p.p_duty_ramp * 0.00005;
        if (p.p_arp_mod >= 0.0) arp_mod = 1.0 - Math.pow(p.p_arp_mod, 2.0) * 0.9;
        else arp_mod = 1.0 + Math.pow(p.p_arp_mod, 2.0) * 10.0;
        arp_time = 0;
        arp_limit = Math.trunc(Math.pow(1.0 - p.p_arp_speed, 2.0) * 20000 + 32);
        if (p.p_arp_speed === 1.0) arp_limit = 0;
        if (!restart) {
            fltp = 0.0;
            fltdp = 0.0;
            fltw = Math.pow(p.p_lpf_freq, 3.0) * 0.1;
            fltw_d = 1.0 + p.p_lpf_ramp * 0.0001;
            fltdmp = 5.0 / (1.0 + Math.pow(p.p_lpf_resonance, 2.0) * 20.0) * (0.01 + fltw);
            if (fltdmp > 0.8) fltdmp = 0.8;
            fltphp = 0.0;
            flthp = Math.pow(p.p_hpf_freq, 2.0) * 0.1;
            flthp_d = 1.0 + p.p_hpf_ramp * 0.0003;
            vib_phase = 0.0;
            vib_speed = Math.pow(p.p_vib_speed, 2.0) * 0.01;
            vib_amp = p.p_vib_strength * 0.5;
            env_vol = 0.0;
            env_stage = 0;
            env_time = 0;
            env_length[0] = Math.trunc(p.p_env_attack * p.p_env_attack * 100000.0);
            env_length[1] = Math.trunc(p.p_env_sustain * p.p_env_sustain * 100000.0);
            env_length[2] = Math.trunc(p.p_env_decay * p.p_env_decay * 100000.0);
            fphase = Math.pow(p.p_pha_offset, 2.0) * 1020.0;
            if (p.p_pha_offset < 0.0) fphase = -fphase;
            fdphase = Math.pow(p.p_pha_ramp, 2.0) * 1.0;
            if (p.p_pha_ramp < 0.0) fdphase = -fdphase;
            iphase = Math.abs(Math.trunc(fphase));
            ipp = 0;
            phaser_buffer.fill(0);
            for (let i = 0; i < 32; i++) noise_buffer[i] = frnd(2.0) - 1.0;
            rep_time = 0;
            rep_limit = Math.trunc(Math.pow(1.0 - p.p_repeat_speed, 2.0) * 20000 + 32);
            if (p.p_repeat_speed === 0.0) rep_limit = 0;
        }
    }

    reset(false);
    const out = [];
    while (playing && out.length < SFXR_RATE * MAX_SECONDS) {
        rep_time++;
        if (rep_limit !== 0 && rep_time >= rep_limit) {
            rep_time = 0;
            reset(true);
        }
        arp_time++;
        if (arp_limit !== 0 && arp_time >= arp_limit) {
            arp_limit = 0;
            fperiod *= arp_mod;
        }
        fslide += fdslide;
        fperiod *= fslide;
        if (fperiod > fmaxperiod) {
            fperiod = fmaxperiod;
            if (p.p_freq_limit > 0.0) playing = false;
        }
        let rfperiod = fperiod;
        if (vib_amp > 0.0) {
            vib_phase += vib_speed;
            rfperiod = fperiod * (1.0 + Math.sin(vib_phase) * vib_amp);
        }
        period = Math.trunc(rfperiod);
        if (period < 8) period = 8;
        square_duty += square_slide;
        if (square_duty < 0.0) square_duty = 0.0;
        if (square_duty > 0.5) square_duty = 0.5;
        env_time++;
        if (env_time > env_length[env_stage]) {
            env_time = 0;
            env_stage++;
            if (env_stage === 3) break;
        }
        if (env_stage === 0) env_vol = env_time / env_length[0];
        if (env_stage === 1)
            env_vol = 1.0 + Math.pow(1.0 - env_time / env_length[1], 1.0) * 2.0 * p.p_env_punch;
        if (env_stage === 2) env_vol = 1.0 - env_time / env_length[2];
        fphase += fdphase;
        iphase = Math.abs(Math.trunc(fphase));
        if (iphase > 1023) iphase = 1023;
        if (flthp_d !== 0.0) {
            flthp *= flthp_d;
            if (flthp < 0.00001) flthp = 0.00001;
            if (flthp > 0.1) flthp = 0.1;
        }
        let ssample = 0.0;
        for (let si = 0; si < 8; si++) {
            let sample = 0.0;
            phase++;
            if (phase >= period) {
                phase %= period;
                if (p.wave_type === 3)
                    for (let i = 0; i < 32; i++) noise_buffer[i] = frnd(2.0) - 1.0;
            }
            const fp = phase / period;
            switch (p.wave_type) {
            case 0: sample = fp < square_duty ? 0.5 : -0.5; break;
            case 1: sample = 1.0 - fp * 2; break;
            case 2: sample = Math.sin(fp * 2 * Math.PI); break;
            case 3: sample = noise_buffer[Math.trunc(phase * 32 / period)]; break;
            }
            const pp = fltp;
            fltw *= fltw_d;
            if (fltw < 0.0) fltw = 0.0;
            if (fltw > 0.1) fltw = 0.1;
            if (p.p_lpf_freq !== 1.0) {
                fltdp += (sample - fltp) * fltw;
                fltdp -= fltdp * fltdmp;
            } else {
                fltp = sample;
                fltdp = 0.0;
            }
            fltp += fltdp;
            fltphp += fltp - pp;
            fltphp -= fltphp * flthp;
            sample = fltphp;
            phaser_buffer[ipp & 1023] = sample;
            sample += phaser_buffer[(ipp - iphase + 1024) & 1023];
            ipp = (ipp + 1) & 1023;
            ssample += sample * env_vol;
        }
        ssample = ssample / 8 * master_vol;
        ssample *= 2.0 * p.sound_vol;
        ssample *= 4.0;                 /* the original's gain for a WAV */
        if (ssample > 1.0) ssample = 1.0;
        if (ssample < -1.0) ssample = -1.0;
        out.push(ssample);
    }
    return Float64Array.from(out);
}

/* The noise seed a sound takes from its name, so it builds the same each
 * time and the IDE plays what the build stores */
export function soundSeed(name) {
    let seed = 2166136261;
    for (const ch of name) seed = Math.imul(seed ^ ch.charCodeAt(0), 16777619);
    return seed >>> 0;
}

/* Halved to 22,050 Hz, a pair of samples averaged, as 16-bit */
export function halve(samples) {
    const n = samples.length >> 1, out = new Int16Array(n);
    for (let i = 0; i < n; i++) {
        const v = (samples[i * 2] + samples[i * 2 + 1]) * 0.5 * 32767;
        out[i] = Math.max(-32768, Math.min(32767, Math.round(v)));
    }
    return out;
}
