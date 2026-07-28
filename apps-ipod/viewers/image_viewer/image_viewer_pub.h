/***************************************************************************
 * Public entry point for the core-linked image viewer. Kept separate from
 * image_viewer.h (which defines internal status codes that would clash with
 * plugin.h) so call sites that also include plugin.h can use it.
 * GNU General Public License (version 2+)
 *
 * The image viewer's public entry point, for callers outside viewers/.
 ****************************************************************************/

#ifndef _IMAGE_VIEWER_PUB_H_
#define _IMAGE_VIEWER_PUB_H_

/* Open `file` (jpg/png/bmp/gif/ppm) in the full-screen image viewer, or the
 * current track's album art when `file` is NULL. Returns a GO_TO_* code. */
int image_viewer(const char *file);

#endif /* _IMAGE_VIEWER_PUB_H_ */
