/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to text_reel.c.
 ****************************************************************************/

#ifndef _TEXT_REEL_H_
#define _TEXT_REEL_H_

/* Crawls `count` lines up the whole screen, centred, in the theme's colours,
 * word-wrapping each to the screen width. An empty line renders as a one-line
 * gap, which is how sections are spaced apart. The wheel takes the reel over
 * while it is being turned and auto-scroll resumes a second after it stops;
 * the reel closes itself once the last line has cleared the top.
 *
 * `heights` is the caller's, one byte per line: each line's height is measured
 * on open and kept, so scroll positions stay stable and only the lines on
 * screen are wrapped again per frame.
 *
 * Returns a menu return code: SYS_USB_CONNECTED if USB was attached, else 0. */
int text_reel_run(const char *const *lines, unsigned char *heights, int count);

#endif /* _TEXT_REEL_H_ */
