/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The scratch buffer, as a plain array.
 *
 * apps-ipod/system/app_buffer.c takes `pluginbuf` from the target linker
 * script -- the region a plugin used to run in, which core screens now borrow.
 * A simulator has no linker script, so the symbol has to come from somewhere.
 * Upstream does the same thing at apps/plugin.c for hosted builds.
 *
 * Cover Flow, album-art scaling and the image viewer all reach this through
 * app_get_buffer() / app_claim_buffer(), so nothing worth simulating runs
 * without it.
 ****************************************************************************/
#include "config.h"

#ifdef SIMULATOR

unsigned char pluginbuf[PLUGIN_BUFFER_SIZE];

/* Same story for the codec region (audio/codecs.c). CODEC_SIZE is 0 off
 * target, because a sim loads codecs as host shared objects and never copies
 * one into a buffer -- the array exists to satisfy the reference, and
 * codec_get_buffer_callback() correctly reports no room in it. */
unsigned char codecbuf[CODEC_SIZE];

#endif /* SIMULATOR */
