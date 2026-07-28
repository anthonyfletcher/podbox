/***************************************************************************
 * The plugin system has been removed from this fork. All that remains of it in
 * the core is the RAM region a plugin used to run in (pluginbuf, from the
 * linker script), which a handful of core screens borrow as scratch space --
 * the bookmark list, cuesheet display, file copy/move, skin parsing, the
 * playlist viewer and the album art carousel.
 *
 * The rb-> API struct, the .rock loader and the rest of the plugin machinery
 * are all gone. The linker symbol and PLUGIN_BUFFER_SIZE keep their old names
 * because both live outside apps/.
 *
 * One buffer, one user. While the plugin system existed that held by
 * construction -- only one plugin ran at a time. It no longer does: several
 * core screens can be reached from one another, and two of them (the carousel
 * and the playlist viewer) hold the buffer for as long as their screen is up.
 * Nothing copies out of it, so an overlap silently aliases memory.
 *
 * Ownership is therefore tracked, but only for the holders that actually need
 * it. The long-lived pair claim and release around their screen's lifetime;
 * transient callers, which finish before returning to the UI loop, only assert
 * that nobody is holding it. That asymmetry is deliberate: a transient caller
 * cannot reliably release, because at least one of them (the bookmark list)
 * hands a pointer into this buffer back to *its* caller, so the region stays
 * live past the function that acquired it. Requiring a release there would
 * mean threading ownership through four unrelated call sites for no gain --
 * and a missed release would panic during ordinary use, which is a worse
 * failure than the one being prevented.
 * GNU General Public License (version 2+)
 *
 * Hands out the linker-reserved scratch buffer to whichever screen needs a
 * large temporary allocation, guarding against two owners at once.
 ****************************************************************************/
#include "config.h"
#include "panic.h"
#include "app_buffer.h"

/* The scratch region, laid out by the linker script. */
extern unsigned char pluginbuf[];

/* Which long-lived screen holds the buffer, or NULL if none does. */
static const char *buffer_owner;

/* Transient use: finished before returning to the UI loop, so no release. */
void* app_get_buffer(size_t *buffer_size, const char *owner)
{
    if (buffer_owner)
        panicf("app_buffer: %s wants it, %s holds it", owner, buffer_owner);

    *buffer_size = PLUGIN_BUFFER_SIZE;
    return pluginbuf;
}

/* Held for a screen's lifetime; must be matched by app_release_buffer(). */
void* app_claim_buffer(size_t *buffer_size, const char *owner)
{
    if (buffer_owner)
        panicf("app_buffer: %s wants it, %s holds it", owner, buffer_owner);

    buffer_owner = owner;
    *buffer_size = PLUGIN_BUFFER_SIZE;
    return pluginbuf;
}

/* Releasing a buffer nobody holds is deliberately allowed: teardown paths run
 * whether or not setup got as far as claiming (carousel's cleanup() is called
 * on its init-failure path too), and panicking there would break ordinary use
 * to report a non-problem. Releasing a buffer someone *else* holds is a real
 * bug and still panics. */
void app_release_buffer(const char *owner)
{
    if (buffer_owner && buffer_owner != owner)
        panicf("app_buffer: %s released it, %s holds it", owner, buffer_owner);

    buffer_owner = NULL;
}
