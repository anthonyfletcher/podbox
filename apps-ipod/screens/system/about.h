/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to about.c.
 ****************************************************************************/

#ifndef _ABOUT_H_
#define _ABOUT_H_

/* The About page: what this firmware is, scrolling up the screen. Returns a
 * menu return code (SYS_USB_CONNECTED if USB was attached, else 0). */
int about_screen(void);

#endif /* _ABOUT_H_ */
