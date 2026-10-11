/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Album art for serial accessories, scaled from the art cache.
 ****************************************************************************/
#ifndef _IAP_ARTWORK_H_
#define _IAP_ARTWORK_H_

#include <stdbool.h>
#include <stdint.h>

/* RetArtworkFormats' records: formatID, pixel format, width, height */
void iap_artwork_put_formats(void);
/* The records of the artwork count track info type: formatID and count */
void iap_artwork_put_counts(uint32_t track);
/* Whether the track, counted from the playlist's first, has art */
bool iap_artwork_exists(uint32_t track);
bool iap_artwork_format_valid(uint16_t format);

/* Sends the track's art in that format as RetTrackArtworkData packets of
 * the lingo and command given, which iap_artwork_send_next() continues
 * from the iAP thread. False with no art to send. */
bool iap_artwork_start(unsigned char lingo, uint16_t command,
                       uint32_t track, uint16_t format);
/* True while packets remain */
bool iap_artwork_send_next(void);
bool iap_artwork_active(void);
void iap_artwork_stop(void);

#endif /* _IAP_ARTWORK_H_ */
