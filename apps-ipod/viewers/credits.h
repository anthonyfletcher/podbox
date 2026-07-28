/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to credits.c.
 ****************************************************************************/
#ifndef _CREDITS_H_
#define _CREDITS_H_

/* Full-screen scrolling credits, launched from the System menu. Returns
 * SYS_USB_CONNECTED if a USB connection ended the screen, else 0. */
int credits_screen(void);

#endif /* _CREDITS_H_ */
