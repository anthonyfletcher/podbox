/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Album art for accessories, scaled from the art cache. A track's art is
 * its folder's cached thumbnail, so a folder the cache has not reached, or
 * one with no art, has none to send. A serial transfer is one packet an
 * iAP thread event, so the accessory's commands are answered between them;
 * USB iAP loads the art here and sends it itself.
 *
 * Parts, in order:
 *   - art_path(): the thumbnail a track's art is read from
 *   - the queries: iap_artwork_format_valid(), _exists(), _put_formats(),
 *     _put_counts()
 *   - iap_artwork_load(): the art scaled, for either transport
 *   - the serial transfer: iap_artwork_start(), _send_next(), _active(),
 *     _stop()
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "file.h"
#include "lcd.h"
#include "playlist/playlist.h"
#include "metadata/art_cache.h"
#include "settings.h"
#include "usb_log.h"
#include "iap-core.h"
#include "iap-artwork.h"

/* The formats offered, each formatID its index: a small image for a slow
 * link, and a larger one */
static const unsigned char art_dims[] = { 56, 128 };
#define ART_FORMATS ((int)sizeof(art_dims))
#define ART_PIXEL_RGB565LE 0x02
/* The first packet's fields before the pixels, its index included */
#define ART_HEADER 19

static fb_data art_buf[128 * 128];

static struct {
    bool active;
    unsigned char lingo;
    uint16_t command;
    uint16_t tid;           /* the request's */
    uint16_t index;         /* the next packet's */
    int dim;
    size_t offset;          /* bytes of art_buf sent */
} art_tx;

static int wps_size = -2;

/* The thumbnail of the track's folder, keyed as the playback engine keys
 * it; there is a file at the path only if the folder has art. None with
 * Accessory Artwork off, so no track has art to send. */
static bool art_path(uint32_t track, char *out, int out_len)
{
    struct playlist_track_info info;
    char dir[MAX_PATH];
    const char *sep;
    int tracknum, dlen;

    if (global_settings.car_artwork == CAR_ARTWORK_OFF)
        return false;
    if (wps_size == -2)
        wps_size = art_cache_size_index("wps");
    if (wps_size < 0 || track >= (uint32_t)playlist_amount())
        return false;

    tracknum = track + playlist_get_first_index(NULL);
    if (tracknum >= playlist_amount())
        tracknum -= playlist_amount();
    if (playlist_get_track_info(NULL, tracknum, &info) < 0)
        return false;

    sep = strrchr(info.filename, '/');
    dlen = sep ? sep - info.filename : 0;
    if (dlen <= 0 || dlen >= (int)sizeof(dir))
        return false;
    memcpy(dir, info.filename, dlen);
    dir[dlen] = '\0';

    art_cache_thumb_path(art_cache_dir_hash(dir), wps_size, out, out_len);
    return true;
}

/* Payload bytes a packet may carry, lingo and command included */
static int art_limit(void)
{
    int cap = device.max_payload - (device.transid ? 2 : 0);

    return (device.max_payload && cap < TX_BUFLEN) ? cap : TX_BUFLEN;
}

bool iap_artwork_format_valid(uint16_t format)
{
    return format < ART_FORMATS;
}

/* Only a thumbnail art_cache_load_aat() can scale to every format counts,
 * so a track reported as having art always has it to send */
int iap_artwork_check(uint32_t track)
{
    char path[MAX_PATH];
    struct art_cache_header hdr;
    int fd;
    bool ok;

    if (global_settings.car_artwork == CAR_ARTWORK_OFF)
        return USB_LOG_IAP_ART_CACHE_OFF;
    if (!art_path(track, path, sizeof(path)))
        return USB_LOG_IAP_ART_CACHE_NO_TRACK;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return USB_LOG_IAP_ART_CACHE_NO_FILE;
    ok = read(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)
         && hdr.magic == ART_CACHE_MAGIC
         && hdr.version == ART_CACHE_FORMAT_VERSION
         && hdr.layout == AA_ROWS && hdr.width == hdr.height
         && hdr.width >= art_dims[ART_FORMATS - 1];
    close(fd);
    return ok ? USB_LOG_IAP_ART_CACHE_OK : USB_LOG_IAP_ART_CACHE_BAD_FILE;
}

bool iap_artwork_exists(uint32_t track)
{
    return iap_artwork_check(track) == USB_LOG_IAP_ART_CACHE_OK;
}

void iap_artwork_put_formats(void)
{
    int i;

    for (i = 0; i < ART_FORMATS; i++)
    {
        IAP_TX_PUT_U16(i);
        IAP_TX_PUT(ART_PIXEL_RGB565LE);
        IAP_TX_PUT_U16(art_dims[i]);
        IAP_TX_PUT_U16(art_dims[i]);
    }
}

void iap_artwork_put_counts(uint32_t track)
{
    int i;

    if (!iap_artwork_exists(track))
        return;
    for (i = 0; i < ART_FORMATS; i++)
    {
        IAP_TX_PUT_U16(i);
        IAP_TX_PUT_U16(1);
    }
}

bool iap_artwork_load(uint32_t track, int dim, void *buf, int size)
{
    char path[MAX_PATH];
    struct bitmap bm;
    int fd, rc;

    if (!art_path(track, path, sizeof(path)))
        return false;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;

    memset(&bm, 0, sizeof(bm));
    bm.width = bm.height = dim;
    bm.format = FORMAT_NATIVE;
    bm.data = buf;
    rc = art_cache_load_aat(fd, &bm, size, NULL);
    close(fd);
    return rc > 0;
}

bool iap_artwork_start(unsigned char lingo, uint16_t command,
                       uint32_t track, uint16_t format)
{
    iap_artwork_stop();
    /* the first packet carries its header and at least one byte */
    if (!iap_artwork_format_valid(format)
        || art_limit() <= (lingo == 0x04 ? 3 : 2) + ART_HEADER
        || !iap_artwork_load(track, art_dims[format], art_buf,
                             sizeof(art_buf)))
        return false;

    art_tx.lingo = lingo;
    art_tx.command = command;
    art_tx.tid = iap_rx_tid();
    art_tx.index = 0;
    art_tx.dim = art_dims[format];
    art_tx.offset = 0;
    art_tx.active = true;
    iap_artwork_post();
    return true;
}

bool iap_artwork_send_next(void)
{
    size_t size = (size_t)art_tx.dim * art_tx.dim * sizeof(fb_data);
    int limit = art_limit(), used;
    size_t chunk;

    if (!art_tx.active)
        return false;

    if (art_tx.lingo == 0x04)
        IAP_TX_INIT4(0x04, art_tx.command);
    else
        IAP_TX_INIT(art_tx.lingo, art_tx.command);
    IAP_TX_PUT_U16(art_tx.index);
    if (art_tx.index == 0)
    {
        IAP_TX_PUT(ART_PIXEL_RGB565LE);
        IAP_TX_PUT_U16(art_tx.dim);
        IAP_TX_PUT_U16(art_tx.dim);
        /* the inset rectangle: all of it */
        IAP_TX_PUT_U16(0);
        IAP_TX_PUT_U16(0);
        IAP_TX_PUT_U16(art_tx.dim - 1);
        IAP_TX_PUT_U16(art_tx.dim - 1);
        IAP_TX_PUT_U32(art_tx.dim * sizeof(fb_data));
    }

    /* No packet larger than the accessory takes; a limit it lowers during
     * the transfer can leave no room */
    used = iap_txnext - iap_txpayload;
    if (limit <= used)
    {
        iap_artwork_stop();
        return false;
    }

    /* Native RGB565 is little-endian on both players, the format offered */
    chunk = MIN((size_t)(limit - used), size - art_tx.offset);
    IAP_TX_PUT_DATA((unsigned char *)art_buf + art_tx.offset, chunk);
    iap_send_tx_tid(art_tx.tid);

    art_tx.offset += chunk;
    art_tx.index++;
    art_tx.active = art_tx.offset < size;
    return art_tx.active;
}

bool iap_artwork_active(void)
{
    return art_tx.active;
}

void iap_artwork_stop(void)
{
    art_tx.active = false;
}
