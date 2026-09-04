/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Board bring-up that a simulator has no hardware for.
 *
 * main.c's init() is the native sequence and calls straight into the target
 * tree. Supplying these eleven-odd symbols is what lets that function run
 * unmodified in a sim -- upstream keeps a whole parallel PLATFORM_HOSTED
 * init() instead, which this fork then would have had to keep in sync.
 ****************************************************************************/
#include "config.h"

#ifdef SIMULATOR

#include <stdbool.h>
#include <stdint.h>
#include <SDL.h>
#include "system.h"
#include "adc.h"
#include "i2c.h"
#include "power.h"
#include "serial.h"
#include "piezo.h"
#include "button.h"
#include "iap.h"
#include "audio.h"
#include "rolo.h"

void i2c_init(void)
{
}

void adc_init(void)
{
}

void power_init(void)
{
}

void serial_setup(void)
{
}

void piezo_init(void)
{
}

#ifdef HAVE_MIKEY_REMOTE
/* No headphone jack to read a remote from. settings_apply() sets this on
 * every boot and the settings menu asks whether the board has one, so both
 * have to exist even where nothing polls anything. The sim says yes, so the
 * row is there to look at. */
void mikey_set_track_skip(bool enable)
{
    (void)enable;
}

bool mikey_supported(void)
{
    return true;
}
#endif

/* The keyclick beep. misc.c calls this when keyclick_hardware is set; the
 * sim's own SOUND_KEYCLICK path covers the audible half. */
void piezo_button_beep(bool beep, bool force)
{
    (void)beep;
    (void)force;
}

/* Debug > View I/O ports. Defined per target under firmware/target/, none of
 * which is built here. Returning false leaves the debug menu on screen, which
 * is what every other unavailable entry does. */
bool dbg_ports(void)
{
    return false;
}

/* The battery-capacity setting's default. The real one picks by RAM size --
 * 400mAh for a thin 5G, 600 for a thick one. Take the thin figure; nothing in
 * a sim discharges. */
int battery_default_capacity(void)
{
    return BATTERY_CAPACITY_DEFAULT;
}

/* Backs USEC_TIMER (see sim/include/system-sim.h). Wraps at the same 32 bits
 * the hardware counters do, so the subtractions the callers do around a render
 * still work across a wrap.
 *
 * SDL's counter rather than clock_gettime(): the latter reaches mingw's
 * pthread_time.h on a Windows build and wants clock_gettime64 from a library
 * the sim does not link. SDL is linked either way.
 *
 * The division is split rather than written as counter * 1000000 / freq --
 * on a nanosecond-resolution counter that product overflows 64 bits after
 * about five hours of uptime. */
unsigned long podbox_sim_usec_timer(void)
{
    Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 now  = SDL_GetPerformanceCounter();

    if (freq == 0)
        return 0;

    return (unsigned long)(uint32_t)((now / freq) * 1000000 +
                                     (now % freq) * 1000000 / freq);
}

#ifdef HAVE_CS42L55
/* 6G only. pcmbuf.c powers the codec's DAC down between tracks, which the
 * CS42L55 needs done around I2S/MCLK; the driver that implements it lives in
 * firmware/drivers/audio and is not built here. */
void audiohw_idle_powerup(void)
{
}

void audiohw_idle_powerdown(void)
{
}
#endif /* HAVE_CS42L55 */

/* The audio path. Both are target driver entry points, reached from
 * audio_set_source(); the sim has one fixed path and nothing to switch. */
void audio_input_mux(int source, unsigned flags)
{
    (void)source;
    (void)flags;
}

void audio_set_output_source(int source)
{
    (void)source;
}

/* ROLO -- loading another firmware image and jumping to it. There is nothing
 * to jump to here. Negative is the "could not load" answer the file browser
 * already handles. */
int rolo_load(const char *file)
{
    (void)file;
    return -1;
}

/* Accessory protocol. apps-ipod/iap/ drops out of SOURCES without
 * IPOD_ACCESSORY_PROTOCOL, but main.c and settings_list.c still reach for
 * these two. */
void iap_setup(int ratenum)
{
    (void)ratenum;
}

void iap_bitrate_set(int ratenum)
{
    (void)ratenum;
}

#endif /* SIMULATOR */
