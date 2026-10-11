/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The playing track's chapters, for serial and USB iAP alike: the lingo
 * handlers call it from the iAP thread, platform.c from the USB thread.
 *
 * Parts, in order:
 *   - iap_chapter_at(): one chapter of a track's cue sheet
 *   - iap_current_chapter(): the playing one
 *   - iap_set_chapter(), iap_skip_chapter(): seeking to one
 ****************************************************************************/
#include "config.h"
#include "audio.h"
#include "metadata.h"
#include "metadata/cuesheet.h"
#include "iap-chapters.h"

bool iap_chapter_at(const struct mp3entry *id3, uint32_t index,
                    struct iap_chapter_info *chapter)
{
    struct cuesheet *cue;
    unsigned long start, end;

    if (!id3 || !(cue = id3->cuesheet) || cue->track_count <= 0
        || index >= (uint32_t)cue->track_count)
        return false;

    start = cue->tracks[index].offset;
    end = index + 1 < (uint32_t)cue->track_count
        ? cue->tracks[index + 1].offset : id3->length;
    if (end < start)
        end = start;

    chapter->index = index;
    chapter->count = cue->track_count;
    chapter->offset_ms = start;
    chapter->length_ms = end - start;
    chapter->elapsed_ms = id3->elapsed > start ? id3->elapsed - start : 0;
    if (chapter->elapsed_ms > chapter->length_ms)
        chapter->elapsed_ms = chapter->length_ms;
    chapter->name = cue->tracks[index].title;
    return true;
}

bool iap_current_chapter(struct iap_chapter_info *chapter)
{
    struct mp3entry *id3 = audio_current_track();
    struct cuesheet *cue;
    uint32_t index = 0;

    if (!(audio_status() & AUDIO_STATUS_PLAY) || !id3
        || !(cue = id3->cuesheet) || cue->track_count <= 0)
        return false;
    while (index + 1 < (uint32_t)cue->track_count
           && cue->tracks[index + 1].offset <= id3->elapsed)
        index++;
    return iap_chapter_at(id3, index, chapter);
}

bool iap_set_chapter(uint32_t index)
{
    struct iap_chapter_info chapter;

    if (!(audio_status() & AUDIO_STATUS_PLAY)
        || !iap_chapter_at(audio_current_track(), index, &chapter))
        return false;
    audio_pre_ff_rewind();
    audio_ff_rewind(chapter.offset_ms);
    return true;
}

bool iap_skip_chapter(int direction)
{
    struct iap_chapter_info chapter;
    uint32_t target;

    if (!iap_current_chapter(&chapter))
        return false;
    if (direction > 0)
    {
        if (chapter.index + 1 >= chapter.count)
            return false;
        target = chapter.index + 1;
    }
    else if (chapter.elapsed_ms > 2000)
        target = chapter.index;
    else if (chapter.index > 0)
        target = chapter.index - 1;
    else
        return false;
    return iap_set_chapter(target);
}
