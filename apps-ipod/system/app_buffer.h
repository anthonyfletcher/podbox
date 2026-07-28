/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to app_buffer.c.
 ****************************************************************************/

#ifndef _APP_BUFFER_H_
#define _APP_BUFFER_H_

#include <stddef.h>

/* A single fixed scratch region, always resident, laid out by the linker
 * script. It was the RAM a loadable plugin ran in; with the plugin system
 * gone, several core screens borrow it as working space instead.
 *
 * There is only one of these buffers, it is not reference counted, and
 * nothing copies out of it -- so two overlapping users silently alias each
 * other's memory. While plugins existed that could not happen, because only
 * one plugin ran at a time. It can now: a screen that holds the buffer can
 * reach another screen that wants it.
 *
 * Two ways to take it, depending on how long you need it:
 *
 *   app_get_buffer()   Transient use, finished before returning to the main
 *                      UI loop. Does not take ownership, so needs no release
 *                      -- but panics if a long-lived holder has it, which is
 *                      the case that would corrupt memory.
 *
 *   app_claim_buffer() Held across UI interaction, for as long as a screen is
 *                      up. Takes ownership; MUST be matched by
 *                      app_release_buffer() when the screen tears down.
 *
 * `owner` is a short literal naming the caller. It appears in the panic
 * message, so make it something worth reading on a device screen.
 */
void* app_get_buffer(size_t *buffer_size, const char *owner);
void* app_claim_buffer(size_t *buffer_size, const char *owner);
void app_release_buffer(const char *owner);

#endif /* _APP_BUFFER_H_ */
