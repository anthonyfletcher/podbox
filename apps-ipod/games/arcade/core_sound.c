/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade: sound. Four voices, each an effect or a tone, mixed for the host
 * at whatever rate its mixer runs.
 *
 * An effect was rendered by sfxr when the cart was compiled and arrives as
 * 8-bit samples at 22,050 Hz with a scale (arcade/sdk/src/assets.js
 * writes them); a voice steps through it in 16.16, so sfxp's pitch and the
 * mixer's rate are one resampling. A tone is made live.
 *
 * Parts, in order:
 *   the effects     the SFX_ chunk checked
 *   the voices      sfx, sfxp, tone, click
 *   mixing          arcade_audio()
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)
#define SOURCE_RATE 22050

/* ---- the effects ------------------------------------------------------- */

/* The voice's next sample, or silence past its end */
static int32_t decode(struct voice *v)
{
    if (v->next >= v->n)
        return 0;
    return v->data[v->next++] * v->scale;
}

static uint32_t le16(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8;
}

/* The SFX_ chunk, checked whole: each effect's place, in sfxdir */
int sound_load(struct vm *vm)
{
    const uint8_t *p = vm->sfx, *end = p + vm->sfx_len;
    if (!p)
        return 1;
    if (end - p < 2)
        return vm_fail(vm, "this game file is damaged: ", "sounds", "");
    uint32_t n = le16(p);
    p += 2;
    uint32_t at = perm_alloc(vm, n * sizeof(struct sfxinfo) + 8);
    if (!at)
        return vm_fail(vm, "not enough memory for this game", "", "");
    vm->sfxdir = MEM(vm, at);
    for (uint32_t i = 0; i < n; i++) {
        struct sfxinfo *s = &vm->sfxdir[i];
        if (end - p < 6)
            return vm_fail(vm, "this game file is damaged: ", "sounds", "");
        s->n = le16(p) | le16(p + 2) << 16;
        s->scale = (int32_t)le16(p + 4);
        p += 6;
        if (!s->scale || s->scale > 259 || s->n > (uint32_t)(end - p))
            return vm_fail(vm, "this game file is damaged: ", "sounds", "");
        s->data = (const int8_t *)p;
        p += s->n;
    }
    vm->nsfx = n;
    return 1;
}

/* ---- the voices ------------------------------------------------------- */

/* 2^(k/12) in 16.16, for a semitone k from 0 to 11 */
static const uint32_t semitone[12] = {
    65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218,
    116772, 123715
};

static int whole_arg(struct vm *vm, struct value *a, int argc, int i,
                     int32_t def, const char *fn, int32_t *out)
{
    *out = def;
    if (i >= argc || a[i].tag == T_NULL)
        return 1;
    if (!is_num(a[i]))
        return vm_fail(vm, fn, "() needs a number, not a ",
                       vm_typename(a[i].tag));
    *out = floor_int(a[i]);
    return 1;
}

/* The channel given, or the first idle one, else the one nearest its end */
static int channel(struct vm *vm, struct value *a, int argc, int i,
                   const char *fn, int32_t *ch)
{
    if (!whole_arg(vm, a, argc, i, -1, fn, ch))
        return 0;
    if (*ch >= 0) {
        if (*ch > 3)
            return vm_fail(vm, fn, "() takes a channel from 0 to 3", "");
        return 1;
    }
    uint32_t most = 0xffffffffu;
    for (int c = 0; c < 4; c++) {
        const struct voice *v = &vm->voices[c];
        uint32_t left = v->kind == VOICE_SFX ? v->n - v->next
                      : v->kind == VOICE_TONE ? (uint32_t)(v->left >> 16) : 0;
        if (v->kind == VOICE_OFF) {
            *ch = c;
            return 1;
        }
        if (left < most) {
            most = left;
            *ch = c;
        }
    }
    return 1;
}

static int play(struct vm *vm, struct value *a, int argc, int ch_arg,
                int32_t semis, const char *fn)
{
    int32_t n, ch;
    if (!whole_arg(vm, a, argc, 0, 0, fn, &n) ||
        !channel(vm, a, argc, ch_arg, fn, &ch))
        return 0;
    struct voice *v = &vm->voices[ch];
    if (n < 0) {
        v->kind = VOICE_OFF;
        return 1;
    }
    if ((uint32_t)n >= vm->nsfx)
        return vm_fail(vm, fn, "() needs one of the cart's sounds, SFX.name",
                       "");
    const struct sfxinfo *s = &vm->sfxdir[n];
    if (semis < -48) semis = -48;
    if (semis > 48) semis = 48;
    int32_t oct = semis >= 0 ? semis / 12 : -((11 - semis) / 12);
    uint32_t pitch = semitone[semis - oct * 12];
    pitch = oct >= 0 ? pitch << oct : pitch >> -oct;
    memset(v, 0, sizeof(*v));
    v->kind = VOICE_SFX;
    v->id = (int16_t)n;
    v->data = s->data;
    v->n = s->n;
    v->scale = s->scale;
    v->pitch = pitch;
    v->s1 = decode(v);
    vm_charge(vm, 8);
    return 1;
}

NATIVE(sfx)
{
    (void)out;
    return play(vm, a, argc, 1, 0, "sfx");
}

NATIVE(sfxp)
{
    (void)out;
    int32_t semis;
    if (!whole_arg(vm, a, argc, 1, 0, "sfxp", &semis))
        return 0;
    return play(vm, a, argc, 2, semis, "sfxp");
}

/* tone(channel, hz, ticks, vol, wave) */
NATIVE(tone)
{
    (void)out;
    int32_t ch, hz, ticks, vol, wave;
    if (!channel(vm, a, argc, 0, "tone", &ch) ||
        !whole_arg(vm, a, argc, 1, 440, "tone", &hz) ||
        !whole_arg(vm, a, argc, 2, 1, "tone", &ticks) ||
        !whole_arg(vm, a, argc, 3, 128, "tone", &vol) ||
        !whole_arg(vm, a, argc, 4, 0, "tone", &wave))
        return 0;
    if (wave < 0 || wave > 4)
        return vm_fail(vm, "tone() needs a WAVE", "", "");
    struct voice *v = &vm->voices[ch];
    memset(v, 0, sizeof(*v));
    if (hz <= 0 || ticks <= 0 || vol <= 0)
        return 1;
    if (hz > 20000) hz = 20000;
    if (vol > 255) vol = 255;
    if (ticks > 60 * 50) ticks = 60 * 50;
    v->kind = VOICE_TONE;
    v->id = -2;
    v->wave = (uint8_t)wave;
    v->hz = (uint32_t)hz;
    v->vol = vol;
    v->noise = 0xace1u;
    /* its length in source samples, 16.16, so any mixer rate agrees */
    v->left = (uint64_t)ticks * SOURCE_RATE / (uint32_t)vm->rate << 16;
    vm_charge(vm, 8);
    return 1;
}

#define RET_INT(v) (*out = mk(T_INT, (int32_t)(v)), 1)

/* sfxat(channel): the effect playing on it, -2 for a tone, -1 nothing */
NATIVE(sfxat)
{
    int32_t ch;
    if (!whole_arg(vm, a, argc, 0, 0, "sfxat", &ch))
        return 0;
    if (ch < 0 || ch > 3)
        return vm_fail(vm, "sfxat() takes a channel from 0 to 3", "", "");
    const struct voice *v = &vm->voices[ch];
    vm_charge(vm, 4);
    return RET_INT(v->kind == VOICE_OFF ? -1 : v->id);
}

void arcade_mute(struct vm *vm, int on)
{
    vm->muted = on ? 1 : 0;
}

int arcade_muted(const struct vm *vm)
{
    return vm->muted;
}

/* click([beep]): the wheel's tick, or its beep */
NATIVE(click)
{
    (void)out;
    int beep = argc > 0 && vm_truthy(vm, a[0]);
    if (vm->host && vm->host->click)
        vm->host->click(vm->host->ctx, beep);
    return 1;
}

/* ---- mixing ------------------------------------------------------------ */

/* A tone's sample: a 32-bit phase, at full scale before its volume */
static int32_t wave_at(struct voice *v)
{
    uint32_t ph = v->phase;
    switch (v->wave) {
    case 0: return ph < 0x80000000u ? 16384 : -16384;                 /* square */
    case 1: return (int32_t)(ph >> 17) - 16384;                         /* saw */
    case 2: {                                                         /* triangle */
        int32_t t = (int32_t)(ph >> 16);
        return t < 32768 ? t - 16384 : 49151 - t;
    }
    case 3: {                                     /* sine: a parabola each half */
        int32_t x = (int32_t)(ph >> 17) - 16384;        /* -16384..16383 */
        int32_t y = 16384 - (int32_t)(((int64_t)x * x) >> 14);
        return ph < 0x80000000u ? y : -y;
    }
    default:                                                          /* noise */
        return v->noise & 1 ? 16384 : -16384;
    }
}

int arcade_audio(struct vm *vm, int16_t *out, int frames, int rate)
{
    int any = 0;
    if (rate <= 0)
        rate = 44100;
    uint32_t src = (uint32_t)(((uint64_t)SOURCE_RATE << 16) / (uint32_t)rate);
    /* Each voice's step at this rate, once a call */
    for (int c = 0; c < 4; c++) {
        struct voice *v = &vm->voices[c];
        if (v->kind == VOICE_OFF)
            continue;
        any = 1;
        if (v->kind == VOICE_SFX)
            v->step = (uint32_t)(((uint64_t)src * v->pitch) >> 16);
        else
            v->dphase = (uint32_t)(((uint64_t)v->hz << 32) / (uint32_t)rate);
    }
    if (!any) {
        memset(out, 0, (size_t)frames * 4);
        return 0;
    }
    for (int f = 0; f < frames; f++) {
        int32_t sum = 0;
        for (int c = 0; c < 4; c++) {
            struct voice *v = &vm->voices[c];
            if (v->kind == VOICE_SFX) {
                sum += v->s0 + (((v->s1 - v->s0) * (int32_t)(v->frac >> 2)) >> 14);
                v->frac += v->step;
                while (v->frac >= 0x10000) {
                    v->frac -= 0x10000;
                    v->s0 = v->s1;
                    if (v->next >= v->n && v->done++) {
                        v->kind = VOICE_OFF;
                        break;
                    }
                    v->s1 = decode(v);
                }
            } else if (v->kind == VOICE_TONE) {
                sum += (wave_at(v) * v->vol) >> 8;
                uint32_t before = v->phase;
                v->phase += v->dphase;
                if (v->phase < before) {
                    /* a turn completed: the noise steps once a period */
                    uint32_t bit = (v->noise ^ v->noise >> 2 ^ v->noise >> 3 ^
                                    v->noise >> 5) & 1;
                    v->noise = (v->noise >> 1) | bit << 15;
                }
                if (v->left <= src)
                    v->kind = VOICE_OFF;
                else
                    v->left -= src;
            }
        }
        if (sum > 32767) sum = 32767;
        if (sum < -32768) sum = -32768;
        out[f * 2] = out[f * 2 + 1] = (int16_t)sum;
    }
    /* muted, mixed all the same, so every effect ends when it would */
    if (vm->muted)
        memset(out, 0, (size_t)frames * 4);
    return 1;
}
