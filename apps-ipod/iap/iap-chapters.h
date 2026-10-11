/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The playing track's chapters, for serial and USB iAP alike.
 ****************************************************************************/
#ifndef _IAP_CHAPTERS_H_
#define _IAP_CHAPTERS_H_

#include <stdbool.h>
#include <stdint.h>

struct mp3entry;

/* The playing track's chapters are its cue sheet's tracks, which an M4B's
 * chapters become */
struct iap_chapter_info {
    uint32_t index;
    uint32_t count;
    uint32_t offset_ms;
    uint32_t length_ms;
    uint32_t elapsed_ms;
    const char *name;
};
bool iap_chapter_at(const struct mp3entry *id3, uint32_t index,
                    struct iap_chapter_info *chapter);
/* False with nothing playing, or a track with no chapters */
bool iap_current_chapter(struct iap_chapter_info *chapter);
bool iap_set_chapter(uint32_t index);
/* Back goes to the chapter's start, or within two seconds of it the one
 * before. False with no chapter to go to. */
bool iap_skip_chapter(int direction);

#endif /* _IAP_CHAPTERS_H_ */
