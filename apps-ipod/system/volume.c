/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.c (volume and replaygain)
 * Copyright (C) 2002 by Daniel Stenberg
 * GNU General Public License (version 2+)
 *
 * Volume control: the perceptual and direct adjustment modes, the
 * normalised-volume mapping, replaygain mode selection and sound-value
 * formatting.
 ****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"
#include "sound.h"
#include "audio.h"
#include "lang.h"
#include "metadata.h"
#include "fixedpoint.h"
#include "debug.h"
#include "kernel.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "volume.h"

/* check range, set volume and save settings */
void setvol(void)
{
    const int min_vol = sound_min(SOUND_VOLUME);
    const int max_vol = sound_max(SOUND_VOLUME);
    int volume = global_status.volume;
    if (volume < min_vol)
        volume = min_vol;
    if (volume > max_vol)
        volume = max_vol;
    if (volume > global_settings.volume_limit)
        volume = global_settings.volume_limit;

    sound_set_volume(volume);
    global_status.last_volume_change = current_tick;
    status_save(false);
}

static short norm_tab[MAX_NORM_VOLUME_STEPS+2];
static int norm_tab_num_steps;
static int norm_tab_size;

static void update_norm_tab(void)
{
    const int lim = global_settings.volume_adjust_norm_steps;
    if (lim == norm_tab_num_steps)
        return;
    norm_tab_num_steps = lim;

    const int min = sound_min(SOUND_VOLUME);
    const int max = sound_max(SOUND_VOLUME);
    const int step = sound_steps(SOUND_VOLUME);

    /* Ensure the table contains the minimum volume */
    norm_tab[0] = min;
    norm_tab_size = 1;

    for (int i = 1; i < lim - 1; ++i)
    {
        int vol = from_normalized_volume(i, min, max, lim);
        int rem = vol % step;

        vol -= rem;
        if (abs(rem) > step/2)
            vol += rem < 0 ? -step : step;

        /* Add volume step, ignoring any duplicate entries that may
         * occur due to rounding */
        if (vol != norm_tab[norm_tab_size-1])
            norm_tab[norm_tab_size++] = vol;
    }

    /* Ensure the table contains the maximum volume */
    if (norm_tab[norm_tab_size-1] != max)
        norm_tab[norm_tab_size++] = max;
}

void set_normalized_volume(int vol)
{
    update_norm_tab();

    if (vol < 0)
        vol = 0;
    if (vol >= norm_tab_size)
        vol = norm_tab_size - 1;

    global_status.volume = norm_tab[vol];
}

int get_normalized_volume(void)
{
    update_norm_tab();

    int a = 0, b = norm_tab_size - 1;
    while (a != b)
    {
        int i = (a + b + 1) / 2;
        if (global_status.volume < norm_tab[i])
            b = i - 1;
        else
            a = i;
    }

    return a;
}

void adjust_volume(int steps)
{
    adjust_volume_ex(steps, global_settings.volume_adjust_mode);
}

void adjust_volume_ex(int steps, enum volume_adjust_mode mode)
{
    switch (mode)
    {
    case VOLUME_ADJUST_PERCEPTUAL:
        set_normalized_volume(get_normalized_volume() + steps);
        break;
    case VOLUME_ADJUST_DIRECT:
    default:
        global_status.volume += steps * sound_steps(SOUND_VOLUME);
        break;
    }

    setvol();
}

/* Return the ReplayGain mode adjusted by other relevant settings */
static int replaygain_setting_mode(int type)
{
    switch (type)
    {
    case REPLAYGAIN_SHUFFLE:
        type = global_settings.playlist_shuffle ?
            REPLAYGAIN_TRACK : REPLAYGAIN_ALBUM;
    case REPLAYGAIN_ALBUM:
    case REPLAYGAIN_TRACK:
    case REPLAYGAIN_OFF:
    default:
        break;
    }

    return type;
}

/* Return the ReplayGain mode adjusted for display purposes */
int id3_get_replaygain_mode(const struct mp3entry *id3)
{
    if (!id3)
        return -1;

    int type = global_settings.replaygain_settings.type;
    type = replaygain_setting_mode(type);

    return (type != REPLAYGAIN_TRACK && id3->album_gain != 0) ?
        REPLAYGAIN_ALBUM : (id3->track_gain != 0 ? REPLAYGAIN_TRACK : -1);
}

/* Update DSP's replaygain from global settings */
void replaygain_update(void)
{
    struct replaygain_settings settings = global_settings.replaygain_settings;
    settings.type = replaygain_setting_mode(settings.type);
    dsp_replaygain_set_settings(&settings);
}

void format_sound_value_ex(char *buf, size_t buf_sz, int snd, int val, bool skin_token)
{
    int numdec = sound_numdecimals(snd);
    const char *unit = sound_unit(snd);
    int physval = sound_val2phys(snd, val);

    unsigned int factor = ipow(10, numdec);
    if (factor == 0)
    {
        DEBUGF("DIVISION BY ZERO: format_sound_value s:%d v:%d", snd, val);
        factor = 1;
    }
    unsigned int av = abs(physval);
    unsigned int i = av / factor;
    unsigned int d = av - i*factor;

    snprintf(buf, buf_sz, "%s%u%.*s%.*u%s%s", physval < 0 ? "-" : &" "[skin_token],
             i, numdec, ".", numdec, d, &" "[skin_token], skin_token ? "" : unit);
}

/* format a sound value as "-1.05 dB", or " 1.05 dB" */
void format_sound_value(char *buf, size_t buf_sz, int snd, int val)
{
    format_sound_value_ex(buf, buf_sz, snd, val, false);
}

/*
 * "The mapping is designed so that the position in the interval is proportional
 * to the volume as a human ear would perceive it (i.e., the position is the
 * cubic root of the linear sample multiplication factor).  For controls with
 * a small range (24 dB or less), the mapping is linear in the dB values so
 * that each step has the same size visually.  Only for controls without dB
 * information, a linear mapping of the hardware volume register values is used
 * (this is the same algorithm as used in the old alsamixer)."
 */

#define NVOL_FRACBITS 16
#define NVOL_UNITY    (1L << NVOL_FRACBITS)

#define nvol_div(x,y) fp_div((x), (y), NVOL_FRACBITS)
#define nvol_mul(x,y) fp_mul((x), (y), NVOL_FRACBITS)
#define nvol_exp10(x) fp_exp10((x), NVOL_FRACBITS)
#define nvol_log10(x) fp_log10((x), NVOL_FRACBITS)

static long get_nvol_factor(void)
{
    long factor = 600L << NVOL_FRACBITS;
    long numdecimals = sound_numdecimals(SOUND_VOLUME);

    if (numdecimals == 0)
        factor /= 10;

    /* nothing *actually* needs this, but: */
    while (numdecimals-- > 1)
        factor *= 10;

    return factor;
}

long to_normalized_volume(long vol, long min_vol, long max_vol, long max_norm)
{
    long norm, min_norm;
    long factor = get_nvol_factor();

    vol <<= NVOL_FRACBITS;
    min_vol <<= NVOL_FRACBITS;
    max_vol <<= NVOL_FRACBITS;
    max_norm <<= NVOL_FRACBITS;

    min_norm = nvol_exp10(nvol_div(min_vol - max_vol, factor));
    norm = nvol_exp10(nvol_div(vol - max_vol, factor));
    norm = nvol_div(norm - min_norm, NVOL_UNITY - min_norm);

    return nvol_mul(norm, max_norm) >> NVOL_FRACBITS;
}

long from_normalized_volume(long norm, long min_vol, long max_vol, long max_norm)
{
    long vol, min_norm;
    long factor = get_nvol_factor();

    norm <<= NVOL_FRACBITS;
    min_vol <<= NVOL_FRACBITS;
    max_vol <<= NVOL_FRACBITS;
    max_norm <<= NVOL_FRACBITS;

    vol = nvol_div(norm, max_norm);

    min_norm = nvol_exp10(nvol_div(min_vol - max_vol, factor));
    vol = nvol_mul(vol, NVOL_UNITY - min_norm) + min_norm;
    vol = nvol_mul(nvol_log10(vol), factor) + max_vol;

    return vol >> NVOL_FRACBITS;
}

