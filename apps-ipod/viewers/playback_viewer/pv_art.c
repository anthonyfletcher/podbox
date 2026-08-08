/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Album and artist thumbnails for the top-five cards.
 *
 * This is nearly all join and almost no work. The artwork cache keys its
 * thumbnails by a hash of the folder path (metadata/art_cache.c), and the log
 * carries the full path of every play -- so the aggregates were able to
 * capture that hash for free while parsing. Showing a row's artwork is then
 * one open and one read of pixels that are already in the display's format.
 *
 * Nothing here searches the database, and nothing decodes an image. If the
 * cache has not got to a folder yet, the row shows its rank instead and no
 * one has to wait.
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <file.h>
#include "config.h"
#include "lcd.h"
#include "pv_art.h"

#ifdef HAVE_ALBUMART
#include "metadata/art_cache.h"
#endif

static fb_data      *slots;                    /* PV_ART_SLOTS thumbnails */
static struct bitmap bm[PV_ART_SLOTS];
static unsigned int  slot_hash[PV_ART_SLOTS];  /* what each slot holds */
static bool          slot_ok[PV_ART_SLOTS];
static int           size_idx = -1;

void pv_art_init(void *buf, size_t bufsz)
{
    slots = NULL;
    size_idx = -1;
    pv_art_reset();

    if (!buf || bufsz < (size_t)PV_ART_BYTES)
        return;

    slots = buf;

#ifdef HAVE_ALBUMART
    /* By name, never by index: the size table is edited over time and the
     * indices move with it (see art_sizes.h). "list" is the 44px square the
     * database rows use, which is the smallest that is still bigger than the
     * 30px these rows draw at. */
    size_idx = art_cache_size_index("list");
#endif
}

void pv_art_reset(void)
{
    for (int i = 0; i < PV_ART_SLOTS; i++)
    {
        slot_hash[i] = 0;
        slot_ok[i] = false;
    }
}

const struct bitmap *pv_art_get(unsigned int hash, int slot)
{
#ifdef HAVE_ALBUMART
    char aat[MAX_PATH];
    int fd, rc;

    if (!slots || size_idx < 0 || hash == 0)
        return NULL;
    if (slot < 0 || slot >= PV_ART_SLOTS)
        return NULL;

    /* Rows are redrawn every animation frame, so a slot already holding the
     * right folder must not be re-read. Without this the card would open
     * five files a frame. */
    if (slot_hash[slot] == hash)
        return slot_ok[slot] ? &bm[slot] : NULL;

    slot_hash[slot] = hash;
    slot_ok[slot] = false;

    /* Ask where the thumbnail WOULD be and try to open it, rather than asking
     * whether it exists and then opening it -- that is two of what should be
     * one, and the open is the existence test anyway. */
    art_cache_thumb_path(hash, size_idx, aat, sizeof(aat));
    fd = open(aat, O_RDONLY);
    if (fd < 0)
        return NULL;

    bm[slot].width  = PV_ART_PX;
    bm[slot].height = PV_ART_PX;
    bm[slot].format = FORMAT_NATIVE;
    bm[slot].data   = (unsigned char *)(slots + (size_t)slot
                                                * PV_ART_PX * PV_ART_PX);

    /* The cached square is 44px and these rows are 30px, so this scales on
     * the way in. Area-averaging 1,936 pixels down to 900 is arithmetic, not
     * decoding -- the expensive part was done by the cache long ago. */
    rc = art_cache_load_aat(fd, &bm[slot],
                            PV_ART_PX * PV_ART_PX * (int)sizeof(fb_data));
    close(fd);

    if (rc <= 0)
        return NULL;

    slot_ok[slot] = true;
    return &bm[slot];
#else
    (void)hash; (void)slot;
    return NULL;
#endif
}
