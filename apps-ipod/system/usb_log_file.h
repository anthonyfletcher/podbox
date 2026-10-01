/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The USB log (usb_log.h) on disk, at USB_LOG_FILE. Two things write it: the
 * debug menu's USB Log screen while it is open, and, with the Write Debug Log
 * setting on, a background thread that keeps the file current whenever the
 * player has its disk. Either way a step that could hang the player waits for
 * the file first, so the file's last line names it.
 ****************************************************************************/
#ifndef _USB_LOG_FILE_H
#define _USB_LOG_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include "rbpaths.h"

#define USB_LOG_FILE ROCKBOX_DIR "/usb-log.txt"

struct usb_log_entry;

/* One entry as a line of text, without the newline. */
void usb_log_file_format(const struct usb_log_entry *e, char *buf,
                         size_t size);

/* Append everything not yet in the file. */
void usb_log_file_write(void);

/* The screen opening and closing. Opening writes a header line. */
void usb_log_file_screen(bool open);

/* The setting's callback. */
void usb_log_file_enable(bool on);

void usb_log_file_init(void);

#endif /* _USB_LOG_FILE_H */
